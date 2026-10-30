/*
 * nvn_metal.mm -- NVN -> Metal host backend (Apple platforms only).
 *
 * Scope, stated up front so nobody mistakes this for finished:
 *
 *   DONE  device / queue / window / swapchain lifecycle
 *         memory pool -> zero-copy MTLBuffer over guest memory
 *         command buffer -> MTLCommandBuffer, viewport / scissor / drawArrays
 *         present (blit fast path, cleared-drawable fallback)
 *
 *   TODO  render-target binding (nvnCommandBufferSetRenderTargets): until it
 *         exists no MTLRenderCommandEncoder is ever created, so guest draws
 *         are counted but not executed
 *         texture objects (builder field offsets unverified) -> no MTLTexture
 *         shader translation (NVN ships prebuilt Maxwell SASS, not source)
 *         texture tiling, BCn -> ASTC/ETC2 transcode, compute, sync/fence
 *
 * Argument ABI: measured, not guessed
 * -----------------------------------
 * Every host proc receives the guest's x0..x7 in x[].  The shapes live in
 * ios/nvn/signatures.json, extracted from the public nxnative prototype list
 * (MIT).  Three consequences decide this whole file:
 *
 *   - NVN objects are GUEST memory.  nvnWindowInitialize(NVNwindow*, builder)
 *     takes a pointer to the guest's own object; the host never owns that
 *     storage and never hands one back.  Host state is therefore keyed by the
 *     guest address, which is what x[0] holds on every object entry point.
 *   - Return values go in w0, i.e. x[0].  NVNboolean is 1/0, and getters
 *     overwrite x[0] with their result.
 *   - Out-parameters are pointers into guest memory.  Unified memory means we
 *     write through them directly, e.g. nvnWindowAcquireTexture(w, sync, &idx).
 *
 * The first version of this file got all three wrong (it treated x[0] as an
 * out-parameter and returned host pointers to the guest), which compiles fine
 * and would have made every later call dereference guest memory as a host
 * object.  ios/nvn/signatures.json exists so that cannot happen quietly again.
 *
 * Two things this file deliberately does NOT do:
 *
 *   1. It never invents NVN enum values.  Formats, primitive types and index
 *      types are numbers whose real values live in nvn.h.  Until
 *      kEnumsVerified is flipped, every enum goes through log_once() and the
 *      call is dropped -- loudly, once per value.
 *
 *   2. It never copies guest GPU memory "just in case".  iOS lives on unified
 *      memory, so the guest heap is already host-readable and Metal can wrap
 *      it with newBufferWithBytesNoCopy.  The requirement is that the guest
 *      allocator hands out page-aligned storage; see docs/IOS_PORTING.md.
 */
#if defined(__APPLE__)

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "nvn_backend.h"
#include "nvn_metal.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>

namespace {

/* ------------------------------------------------------------------ */
/* honesty gate                                                        */
/* ------------------------------------------------------------------ */

/*
 * 0 = no NVN enum table has been verified against nvn.h yet.
 *
 * Two gates enforce this, on purpose at different layers:
 *   - compile time: the #error below refuses an NDEBUG (Release) build;
 *   - repository:   scripts/verify_ios.py --release refuses to sign off.
 * Flipping this to 1 means "every value the switch statements below use has
 * been read out of a real nvn.h", not "the switch compiles".
 */
constexpr int kEnumsVerified = 0;

/* NDEBUG is set explicitly for the Release configuration in ios/project.yml
 * so this guard does not depend on Xcode's default build settings. */
#if defined(NDEBUG)
#error "Release build refused: NVN enum values are unverified (kEnumsVerified = 0). See docs/IOS_PORTING.md, section 'Why Release does not build'."
#endif

/* Deduplicated by name *and* value: the same NVN enum can arrive with many
 * values (formats alone are dozens), and each distinct one deserves one line. */
void log_once(const char* what, unsigned value) {
    static std::unordered_map<std::string, std::set<unsigned>> seen;
    if (!seen[what].insert(value).second) return;
    std::fprintf(stderr, "[nvn] UNVERIFIED %s = 0x%X (call dropped, see nvn_metal.mm)\n",
                 what, value);
}

/* ------------------------------------------------------------------ */
/* host state, keyed by the guest object address                       */
/* ------------------------------------------------------------------ */

struct Ctx {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    /* The layer the app displays.  Borrowed: the view owns it. */
    CAMetalLayer* nativeLayer = nil;
    unsigned presents = 0;
    unsigned dropped = 0;
    bool ready = false;
};

struct Win {
    CAMetalLayer* layer = nil;
    unsigned numTextures = 0;
    unsigned numActive = 0;
    unsigned presentInterval = 1;
    unsigned cursor = 0;
    uint64_t textures[4] = {0, 0, 0, 0};   /* guest NVNtexture addresses */
};

struct Pool {
    void* storage = nullptr;
    size_t size = 0;
    uint32_t flags = 0;
    id<MTLBuffer> buffer = nil;
};

struct Tex {
    unsigned width = 0, height = 0, depth = 1, levels = 1, layers = 1;
    uint32_t format = 0;
    uint32_t target = 0;
    uint64_t pool = 0;
    ptrdiff_t offset = 0;
    id<MTLTexture> mtl = nil;
};

struct CmdBuf {
    bool recording = false;
    unsigned draws = 0;
    id<MTLCommandBuffer> cb = nil;
    id<MTLRenderCommandEncoder> enc = nil;
    /* Recorded but not applied: there is no render target to apply them to. */
    int clearIndex = 0;
    int clearMask = 0;
    float clear[4] = {0, 0, 0, 0};
};

/* Builder specs are gathered in a side table keyed by the guest's builder
 * pointer, because the guest owns that struct and its layout is not verified
 * yet.  Keeping the mapping here means only one place has to change once the
 * real offsets are known. */
struct WinSpec {
    uint64_t nativeWindow = 0;
    unsigned presentInterval = 1;
    unsigned numActive = 0;
    unsigned numTextures = 0;
    uint64_t textures[4] = {0, 0, 0, 0};
};

struct PoolSpec {
    void* storage = nullptr;
    size_t size = 0;
    uint32_t flags = 0;
};

Ctx* ctx_of(nvngfx_ctx* ctx) { return reinterpret_cast<Ctx*>(ctx); }

template <class T>
std::unordered_map<uint64_t, T*>& table() {
    static std::unordered_map<uint64_t, T*> instance;
    return instance;
}

template <class T>
T* find(uint64_t guest) {
    auto& m = table<T>();
    auto it = m.find(guest);
    return it == m.end() ? nullptr : it->second;
}

template <class T>
T* find_or_add(uint64_t guest) {
    T*& slot = table<T>()[guest];
    if (!slot) slot = new T();
    return slot;
}

template <class T>
void forget(uint64_t guest) {
    auto& m = table<T>();
    auto it = m.find(guest);
    if (it == m.end()) return;
    delete it->second;
    m.erase(it);
}

std::unordered_map<uint64_t, WinSpec> g_win_specs;
std::unordered_map<uint64_t, PoolSpec> g_pool_specs;
std::unordered_map<uint64_t, int> g_device_flags;

/* Guest memory is host-visible, so a pointer argument is just a cast.  This
 * is the one place that assumption is spelled out. */
template <class T>
T* guest_ptr(uint64_t value) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
}

/* ------------------------------------------------------------------ */
/* NVN -> Metal primitives                                             */
/* ------------------------------------------------------------------ */

MTLPrimitiveType to_metal_primitive(uint32_t nvn) {
    if (!kEnumsVerified) {
        log_once("NVNdrawPrimitive", nvn);
        return MTLPrimitiveTypeTriangle;
    }
    switch (nvn) {  /* TODO(verify): fill from nvn.h */
        case 0: return MTLPrimitiveTypeTriangle;
        default: log_once("NVNdrawPrimitive", nvn); return MTLPrimitiveTypeTriangle;
    }
}

[[maybe_unused]] MTLPixelFormat to_metal_format(uint32_t nvn) {
    if (!kEnumsVerified) {
        log_once("NVNformat", nvn);
        return MTLPixelFormatBGRA8Unorm;
    }
    switch (nvn) {  /* TODO(verify) */
        case 0: return MTLPixelFormatRGBA8Unorm;
        default: log_once("NVNformat", nvn); return MTLPixelFormatBGRA8Unorm;
    }
}

/* ------------------------------------------------------------------ */
/* present                                                             */
/* ------------------------------------------------------------------ */

/* Clears `layer` to one colour and presents it.  Returns true if the frame
 * reached the display.  Shared by the host self-test and by
 * nvnQueuePresentTexture when the guest texture has no MTLTexture yet. */
bool present_clear(Ctx* c, CAMetalLayer* layer, float r, float g, float b) {
    if (!c || !c->ready || !layer) return false;
    if (layer.drawableSize.width < 1.0 || layer.drawableSize.height < 1.0) return false;
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) {
            c->dropped++;
            return false;
        }
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = drawable.texture;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, 1.0);
        id<MTLCommandBuffer> cb = [c->queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
        [enc endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];
    }
    c->presents++;
    return true;
}

/* ------------------------------------------------------------------ */
/* device                                                              */
/* ------------------------------------------------------------------ */

/* nvnDeviceBuilderSetDefaults(NVNdeviceBuilder*) */
void dev_builder_set_defaults(nvngfx_ctx*, uint64_t* x) {
    if (x[0]) std::memset(guest_ptr<void>(x[0]), 0, 0x20);
}

/* nvnDeviceBuilderSetFlags(NVNdeviceBuilder*, int) */
void dev_builder_set_flags(nvngfx_ctx*, uint64_t* x) {
    g_device_flags[x[0]] = static_cast<int>(x[1]);
}

/* nvnDeviceBuilderGetFlags(const NVNdeviceBuilder*) -> int
 * NOTE: inferred shape, see ios/nvn/inferred_signatures.json. */
void dev_builder_get_flags(nvngfx_ctx*, uint64_t* x) {
    auto it = g_device_flags.find(x[0]);
    x[0] = it == g_device_flags.end() ? 0u : static_cast<uint64_t>(static_cast<uint32_t>(it->second));
}

/* nvnDeviceInitialize(NVNdevice*, const NVNdeviceBuilder*) -> NVNboolean */
void dev_initialize(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    if (!c) {
        x[0] = 0;
        return;
    }
    /* The app normally already handed us its device (nvngfx_metal_set_device);
     * only fall back to the system default when the backend is used headless. */
    if (!c->device) c->device = MTLCreateSystemDefaultDevice();
    if (!c->device) {
        std::fprintf(stderr, "[nvn] no Metal device\n");
        x[0] = 0;
        return;
    }
    if (!c->queue) c->queue = [c->device newCommandQueue];
    c->ready = (c->queue != nil);
    std::fprintf(stderr, "[nvn] metal device: %s\n", [[c->device name] UTF8String]);
    x[0] = c->ready ? 1u : 0u;
}

/* nvnDeviceGetProcAddress(const NVNdevice*, const char*) -> proc */
void dev_get_proc_address(nvngfx_ctx*, uint64_t* x) {
    const char* name = guest_ptr<const char>(x[1]);
    nvngfx_proc p = nvngfx_lookup(name);
    x[0] = p ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)) : 0;
}

/* ------------------------------------------------------------------ */
/* window                                                              */
/* ------------------------------------------------------------------ */

/* nvnWindowBuilderSetDefaults(NVNwindowBuilder*) */
void win_builder_set_defaults(nvngfx_ctx*, uint64_t* x) {
    if (x[0]) std::memset(guest_ptr<void>(x[0]), 0, 0x40);
}

/* nvnWindowBuilderSetNativeWindow(NVNwindowBuilder*, NVNnativeWindow) */
void win_builder_set_native_window(nvngfx_ctx*, uint64_t* x) {
    g_win_specs[x[0]].nativeWindow = x[1];
}

/* nvnWindowBuilderSetTextures(NVNwindowBuilder*, int n, NVNtexture* const*) */
void win_builder_set_textures(nvngfx_ctx*, uint64_t* x) {
    WinSpec& s = g_win_specs[x[0]];
    const int n = static_cast<int>(x[1]);
    s.numTextures = n <= 0 ? 0u : (n > 4 ? 4u : static_cast<unsigned>(n));
    const uint64_t* list = guest_ptr<const uint64_t>(x[2]);
    for (unsigned i = 0; i < 4; ++i) s.textures[i] = (list && i < s.numTextures) ? list[i] : 0u;
}

/* nvnWindowBuilderSetPresentInterval(NVNwindowBuilder*, int) */
void win_builder_set_present_interval(nvngfx_ctx*, uint64_t* x) {
    g_win_specs[x[0]].presentInterval = static_cast<unsigned>(x[1]);
}

/* nvnWindowBuilderSetNumActiveTextures(NVNwindowBuilder*, int)
 * NOTE: inferred shape, see ios/nvn/inferred_signatures.json. */
void win_builder_set_num_active_textures(nvngfx_ctx*, uint64_t* x) {
    g_win_specs[x[0]].numActive = static_cast<unsigned>(x[1]);
}

/* nvnWindowInitialize(NVNwindow*, const NVNwindowBuilder*) -> NVNboolean */
void win_initialize(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    if (!c || !c->ready) {
        x[0] = 0;
        return;
    }

    WinSpec spec;
    auto it = g_win_specs.find(x[1]);
    if (it != g_win_specs.end()) spec = it->second;

    Win* w = find_or_add<Win>(x[0]);
    /* spec.nativeWindow is the guest's nn::vi handle, which is NOT a
     * CAMetalLayer -- casting it here would message a random pointer.  Until
     * vi layer handles are mapped onto host layers, the only safe surface is
     * the one the app gave us in nvngfx_metal_set_layer().  Falling back to a
     * private layer keeps the backend usable headless; frames then go nowhere
     * visible, which is what botw_core_nvn_stats.presents reports as 0. */
    w->layer = c->nativeLayer ? c->nativeLayer
                              : static_cast<CAMetalLayer*>([CAMetalLayer layer]);
    w->layer.device = c->device;
    w->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    w->layer.framebufferOnly = NO;  /* we blit the guest's target in */
    /* There is no present-interval property on iOS: CAMetalLayer.displaySyncEnabled
     * is macOS-only (the iOS 17 SDK rejects it -- CI caught this).  On iOS v-sync
     * is implicit, so maximumDrawableCount below is the only pacing knob we own. */
    w->numTextures = spec.numTextures;
    w->numActive = spec.numActive ? spec.numActive : spec.numTextures;
    w->presentInterval = spec.presentInterval;
    for (unsigned i = 0; i < 4; ++i) w->textures[i] = spec.textures[i];

    /* Keep the drawable count to the active texture count so the CPU can run
     * ahead exactly as far as the guest's own swapchain allows. */
    unsigned n = w->numActive;
    if (n < 2) n = 2;
    if (n > 3) n = 3;
    w->layer.maximumDrawableCount = n;

    x[0] = 1;  /* NVNboolean true */
}

/* nvnWindowAcquireTexture(NVNwindow*, NVNsync*, int* index)
 * -> NVNwindowAcquireTextureResult (0 = success; the enum itself is unverified) */
void win_acquire_texture(nvngfx_ctx*, uint64_t* x) {
    Win* w = find<Win>(x[0]);
    int* outIndex = guest_ptr<int>(x[2]);
    if (!w || w->numTextures == 0) {
        log_once("nvnWindowAcquireTextureResult.error", 1);
        x[0] = 1;  /* not SUCCESS; exact value unverified */
        return;
    }
    const unsigned index = w->cursor % w->numTextures;
    w->cursor = (w->cursor + 1) % w->numTextures;
    if (outIndex) *outIndex = static_cast<int>(index);
    x[0] = 0;  /* SUCCESS */
}

/* nvnWindowGetNumTextures(const NVNwindow*) -> int
 * NOTE: inferred shape, see ios/nvn/inferred_signatures.json. */
void win_get_num_textures(nvngfx_ctx*, uint64_t* x) {
    Win* w = find<Win>(x[0]);
    x[0] = w ? w->numTextures : 0u;
}

/* nvnWindowSetPresentInterval(NVNwindow*, int) */
void win_set_present_interval(nvngfx_ctx*, uint64_t* x) {
    Win* w = find<Win>(x[0]);
    if (!w) return;
    /* Recorded only.  iOS has no displaySyncEnabled and the interval is
     * enforced by the guest's own swapchain, not by Metal. */
    w->presentInterval = static_cast<unsigned>(x[1]);
}

/* nvnWindowSetNumActiveTextures(NVNwindow*, int)
 * NOTE: inferred shape, see ios/nvn/inferred_signatures.json. */
void win_set_num_active_textures(nvngfx_ctx*, uint64_t* x) {
    Win* w = find<Win>(x[0]);
    if (w) w->numActive = static_cast<unsigned>(x[1]);
}

/* nvnWindowFinalize(NVNwindow*) */
void win_finalize(nvngfx_ctx*, uint64_t* x) { forget<Win>(x[0]); }

/* ------------------------------------------------------------------ */
/* memory pools                                                        */
/* ------------------------------------------------------------------ */

/* nvnMemoryPoolBuilderSetDefaults(NVNmemoryPoolBuilder*) */
void pool_builder_set_defaults(nvngfx_ctx*, uint64_t* x) {
    if (x[0]) std::memset(guest_ptr<void>(x[0]), 0, 0x30);
}

/* nvnMemoryPoolBuilderSetStorage(NVNmemoryPoolBuilder*, void*, size_t) */
void pool_builder_set_storage(nvngfx_ctx*, uint64_t* x) {
    PoolSpec& s = g_pool_specs[x[0]];
    s.storage = guest_ptr<void>(x[1]);
    s.size = static_cast<size_t>(x[2]);
}

/* nvnMemoryPoolBuilderSetFlags(NVNmemoryPoolBuilder*, int) */
void pool_builder_set_flags(nvngfx_ctx*, uint64_t* x) {
    g_pool_specs[x[0]].flags = static_cast<uint32_t>(x[1]);
}

/* nvnMemoryPoolInitialize(NVNmemoryPool*, const NVNmemoryPoolBuilder*) -> NVNboolean */
void pool_initialize(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    if (!c || !c->ready) {
        x[0] = 0;
        return;
    }

    PoolSpec spec;
    auto it = g_pool_specs.find(x[1]);
    if (it != g_pool_specs.end()) spec = it->second;
    if (!spec.storage || !spec.size) {
        std::fprintf(stderr, "[nvn] nvnMemoryPoolInitialize without storage\n");
        x[0] = 0;
        return;
    }

    Pool* p = find_or_add<Pool>(x[0]);
    p->storage = spec.storage;
    p->size = spec.size;
    p->flags = spec.flags;

    /* Unified memory: wrap, never copy. */
    if ((reinterpret_cast<uintptr_t>(p->storage) & 0xFFF) != 0) {
        std::fprintf(stderr, "[nvn] WARNING pool storage %p is not page aligned\n", p->storage);
    }
    p->buffer = [c->device newBufferWithBytesNoCopy:p->storage
                                             length:p->size
                                            options:MTLResourceStorageModeShared
                                        deallocator:nil];
    if (!p->buffer) {
        std::fprintf(stderr, "[nvn] pool wrap failed (%zu bytes)\n", p->size);
        forget<Pool>(x[0]);
        x[0] = 0;
        return;
    }
    x[0] = 1;
}

/* nvnMemoryPoolFinalize(NVNmemoryPool*) */
void pool_finalize(nvngfx_ctx*, uint64_t* x) { forget<Pool>(x[0]); }

/* nvnMemoryPoolGetSize(const NVNmemoryPool*) -> size_t */
void pool_get_size(nvngfx_ctx*, uint64_t* x) {
    Pool* p = find<Pool>(x[0]);
    x[0] = p ? static_cast<uint64_t>(p->size) : 0;
}

/* ------------------------------------------------------------------ */
/* textures                                                            */
/* ------------------------------------------------------------------ */

/* nvnTextureBuilderSetDefaults(NVNtextureBuilder*) */
void tex_builder_set_defaults(nvngfx_ctx*, uint64_t* x) {
    if (x[0]) std::memset(guest_ptr<void>(x[0]), 0, 0x80);
}

/* nvnTextureInitialize(NVNtexture*, const NVNtextureBuilder*) -> NVNboolean
 *
 * The NVNtextureBuilder field offsets are not verified yet, so this records
 * the guest object and stops.  Turning it on means reading width/height/
 * format/target/storage out of the builder -- guessing them would produce a
 * backend that runs and samples black.  See docs/IOS_PORTING.md, texture path.
 */
void tex_initialize(nvngfx_ctx*, uint64_t* x) {
    log_once("nvnTextureInitialize.builderOffsets", 1);
    find_or_add<Tex>(x[0]);
    x[0] = 1;
}

/* ------------------------------------------------------------------ */
/* command buffer                                                      */
/* ------------------------------------------------------------------ */

/* nvnCommandBufferBeginRecording(NVNcommandBuffer*) */
void cmdbuf_begin_recording(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!c || !c->ready || !b || b->recording) return;
    b->cb = [c->queue commandBuffer];
    b->recording = true;
    b->draws = 0;
}

/* nvnCommandBufferEndRecording(NVNcommandBuffer*) -> NVNcommandHandle
 * The handle is what the guest submits through nvnQueueSubmitCommands.  We do
 * not record a command stream, so 0 ("nothing to submit") is the honest value. */
void cmdbuf_end_recording(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b || !b->recording) {
        x[0] = 0;
        return;
    }
    if (b->enc) {
        [b->enc endEncoding];
        b->enc = nil;
    }
    b->recording = false;
    x[0] = 0;
}

/* nvnCommandBufferClearColor(NVNcommandBuffer*, int index, const float* rgba, int mask) */
void cmdbuf_clear_color(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    const float* rgba = guest_ptr<const float>(x[2]);
    if (!b || !rgba) return;
    b->clearIndex = static_cast<int>(x[1]);
    b->clearMask = static_cast<int>(x[3]);
    for (int i = 0; i < 4; ++i) b->clear[i] = rgba[i];
    /* Applied when a render target exists; without one there is nothing to
     * clear, so this is recorded and dropped. */
    log_once("nvnCommandBufferClearColor.mask", static_cast<unsigned>(b->clearMask));
}

/* nvnCommandBufferDrawArrays(NVNcommandBuffer*, NVNdrawPrimitive, int first, int count) */
void cmdbuf_draw_arrays(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    const uint32_t primitive = static_cast<uint32_t>(x[1]);
    const unsigned first = static_cast<unsigned>(x[2]);
    const unsigned count = static_cast<unsigned>(x[3]);
    b->draws++;
    if (!b->enc) return;
    [b->enc drawPrimitives:to_metal_primitive(primitive)
               vertexStart:first
               vertexCount:count];
}

/* nvnCommandBufferDrawElements(NVNcommandBuffer*, NVNdrawPrimitive,
 *                              NVNindexType, int count, NVNbufferAddress) */
void cmdbuf_draw_elements(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    const uint32_t indexType = static_cast<uint32_t>(x[2]);
    log_once("nvnCommandBufferDrawElements.indexType", indexType);
    b->draws++;
    /* An indexed draw needs the bound index buffer and the real NVNindexType
     * values; neither is verified, so this counts and stops. */
}

/* nvnCommandBufferSetViewport(NVNcommandBuffer*, int x, int y, int w, int h) */
void cmdbuf_set_viewport(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b || !b->enc) return;
    MTLViewport vp = {static_cast<double>(static_cast<int>(x[1])),
                      static_cast<double>(static_cast<int>(x[2])),
                      static_cast<double>(static_cast<int>(x[3])),
                      static_cast<double>(static_cast<int>(x[4])), 0.0, 1.0};
    [b->enc setViewport:vp];
}

/* nvnCommandBufferSetScissor(NVNcommandBuffer*, int x, int y, int w, int h) */
void cmdbuf_set_scissor(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b || !b->enc) return;
    MTLScissorRect r = {static_cast<NSUInteger>(static_cast<int>(x[1])),
                        static_cast<NSUInteger>(static_cast<int>(x[2])),
                        static_cast<NSUInteger>(static_cast<int>(x[3])),
                        static_cast<NSUInteger>(static_cast<int>(x[4]))};
    [b->enc setScissorRect:r];
}

/* ------------------------------------------------------------------ */
/* queue                                                               */
/* ------------------------------------------------------------------ */

/* nvnQueuePresentTexture(NVNqueue*, NVNwindow*, int index) */
void queue_present_texture(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    Win* w = find<Win>(x[1]);
    const unsigned index = static_cast<unsigned>(static_cast<int>(x[2]));
    if (!c || !c->ready || !w || !w->layer) return;

    const uint64_t guestTex = index < w->numTextures ? w->textures[index] : 0;
    Tex* t = guestTex ? find<Tex>(guestTex) : nullptr;

    if (!t || !t->mtl) {
        /* The guest asked to present a texture the host never materialised
         * (nvnTextureInitialize is still a stub).  Presenting a cleared frame
         * keeps the swapchain honest: the frame counter moves and the HUD can
         * show a present count.  Dropping the frame instead would look like
         * the app is hung. */
        log_once("nvnQueuePresentTexture.noTexture", 1);
        present_clear(c, w->layer, 0.05f, 0.06f, 0.09f);
        return;
    }

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [w->layer nextDrawable];
        if (!drawable) {
            c->dropped++;
            return;
        }
        id<MTLCommandBuffer> cb = [c->queue commandBuffer];
        if (t->mtl.width == drawable.texture.width &&
            t->mtl.height == drawable.texture.height &&
            t->mtl.pixelFormat == drawable.texture.pixelFormat) {
            /* Fast path: same layout, straight blit. */
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:t->mtl
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(t->mtl.width, t->mtl.height, 1)
                        toTexture:drawable.texture
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
        } else {
            /* Needs a fullscreen resolve pass (format/scale mismatch). */
            log_once("present.formatMismatch",
                     static_cast<unsigned>(t->mtl.pixelFormat));
            c->dropped++;
        }
        [cb presentDrawable:drawable];
        [cb commit];
    }
    c->presents++;
}

/* ------------------------------------------------------------------ */
/* resolve table                                                       */
/* ------------------------------------------------------------------ */

struct Binding {
    const char* name;
    nvngfx_proc fn;
};

#define BIND(name, impl) {name, reinterpret_cast<nvngfx_proc>(impl)}

const Binding kBindings[] = {
    BIND("nvnDeviceBuilderSetDefaults", dev_builder_set_defaults),
    BIND("nvnDeviceBuilderSetFlags", dev_builder_set_flags),
    BIND("nvnDeviceBuilderGetFlags", dev_builder_get_flags),
    BIND("nvnDeviceInitialize", dev_initialize),
    BIND("nvnDeviceGetProcAddress", dev_get_proc_address),

    BIND("nvnWindowBuilderSetDefaults", win_builder_set_defaults),
    BIND("nvnWindowBuilderSetNativeWindow", win_builder_set_native_window),
    BIND("nvnWindowBuilderSetTextures", win_builder_set_textures),
    BIND("nvnWindowBuilderSetPresentInterval", win_builder_set_present_interval),
    BIND("nvnWindowBuilderSetNumActiveTextures", win_builder_set_num_active_textures),
    BIND("nvnWindowInitialize", win_initialize),
    BIND("nvnWindowAcquireTexture", win_acquire_texture),
    BIND("nvnWindowGetNumTextures", win_get_num_textures),
    BIND("nvnWindowSetPresentInterval", win_set_present_interval),
    BIND("nvnWindowSetNumActiveTextures", win_set_num_active_textures),
    BIND("nvnWindowFinalize", win_finalize),

    BIND("nvnMemoryPoolBuilderSetDefaults", pool_builder_set_defaults),
    BIND("nvnMemoryPoolBuilderSetStorage", pool_builder_set_storage),
    BIND("nvnMemoryPoolBuilderSetFlags", pool_builder_set_flags),
    BIND("nvnMemoryPoolInitialize", pool_initialize),
    BIND("nvnMemoryPoolFinalize", pool_finalize),
    BIND("nvnMemoryPoolGetSize", pool_get_size),

    BIND("nvnTextureBuilderSetDefaults", tex_builder_set_defaults),
    BIND("nvnTextureInitialize", tex_initialize),

    BIND("nvnCommandBufferBeginRecording", cmdbuf_begin_recording),
    BIND("nvnCommandBufferEndRecording", cmdbuf_end_recording),
    BIND("nvnCommandBufferClearColor", cmdbuf_clear_color),
    BIND("nvnCommandBufferDrawArrays", cmdbuf_draw_arrays),
    BIND("nvnCommandBufferDrawElements", cmdbuf_draw_elements),
    BIND("nvnCommandBufferSetViewport", cmdbuf_set_viewport),
    BIND("nvnCommandBufferSetScissor", cmdbuf_set_scissor),

    BIND("nvnQueuePresentTexture", queue_present_texture),
};

nvngfx_proc metal_resolve(void*, const char* name) {
    for (const Binding& b : kBindings) {
        if (std::strcmp(b.name, name) == 0) return b.fn;
    }
    return 0;
}

const nvngfx_backend k_metal = {"metal", nullptr, metal_resolve};

}  /* namespace */

extern "C" const nvngfx_backend* nvngfx_metal_backend(void) {
    return &k_metal;
}

/* Lifetime is owned by the app: create once, install, destroy at exit. */
extern "C" nvngfx_ctx* nvngfx_metal_create(void) {
    return reinterpret_cast<nvngfx_ctx*>(new Ctx());
}

/* __bridge everywhere below: the app owns the device, the queue and the layer,
 * and they outlive the core.  Retaining them here would just hide ownership.
 * A caller that replaces one of them must call the setter again. */
extern "C" void nvngfx_metal_set_device(nvngfx_ctx* ctx, void* mtlDevice,
                                        void* mtlCommandQueue) {
    Ctx* c = reinterpret_cast<Ctx*>(ctx);
    if (!c) return;
    if (mtlDevice) c->device = (__bridge id<MTLDevice>)mtlDevice;
    if (mtlCommandQueue) c->queue = (__bridge id<MTLCommandQueue>)mtlCommandQueue;
    if (c->device && !c->queue) c->queue = [c->device newCommandQueue];
    c->ready = (c->device != nil && c->queue != nil);
}

extern "C" void nvngfx_metal_set_layer(nvngfx_ctx* ctx, void* caMetalLayer) {
    Ctx* c = reinterpret_cast<Ctx*>(ctx);
    if (!c) return;
    c->nativeLayer = (__bridge CAMetalLayer*)caMetalLayer;
}

extern "C" int nvngfx_metal_present_clear(nvngfx_ctx* ctx, float r, float g, float b) {
    Ctx* c = reinterpret_cast<Ctx*>(ctx);
    if (!c) return 0;
    return present_clear(c, c->nativeLayer, r, g, b) ? 1 : 0;
}

extern "C" void nvngfx_metal_destroy(nvngfx_ctx* ctx) {
    delete reinterpret_cast<Ctx*>(ctx);
}

#endif /* __APPLE__ */
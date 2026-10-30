/*
 * nvn_metal.mm -- NVN -> Metal host backend (Apple platforms only).
 *
 * Scope, stated up front so nobody mistakes this for finished:
 *
 *   DONE  device / queue / window / swapchain lifecycle
 *         memory pool -> zero-copy MTLBuffer over guest memory
 *         command buffer -> MTLCommandBuffer, viewport / scissor / drawArrays
 *         render targets + one render pass (colour[0]) + ClearColor as a load
 *           action, opened lazily by the first draw
 *         texture objects, built from the values nvnTextureBuilderSet* was
 *           given (no guest struct offsets are read)
 *         program objects; the pipeline cache is keyed by colour format and
 *           vertex layout and fed by the shader key -> offline MSL blob
 *           (docs/SHADER_CHAIN.md), with nvngfx_shader_to_msl() as the
 *           fallback for a host that supplies MSL directly
 *         vertex input: NVNvertexAttribState / NVNvertexStreamState decoded
 *           from the guest's own bytes -> MTLVertexDescriptor, and the
 *           stream's guest address -> the MTLBuffer of the pool that holds it
 *         present (blit fast path, cleared-drawable fallback)
 *
 *   TODO  a guest draw still stops at "noPipeline" whenever the offline blob
 *         table is empty (the committed one is): it is filled from the game
 *         on a computer, never in the repository.  Everything else on that
 *         path -- pipeline creation with a real vertex descriptor, buffer
 *         binding, draw encoding, pixel readback -- is executed by
 *         nvngfx_metal_selftest() in CI's iOS Simulator (two draws: one from
 *         [[vertex_id]], one from [[attribute(0)]] out of a memory pool).
 *         MRT / depth attachment / texture views
 *         guest-pixel upload (textures are host-owned, not aliased)
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
#include "nvn_shader.h"
#include "nvn_texture.h"
#include "nvn_vertex.h"

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

/* Same dedupe, but for a message that is already a string (a translator's
 * reason, a Metal error).  Repeats of the identical message are dropped. */
void log_once_str(const char* what, const char* msg) {
    static std::unordered_map<std::string, std::string> seen;
    const char* text = msg ? msg : "(no message)";
    auto it = seen.find(what);
    if (it != seen.end() && it->second == text) return;
    seen[what] = text;
    std::fprintf(stderr, "[nvn] %s: %s\n", what, text);
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
    /* Render targets of the current pass, as guest NVNtexture addresses. */
    unsigned numColor = 0;
    uint64_t color[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint64_t depth = 0;
    /* A ClearColor that has not been turned into a load action yet. */
    bool pendingClear = false;
    int clearIndex = 0;
    int clearMask = 0;
    float clear[4] = {0, 0, 0, 0};
    /* Program bound by nvnCommandBufferBindProgram and the pipeline it
     * produced.  The pipeline stays nil until nvngfx_shader_to_msl() can
     * actually translate something. */
    uint64_t program = 0;
    id<MTLRenderPipelineState> pso = nil;
    /* Vertex buffers as guest addresses.  Recorded, not consumed: Metal needs
     * an MTLBuffer, which needs the pool the address belongs to, and no draw
     * path fetches vertices before a pipeline exists. */
    unsigned numVb = 0;
    uint64_t vb[16] = {0};
    uint64_t vbSize[16] = {0};
    /* Vertex input: the attributes and streams the guest bound, decoded out
     * of its own 4- and 8-byte records (nvn_vertex.h).  A vertex descriptor
     * is baked into the Metal pipeline, so changing either invalidates it. */
    nvngfx_vattr attrib[NVNGFX_MAX_VERTEX_ATTRIBS] = {};
    unsigned numAttrib = 0;
    nvngfx_vstream stream[NVNGFX_MAX_VERTEX_STREAMS] = {};
    /* The last committed buffer.  nvnQueueSubmitCommands is not implemented,
     * so a frame is committed at EndRecording; keeping the handle lets a
     * caller (the self-test) wait for it and keeps the door open for real
     * submit semantics. */
    id<MTLCommandBuffer> last = nil;
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
/* Builder state, keyed by the guest builder address.  The guest owns the
 * struct and its layout is unverified, but that does not matter: every
 * field we need arrives through a setter we implement ourselves. */
std::unordered_map<uint64_t, nvngfx_texdesc> g_tex_specs;

/* Guest memory is host-visible, so a pointer argument is just a cast.  This
 * is the one place that assumption is spelled out. */
template <class T>
T* guest_ptr(uint64_t value) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
}

/* ------------------------------------------------------------------ */
/* NVN -> Metal primitives                                             */
/* ------------------------------------------------------------------ */

[[maybe_unused]] MTLPrimitiveType to_metal_primitive(uint32_t nvn) {
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
    nvngfx_texdesc_defaults(&g_tex_specs[x[0]]);
}

/* The rest of the builder setters only record what the guest told us.  Their
 * shapes come from ios/nvn/signatures.json, so there is no offset guessing:
 * after nvnTextureBuilderSetSize2D(b, w, h) the state *is* w x h. */
void tex_builder_set_target(nvngfx_ctx*, uint64_t* x) {
    g_tex_specs[x[0]].target = static_cast<int>(x[1]);
}
void tex_builder_set_format(nvngfx_ctx*, uint64_t* x) {
    g_tex_specs[x[0]].format = static_cast<int>(x[1]);
}
void tex_builder_set_size1d(nvngfx_ctx*, uint64_t* x) {
    nvngfx_texdesc& d = g_tex_specs[x[0]];
    d.rank = 1;
    d.width = static_cast<int>(x[1]);
    d.height = 1;
    d.depth = 1;
}
void tex_builder_set_size2d(nvngfx_ctx*, uint64_t* x) {
    nvngfx_texdesc& d = g_tex_specs[x[0]];
    d.rank = 2;
    d.width = static_cast<int>(x[1]);
    d.height = static_cast<int>(x[2]);
    d.depth = 1;
}
void tex_builder_set_size3d(nvngfx_ctx*, uint64_t* x) {
    nvngfx_texdesc& d = g_tex_specs[x[0]];
    d.rank = 3;
    d.width = static_cast<int>(x[1]);
    d.height = static_cast<int>(x[2]);
    d.depth = static_cast<int>(x[3]);
}
void tex_builder_set_width(nvngfx_ctx*, uint64_t* x) {
    g_tex_specs[x[0]].width = static_cast<int>(x[1]);
}
void tex_builder_set_height(nvngfx_ctx*, uint64_t* x) {
    g_tex_specs[x[0]].height = static_cast<int>(x[1]);
}
void tex_builder_set_depth(nvngfx_ctx*, uint64_t* x) {
    g_tex_specs[x[0]].depth = static_cast<int>(x[1]);
}
void tex_builder_set_levels(nvngfx_ctx*, uint64_t* x) {
    g_tex_specs[x[0]].levels = static_cast<int>(x[1]);
}
void tex_builder_set_samples(nvngfx_ctx*, uint64_t* x) {
    g_tex_specs[x[0]].samples = static_cast<int>(x[1]);
}
void tex_builder_set_storage(nvngfx_ctx*, uint64_t* x) {
    nvngfx_texdesc& d = g_tex_specs[x[0]];
    d.pool = x[1];
    d.offset = static_cast<long long>(x[2]);
}
void tex_builder_set_stride(nvngfx_ctx*, uint64_t* x) {
    g_tex_specs[x[0]].stride = static_cast<long long>(x[1]);
}
void tex_builder_set_flags(nvngfx_ctx*, uint64_t* x) {
    g_tex_specs[x[0]].flags = static_cast<int>(x[1]);
}

MTLTextureType to_metal_texture_type(int rank) {
    if (rank == 1) return MTLTextureType1D;
    if (rank == 3) return MTLTextureType3D;
    return MTLTextureType2D;
}

/* nvnTextureInitialize(NVNtexture*, const NVNtextureBuilder*) -> NVNboolean
 *
 * Builds a real MTLTexture from what the builder setters recorded.  Important
 * limitation, stated here and in the docs: the texture is HOST allocated and
 * does not alias the guest's storage.  That is harmless for a render target
 * (we are the only writer and the pixels only leave through present), and it
 * is wrong for a sampled texture (the guest's pixels would not be seen).
 * Until the block-linear deswizzle path exists, only render targets are
 * meaningful -- see docs/IOS_PORTING.md, texture path.
 */
void tex_initialize(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    if (!c || !c->ready) {
        x[0] = 0;
        return;
    }

    nvngfx_texdesc d;
    auto it = g_tex_specs.find(x[1]);
    if (it != g_tex_specs.end()) {
        d = it->second;
    } else {
        nvngfx_texdesc_defaults(&d);
    }

    int lossy = 0;
    const unsigned pf = nvngfx_format_to_mtl(d.format, &lossy);
    if (!pf) {
        log_once("nvnTextureInitialize.formatWithoutMetalEquivalent",
                 static_cast<unsigned>(d.format));
        x[0] = 0;
        return;
    }
    if (lossy) log_once("nvnTextureInitialize.substitutedFormat", static_cast<unsigned>(d.format));
    if (d.width <= 0 || d.height <= 0 || d.depth <= 0) {
        log_once("nvnTextureInitialize.badSize", static_cast<unsigned>(d.width));
        x[0] = 0;
        return;
    }

    @autoreleasepool {
        MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
        td.pixelFormat = static_cast<MTLPixelFormat>(pf);
        td.width = static_cast<NSUInteger>(d.width);
        td.height = static_cast<NSUInteger>(d.height);
        td.depth = d.depth > 1 ? static_cast<NSUInteger>(d.depth) : 1;
        td.mipmapLevelCount = d.levels > 1 ? static_cast<NSUInteger>(d.levels) : 1;
        td.sampleCount = d.samples > 1 ? static_cast<NSUInteger>(d.samples) : 1;
        td.textureType = to_metal_texture_type(d.rank);
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
                   MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;

        Tex* t = find_or_add<Tex>(x[0]);
        t->width = static_cast<unsigned>(d.width);
        t->height = static_cast<unsigned>(d.height);
        t->depth = static_cast<unsigned>(d.depth);
        t->levels = static_cast<unsigned>(d.levels);
        t->format = static_cast<uint32_t>(d.format);
        t->target = static_cast<uint32_t>(d.target);
        t->pool = d.pool;
        t->offset = static_cast<ptrdiff_t>(d.offset);
        t->mtl = [c->device newTextureWithDescriptor:td];
        x[0] = t->mtl ? 1u : 0u;
    }
}

/* ------------------------------------------------------------------ */
/* programs and pipelines                                              */
/* ------------------------------------------------------------------ */

/* One guest NVNprogram reduced to what the replay layer needs.  The shader
 * data is recorded exactly as handed over (a guest GPU address plus the
 * control section); no guest memory is read here. */
struct Program {
    unsigned numStages = 0;
    uint64_t code[3] = {0, 0, 0};
    uint64_t control[3] = {0, 0, 0};
    /* nvngfx_shader_key_from_guest() of each stage, or "" when the control
     * section did not describe a code area.  Computed at SetShaders, before
     * the guest is allowed to free the control section (Unity does). */
    char key[3][17] = {};
    /* Metal bakes the colour format *and* the vertex descriptor into a render
     * pipeline.  A handful of slots is enough here: a title alternates between
     * a few layouts, and a single slot would recompile on every switch (which
     * on a device costs milliseconds).  slots[0] is the most recent. */
    struct PsoSlot {
        id<MTLRenderPipelineState> pso = nil;
        MTLPixelFormat format = static_cast<MTLPixelFormat>(0);
        unsigned long long layout = 0;   /* nvngfx_vertex_layout_id() */
        bool hasLayout = false;          /* false: no vertex descriptor */
    };
    PsoSlot slots[4];
};

/* The cached pipeline for (format, layout), or nil.  Also records the hit in
 * slot 0 so the next draw with the same key finds it first. */
id<MTLRenderPipelineState> pso_lookup(Program* p, MTLPixelFormat fmt,
                                     unsigned long long layout, bool hasLayout) {
    for (unsigned i = 0; i < 4; ++i) {
        Program::PsoSlot& s = p->slots[i];
        if (s.pso && s.format == fmt && s.layout == layout && s.hasLayout == hasLayout) {
            if (i != 0) {
                const Program::PsoSlot keep = s;
                s = p->slots[0];
                p->slots[0] = keep;
            }
            return s.pso;
        }
    }
    return nil;
}

void pso_store(Program* p, id<MTLRenderPipelineState> pso, MTLPixelFormat fmt,
               unsigned long long layout, bool hasLayout) {
    /* Shift down; slot 3 falls out.  Releasing the object it held is fine:
     * the command buffers that used it keep their own reference. */
    for (unsigned i = 3; i > 0; --i) p->slots[i] = p->slots[i - 1];
    p->slots[0].pso = pso;
    p->slots[0].format = fmt;
    p->slots[0].layout = layout;
    p->slots[0].hasLayout = hasLayout;
}

void pso_drop(Program* p) {
    for (unsigned i = 0; i < 4; ++i) {
        p->slots[i].pso = nil;
        p->slots[i].format = static_cast<MTLPixelFormat>(0);
        p->slots[i].layout = 0;
        p->slots[i].hasLayout = false;
    }
}

/* Compiles the program's MSL into a render pipeline for `fmt`.
 *
 * This is the function a translator plugs into: it only knows
 * nvngfx_shader_to_msl().  Today that returns NULL, so the reason is reported
 * once and every draw afterwards stops at "noPipeline". */
/* nvngfx_vfmt + component count -> MTLVertexFormat.  Symbolic names on
 * purpose: the numeric values of MTLVertexFormat are Apple's business, and a
 * wrong integer here would be a silent vertex misread.  Rows follow the order
 * of nvngfx_vfmt's scalar kinds: u8, u8n, i8, i8n, u16, u16n, i16, i16n,
 * f16, u32, i32, f32. */
static MTLVertexFormat mtl_vertex_format(nvngfx_vfmt kind, int comps) {
    static const MTLVertexFormat kTable[12][4] = {
        {MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4},
        {MTLVertexFormatUCharNormalized, MTLVertexFormatUChar2Normalized,
         MTLVertexFormatUChar3Normalized, MTLVertexFormatUChar4Normalized},
        {MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4},
        {MTLVertexFormatCharNormalized, MTLVertexFormatChar2Normalized,
         MTLVertexFormatChar3Normalized, MTLVertexFormatChar4Normalized},
        {MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4},
        {MTLVertexFormatUShortNormalized, MTLVertexFormatUShort2Normalized,
         MTLVertexFormatUShort3Normalized, MTLVertexFormatUShort4Normalized},
        {MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4},
        {MTLVertexFormatShortNormalized, MTLVertexFormatShort2Normalized,
         MTLVertexFormatShort3Normalized, MTLVertexFormatShort4Normalized},
        {MTLVertexFormatHalf, MTLVertexFormatHalf2, MTLVertexFormatHalf3, MTLVertexFormatHalf4},
        {MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4},
        {MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4},
        {MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4},
    };
    if (comps < 1 || comps > 4) return MTLVertexFormatInvalid;
    switch (kind) {
    case NVNGFX_VF_BGRA8N: return MTLVertexFormatUChar4Normalized_BGRA;
    case NVNGFX_VF_F11_11_10: return MTLVertexFormatFloatRG11B10;
    case NVNGFX_VF_F9E5: return MTLVertexFormatFloatRGB9E5;
    case NVNGFX_VF_U10_10_10_2N: return MTLVertexFormatUInt1010102Normalized;
    case NVNGFX_VF_I10_10_10_2N: return MTLVertexFormatInt1010102Normalized;
    default: break;
    }
    const int row = static_cast<int>(kind) - static_cast<int>(NVNGFX_VF_U8);
    if (row < 0 || row > 11) return MTLVertexFormatInvalid;
    return kTable[row][comps - 1];
}

/* Builds the MTLVertexDescriptor for the vertex state bound on `b`.
 *
 * The attribute index is the shader's own: nvnCommandBufferBindVertexAttribState
 * binds an array whose element i is attribute i, and spirv-cross emits
 * [[attribute(i)]] in the same order, so no remapping is needed.  Each
 * attribute names the stream that feeds it; the stream's stride and divisor
 * come from nvnCommandBufferBindVertexStreamState.
 *
 * Returns nil and a reason when a bound attribute has no Metal counterpart or
 * a used stream has no stride -- both are cases where drawing anyway would
 * fetch garbage. */
MTLVertexDescriptor* vertex_descriptor(const CmdBuf* b, unsigned long long* layout,
                                      const char** why) {
    *layout = nvngfx_vertex_layout_id(b->attrib, b->numAttrib, b->stream,
                                     NVNGFX_MAX_VERTEX_STREAMS);
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    bool used[NVNGFX_MAX_VERTEX_STREAMS] = {};
    for (unsigned i = 0; i < b->numAttrib && i < NVNGFX_MAX_VERTEX_ATTRIBS; ++i) {
        const nvngfx_vattr& a = b->attrib[i];
        const nvngfx_vfmt kind = nvngfx_vertex_type(a.format);
        if (kind == NVNGFX_VF_NONE) continue;
        const int comps = nvngfx_vertex_components(a.format);
        if (kind == NVNGFX_VF_UNSUPPORTED || comps < 1 ||
            a.stream >= NVNGFX_MAX_VERTEX_STREAMS) {
            log_once("nvnPipeline.vertexFormat", a.format);
            if (why) *why = "vertex attribute format has no Metal counterpart";
            return nil;
        }
        const MTLVertexFormat f = mtl_vertex_format(kind, comps);
        if (f == MTLVertexFormatInvalid) {
            log_once("nvnPipeline.vertexFormatRow", a.format);
            if (why) *why = "vertex attribute format has no Metal vertex format";
            return nil;
        }
        vd.attributes[i].format = f;
        vd.attributes[i].offset = a.offset;
        vd.attributes[i].bufferIndex = a.stream;
        used[a.stream] = true;
    }
    for (unsigned st = 0; st < NVNGFX_MAX_VERTEX_STREAMS; ++st) {
        if (!used[st]) continue;
        if (b->stream[st].stride == 0) {
            log_once("nvnPipeline.vertexStride", st);
            if (why) *why = "vertex stream stride is zero";
            return nil;
        }
        vd.layouts[st].stride = b->stream[st].stride;
        if (b->stream[st].divisor) {
            vd.layouts[st].stepFunction = MTLVertexStepFunctionPerInstance;
            vd.layouts[st].stepRate = b->stream[st].divisor;
        } else {
            vd.layouts[st].stepFunction = MTLVertexStepFunctionPerVertex;
            vd.layouts[st].stepRate = 1;
        }
    }
    return vd;
}

id<MTLRenderPipelineState> program_pipeline(Ctx* c, Program* p, MTLPixelFormat fmt,
                                            const CmdBuf* b, const char** why) {
    const bool hasLayout = b && b->numAttrib > 0;
    unsigned long long layout = 0;
    MTLVertexDescriptor* vd = nil;
    if (hasLayout) {
        vd = vertex_descriptor(b, &layout, why);
        if (!vd) return nil;
    }
    if (id<MTLRenderPipelineState> hit = pso_lookup(p, fmt, layout, hasLayout)) return hit;

    const char* whyVs = nullptr;
    const char* whyFs = nullptr;
    /* An offline blob is the normal source: the chain translates on a
     * computer, and the key computed at SetShaders finds the result here.
     * nvngfx_shader_to_msl() stays as the fallback for a host that provides
     * MSL directly (the self-test's passthrough). */
    const char* vs = p->key[NVNGFX_STAGE_VERTEX][0]
                          ? nvngfx_shader_lookup(p->key[NVNGFX_STAGE_VERTEX],
                                                 NVNGFX_STAGE_VERTEX, &whyVs)
                          : nullptr;
    if (!vs) {
        vs = nvngfx_shader_to_msl(
            reinterpret_cast<const void*>(p->code[NVNGFX_STAGE_VERTEX]), 0,
            NVNGFX_STAGE_VERTEX, &whyVs);
    }
    const char* fs = p->key[NVNGFX_STAGE_FRAGMENT][0]
                          ? nvngfx_shader_lookup(p->key[NVNGFX_STAGE_FRAGMENT],
                                                 NVNGFX_STAGE_FRAGMENT, &whyFs)
                          : nullptr;
    if (!fs) {
        fs = nvngfx_shader_to_msl(
            reinterpret_cast<const void*>(p->code[NVNGFX_STAGE_FRAGMENT]), 0,
            NVNGFX_STAGE_FRAGMENT, &whyFs);
    }
    if (!vs || !fs) {
        if (why) *why = !vs ? (whyVs ? whyVs : "no vertex shader")
                            : (whyFs ? whyFs : "no fragment shader");
        return nil;
    }

    @autoreleasepool {
        NSError* err = nil;
        id<MTLLibrary> vlib =
            [c->device newLibraryWithSource:[NSString stringWithUTF8String:vs]
                                    options:nil
                                      error:&err];
        if (!vlib) {
            log_once_str("nvnProgram.pipeline.vertexLibrary",
                         err ? [[err localizedDescription] UTF8String] : "nil library");
            return nil;
        }
        err = nil;
        id<MTLLibrary> flib =
            [c->device newLibraryWithSource:[NSString stringWithUTF8String:fs]
                                    options:nil
                                      error:&err];
        if (!flib) {
            log_once_str("nvnProgram.pipeline.fragmentLibrary",
                         err ? [[err localizedDescription] UTF8String] : "nil library");
            return nil;
        }

        /* The names are the contract a translator has to emit. */
        id<MTLFunction> vfn = [vlib newFunctionWithName:@"nvngfx_main_vs"];
        id<MTLFunction> ffn = [flib newFunctionWithName:@"nvngfx_main_fs"];
        if (!vfn || !ffn) {
            log_once_str("nvnProgram.pipeline.function",
                         "MSL did not define nvngfx_main_vs / nvngfx_main_fs");
            return nil;
        }

        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = vfn;
        pd.fragmentFunction = ffn;
        pd.colorAttachments[0].pixelFormat = fmt;
        if (vd) pd.vertexDescriptor = vd;

        err = nil;
        id<MTLRenderPipelineState> pso =
            [c->device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) {
            log_once_str("nvnProgram.pipeline.state",
                         err ? [[err localizedDescription] UTF8String] : "nil pipeline");
            return nil;
        }
        pso_store(p, pso, fmt, layout, hasLayout);
        return pso;
    }
}

/* The MTLBuffer holding [addr, addr+len) and the byte offset into it.
 *
 * pool_initialize() wraps the guest's own storage with
 * newBufferWithBytesNoCopy, so a guest address inside a pool is the same byte
 * inside that MTLBuffer -- no copy, no per-draw upload. */
bool pool_span(uint64_t addr, size_t len, id<MTLBuffer>* buf, size_t* off) {
    if (!addr) return false;
    for (auto& kv : table<Pool>()) {
        Pool* p = kv.second;
        if (!p || !p->buffer || !p->storage) continue;
        const uint64_t base = reinterpret_cast<uint64_t>(p->storage);
        if (addr < base) continue;
        const uint64_t o = addr - base;
        if (o > p->size || len > p->size - o) continue;
        *buf = p->buffer;
        *off = static_cast<size_t>(o);
        return true;
    }
    return false;
}

/* Binds one MTLBuffer per stream that a bound attribute reads.
 *
 * A stream whose guest address is not inside any memory pool is not bindable:
 * Metal would read whatever is at the offset, so the draw is dropped instead
 * (once per stream, with the address, so the reason is visible). */
bool bind_vertex_streams(CmdBuf* b) {
    bool used[NVNGFX_MAX_VERTEX_STREAMS] = {};
    for (unsigned i = 0; i < b->numAttrib && i < NVNGFX_MAX_VERTEX_ATTRIBS; ++i) {
        const nvngfx_vattr& a = b->attrib[i];
        if (nvngfx_vertex_type(a.format) == NVNGFX_VF_NONE) continue;
        if (a.stream < NVNGFX_MAX_VERTEX_STREAMS) used[a.stream] = true;
    }
    for (unsigned st = 0; st < NVNGFX_MAX_VERTEX_STREAMS; ++st) {
        if (!used[st]) continue;
        id<MTLBuffer> buf = nil;
        size_t off = 0;
        uint64_t len = b->vbSize[st];
        if (len == 0 && b->stream[st].stride) len = b->stream[st].stride;
        if (!pool_span(b->vb[st], static_cast<size_t>(len ? len : 1u), &buf, &off)) {
            log_once("nvnCommandBufferDraw.vertexBuffer", st);
            return false;
        }
        [b->enc setVertexBuffer:buf offset:off atIndex:st];
    }
    return true;
}

/* Builds the pipeline for the bound program if it is not built yet.  Failures
 * are reported once per reason; a draw then stops instead of encoding
 * something Metal would reject. */
bool ensure_pipeline(Ctx* c, CmdBuf* b) {
    if (b->pso) return true;
    if (!b->program) return false;
    if (!c || !c->ready || b->numColor < 1) return false;
    Program* p = find<Program>(b->program);
    Tex* t = find<Tex>(b->color[0]);
    if (!p || !t || !t->mtl) return false;
    const char* why = nullptr;
    b->pso = program_pipeline(c, p, t->mtl.pixelFormat, b, &why);
    if (!b->pso) log_once_str("nvnCommandBufferDraw.pipeline", why ? why : "unknown");
    return b->pso != nil;
}

/* nvnProgramInitialize(NVNprogram*, NVNdevice*) -> NVNboolean */
void prog_initialize(nvngfx_ctx*, uint64_t* x) {
    Program* p = find_or_add<Program>(x[0]);
    x[0] = p ? 1u : 0u;
}

/* nvnProgramSetShaders(NVNprogram*, int, const NVNshaderData*) -> NVNboolean
 *
 * NVNshaderData is { NVNbufferAddress data; const void* control; }, 16 bytes,
 * the layout the public nxnative HLE reads (its nvnProgramSetShaders copies
 * n * sizeof *sd).  The array order is the stage order.
 *
 * The control section is only guaranteed valid during this call -- a real
 * implementation has to consume it here (that is where the shader key lives).
 * This one records the address and nothing else, and says so. */
void prog_set_shaders(nvngfx_ctx*, uint64_t* x) {
    Program* p = find_or_add<Program>(x[0]);
    const int n = static_cast<int>(x[1]);
    const uint64_t* sd = guest_ptr<const uint64_t>(x[2]);
    if (!p) {
        x[0] = 0;
        return;
    }
    if (n < 0 || n > 3 || (n > 0 && !sd)) {
        log_once("nvnProgramSetShaders.stageCount", static_cast<unsigned>(n));
        x[0] = 0;
        return;
    }
    p->numStages = static_cast<unsigned>(n);
    for (int i = 0; i < 3; ++i) {
        p->code[i] = i < n ? sd[2 * i] : 0;
        p->control[i] = i < n ? sd[2 * i + 1] : 0;
        p->key[i][0] = 0;
        if (p->code[i] && p->control[i]) {
            if (nvngfx_shader_key_from_guest(
                    reinterpret_cast<const void*>(p->control[i]),
                    reinterpret_cast<const void*>(p->code[i]), p->key[i]) != 0) {
                /* Once per process, and not a failure: a program whose control
                 * section is not readable yet is legal while it is being set. */
                log_once("nvnProgramSetShaders.noKey", static_cast<unsigned>(i));
            }
        }
    }
    /* New shaders invalidate the cached pipelines. */
    pso_drop(p);
    x[0] = 1;
}

/* nvnProgramFinalize(NVNprogram*) */
void prog_finalize(nvngfx_ctx*, uint64_t* x) {
    Program* p = find<Program>(x[0]);
    if (p) pso_drop(p);
    forget<Program>(x[0]);
}

/* nvnCommandBufferBindProgram(NVNcommandBuffer*, const NVNprogram*, int)
 *
 * The third argument is a "what changed" flag; it does not affect the state
 * this layer keeps, so it is ignored rather than guessed at. */
void cmdbuf_bind_program(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    b->program = x[1];
    b->pso = nil;
    /* BindProgram may run before the targets are known; ensure_pipeline()
     * then returns false here and the draw path retries once the pass knows
     * its colour format. */
    ensure_pipeline(c, b);
}

/* nvnCommandBufferBindVertexBuffer(NVNcommandBuffer*, int index,
 *                                  NVNbufferAddress, size_t) */
void cmdbuf_bind_vertex_buffer(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    const unsigned i = static_cast<unsigned>(static_cast<int>(x[1]));
    if (i >= 16) {
        log_once("nvnCommandBufferBindVertexBuffer.index", i);
        return;
    }
    b->vb[i] = x[2];
    b->vbSize[i] = x[3];
    if (i + 1 > b->numVb) b->numVb = i + 1;
}

/* nvnCommandBufferBindVertexBuffers(NVNcommandBuffer*, int first, int count,
 *                                   const NVNbufferRange*)
 *
 * NVNbufferRange is { address, size }.  Its contents are deliberately not
 * read: nothing consumes vertex buffers yet, and a wrong stride would be a
 * silent misread rather than a crash.  Only the range is recorded. */
void cmdbuf_bind_vertex_buffers(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    const int first = static_cast<int>(x[1]);
    const int count = static_cast<int>(x[2]);
    if (first < 0 || count < 0 || first + count > 16) {
        log_once("nvnCommandBufferBindVertexBuffers.range",
                 static_cast<unsigned>(count));
        return;
    }
    if (first + count > static_cast<int>(b->numVb)) {
        b->numVb = static_cast<unsigned>(first + count);
    }
    /* NVNbufferRange is { NVNbufferAddress address; uint64_t size; }.  With a
     * vertex descriptor in place these ranges are what the draw reads, so they
     * are recorded now (the earlier version deliberately did not). */
    const uint64_t* rg = guest_ptr<const uint64_t>(x[3]);
    if (!rg) return;
    for (int i = 0; i < count; ++i) {
        b->vb[first + i] = rg[2 * i];
        b->vbSize[first + i] = rg[2 * i + 1];
    }
}

/* ------------------------------------------------------------------ */
/* vertex input state                                                  */
/* ------------------------------------------------------------------ */

/* The guest owns these structs (4 and 8 bytes), so the setters write its own
 * bytes and the getters read them back.  A side table keyed by address would
 * go stale the moment the guest copies a state struct, which is exactly what
 * engines do when they keep a small array of states. */

/* nvnVertexAttribStateSetDefaults(NVNvertexAttribState*) */
void vattrib_set_defaults(nvngfx_ctx*, uint64_t* x) {
    unsigned* w = guest_ptr<unsigned>(x[0]);
    if (!w) return;
    const nvngfx_vattr zero = {0u, 0u, 0u};
    *w = nvngfx_vattr_pack(zero);
}

/* nvnVertexAttribStateSetFormat(NVNvertexAttribState*, NVNformat, ptrdiff_t) */
void vattrib_set_format(nvngfx_ctx*, uint64_t* x) {
    unsigned* w = guest_ptr<unsigned>(x[0]);
    if (!w) return;
    nvngfx_vattr a = nvngfx_vattr_unpack(*w);
    const long long off = static_cast<long long>(x[2]);
    if (off < 0 || off > 0x7FFFF) {
        /* The hardware field is 19 bits wide; a larger offset would be
         * silently truncated, so it is reported and dropped instead. */
        log_once("nvnVertexAttribStateSetFormat.offset", static_cast<unsigned>(off));
        if (off < 0) return;
    }
    if (x[1] > 0xFFu) log_once("nvnVertexAttribStateSetFormat.format",
                               static_cast<unsigned>(x[1]));
    a.format = static_cast<unsigned>(x[1]) & 0xFFu;
    a.offset = static_cast<unsigned>(off) & 0x7FFFFu;
    *w = nvngfx_vattr_pack(a);
}

/* nvnVertexAttribStateSetStreamIndex(NVNvertexAttribState*, int) */
void vattrib_set_stream(nvngfx_ctx*, uint64_t* x) {
    unsigned* w = guest_ptr<unsigned>(x[0]);
    if (!w) return;
    const int idx = static_cast<int>(x[1]);
    if (idx < 0 || idx >= 32) {
        log_once("nvnVertexAttribStateSetStreamIndex.index", static_cast<unsigned>(idx));
        return;
    }
    nvngfx_vattr a = nvngfx_vattr_unpack(*w);
    a.stream = static_cast<unsigned>(idx);
    *w = nvngfx_vattr_pack(a);
}

/* nvnVertexAttribStateGetFormat(const NVNvertexAttribState*, NVNformat*,
 *                               ptrdiff_t*) */
void vattrib_get_format(nvngfx_ctx*, uint64_t* x) {
    const unsigned* w = guest_ptr<const unsigned>(x[0]);
    unsigned* fmt = guest_ptr<unsigned>(x[1]);
    long long* off = guest_ptr<long long>(x[2]);
    nvngfx_vattr a = {0u, 0u, 0u};
    if (w) a = nvngfx_vattr_unpack(*w);
    if (fmt) *fmt = a.format;
    if (off) *off = static_cast<long long>(a.offset);
}

/* nvnVertexAttribStateGetStreamIndex(const NVNvertexAttribState*) -> int */
void vattrib_get_stream(nvngfx_ctx*, uint64_t* x) {
    const unsigned* w = guest_ptr<const unsigned>(x[0]);
    x[0] = w ? nvngfx_vattr_unpack(*w).stream : 0u;
}

/* nvnVertexStreamStateSetDefaults(NVNvertexStreamState*) */
void vstream_set_defaults(nvngfx_ctx*, uint64_t* x) {
    unsigned* w = guest_ptr<unsigned>(x[0]);
    if (!w) return;
    nvngfx_vstream s;
    nvngfx_vstream_defaults(&s);
    w[0] = s.stride;
    w[1] = s.divisor;
}

/* nvnVertexStreamStateSetStride(NVNvertexStreamState*, ptrdiff_t) */
void vstream_set_stride(nvngfx_ctx*, uint64_t* x) {
    unsigned* w = guest_ptr<unsigned>(x[0]);
    if (!w) return;
    const long long stride = static_cast<long long>(x[1]);
    if (stride < 0 || stride > 0x7FFFFFFFll) {
        log_once("nvnVertexStreamStateSetStride.value",
                 static_cast<unsigned>(stride));
        if (stride < 0) return;
    }
    w[0] = static_cast<unsigned>(stride);
}

/* nvnVertexStreamStateSetDivisor(NVNvertexStreamState*, int) */
void vstream_set_divisor(nvngfx_ctx*, uint64_t* x) {
    unsigned* w = guest_ptr<unsigned>(x[0]);
    if (!w) return;
    w[1] = static_cast<unsigned>(x[1]);
}

/* nvnVertexStreamStateGetStride(const NVNvertexStreamState*) -> ptrdiff_t */
void vstream_get_stride(nvngfx_ctx*, uint64_t* x) {
    const unsigned* w = guest_ptr<const unsigned>(x[0]);
    x[0] = w ? w[0] : 0u;
}

/* nvnVertexStreamStateGetDivisor(const NVNvertexStreamState*) -> int */
void vstream_get_divisor(nvngfx_ctx*, uint64_t* x) {
    const unsigned* w = guest_ptr<const unsigned>(x[0]);
    x[0] = w ? w[1] : 0u;
}

/* nvnCommandBufferBindVertexAttribState(NVNcommandBuffer*, int,
 *                                      const NVNvertexAttribState*)
 *
 * Element i of the array is shader attribute i -- the same index spirv-cross
 * writes into [[attribute(i)]].  The guest's 4 bytes are copied out here: it
 * is free to reuse that memory once the call returns. */
void cmdbuf_bind_vertex_attrib_state(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    const int n = static_cast<int>(x[1]);
    const unsigned* st = guest_ptr<const unsigned>(x[2]);
    if (n < 0 || n > NVNGFX_MAX_VERTEX_ATTRIBS || (n > 0 && !st)) {
        log_once("nvnCommandBufferBindVertexAttribState.count", static_cast<unsigned>(n));
        return;
    }
    for (int i = 0; i < NVNGFX_MAX_VERTEX_ATTRIBS; ++i) {
        b->attrib[i].format = 0;
        b->attrib[i].offset = 0;
        b->attrib[i].stream = 0;
        if (i < n) b->attrib[i] = nvngfx_vattr_unpack(st[i]);
    }
    b->numAttrib = static_cast<unsigned>(n);
    /* The vertex descriptor is baked into the Metal pipeline. */
    b->pso = nil;
}

/* nvnCommandBufferBindVertexStreamState(NVNcommandBuffer*, int,
 *                                      const NVNvertexStreamState*)
 *
 * Interpreted as (count, array), the way nxnative's recorder does it and the
 * way nvnCommandBufferBindVertexAttribState takes its array.  A single stream
 * bound with index 0 behaves identically under either reading; a stream index
 * other than 0 is not expressible in this shape. */
void cmdbuf_bind_vertex_stream_state(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    const int n = static_cast<int>(x[1]);
    const unsigned* st = guest_ptr<const unsigned>(x[2]);
    if (n < 0 || n > NVNGFX_MAX_VERTEX_STREAMS || (n > 0 && !st)) {
        log_once("nvnCommandBufferBindVertexStreamState.count", static_cast<unsigned>(n));
        return;
    }
    for (int i = 0; i < NVNGFX_MAX_VERTEX_STREAMS; ++i) {
        b->stream[i].stride = 0;
        b->stream[i].divisor = 0;
        if (i < n) {
            b->stream[i].stride = st[2 * i];
            b->stream[i].divisor = st[2 * i + 1];
        }
    }
    b->pso = nil;
}

/* ------------------------------------------------------------------ */
/* command buffer                                                      */
/* ------------------------------------------------------------------ */

/* An open encoder is a render pass.  Closing it is how a new load action
 * (ClearColor) or a new set of render targets takes effect. */
void end_encoder(CmdBuf* b) {
    if (b && b->enc) {
        [b->enc endEncoding];
        b->enc = nil;
    }
}

/* Opens the render pass for the targets recorded by SetRenderTargets.
 * Returns false while there is nothing to render into -- which is the normal
 * case until a texture exists for color[0]. */
bool ensure_encoder(Ctx* c, CmdBuf* b) {
    if (!b) return false;
    if (b->enc) return true;
    if (!c || !c->ready || !b->cb || b->numColor < 1) return false;

    Tex* t0 = find<Tex>(b->color[0]);
    if (!t0 || !t0->mtl) return false;

    @autoreleasepool {
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = t0->mtl;
        rpd.colorAttachments[0].loadAction =
            b->pendingClear ? MTLLoadActionClear : MTLLoadActionLoad;
        if (b->pendingClear) {
            rpd.colorAttachments[0].clearColor = MTLClearColorMake(
                b->clear[0], b->clear[1], b->clear[2], b->clear[3]);
        }
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        b->enc = [b->cb renderCommandEncoderWithDescriptor:rpd];
    }
    b->pendingClear = false;
    return b->enc != nil;
}

/* nvnCommandBufferBeginRecording(NVNcommandBuffer*) */
void cmdbuf_begin_recording(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!c || !c->ready || !b || b->recording) return;
    b->cb = [c->queue commandBuffer];
    b->recording = true;
    b->draws = 0;
    b->numColor = 0;
    b->depth = 0;
    b->pendingClear = false;
}

/* nvnCommandBufferEndRecording(NVNcommandBuffer*) -> NVNcommandHandle
 * The handle is what the guest submits through nvnQueueSubmitCommands.  We do
 * not record a command stream, so 0 ("nothing to submit") is the honest value;
 * the Metal work is already committed here, in queue order. */
void cmdbuf_end_recording(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b || !b->recording) {
        x[0] = 0;
        return;
    }
    end_encoder(b);
    if (b->cb) {
        [b->cb commit];
        b->last = b->cb;
        b->cb = nil;
    }
    b->recording = false;
    x[0] = 0;
}

/* nvnCommandBufferSetRenderTargets(NVNcommandBuffer*, int n,
 *     const NVNtexture* const* colors, const NVNtextureView* const* views,
 *     const NVNtexture* depth, const NVNtextureView* depthView) */
void cmdbuf_set_render_targets(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    const int n = static_cast<int>(x[1]);
    if (n < 0 || n > 8) {
        log_once("nvnCommandBufferSetRenderTargets.count", static_cast<unsigned>(n));
    }
    end_encoder(b);
    b->numColor = n <= 0 ? 0u : (n > 8 ? 8u : static_cast<unsigned>(n));
    const uint64_t* list = guest_ptr<const uint64_t>(x[2]);
    for (unsigned i = 0; i < 8; ++i) {
        b->color[i] = (list && i < b->numColor) ? list[i] : 0u;
    }
    b->depth = x[4];
    /* A pipeline is compiled for one colour format. */
    b->pso = nil;
    /* Texture views are not supported yet; passing a view is legal but the
     * base texture format is what gets attached. */
    if (x[3] || x[5]) log_once("nvnCommandBufferSetRenderTargets.views", 1);
    b->pendingClear = false;
}

/* nvnCommandBufferClearColor(NVNcommandBuffer*, int index, const float* rgba, int mask) */
void cmdbuf_clear_color(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    const float* rgba = guest_ptr<const float>(x[2]);
    if (!b || !rgba) return;
    b->clearIndex = static_cast<int>(x[1]);
    b->clearMask = static_cast<int>(x[3]);
    for (int i = 0; i < 4; ++i) b->clear[i] = rgba[i];
    b->pendingClear = true;
    /* A clear mid-pass cannot be expressed as a load action, so close the pass;
     * the next draw (or EndRecording) opens one that clears. */
    end_encoder(b);
    (void)c;
    /* ClearColori / ClearColorui use integer masks; this path only has floats.
     * The mask semantics are an unverified enum, so it is recorded, not acted on. */
    log_once("nvnCommandBufferClearColor.mask", static_cast<unsigned>(b->clearMask));
}

/* nvnCommandBufferDrawArrays(NVNcommandBuffer*, NVNdrawPrimitive, int first, int count) */
void cmdbuf_draw_arrays(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    b->draws++;
    /* Opening the pass is what makes a recorded ClearColor happen. */
    if (!ensure_encoder(c, b)) return;
    if (!ensure_pipeline(c, b)) {
        /* The honest line: there is no translated shader, so there is no
         * pipeline, so this draw is dropped.  Once per process. */
        log_once("nvnCommandBufferDrawArrays.noPipeline", 1);
        return;
    }
    const uint32_t primitive = static_cast<uint32_t>(x[1]);
    const unsigned first = static_cast<unsigned>(x[2]);
    const unsigned count = static_cast<unsigned>(x[3]);
    [b->enc setRenderPipelineState:b->pso];
    /* No attribute state bound (the self-test's [[vertex_id]] program) means
     * no vertex descriptor and nothing to bind. */
    if (b->numAttrib > 0 && !bind_vertex_streams(b)) return;
    [b->enc drawPrimitives:to_metal_primitive(primitive)
               vertexStart:first
               vertexCount:count];
}

/* nvnCommandBufferDrawElements(NVNcommandBuffer*, NVNdrawPrimitive,
 *                              NVNindexType, int count, NVNbufferAddress) */
void cmdbuf_draw_elements(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    b->draws++;
    if (!ensure_encoder(c, b)) return;
    const uint32_t indexType = static_cast<uint32_t>(x[2]);
    log_once("nvnCommandBufferDrawElements.indexType", indexType);
    /* An indexed draw needs the bound index buffer and the real NVNindexType
     * values; neither is verified, so this counts and stops. */
}

/* nvnCommandBufferSetViewport(NVNcommandBuffer*, int x, int y, int w, int h) */
void cmdbuf_set_viewport(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!c || !b || !ensure_encoder(c, b)) return;
    MTLViewport vp = {static_cast<double>(static_cast<int>(x[1])),
                      static_cast<double>(static_cast<int>(x[2])),
                      static_cast<double>(static_cast<int>(x[3])),
                      static_cast<double>(static_cast<int>(x[4])), 0.0, 1.0};
    [b->enc setViewport:vp];
}

/* nvnCommandBufferSetScissor(NVNcommandBuffer*, int x, int y, int w, int h) */
void cmdbuf_set_scissor(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!c || !b || !ensure_encoder(c, b)) return;
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
    BIND("nvnTextureBuilderSetTarget", tex_builder_set_target),
    BIND("nvnTextureBuilderSetFormat", tex_builder_set_format),
    BIND("nvnTextureBuilderSetSize1D", tex_builder_set_size1d),
    BIND("nvnTextureBuilderSetSize2D", tex_builder_set_size2d),
    BIND("nvnTextureBuilderSetSize3D", tex_builder_set_size3d),
    BIND("nvnTextureBuilderSetWidth", tex_builder_set_width),
    BIND("nvnTextureBuilderSetHeight", tex_builder_set_height),
    BIND("nvnTextureBuilderSetDepth", tex_builder_set_depth),
    BIND("nvnTextureBuilderSetLevels", tex_builder_set_levels),
    BIND("nvnTextureBuilderSetSamples", tex_builder_set_samples),
    BIND("nvnTextureBuilderSetStorage", tex_builder_set_storage),
    BIND("nvnTextureBuilderSetStride", tex_builder_set_stride),
    BIND("nvnTextureBuilderSetFlags", tex_builder_set_flags),
    BIND("nvnTextureInitialize", tex_initialize),

    BIND("nvnProgramInitialize", prog_initialize),
    BIND("nvnProgramSetShaders", prog_set_shaders),
    BIND("nvnProgramFinalize", prog_finalize),

    BIND("nvnCommandBufferBeginRecording", cmdbuf_begin_recording),
    BIND("nvnCommandBufferEndRecording", cmdbuf_end_recording),
    BIND("nvnCommandBufferSetRenderTargets", cmdbuf_set_render_targets),
    BIND("nvnCommandBufferClearColor", cmdbuf_clear_color),
    BIND("nvnCommandBufferDrawArrays", cmdbuf_draw_arrays),
    BIND("nvnCommandBufferDrawElements", cmdbuf_draw_elements),
    BIND("nvnCommandBufferSetViewport", cmdbuf_set_viewport),
    BIND("nvnCommandBufferSetScissor", cmdbuf_set_scissor),
    BIND("nvnCommandBufferBindProgram", cmdbuf_bind_program),
    BIND("nvnCommandBufferBindVertexBuffer", cmdbuf_bind_vertex_buffer),
    BIND("nvnCommandBufferBindVertexBuffers", cmdbuf_bind_vertex_buffers),
    BIND("nvnCommandBufferBindVertexAttribState", cmdbuf_bind_vertex_attrib_state),
    BIND("nvnCommandBufferBindVertexStreamState", cmdbuf_bind_vertex_stream_state),
    BIND("nvnVertexAttribStateSetDefaults", vattrib_set_defaults),
    BIND("nvnVertexAttribStateSetFormat", vattrib_set_format),
    BIND("nvnVertexAttribStateSetStreamIndex", vattrib_set_stream),
    BIND("nvnVertexAttribStateGetFormat", vattrib_get_format),
    BIND("nvnVertexAttribStateGetStreamIndex", vattrib_get_stream),
    BIND("nvnVertexStreamStateSetDefaults", vstream_set_defaults),
    BIND("nvnVertexStreamStateSetStride", vstream_set_stride),
    BIND("nvnVertexStreamStateSetDivisor", vstream_set_divisor),
    BIND("nvnVertexStreamStateGetStride", vstream_get_stride),
    BIND("nvnVertexStreamStateGetDivisor", vstream_get_divisor),

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

/* ------------------------------------------------------------------ */
/* self-test: one draw, through the entry points the guest would call  */
/* ------------------------------------------------------------------ */

/* Runs the whole replay path on whatever Metal device this process has:
 * texture builder -> nvnTextureInitialize -> BeginRecording -> SetRenderTargets
 * -> ClearColor -> BindProgram -> DrawArrays -> EndRecording, then blits the
 * target into shared memory and checks the centre pixel.
 *
 * The program is the built-in MSL from nvn_shader.h, reached through the same
 * nvngfx_shader_to_msl() seam a translator will use.  So this proves the
 * Metal half of the pipeline, not that a guest shader can be translated.
 *
 * Returns 0 on success, or a bit telling which step failed:
 *   1 no Metal device   2 backend not ready   4 texture     8 program
 *  16 recording       32 pipeline         64 draw colour  128 readback */
/* Copies the whole target into a shared buffer and hands back the centre
 * pixel.  Blit + waitUntilCompleted is the same readback the first version
 * did inline; pulling it out lets a second draw be checked the same way. */
bool read_center_pixel(id<MTLDevice> dev, id<MTLCommandQueue> queue,
                       id<MTLTexture> tex, NSUInteger side, unsigned char px[4]) {
    if (!dev || !queue || !tex) return false;
    const NSUInteger bpr = side * 4;
    id<MTLBuffer> buf = [dev newBufferWithLength:bpr * side
                                        options:MTLResourceStorageModeShared];
    if (!buf) return false;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(side, side, 1)
                 toBuffer:buf
        destinationOffset:0
   destinationBytesPerRow:bpr
 destinationBytesPerImage:bpr * side];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError) return false;
    const unsigned char* p = static_cast<const unsigned char*>([buf contents]) +
                             (side / 2) * bpr + (side / 2) * 4;
    for (int i = 0; i < 4; ++i) px[i] = p[i];
    return true;
}

/* True when every channel is within `tolerance` of `want`. */
bool pixels_near(const unsigned char px[4], const int want[4], int tolerance) {
    for (int i = 0; i < 4; ++i) {
        const int got = static_cast<int>(px[i]);
        const int diff = got > want[i] ? got - want[i] : want[i] - got;
        if (diff > tolerance) return false;
    }
    return true;
}

extern "C" int nvngfx_metal_selftest(void) {
    /* NVN objects live in guest memory, and guest memory is host memory here.
     * The texture and its builder are written through by the backend
     * (nvnTextureBuilderSetDefaults memsets 0x80 bytes), so they get real
     * backing store.  The command buffer and program are only table keys.
     * Passing a made-up address for the builder is how the first version of
     * this test crashed: the key looked fine until the memset. */
    alignas(16) unsigned char texObject[0x80] = {};
    alignas(16) unsigned char texBuilder[0x80] = {};
    const uint64_t kTex = reinterpret_cast<uint64_t>(texObject);
    const uint64_t kTexBuilder = reinterpret_cast<uint64_t>(texBuilder);
    const uint64_t kCb = 0x5200;
    const uint64_t kProg = 0x5300;
    const NSUInteger kSide = 64;

    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            std::fprintf(stderr, "[selftest] no Metal device\n");
            return 1;
        }

        nvngfx_ctx* ctx = nvngfx_metal_create();
        nvngfx_metal_set_device(ctx, (__bridge void*)dev, nullptr);
        Ctx* c = ctx_of(ctx);
        if (!c || !c->ready) {
            nvngfx_metal_destroy(ctx);
            std::fprintf(stderr, "[selftest] backend not ready\n");
            return 2;
        }

        uint64_t x[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int rc = 0;

        /* 64x64 RGBA8 render target, through the setters the guest uses. */
        x[0] = kTexBuilder;
        tex_builder_set_defaults(ctx, x);
        x[0] = kTexBuilder; x[1] = 0;
        tex_builder_set_target(ctx, x);            /* 2D */
        x[0] = kTexBuilder; x[1] = 0x25;
        tex_builder_set_format(ctx, x);            /* RGBA8_UNORM */
        x[0] = kTexBuilder; x[1] = (uint64_t)kSide; x[2] = (uint64_t)kSide;
        tex_builder_set_size2d(ctx, x);
        x[0] = kTexBuilder; x[1] = 1;
        tex_builder_set_levels(ctx, x);
        x[0] = kTexBuilder; x[1] = 1;
        tex_builder_set_samples(ctx, x);
        x[0] = kTex; x[1] = kTexBuilder;
        tex_initialize(ctx, x);
        Tex* t = find<Tex>(kTex);
        if (x[0] != 1 || !t || !t->mtl) {
            std::fprintf(stderr, "[selftest] nvnTextureInitialize failed\n");
            rc = 4;
            goto done;
        }

        {
            /* The built-in program, handed over exactly the way the guest hands
             * over its shaders: NVNbufferAddress + control pointer. */
            struct { uint64_t data; uint64_t control; } sd[2];
            nvngfx_shader_set_msl_passthrough(1);
            sd[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl(NVNGFX_STAGE_VERTEX)));
            sd[0].control = 0;
            sd[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl(NVNGFX_STAGE_FRAGMENT)));
            sd[1].control = 0;

            x[0] = kProg; x[1] = 0;
            prog_initialize(ctx, x);
            x[0] = kProg; x[1] = 2; x[2] = reinterpret_cast<uint64_t>(&sd[0]);
            prog_set_shaders(ctx, x);
            if (x[0] != 1) {
                std::fprintf(stderr, "[selftest] nvnProgramSetShaders failed\n");
                nvngfx_shader_set_msl_passthrough(0);
                rc = 8;
                goto done;
            }
        }

        {
            const uint64_t colourTarget = kTex;
            const float clear[4] = {1.0f, 0.0f, 0.0f, 1.0f};

            x[0] = kCb;
            cmdbuf_begin_recording(ctx, x);
            x[0] = kCb; x[1] = 1; x[2] = reinterpret_cast<uint64_t>(&colourTarget);
            x[3] = 0; x[4] = 0; x[5] = 0;
            cmdbuf_set_render_targets(ctx, x);
            x[0] = kCb; x[1] = 0; x[2] = reinterpret_cast<uint64_t>(clear); x[3] = 0xF;
            cmdbuf_clear_color(ctx, x);
            x[0] = kCb; x[1] = kProg; x[2] = 0;
            cmdbuf_bind_program(ctx, x);
            x[0] = kCb; x[1] = 0; x[2] = 0; x[3] = 3;
            cmdbuf_draw_arrays(ctx, x);
            x[0] = kCb;
            cmdbuf_end_recording(ctx, x);

            CmdBuf* b = find<CmdBuf>(kCb);
            if (!b || !b->last) {
                std::fprintf(stderr, "[selftest] no command buffer was committed\n");
                rc = 16;
                goto done;
            }
            [b->last waitUntilCompleted];
            if (b->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr, "[selftest] command buffer error: %s\n",
                             b->last.error ? [[b->last.error localizedDescription] UTF8String]
                                           : "(no description)");
                rc = 32;
                goto done;
            }
            if (b->draws != 1) {
                std::fprintf(stderr, "[selftest] %u draws recorded\n", b->draws);
                rc = 64;
                goto done;
            }
        }

        {
            /* Read back the centre pixel: this is the step that turns "a draw
             * was encoded" into "a draw produced pixels".  The fragment shader
             * returns (0.25, 0.5, 0.75, 1.0). */
            unsigned char px[4] = {0, 0, 0, 0};
            const int want[4] = {64, 128, 191, 255};
            if (!read_center_pixel(dev, c->queue, t->mtl, kSide, px)) {
                std::fprintf(stderr, "[selftest] readback failed\n");
                rc = 128;
                goto done;
            }
            if (!pixels_near(px, want, 2)) {
                std::fprintf(stderr,
                             "[selftest] centre pixel is (%u,%u,%u,%u), want (%d,%d,%d,%d)\n",
                             px[0], px[1], px[2], px[3], want[0], want[1], want[2], want[3]);
                rc = 128;
                goto done;
            }
            std::fprintf(stderr, "[selftest] draw 1 ([[vertex_id]]) produced the expected pixel\n");
        }

        {
            /* --- Second draw: a real vertex attribute -------------------
             * The pieces a guest actually uses, driven through the guest
             * entry points: a memory pool over host memory, an attribute
             * state, a stream state and a vertex buffer.  The shader reads
             * [[attribute(0)]] float2, so a vertex descriptor has to exist
             * and the pool has to be found for the vertex address. */
            const uint64_t kPool = 0x5400;
            const uint64_t kProg2 = 0x5600;
            const uint64_t kCb2 = 0x5700;
            /* The pool wraps all 4096 bytes (Metal requires the range to be
             * a page), and the triangle lives at offset 0. */
            alignas(4096) static unsigned char vertexPool[4096] = {};
            static bool pool_filled = false;
            if (!pool_filled) {
                float* v = reinterpret_cast<float*>(vertexPool);
                v[0] = -1.0f; v[1] = -1.0f;
                v[2] = 3.0f;  v[3] = -1.0f;
                v[4] = -1.0f; v[5] = 3.0f;
                pool_filled = true;
            }
            alignas(16) unsigned char poolBuilder[0x40] = {};
            const uint64_t kPoolBuilder = reinterpret_cast<uint64_t>(poolBuilder);
            alignas(16) unsigned char attribState[8] = {};
            alignas(16) unsigned char streamState[8] = {};
            const uint64_t kAttrib = reinterpret_cast<uint64_t>(attribState);
            const uint64_t kStream = reinterpret_cast<uint64_t>(streamState);

            x[0] = kPoolBuilder;
            pool_builder_set_defaults(ctx, x);
            x[0] = kPoolBuilder;
            x[1] = reinterpret_cast<uint64_t>(vertexPool);
            x[2] = 4096;
            pool_builder_set_storage(ctx, x);
            x[0] = kPool;
            x[1] = kPoolBuilder;
            pool_initialize(ctx, x);
            if (x[0] != 1) {
                std::fprintf(stderr, "[selftest] nvnMemoryPoolInitialize failed\n");
                rc = 256;
                goto done;
            }

            /* RG32F attribute 0 at offset 0, fed by stream 0 with stride 8. */
            x[0] = kAttrib;
            vattrib_set_defaults(ctx, x);
            x[0] = kAttrib; x[1] = 0x16; x[2] = 0;
            vattrib_set_format(ctx, x);
            x[0] = kAttrib; x[1] = 0;
            vattrib_set_stream(ctx, x);
            x[0] = kStream;
            vstream_set_defaults(ctx, x);
            x[0] = kStream; x[1] = 8;
            vstream_set_stride(ctx, x);

            /* Read the state back the way a guest would: the setters must
             * have written the 4-byte record the hardware format describes. */
            unsigned gotFmt = 0xDEADu;
            long long gotOff = -1;
            x[0] = kAttrib; x[1] = reinterpret_cast<uint64_t>(&gotFmt);
            x[2] = reinterpret_cast<uint64_t>(&gotOff);
            vattrib_get_format(ctx, x);
            if (gotFmt != 0x16u || gotOff != 0) {
                std::fprintf(stderr,
                             "[selftest] attrib state round trip gave format %#x offset %lld\n",
                             gotFmt, gotOff);
                rc = 512;
                goto done;
            }

            {
                struct { uint64_t data; uint64_t control; } sd2[2];
                sd2[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                    nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 1)));
                sd2[0].control = 0;
                sd2[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                    nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 1)));
                sd2[1].control = 0;
                x[0] = kProg2; x[1] = 0;
                prog_initialize(ctx, x);
                x[0] = kProg2; x[1] = 2; x[2] = reinterpret_cast<uint64_t>(&sd2[0]);
                prog_set_shaders(ctx, x);
                if (x[0] != 1) {
                    std::fprintf(stderr, "[selftest] program 2: SetShaders failed\n");
                    rc = 512;
                    goto done;
                }
            }

            {
                const uint64_t target = kTex;
                const float clear2[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                x[0] = kCb2;
                cmdbuf_begin_recording(ctx, x);
                x[0] = kCb2; x[1] = 1; x[2] = reinterpret_cast<uint64_t>(&target);
                x[3] = 0; x[4] = 0; x[5] = 0;
                cmdbuf_set_render_targets(ctx, x);
                x[0] = kCb2; x[1] = 0; x[2] = reinterpret_cast<uint64_t>(clear2);
                x[3] = 0xF;
                cmdbuf_clear_color(ctx, x);
                x[0] = kCb2; x[1] = 1;
                x[2] = reinterpret_cast<uint64_t>(&attribState[0]);
                cmdbuf_bind_vertex_attrib_state(ctx, x);
                x[0] = kCb2; x[1] = 1;
                x[2] = reinterpret_cast<uint64_t>(&streamState[0]);
                cmdbuf_bind_vertex_stream_state(ctx, x);
                x[0] = kCb2; x[1] = 0;
                x[2] = reinterpret_cast<uint64_t>(vertexPool); x[3] = 24;
                cmdbuf_bind_vertex_buffer(ctx, x);
                x[0] = kCb2; x[1] = kProg2; x[2] = 0;
                cmdbuf_bind_program(ctx, x);
                x[0] = kCb2; x[1] = 0; x[2] = 0; x[3] = 3;
                cmdbuf_draw_arrays(ctx, x);
                x[0] = kCb2;
                cmdbuf_end_recording(ctx, x);

                CmdBuf* b2 = find<CmdBuf>(kCb2);
                if (!b2 || !b2->last) {
                    std::fprintf(stderr, "[selftest] draw 2 committed nothing\n");
                    rc = 1024;
                    goto done;
                }
                [b2->last waitUntilCompleted];
                if (b2->last.status == MTLCommandBufferStatusError) {
                    std::fprintf(stderr, "[selftest] draw 2 command buffer error: %s\n",
                                 b2->last.error
                                     ? [[b2->last.error localizedDescription] UTF8String]
                                     : "(no description)");
                    rc = 1024;
                    goto done;
                }
            }

            {
                unsigned char px[4] = {0, 0, 0, 0};
                const int want[4] = {0, 255, 0, 255};
                if (!read_center_pixel(dev, c->queue, t->mtl, kSide, px)) {
                    std::fprintf(stderr, "[selftest] draw 2 readback failed\n");
                    rc = 2048;
                    goto done;
                }
                if (!pixels_near(px, want, 2)) {
                    std::fprintf(stderr,
                                 "[selftest] draw 2 centre pixel is (%u,%u,%u,%u), "
                                 "want (%d,%d,%d,%d)\n",
                                 px[0], px[1], px[2], px[3], want[0], want[1], want[2],
                                 want[3]);
                    rc = 2048;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] draw 2 ([[attribute(0)]] from a pool) produced "
                             "the expected pixel\n");
            }
        }

    done:
        nvngfx_shader_set_msl_passthrough(0);
        forget<CmdBuf>(kCb);
        forget<Tex>(kTex);
        forget<Program>(kProg);
        forget<Program>(0x5600);
        forget<CmdBuf>(0x5700);
        forget<Pool>(0x5400);
        g_pool_specs.erase(0x5500);
        g_tex_specs.erase(kTexBuilder);
        nvngfx_metal_destroy(ctx);
        return rc;
    }
}

extern "C" const nvngfx_backend* nvngfx_metal_backend(void) {
    return &k_metal;
}

/* Lifetime is owned by the app: create once, install, destroy at exit. */
extern "C" nvngfx_ctx* nvngfx_metal_create(void) {
    /* The offline chain's output, if this build has any: the table lives in
     * nvn_shader_blobs.gen.c and is empty in the committed tree.  Installing
     * it here means a program's key finds its MSL without any further setup. */
    nvngfx_shader_install_offline_blobs();
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
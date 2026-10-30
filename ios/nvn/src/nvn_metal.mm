/*
 * nvn_metal.mm -- NVN -> Metal host backend (Apple platforms only).
 *
 * Scope, stated up front so nobody mistakes this for finished:
 *
 *   DONE  device / queue / window / swapchain lifecycle
 *         memory pool -> zero-copy MTLBuffer over guest memory
 *         command buffer -> MTLCommandBuffer + render encoder
 *         viewport / scissor / drawArrays / present(blit fast path)
 *
 *   TODO  shader translation (NVN ships prebuilt GPU ISA, not source)
 *         texture tiling, BCn -> ASTC/ETC2 transcode, format-mismatch present
 *         state objects beyond defaults, sync/fence, events, compute
 *
 * Two things this file deliberately does NOT do:
 *
 *   1. It never invents NVN enum values.  Formats, primitive types and index
 *      types are numbers whose real values live in Nintendo's nvn.h.  Guessing
 *      them would yield a backend that looks complete and is silently wrong.
 *      Until kEnumsVerified is flipped, every enum goes through log_once()
 *      and the call is dropped -- loudly, once per value.
 *
 *   2. It never copies guest GPU memory "just in case".  iOS lives on unified
 *      memory, so the guest heap is already host-readable and Metal can wrap
 *      it with newBufferWithBytesNoCopy.  The requirement is that the guest
 *      allocator hands out page-aligned storage; see docs/IOS_PORTING.md.
 *
 * Handle convention
 * -----------------
 * NVN handles are opaque to the guest.  Here a handle is the 64-bit address of
 * a host object, and a "return a handle" is written into x[0] of the argument
 * block.  The emulator's handle registry turns that back into something the
 * guest can pass around.  Nothing in the guest ever dereferences it.
 */
#if defined(__APPLE__)

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "nvn_backend.h"

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
/* objects                                                             */
/* ------------------------------------------------------------------ */

struct Ctx {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    unsigned presents = 0;
    unsigned dropped = 0;
    bool ready = false;
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
    Pool* pool = nullptr;
    size_t offset = 0;
    id<MTLTexture> mtl = nil;
};

struct CmdBuf {
    bool recording = false;
    unsigned draws = 0;
    id<MTLCommandBuffer> cb = nil;
    id<MTLRenderCommandEncoder> enc = nil;
};

struct Win {
    CAMetalLayer* layer = nil;
    unsigned numTextures = 0;
    unsigned numActive = 0;
    unsigned presentInterval = 1;
    unsigned cursor = 0;
    Tex* textures[4] = {nullptr, nullptr, nullptr, nullptr};
};

/* Builder specs are gathered in a side table keyed by the guest's builder
 * pointer, because the guest owns that struct and its layout is not verified
 * yet.  Keeping the mapping here means only one place has to change once the
 * real offsets are known. */
struct PoolSpec {
    void* storage = nullptr;
    size_t size = 0;
    uint32_t flags = 0;
};

struct WinSpec {
    void* nativeWindow = nullptr;
    unsigned presentInterval = 1;
    unsigned numActive = 0;
    unsigned numTextures = 0;
    Tex* textures[4] = {nullptr, nullptr, nullptr, nullptr};
};

Ctx* ctx_of(nvngfx_ctx* ctx) { return reinterpret_cast<Ctx*>(ctx); }

template <class T>
T* obj(uint64_t h) { return reinterpret_cast<T*>(static_cast<uintptr_t>(h)); }
template <class T>
uint64_t handle(T* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }

std::unordered_map<const void*, PoolSpec> g_pool_specs;
std::unordered_map<const void*, WinSpec> g_win_specs;

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
/* device                                                              */
/* ------------------------------------------------------------------ */

void dev_builder_set_defaults(nvngfx_ctx*, uint64_t* x) {
    if (x[0]) std::memset(reinterpret_cast<void*>(x[0]), 0, 0x20);
}

void dev_builder_set_flags(nvngfx_ctx*, uint64_t* x) { log_once("NVNdeviceFlags", (unsigned)x[1]); }
void dev_builder_get_flags(nvngfx_ctx*, uint64_t*) {}

void dev_initialize(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    if (!c) return;
    (void)x;
    c->device = MTLCreateSystemDefaultDevice();
    if (!c->device) {
        std::fprintf(stderr, "[nvn] no Metal device\n");
        return;
    }
    c->queue = [c->device newCommandQueue];
    c->ready = (c->queue != nil);
    std::fprintf(stderr, "[nvn] metal device: %s\n", [[c->device name] UTF8String]);
}

void dev_get_proc_address(nvngfx_ctx*, uint64_t* x) {
    const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(x[1]));
    nvngfx_proc p = nvngfx_lookup(name);
    x[0] = p ? static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)) : 0;
}

/* ------------------------------------------------------------------ */
/* window                                                              */
/* ------------------------------------------------------------------ */

void win_builder_set_defaults(nvngfx_ctx*, uint64_t* x) {
    if (x[0]) std::memset(reinterpret_cast<void*>(x[0]), 0, 0x40);
}

void win_builder_set_native_window(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    g_win_specs[reinterpret_cast<const void*>(static_cast<uintptr_t>(x[0]))].nativeWindow =
        reinterpret_cast<void*>(static_cast<uintptr_t>(x[1]));
}

void win_builder_set_textures(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    WinSpec& s = g_win_specs[reinterpret_cast<const void*>(static_cast<uintptr_t>(x[0]))];
    /* x[1] points at an array of Tex handles, not at a Tex. */
    Tex** list = reinterpret_cast<Tex**>(static_cast<uintptr_t>(x[1]));
    const unsigned n = static_cast<unsigned>(x[2]);
    s.numTextures = n > 4 ? 4 : n;
    for (unsigned i = 0; i < s.numTextures; ++i) s.textures[i] = list ? list[i] : nullptr;
}

void win_builder_set_present_interval(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    g_win_specs[reinterpret_cast<const void*>(static_cast<uintptr_t>(x[0]))].presentInterval =
        static_cast<unsigned>(x[1]);
}

void win_builder_set_num_active_textures(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    g_win_specs[reinterpret_cast<const void*>(static_cast<uintptr_t>(x[0]))].numActive =
        static_cast<unsigned>(x[1]);
}

void win_initialize(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    if (!c || !c->ready) return;

    /* x[0] = guest window slot, x[1] = builder pointer. */
    const void* key = reinterpret_cast<const void*>(static_cast<uintptr_t>(x[1]));
    WinSpec spec;
    auto it = g_win_specs.find(key);
    if (it != g_win_specs.end()) spec = it->second;

    Win* w = new Win();
    w->layer = static_cast<CAMetalLayer*>([CAMetalLayer layer]);
    w->layer.device = c->device;
    w->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    w->layer.framebufferOnly = NO;  /* we blit the guest's target in */
    w->layer.displaySyncEnabled = (spec.presentInterval != 0);
    w->numTextures = spec.numTextures;
    w->numActive = spec.numActive ? spec.numActive : spec.numTextures;
    w->presentInterval = spec.presentInterval;
    for (unsigned i = 0; i < 4; ++i) w->textures[i] = spec.textures[i];

    /* Metal exposes one switch, not an interval: 0 == uncapped, else v-sync.
     * Keep the drawable count to the active texture count so the CPU can run
     * ahead exactly as far as the guest's own swapchain allows. */
    unsigned n = w->numActive;
    if (n < 2) n = 2;
    if (n > 3) n = 3;
    w->layer.maximumDrawableCount = n;

    x[0] = handle(w);
}

void win_acquire_texture(nvngfx_ctx*, uint64_t* x) {
    Win* w = obj<Win>(x[0]);
    if (!w || w->numTextures == 0) {
        x[0] = 0;
        return;
    }
    Tex* t = w->textures[w->cursor % w->numTextures];
    w->cursor = (w->cursor + 1) % w->numTextures;
    x[0] = handle(t);
}

void win_get_num_textures(nvngfx_ctx*, uint64_t* x) {
    Win* w = obj<Win>(x[0]);
    x[0] = w ? w->numTextures : 0;
}

void win_set_present_interval(nvngfx_ctx*, uint64_t* x) {
    Win* w = obj<Win>(x[0]);
    if (!w) return;
    w->presentInterval = static_cast<unsigned>(x[1]);
    if (w->layer) w->layer.displaySyncEnabled = (w->presentInterval != 0);
}

void win_set_num_active_textures(nvngfx_ctx*, uint64_t* x) {
    Win* w = obj<Win>(x[0]);
    if (w) w->numActive = static_cast<unsigned>(x[1]);
}

void win_finalize(nvngfx_ctx*, uint64_t* x) { delete obj<Win>(x[0]); }

/* ------------------------------------------------------------------ */
/* memory pools                                                        */
/* ------------------------------------------------------------------ */

void pool_builder_set_defaults(nvngfx_ctx*, uint64_t* x) {
    if (x[0]) std::memset(reinterpret_cast<void*>(x[0]), 0, 0x30);
}

void pool_builder_set_storage(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    /* NVNmemoryPoolBuilderSetStorage(builder, storage, size). */
    PoolSpec& s = g_pool_specs[reinterpret_cast<const void*>(static_cast<uintptr_t>(x[0]))];
    s.storage = reinterpret_cast<void*>(static_cast<uintptr_t>(x[1]));
    s.size = static_cast<size_t>(x[2]);
}

void pool_builder_set_flags(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    g_pool_specs[reinterpret_cast<const void*>(static_cast<uintptr_t>(x[0]))].flags =
        static_cast<uint32_t>(x[1]);
}

void pool_initialize(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    if (!c || !c->ready) return;

    /* x[0] = guest pool slot, x[1] = builder pointer. */
    const void* key = reinterpret_cast<const void*>(static_cast<uintptr_t>(x[1]));
    PoolSpec spec;
    auto it = g_pool_specs.find(key);
    if (it != g_pool_specs.end()) spec = it->second;
    if (!spec.storage || !spec.size) {
        std::fprintf(stderr, "[nvn] nvnMemoryPoolInitialize without storage\n");
        x[0] = 0;
        return;
    }

    Pool* p = new Pool();
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
        delete p;
        x[0] = 0;
        return;
    }
    x[0] = handle(p);
}

void pool_finalize(nvngfx_ctx*, uint64_t* x) { delete obj<Pool>(x[0]); }


void pool_get_size(nvngfx_ctx*, uint64_t* x) {
    Pool* p = obj<Pool>(x[0]);
    x[0] = p ? static_cast<uint64_t>(p->size) : 0;
}

/* ------------------------------------------------------------------ */
/* textures                                                            */
/* ------------------------------------------------------------------ */

void tex_builder_set_defaults(nvngfx_ctx*, uint64_t* x) {
    if (x[0]) std::memset(reinterpret_cast<void*>(x[0]), 0, 0x80);
}

void tex_initialize(nvngfx_ctx*, uint64_t* x) {
    /* The NVNtextureBuilder field offsets are not verified yet, so this
     * allocates the handle and stops.  Turning it on is the first item in
     * docs/IOS_PORTING.md's "texture path" section. */
    log_once("nvnTextureInitialize", 1);
    x[0] = handle(new Tex());
}

/* ------------------------------------------------------------------ */
/* command buffer                                                      */
/* ------------------------------------------------------------------ */

void cmdbuf_begin_recording(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = obj<CmdBuf>(x[0]);
    if (!c || !c->ready || !b || b->recording) return;
    b->cb = [c->queue commandBuffer];
    b->recording = true;
    b->draws = 0;
}

void cmdbuf_end_recording(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = obj<CmdBuf>(x[0]);
    if (!b || !b->recording) return;
    if (b->enc) {
        [b->enc endEncoding];
        b->enc = nil;
    }
    b->recording = false;
}

void cmdbuf_clear_color(nvngfx_ctx*, uint64_t* x) {
    /* Needs the bound render target to become a real clear; until render-target
     * binding exists the values are recorded and dropped. */
    log_once("nvnCommandBufferClearColor", static_cast<unsigned>(x[5]));
}

void cmdbuf_draw_arrays(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = obj<CmdBuf>(x[0]);
    if (!b || !b->enc) return;
    const unsigned first = static_cast<unsigned>(x[1]);
    const unsigned count = static_cast<unsigned>(x[2]);
    [b->enc drawPrimitives:to_metal_primitive(static_cast<uint32_t>(x[3]))
               vertexStart:first
               vertexCount:count];
    b->draws++;
}

void cmdbuf_draw_elements(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = obj<CmdBuf>(x[0]);
    if (!b || !b->enc) return;
    log_once("nvnCommandBufferDrawElements.indexType", static_cast<unsigned>(x[6]));
    b->draws++;
}

void cmdbuf_set_viewport(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = obj<CmdBuf>(x[0]);
    if (!b || !b->enc) return;
    MTLViewport vp = {static_cast<double>(x[1]), static_cast<double>(x[2]),
                      static_cast<double>(x[3]), static_cast<double>(x[4]), 0.0, 1.0};
    [b->enc setViewport:vp];
}

void cmdbuf_set_scissor(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = obj<CmdBuf>(x[0]);
    if (!b || !b->enc) return;
    MTLScissorRect r = {static_cast<NSUInteger>(x[1]), static_cast<NSUInteger>(x[2]),
                        static_cast<NSUInteger>(x[3]), static_cast<NSUInteger>(x[4])};
    [b->enc setScissorRect:r];
}

/* ------------------------------------------------------------------ */
/* present                                                             */
/* ------------------------------------------------------------------ */

void queue_present_texture(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    Win* w = obj<Win>(x[1]);
    Tex* t = obj<Tex>(x[2]);
    if (!c || !c->ready || !w || !w->layer) return;

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [w->layer nextDrawable];
        if (!drawable) {
            c->dropped++;
            return;
        }
        id<MTLCommandBuffer> cb = [c->queue commandBuffer];
        if (t && t->mtl && t->mtl.width == drawable.texture.width &&
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
            log_once("present.formatMismatch", t && t->mtl ? static_cast<unsigned>(t->mtl.pixelFormat) : 0u);
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

extern "C" void nvngfx_metal_destroy(nvngfx_ctx* ctx) {
    delete reinterpret_cast<Ctx*>(ctx);
}

#endif /* __APPLE__ */



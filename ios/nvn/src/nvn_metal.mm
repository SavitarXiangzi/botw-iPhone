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
 *         nvngfx_metal_selftest() in CI's iOS Simulator: eight draws, a
 *         bindless table probe, and a vertex-stage argument-buffer draw whose
 *         shifted pixels are read back.  The last draw builds a block-linear
 *         texture in a guest memory pool and proves that the sampled pixels
 *         came from that storage, not from a host-side upload.  A sampler
 *         state object is also created, compared and read back.
 *         MRT / depth attachment / texture views
 *         streaming texture updates and a LINEAR-storage upload path
 *         BCn -> ASTC/ETC2 transcode, compute, sync/fence
 *
 * Argument ABI: measured, not guessed
 * -----------------------------------
 * Every host proc receives the guest's x0..x7 in x[] and s0..s7/d0..d7 in
 * f[] (see nvngfx_args).  The shapes live in
 * ios/nvn/signatures.json, extracted from the public nxnative prototype list
 * (MIT).  Three consequences decide this whole file:
 *
 *   - NVN objects are GUEST memory.  nvnWindowInitialize(NVNwindow*, builder)
 *     takes a pointer to the guest's own object; the host never owns that
 *     storage and never hands one back.  Host state is therefore keyed by the
 *     guest address, which is what x[0] holds on every object entry point.
 *   - Integer/pointer returns go in w0, i.e. x[0]; float returns go in s0,
 *     i.e. the low 32 bits of f[0].  NVNboolean is 1/0, and getters overwrite
 *     the return slot with their result.
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
#include "nvn_pool.h"
#include "nvn_sampler.h"
#include "nvn_shader.h"
#include "nvn_texlayout.h"
#include "nvn_texture.h"
#include "nvn_vertex.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

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
    /* The builder state, kept so the upload path can ask nvn_texlayout for
     * the storage geometry without a second side table. */
    nvngfx_texdesc desc = {};
    /* A sampled texture starts empty and is filled from the guest's pool the
     * first time a draw reads it.  `uploadTried` makes that a one-shot even
     * when it failed, so a broken texture does not pay for it every draw. */
    bool uploadTried = false;
    bool uploaded = false;
    /* Set once this texture has been a render target in this process: the
     * pixels in it are ours, not the guest's, so uploading would overwrite
     * what we just drew. */
    bool rendered = false;
    /* How many mip levels the guest storage actually provided. */
    unsigned uploadedLevels = 0;
};

struct Sampler {
    nvngfx_sampler state = {};
    uint64_t device = 0;
    std::string label;
    id<MTLSamplerState> mtl = nil;
};

struct CmdBuf {
    bool recording = false;
    unsigned draws = 0;
    /* Draws actually handed to Metal, as opposed to draws asked for.
     * A self-test pixel check cannot tell "dropped upstream" from
     * "encoded but wrong", and that difference is the whole
     * diagnosis, so it is a number rather than a guess. */
    unsigned encoded = 0;
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
    /* Uniform buffers as guest addresses, per shader stage.  Every NVN
     * Bind* call carries an NVNshaderStage, and the vertex and pixel stages
     * may bind different buffers to the same index, so one shared table
     * would silently let the last binding win.  Index 0 is vertex and 1 is
     * fragment -- NVN's own enum order.  These are what an argument buffer
     * is built from: a spirv-cross shader reads its constants through it,
     * not through setFragmentBytes. */
    uint64_t ubo[2][16] = {};
    uint64_t uboSize[2][16] = {};
    /* The descriptor pools the guest bound, and the handles it bound for
     * each stage.  A handle is a pool ID (texture in the low 20 bits,
     * sampler in the next 20), not a host pointer: the bound pool's base is
     * added when the handle is resolved, so the pool has to be bound before
     * the draw that uses it. */
    uint64_t texPool = 0;
    uint64_t sampPool = 0;
    /* NVN's driver table has 32 combined texture slots (c2 0x20..0x120), so
     * these are sized to the API rather than to the self-test's one slot. */
    uint64_t texHandle[2][NVNGFX_MAX_TEXTURE_SLOTS] = {};
    uint64_t sepTexHandle[2][NVNGFX_MAX_TEXTURE_SLOTS] = {};
    uint64_t sepSampHandle[2][NVNGFX_MAX_SAMPLER_SLOTS] = {};
    /* The argument buffers, one per stage, rebuilt whenever a binding
     * changes and dropped when a new recording starts (the previous one may
     * still be in flight). */
    id<MTLBuffer> argbuf[2] = {nil, nil};
    /* The last committed buffer.  nvnQueueSubmitCommands is not implemented,
     * so a frame is committed at EndRecording; keeping the handle lets a
     * caller (the self-test) wait for it and keeps the door open for real
     * submit semantics. */
    id<MTLCommandBuffer> last = nil;
    /* Viewport and scissor are command-buffer state.  Record them instead of
     * opening a render pass early: a texture upload may need to end the pass,
     * and ensure_encoder() must be able to restore them when it reopens. */
    MTLViewport viewport = {};
    bool hasViewport = false;
    MTLScissorRect scissor = {};
    bool hasScissor = false;
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

struct SamplerSpec {
    nvngfx_sampler state = {};
    uint64_t device = 0;
    float lodSnap = 0.0f;
};

/* A sampler as a pool stores it.  A registration from an NVNsampler object
 * could keep the object address, but a builder registration has no object to
 * look at later, so the pool snapshots the state and the Metal state. */
struct PooledSampler {
    nvngfx_sampler state = {};
    id<MTLSamplerState> mtl = nil;
};

/* NVNtexturePool / NVNsamplerPool.  `base` is the pool's first slot in the
 * host table; registrations are keyed by base + id so two pools can hold
 * different resources under the same guest ID. */
struct TexturePool {
    uint64_t memory = 0;
    long long offset = 0;
    int count = 0;
    int base = 0;
    std::string label;
    std::unordered_map<int, uint64_t> textures;  /* slot -> NVNtexture* */
    std::unordered_map<int, uint64_t> views;     /* slot -> NVNtextureView* */
};

struct SamplerPool {
    uint64_t memory = 0;
    long long offset = 0;
    int count = 0;
    int base = 0;
    std::string label;
    std::unordered_map<int, PooledSampler> samplers;  /* slot -> snapshot */
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
std::unordered_map<uint64_t, SamplerSpec> g_sampler_specs;
/* Pool numbering is process-wide, like nxnative's dpool_add: finalizing a
 * pool does not give its range to the next one, because a command buffer
 * recorded earlier may still refer to those slots. */
nvngfx_pool_map g_tex_pool_map;
nvngfx_pool_map g_samp_pool_map;

/* Guest memory is host-visible, so a pointer argument is just a cast.  This
 * is the one place that assumption is spelled out. */
template <class T>
T* guest_ptr(uint64_t value) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(value));
}

/* Zeroes a guest struct through its address.  Guest memory is host memory,
 * but an address that cannot be mapped must drop the call, not kill the
 * process: a bad handle in the guest is a bug report, not a crash.  The
 * first 64 KiB are never mapped on iOS, and an address that wraps is not a
 * pointer either. */
bool guest_write(uint64_t addr, size_t n) {
    if (addr < 0x10000u || n > UINTPTR_MAX - addr) {
        log_once("nvn.guestPointer.write", static_cast<unsigned>(addr >> 32));
        return false;
    }
    std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), 0, n);
    return true;
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
    if (x[0]) (void)guest_write(x[0], 0x20);
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
    if (x[0]) (void)guest_write(x[0], 0x40);
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
    if (x[0]) (void)guest_write(x[0], 0x30);
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
    if (x[0]) (void)guest_write(x[0], 0x80);
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
 * does not alias the guest's storage.  A render target is filled by this
 * backend and is never overwritten from the guest.  A sampled texture is
 * copied lazily from the guest pool on its first draw by tex_upload(), which
 * deswizzles every mip level first.  See docs/IOS_PORTING.md, texture path.
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
        t->desc = d;
        t->layers = 1;
        t->uploadTried = false;
        t->uploaded = false;
        t->rendered = false;
        t->uploadedLevels = 0;
        t->mtl = [c->device newTextureWithDescriptor:td];
        x[0] = t->mtl ? 1u : 0u;
    }
}

/* ------------------------------------------------------------------ */
/* samplers                                                            */
/* ------------------------------------------------------------------ */

/* The guest argument block starts with x[0..7]; the FP half follows it.  The
 * dispatcher hands every handler a pointer to the first word, so recovering
 * the whole block is a cast, not a second ABI. */
nvngfx_args* sampler_args(uint64_t* x) { return reinterpret_cast<nvngfx_args*>(x); }

const SamplerSpec& sampler_spec_or_default(uint64_t builder) {
    static const SamplerSpec fallback = [] {
        SamplerSpec s;
        nvngfx_sampler_defaults(&s.state);
        return s;
    }();
    auto it = g_sampler_specs.find(builder);
    return it == g_sampler_specs.end() ? fallback : it->second;
}

SamplerSpec& sampler_spec(uint64_t builder) {
    auto it = g_sampler_specs.find(builder);
    if (it == g_sampler_specs.end()) {
        /* A builder seen for the first time behaves like one that has already
         * received SetDefaults.  The guest is still expected to call it, but a
         * setter must not start from an all-zero filter/wrap enum by accident. */
        SamplerSpec s;
        nvngfx_sampler_defaults(&s.state);
        it = g_sampler_specs.emplace(builder, s).first;
    }
    return it->second;
}

MTLSamplerAddressMode to_metal_address(nvngfx_wrap wrap) {
    switch (wrap) {
    case NVNGFX_WRAP_CLAMP_TO_EDGE: return MTLSamplerAddressModeClampToEdge;
    case NVNGFX_WRAP_REPEAT: return MTLSamplerAddressModeRepeat;
    case NVNGFX_WRAP_MIRROR_CLAMP_TO_EDGE: return MTLSamplerAddressModeMirrorClampToEdge;
    case NVNGFX_WRAP_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
    case NVNGFX_WRAP_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
    default: return MTLSamplerAddressModeRepeat;
    }
}

MTLSamplerBorderColor to_metal_border(const float rgba[4]) {
    /* Same rule as nxnative: alpha chooses transparent vs opaque, red chooses
     * black vs white.  Metal has no arbitrary colour border. */
    if (rgba[3] > 0.5f) return rgba[0] > 0.5f ? MTLSamplerBorderColorOpaqueWhite
                                               : MTLSamplerBorderColorOpaqueBlack;
    return MTLSamplerBorderColorTransparentBlack;
}

#if defined(__IPHONE_OS_VERSION_MAX_ALLOWED) && __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
MTLSamplerReductionMode to_metal_reduction(nvngfx_reduction r) {
    switch (r) {
    case NVNGFX_REDUCTION_MIN: return MTLSamplerReductionModeMinimum;
    case NVNGFX_REDUCTION_MAX: return MTLSamplerReductionModeMaximum;
    default: return MTLSamplerReductionModeWeightedAverage;
    }
}
#endif

id<MTLSamplerState> create_sampler_state(Ctx* c, const nvngfx_sampler& st) {
    if (!c || !c->ready) return nil;
    @autoreleasepool {
        MTLSamplerDescriptor* d = [[MTLSamplerDescriptor alloc] init];
        d.minFilter = st.min_filter == NVNGFX_TEXFILTER_LINEAR
                          ? MTLSamplerMinMagFilterLinear
                          : MTLSamplerMinMagFilterNearest;
        d.magFilter = st.mag_filter == NVNGFX_TEXFILTER_LINEAR
                          ? MTLSamplerMinMagFilterLinear
                          : MTLSamplerMinMagFilterNearest;
        switch (st.mip_filter) {
        case NVNGFX_MIPFILTER_NEAREST: d.mipFilter = MTLSamplerMipFilterNearest; break;
        case NVNGFX_MIPFILTER_LINEAR: d.mipFilter = MTLSamplerMipFilterLinear; break;
        default: d.mipFilter = MTLSamplerMipFilterNotMipmapped; break;
        }
        d.sAddressMode = to_metal_address(static_cast<nvngfx_wrap>(st.wrap_s));
        d.tAddressMode = to_metal_address(static_cast<nvngfx_wrap>(st.wrap_t));
        d.rAddressMode = to_metal_address(static_cast<nvngfx_wrap>(st.wrap_r));

        /* GetLodClamp returns the guest's exact values.  Metal needs a
         * non-negative, ordered range, so only the descriptor is adjusted. */
        float lo = st.lod_min;
        float hi = st.lod_max;
        if (lo < 0.0f) lo = 0.0f;
        if (hi < lo) hi = lo;
        d.lodMinClamp = lo;
        d.lodMaxClamp = hi;

        float aniso = st.max_aniso;
        if (aniso < 1.0f) aniso = 1.0f;
        if (aniso > 16.0f) aniso = 16.0f;
        d.maxAnisotropy = static_cast<NSUInteger>(aniso);

        d.compareFunction = static_cast<MTLCompareFunction>(st.compare);
        if (st.reduction != NVNGFX_REDUCTION_AVERAGE) {
            /* MTLSamplerReductionMode is new in iOS 26.  The compile-time
             * guard keeps Xcode 16/iOS 18 SDKs buildable; the runtime guard
             * keeps the iOS 17 deployment target from messaging it.  The
             * guest value is still preserved for the getters on every OS. */
#if defined(__IPHONE_OS_VERSION_MAX_ALLOWED) && __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
            if (@available(iOS 26.0, *)) {
                d.reductionMode = to_metal_reduction(static_cast<nvngfx_reduction>(st.reduction));
                /* Metal ignores reduction with nearest/no mip filtering.  The
                 * nxnative mapping keeps integer-LOD min/max reduction exact
                 * by forcing linear mip selection and clamping to level 0. */
                if (d.mipFilter == MTLSamplerMipFilterNotMipmapped) {
                    d.mipFilter = MTLSamplerMipFilterLinear;
                    d.lodMinClamp = 0.0f;
                    d.lodMaxClamp = 0.0f;
                }
            } else {
                log_once("nvnSamplerInitialize.reductionUnavailable", st.reduction);
            }
#else
            log_once("nvnSamplerInitialize.reductionUnavailable", st.reduction);
#endif
        }
        d.borderColor = to_metal_border(st.border);
        d.normalizedCoordinates = YES;
        d.supportArgumentBuffers = YES;
        return [c->device newSamplerStateWithDescriptor:d];
    }
}

/* nvnSamplerBuilderSetDefaults(NVNsamplerBuilder*) */
void sampler_builder_set_defaults(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    uint64_t device = sampler_spec_or_default(x[0]).device;
    (void)guest_write(x[0], 0x60);
    SamplerSpec& spec = sampler_spec(x[0]);
    std::memset(&spec.state, 0, sizeof spec.state);
    nvngfx_sampler_defaults(&spec.state);
    spec.device = device;   /* SetDefaults preserves the builder's device */
    spec.lodSnap = 0.0f;
}

void sampler_builder_set_device(nvngfx_ctx*, uint64_t* x) {
    if (x[0]) sampler_spec(x[0]).device = x[1];
}
void sampler_builder_set_minmag(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    if (nvngfx_sampler_set_minmag(&sampler_spec(x[0]).state,
                                  static_cast<unsigned>(x[1]),
                                  static_cast<unsigned>(x[2])) != 0) {
        log_once("nvnSamplerBuilderSetMinMagFilter", static_cast<unsigned>(x[1]));
    }
}
void sampler_builder_set_wrap(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    if (nvngfx_sampler_set_wrap(&sampler_spec(x[0]).state,
                                static_cast<unsigned>(x[1]),
                                static_cast<unsigned>(x[2]),
                                static_cast<unsigned>(x[3])) != 0) {
        log_once("nvnSamplerBuilderSetWrapMode", static_cast<unsigned>(x[1]));
    }
}
void sampler_builder_set_lod_clamp(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    nvngfx_args* a = sampler_args(x);
    nvngfx_sampler_set_lod_clamp(&sampler_spec(x[0]).state,
                                 nvngfx_arg_f32(a, 0), nvngfx_arg_f32(a, 1));
}
void sampler_builder_set_lod_bias(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    nvngfx_sampler_set_lod_bias(&sampler_spec(x[0]).state,
                                nvngfx_arg_f32(sampler_args(x), 0));
}
void sampler_builder_set_compare(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    if (nvngfx_sampler_set_compare(&sampler_spec(x[0]).state,
                                   static_cast<unsigned>(x[1]),
                                   static_cast<unsigned>(x[2])) != 0) {
        log_once("nvnSamplerBuilderSetCompare", static_cast<unsigned>(x[1]));
    }
}
void sampler_builder_set_border(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    const float* rgba = guest_ptr<const float>(x[1]);
    if (!rgba) {
        log_once("nvnSamplerBuilderSetBorderColor", 0);
        return;
    }
    nvngfx_sampler_set_border(&sampler_spec(x[0]).state, rgba);
}
void sampler_builder_set_border_i(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    const int* c = guest_ptr<const int>(x[1]);
    if (!c) {
        log_once("nvnSamplerBuilderSetBorderColori", 0);
        return;
    }
    const float rgba[4] = {static_cast<float>(c[0]), static_cast<float>(c[1]),
                           static_cast<float>(c[2]), static_cast<float>(c[3])};
    nvngfx_sampler_set_border(&sampler_spec(x[0]).state, rgba);
}
void sampler_builder_set_border_ui(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    const uint32_t* c = guest_ptr<const uint32_t>(x[1]);
    if (!c) {
        log_once("nvnSamplerBuilderSetBorderColorui", 0);
        return;
    }
    const float rgba[4] = {static_cast<float>(c[0]), static_cast<float>(c[1]),
                           static_cast<float>(c[2]), static_cast<float>(c[3])};
    nvngfx_sampler_set_border(&sampler_spec(x[0]).state, rgba);
}
void sampler_builder_set_max_aniso(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    nvngfx_sampler_set_anisotropy(&sampler_spec(x[0]).state,
                                  nvngfx_arg_f32(sampler_args(x), 0));
}
void sampler_builder_set_reduction(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    if (nvngfx_sampler_set_reduction(&sampler_spec(x[0]).state,
                                     static_cast<unsigned>(x[1])) != 0) {
        log_once("nvnSamplerBuilderSetReductionFilter", static_cast<unsigned>(x[1]));
    }
}
void sampler_builder_set_lod_snap(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) return;
    sampler_spec(x[0]).lodSnap = nvngfx_arg_f32(sampler_args(x), 0);
}

void sampler_builder_get_minmag(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler& s = sampler_spec_or_default(x[0]).state;
    uint32_t* mi = guest_ptr<uint32_t>(x[1]);
    uint32_t* ma = guest_ptr<uint32_t>(x[2]);
    if (mi) *mi = s.nvn_min_filter;
    if (ma) *ma = s.nvn_mag_filter;
}
void sampler_builder_get_wrap(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler& s = sampler_spec_or_default(x[0]).state;
    uint32_t* ws = guest_ptr<uint32_t>(x[1]);
    uint32_t* wt = guest_ptr<uint32_t>(x[2]);
    uint32_t* wr = guest_ptr<uint32_t>(x[3]);
    if (ws) *ws = s.nvn_wrap_s;
    if (wt) *wt = s.nvn_wrap_t;
    if (wr) *wr = s.nvn_wrap_r;
}
void sampler_builder_get_lod_clamp(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler& s = sampler_spec_or_default(x[0]).state;
    float* lo = guest_ptr<float>(x[1]);
    float* hi = guest_ptr<float>(x[2]);
    if (lo) *lo = s.lod_min;
    if (hi) *hi = s.lod_max;
}
void sampler_builder_get_lod_bias(nvngfx_ctx*, uint64_t* x) {
    nvngfx_arg_set_f32(sampler_args(x), 0, sampler_spec_or_default(x[0]).state.lod_bias);
}
void sampler_builder_get_compare(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler& s = sampler_spec_or_default(x[0]).state;
    uint32_t* mode = guest_ptr<uint32_t>(x[1]);
    uint32_t* func = guest_ptr<uint32_t>(x[2]);
    if (mode) *mode = s.nvn_compare_mode;
    if (func) *func = s.nvn_compare_func;
}
void sampler_builder_get_border(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler& s = sampler_spec_or_default(x[0]).state;
    float* c = guest_ptr<float>(x[1]);
    if (c) std::memcpy(c, s.border, sizeof s.border);
}
void sampler_builder_get_border_i(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler& s = sampler_spec_or_default(x[0]).state;
    int* c = guest_ptr<int>(x[1]);
    if (c) for (int i = 0; i < 4; ++i) c[i] = static_cast<int>(s.border[i]);
}
void sampler_builder_get_border_ui(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler& s = sampler_spec_or_default(x[0]).state;
    uint32_t* c = guest_ptr<uint32_t>(x[1]);
    if (c) for (int i = 0; i < 4; ++i) c[i] = static_cast<uint32_t>(s.border[i]);
}
void sampler_builder_get_max_aniso(nvngfx_ctx*, uint64_t* x) {
    nvngfx_arg_set_f32(sampler_args(x), 0, sampler_spec_or_default(x[0]).state.max_aniso);
}
void sampler_builder_get_reduction(nvngfx_ctx*, uint64_t* x) {
    x[0] = sampler_spec_or_default(x[0]).state.nvn_reduction;
}
void sampler_builder_get_lod_snap(nvngfx_ctx*, uint64_t* x) {
    nvngfx_arg_set_f32(sampler_args(x), 0, sampler_spec_or_default(x[0]).lodSnap);
}

/* nvnSamplerInitialize(NVNsampler*, const NVNsamplerBuilder*) -> NVNboolean */
void sampler_initialize(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    if (!c || !c->ready || !x[0]) {
        x[0] = 0;
        return;
    }
    const SamplerSpec spec = sampler_spec_or_default(x[1]);
    if (spec.state.approximate) {
        log_once("nvnSamplerInitialize.approximate", spec.state.approximate);
    }
    Sampler* s = find_or_add<Sampler>(x[0]);
    s->state = spec.state;
    s->device = spec.device;
    s->mtl = create_sampler_state(c, s->state);
    x[0] = s->mtl ? 1u : 0u;
}

void sampler_finalize(nvngfx_ctx*, uint64_t* x) { forget<Sampler>(x[0]); }

void sampler_set_debug_label(nvngfx_ctx*, uint64_t* x) {
    Sampler* s = find<Sampler>(x[0]);
    if (!s) return;
    const char* label = guest_ptr<const char>(x[1]);
    s->label = label ? label : "";
}

const nvngfx_sampler* sampler_object_state(uint64_t guest) {
    Sampler* s = find<Sampler>(guest);
    return s ? &s->state : nullptr;
}

void sampler_get_minmag(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* s = sampler_object_state(x[0]);
    if (!s) return;
    uint32_t* mi = guest_ptr<uint32_t>(x[1]);
    uint32_t* ma = guest_ptr<uint32_t>(x[2]);
    if (mi) *mi = s->nvn_min_filter;
    if (ma) *ma = s->nvn_mag_filter;
}
void sampler_get_wrap(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* s = sampler_object_state(x[0]);
    if (!s) return;
    uint32_t* ws = guest_ptr<uint32_t>(x[1]);
    uint32_t* wt = guest_ptr<uint32_t>(x[2]);
    uint32_t* wr = guest_ptr<uint32_t>(x[3]);
    if (ws) *ws = s->nvn_wrap_s;
    if (wt) *wt = s->nvn_wrap_t;
    if (wr) *wr = s->nvn_wrap_r;
}
void sampler_get_lod_clamp(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* s = sampler_object_state(x[0]);
    if (!s) return;
    float* lo = guest_ptr<float>(x[1]);
    float* hi = guest_ptr<float>(x[2]);
    if (lo) *lo = s->lod_min;
    if (hi) *hi = s->lod_max;
}
void sampler_get_lod_bias(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* s = sampler_object_state(x[0]);
    nvngfx_arg_set_f32(sampler_args(x), 0, s ? s->lod_bias : 0.0f);
}
void sampler_get_compare(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* s = sampler_object_state(x[0]);
    if (!s) return;
    uint32_t* mode = guest_ptr<uint32_t>(x[1]);
    uint32_t* func = guest_ptr<uint32_t>(x[2]);
    if (mode) *mode = s->nvn_compare_mode;
    if (func) *func = s->nvn_compare_func;
}
void sampler_get_border(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* s = sampler_object_state(x[0]);
    if (!s) return;
    float* c = guest_ptr<float>(x[1]);
    if (c) std::memcpy(c, s->border, sizeof s->border);
}
void sampler_get_border_i(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* s = sampler_object_state(x[0]);
    if (!s) return;
    int* c = guest_ptr<int>(x[1]);
    if (c) for (int i = 0; i < 4; ++i) c[i] = static_cast<int>(s->border[i]);
}
void sampler_get_border_ui(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* s = sampler_object_state(x[0]);
    if (!s) return;
    uint32_t* c = guest_ptr<uint32_t>(x[1]);
    if (c) for (int i = 0; i < 4; ++i) c[i] = static_cast<uint32_t>(s->border[i]);
}
void sampler_get_max_aniso(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* s = sampler_object_state(x[0]);
    nvngfx_arg_set_f32(sampler_args(x), 0, s ? s->max_aniso : 0.0f);
}
void sampler_get_reduction(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* s = sampler_object_state(x[0]);
    x[0] = s ? s->nvn_reduction : 0;
}
/* nvnSamplerCompare(const NVNsampler*, const NVNsampler*) -> NVNboolean */
void sampler_compare(nvngfx_ctx*, uint64_t* x) {
    const nvngfx_sampler* a = sampler_object_state(x[0]);
    const nvngfx_sampler* b = sampler_object_state(x[1]);
    x[0] = (a && b && nvngfx_sampler_equals(a, b)) ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/* descriptor pools                                                    */
/* ------------------------------------------------------------------ */

/* nvnTexturePoolInitialize(NVNtexturePool*, const NVNmemoryPool*,
 *                          ptrdiff_t offset, int entries) -> NVNboolean */
void tex_pool_initialize(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) { x[0] = 0; return; }
    int base = 0;
    if (!nvngfx_pool_map_add(&g_tex_pool_map, x[0], static_cast<int>(x[3]), &base)) {
        log_once("nvnTexturePoolInitialize.poolLimit", static_cast<unsigned>(x[0]));
        x[0] = 0;
        return;
    }
    TexturePool* p = find_or_add<TexturePool>(x[0]);
    p->memory = x[1];
    p->offset = static_cast<long long>(x[2]);
    p->count = static_cast<int>(x[3]);
    p->base = base;
    x[0] = 1;
}

void tex_pool_finalize(nvngfx_ctx*, uint64_t* x) { forget<TexturePool>(x[0]); }

void tex_pool_get_memory_offset(nvngfx_ctx*, uint64_t* x) {
    const TexturePool* p = find<TexturePool>(x[0]);
    x[0] = p ? static_cast<uint64_t>(p->offset) : 0;
}
void tex_pool_get_size(nvngfx_ctx*, uint64_t* x) {
    const TexturePool* p = find<TexturePool>(x[0]);
    x[0] = p ? static_cast<uint64_t>(p->count) : 0;
}
void tex_pool_set_debug_label(nvngfx_ctx*, uint64_t* x) {
    TexturePool* p = find<TexturePool>(x[0]);
    if (!p) return;
    const char* label = guest_ptr<const char>(x[1]);
    p->label = label ? label : "";
}

/* Registers a texture (or an image, which shares the pool index space) at
 * `id`.  Only the guest address is stored: the MTLTexture belongs to the
 * NVNtexture object and is looked up again when a draw resolves the handle,
 * so finalizing the texture leaves the slot empty instead of dangling. */
void tex_pool_register(nvngfx_ctx*, uint64_t* x, const char* reason) {
    TexturePool* p = find<TexturePool>(x[0]);
    if (!p) { log_once(reason, 0); return; }
    const int id = static_cast<int>(x[1]);
    if (id < 0 || id >= p->count) { log_once(reason, static_cast<unsigned>(id)); return; }
    p->textures[p->base + id] = x[2];
    p->views[p->base + id] = x[3];
}
void tex_pool_register_texture(nvngfx_ctx* ctx, uint64_t* x) {
    tex_pool_register(ctx, x, "nvnTexturePoolRegisterTexture.invalidSlot");
}
void tex_pool_register_image(nvngfx_ctx* ctx, uint64_t* x) {
    tex_pool_register(ctx, x, "nvnTexturePoolRegisterImage.invalidSlot");
}

/* nvnSamplerPoolInitialize / Finalize / GetMemoryOffset / GetSize /
 * SetDebugLabel, the same shape as the texture pool. */
void samp_pool_initialize(nvngfx_ctx*, uint64_t* x) {
    if (!x[0]) { x[0] = 0; return; }
    int base = 0;
    if (!nvngfx_pool_map_add(&g_samp_pool_map, x[0], static_cast<int>(x[3]), &base)) {
        log_once("nvnSamplerPoolInitialize.poolLimit", static_cast<unsigned>(x[0]));
        x[0] = 0;
        return;
    }
    SamplerPool* p = find_or_add<SamplerPool>(x[0]);
    p->memory = x[1];
    p->offset = static_cast<long long>(x[2]);
    p->count = static_cast<int>(x[3]);
    p->base = base;
    x[0] = 1;
}
void samp_pool_finalize(nvngfx_ctx*, uint64_t* x) { forget<SamplerPool>(x[0]); }
void samp_pool_get_memory_offset(nvngfx_ctx*, uint64_t* x) {
    const SamplerPool* p = find<SamplerPool>(x[0]);
    x[0] = p ? static_cast<uint64_t>(p->offset) : 0;
}
void samp_pool_get_size(nvngfx_ctx*, uint64_t* x) {
    const SamplerPool* p = find<SamplerPool>(x[0]);
    x[0] = p ? static_cast<uint64_t>(p->count) : 0;
}
void samp_pool_set_debug_label(nvngfx_ctx*, uint64_t* x) {
    SamplerPool* p = find<SamplerPool>(x[0]);
    if (!p) return;
    const char* label = guest_ptr<const char>(x[1]);
    p->label = label ? label : "";
}

int samp_pool_slot(const SamplerPool* p, uint64_t id_arg) {
    const int id = static_cast<int>(id_arg);
    if (!p || id < 0 || id >= p->count) return -1;
    return p->base + id;
}

/* nvnSamplerPoolRegisterSampler(pool, id, const NVNsampler*) */
void samp_pool_register_sampler(nvngfx_ctx*, uint64_t* x) {
    SamplerPool* p = find<SamplerPool>(x[0]);
    const int slot = samp_pool_slot(p, x[1]);
    Sampler* s = find<Sampler>(x[2]);
    if (!p || slot < 0 || !s) {
        log_once("nvnSamplerPoolRegisterSampler.invalid", static_cast<unsigned>(x[1]));
        return;
    }
    PooledSampler ps;
    ps.state = s->state;
    ps.mtl = s->mtl;
    p->samplers[slot] = ps;
}

/* ...RegisterSamplerBuilder: the guest never built an NVNsampler, so the
 * state is snapshotted from the builder and the Metal object is created
 * here.  Creating it at registration, not at draw, keeps a per-frame
 * draw from allocating a sampler state. */
void samp_pool_register_builder(nvngfx_ctx* ctx, uint64_t* x) {
    SamplerPool* p = find<SamplerPool>(x[0]);
    const int slot = samp_pool_slot(p, x[1]);
    if (!p || slot < 0) {
        log_once("nvnSamplerPoolRegisterSamplerBuilder.invalid", static_cast<unsigned>(x[1]));
        return;
    }
    const SamplerSpec& spec = sampler_spec_or_default(x[2]);
    PooledSampler ps;
    ps.state = spec.state;
    ps.mtl = create_sampler_state(ctx_of(ctx), ps.state);
    if (!ps.mtl) {
        log_once("nvnSamplerPoolRegisterSamplerBuilder.noState", static_cast<unsigned>(x[1]));
        return;
    }
    p->samplers[slot] = ps;
}

/* nvnCommandBufferSetTexturePool / SetSamplerPool.  The argument buffer
 * currently cached was encoded against the old pool, so it is dropped. */
void cmdbuf_set_texture_pool(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    b->texPool = x[1];
    b->argbuf[0] = b->argbuf[1] = nil;
}
void cmdbuf_set_sampler_pool(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    b->sampPool = x[1];
    b->argbuf[0] = b->argbuf[1] = nil;
}

/* nvnDeviceGetTextureHandle(device, tex, samp): tex is the low 20 bits and
 * samp the next 20, exactly as nxnative defines them.  The guest passes pool
 * IDs; the bases are added when a draw resolves the handle. */
void dev_get_texture_handle(nvngfx_ctx*, uint64_t* x) {
    x[0] = nvngfx_texture_handle(static_cast<unsigned>(x[1]),
                                 static_cast<unsigned>(x[2]));
}
void dev_get_texel_fetch_handle(nvngfx_ctx*, uint64_t* x) {
    x[0] = static_cast<uint32_t>(x[1]);
}
void dev_get_separate_texture_handle(nvngfx_ctx*, uint64_t* x) {
    x[0] = static_cast<uint32_t>(x[1]);
}
void dev_get_separate_sampler_handle(nvngfx_ctx*, uint64_t* x) {
    x[0] = nvngfx_separate_sampler_handle(static_cast<unsigned>(x[1]));
}
void dev_get_image_handle(nvngfx_ctx*, uint64_t* x) {
    x[0] = static_cast<uint32_t>(x[1]);
}

/* NVN's NVNshaderStage as 0 / 1, or -1 for a stage this backend does not
 * run (geometry, tessellation, compute).  The two live stages get their own
 * binding tables because a vertex and a pixel shader may bind different
 * resources to the same index. */
int stage_index(uint64_t stage) {
    if (stage == NVNGFX_STAGE_VERTEX) return 0;
    if (stage == NVNGFX_STAGE_FRAGMENT) return 1;
    return -1;
}

/* nvnCommandBufferBindTexture(cb, stage, slot, handle) and the plural form.
 * Vertex and pixel bindings are kept apart; any other stage is reported as
 * ignored instead of silently landing in a table nothing reads. */
void cmdbuf_bind_texture(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    const int si = stage_index(x[1]);
    if (si < 0) { log_once("nvnCommandBufferBindTexture.stage", static_cast<unsigned>(x[1])); return; }
    const unsigned slot = static_cast<unsigned>(x[2]);
    if (slot >= NVNGFX_MAX_TEXTURE_SLOTS) { log_once("nvnCommandBufferBindTexture.slot", slot); return; }
    b->texHandle[si][slot] = x[3];
    b->argbuf[si] = nil;
}
void cmdbuf_bind_textures(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    const int si = stage_index(x[1]);
    if (si < 0) { log_once("nvnCommandBufferBindTextures.stage", static_cast<unsigned>(x[1])); return; }
    const unsigned first = static_cast<unsigned>(x[2]);
    const unsigned n = static_cast<unsigned>(x[3]);
    const uint64_t* handles = guest_ptr<const uint64_t>(x[4]);
    if (!handles) return;
    for (unsigned i = 0; i < n; ++i) {
        const unsigned slot = first + i;
        if (slot >= NVNGFX_MAX_TEXTURE_SLOTS) { log_once("nvnCommandBufferBindTextures.slot", slot); break; }
        b->texHandle[si][slot] = handles[i];
    }
    b->argbuf[si] = nil;
}
void cmdbuf_bind_separate_texture(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    const int si = stage_index(x[1]);
    if (si < 0) { log_once("nvnCommandBufferBindSeparateTexture.stage", static_cast<unsigned>(x[1])); return; }
    const unsigned slot = static_cast<unsigned>(x[2]);
    if (slot >= NVNGFX_MAX_TEXTURE_SLOTS) { log_once("nvnCommandBufferBindSeparateTexture.slot", slot); return; }
    b->sepTexHandle[si][slot] = x[3];
    b->argbuf[si] = nil;
}
void cmdbuf_bind_separate_textures(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    const int si = stage_index(x[1]);
    if (si < 0) { log_once("nvnCommandBufferBindSeparateTextures.stage", static_cast<unsigned>(x[1])); return; }
    const unsigned first = static_cast<unsigned>(x[2]);
    const unsigned n = static_cast<unsigned>(x[3]);
    const uint64_t* handles = guest_ptr<const uint64_t>(x[4]);
    if (!handles) return;
    for (unsigned i = 0; i < n; ++i) {
        const unsigned slot = first + i;
        if (slot >= NVNGFX_MAX_TEXTURE_SLOTS) { log_once("nvnCommandBufferBindSeparateTextures.slot", slot); break; }
        b->sepTexHandle[si][slot] = handles[i];
    }
    b->argbuf[si] = nil;
}
void cmdbuf_bind_separate_sampler(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    const int si = stage_index(x[1]);
    if (si < 0) { log_once("nvnCommandBufferBindSeparateSampler.stage", static_cast<unsigned>(x[1])); return; }
    const unsigned slot = static_cast<unsigned>(x[2]);
    if (slot >= NVNGFX_MAX_SAMPLER_SLOTS) { log_once("nvnCommandBufferBindSeparateSampler.slot", slot); return; }
    b->sepSampHandle[si][slot] = x[3];
    b->argbuf[si] = nil;
}
void cmdbuf_bind_separate_samplers(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    const int si = stage_index(x[1]);
    if (si < 0) { log_once("nvnCommandBufferBindSeparateSamplers.stage", static_cast<unsigned>(x[1])); return; }
    const unsigned first = static_cast<unsigned>(x[2]);
    const unsigned n = static_cast<unsigned>(x[3]);
    const uint64_t* handles = guest_ptr<const uint64_t>(x[4]);
    if (!handles) return;
    for (unsigned i = 0; i < n; ++i) {
        const unsigned slot = first + i;
        if (slot >= NVNGFX_MAX_SAMPLER_SLOTS) { log_once("nvnCommandBufferBindSeparateSamplers.slot", slot); break; }
        b->sepSampHandle[si][slot] = handles[i];
    }
    b->argbuf[si] = nil;
}

/* Handle -> host resource.  The pool index in the handle is the guest ID;
 * the bound pool's base turns it into the host slot the registration used.
 * A finalised texture or a slot that was never registered resolves to null,
 * and the draw is dropped rather than bound to whatever slot 0 holds. */
const Tex* bound_texture(const CmdBuf* b, uint64_t handle) {
    if (!b || !b->texPool || !handle) return nullptr;
    const int slot = nvngfx_pool_slot(&g_tex_pool_map, b->texPool,
                                      static_cast<int>(nvngfx_texture_handle_tex(handle)));
    if (slot < 0) return nullptr;
    const TexturePool* p = find<TexturePool>(b->texPool);
    if (!p) return nullptr;
    auto it = p->textures.find(slot);
    if (it == p->textures.end() || !it->second) return nullptr;
    return find<Tex>(it->second);
}

const PooledSampler* bound_sampler(const CmdBuf* b, uint64_t handle) {
    if (!b || !b->sampPool || !handle) return nullptr;
    const int slot = nvngfx_pool_slot(&g_samp_pool_map, b->sampPool,
                                      static_cast<int>(nvngfx_texture_handle_samp(handle)));
    if (slot < 0) return nullptr;
    const SamplerPool* p = find<SamplerPool>(b->sampPool);
    if (!p) return nullptr;
    auto it = p->samplers.find(slot);
    return it == p->samplers.end() ? nullptr : &it->second;
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

    /* Argument-buffer layout: each stage's [[id(k)]] numbering, copied out
     * of the offline blob when one is installed.  A real Breath of the Wild
     * fragment shader needs 42 entries, so this is NVNGFX_MAX_ARGS and not
     * the 8 the hand-written self-test started with.  numArgs == 0 keeps the
     * M3.81 behaviour: uniform buffer i is id(i). */
    nvngfx_shader_arg args[NVNGFX_MAX_ARGS] = {};
    unsigned numArgs = 0;
    nvngfx_shader_arg vargs[NVNGFX_MAX_ARGS] = {};
    unsigned vNumArgs = 0;
    /* The shader's embedded c1 segment, copied out of the blob.  A c1-only
     * fragment shader has no command-buffer binding at all, so this also
     * decides whether buffer(0) must be bound when has_arguments() is false. */
    std::vector<unsigned char> c1;
    bool hasC1 = false;
    id<MTLBuffer> c1Buffer = nil;
    std::vector<unsigned char> vc1;
    bool vHasC1 = false;
    id<MTLBuffer> vc1Buffer = nil;
    /* Whether the stage's function declares an argument buffer at
     * buffer(0).  Constant-colour programs do not, and binding the table to
     * those would feed them the wrong bytes. */
    bool hasArgBuffer = false;
    bool vHasArgBuffer = false;
    /* A translated shader can also name its resources directly
     * (`texture2d<T> tex [[texture(0)]]`).  That path does not need a
     * Metal 3 resource ID, so it is the fallback for devices whose
     * gpuResourceID is zero (notably the x86_64 iOS Simulator). */
    bool directBindings = false;
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
    p->hasArgBuffer = false;
    p->vHasArgBuffer = false;
    p->directBindings = false;
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
        if (a.stream >= NVNGFX_VERTEX_STREAM_MAX) {
            /* NVNGFX_VERTEX_STREAM_BASE + stream has to stay inside Metal's
             * 0..30 argument table. */
            log_once("nvnPipeline.vertexStreamIndex", a.stream);
            if (why) *why = "vertex stream index does not fit Metal's table";
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
        vd.attributes[i].bufferIndex = NVNGFX_VERTEX_STREAM_BASE + a.stream;
        used[a.stream] = true;
    }
    for (unsigned st = 0; st < NVNGFX_VERTEX_STREAM_MAX; ++st) {
        if (!used[st]) continue;
        if (b->stream[st].stride == 0) {
            log_once("nvnPipeline.vertexStride", st);
            if (why) *why = "vertex stream stride is zero";
            return nil;
        }
        const unsigned idx = NVNGFX_VERTEX_STREAM_BASE + st;
        vd.layouts[idx].stride = b->stream[st].stride;
        if (b->stream[st].divisor) {
            vd.layouts[idx].stepFunction = MTLVertexStepFunctionPerInstance;
            vd.layouts[idx].stepRate = b->stream[st].divisor;
        } else {
            vd.layouts[idx].stepFunction = MTLVertexStepFunctionPerVertex;
            vd.layouts[idx].stepRate = 1;
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

        /* The shader text is the only dependable signal here: Apple's
         * simulator runtime can return a non-nil argument encoder whose
         * encodedLength is still zero.  The actual table is therefore
         * written with the fixed 8-byte-entry ABI below, matching the
         * working d2r-mobile backend. */
        p->hasArgBuffer = std::strstr(fs, "[[buffer(0)]]") != nullptr;
        p->vHasArgBuffer = std::strstr(vs, "[[buffer(0)]]") != nullptr;
        p->directBindings =
            !p->hasArgBuffer &&
            (std::strstr(fs, "[[texture(") != nullptr ||
             std::strstr(fs, "[[sampler(") != nullptr);

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
        if (a.stream < NVNGFX_VERTEX_STREAM_MAX) used[a.stream] = true;
        else log_once("nvnCommandBufferDraw.vertexStreamIndex", a.stream);
    }
    for (unsigned st = 0; st < NVNGFX_VERTEX_STREAM_MAX; ++st) {
        if (!used[st]) continue;
        id<MTLBuffer> buf = nil;
        size_t off = 0;
        uint64_t len = b->vbSize[st];
        if (len == 0 && b->stream[st].stride) len = b->stream[st].stride;
        if (!pool_span(b->vb[st], static_cast<size_t>(len ? len : 1u), &buf, &off)) {
            log_once("nvnCommandBufferDraw.vertexBuffer", st);
            return false;
        }
        [b->enc setVertexBuffer:buf offset:off
                         atIndex:NVNGFX_VERTEX_STREAM_BASE + st];
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

/* Copies a blob's layout into the program.  pso_drop() must not clear it (the
 * layout belongs to the shaders, not to a pipeline), but SetShaders does,
 * because new shaders may describe a different one. */
void prog_apply_arg_layout(Program* p, const nvngfx_shader_blob* blob,
                           nvngfx_stage stage) {
    if (!p) return;
    const bool isFs = stage == NVNGFX_STAGE_FRAGMENT;
    nvngfx_shader_arg* const args = isFs ? p->args : p->vargs;
    unsigned& numArgs = isFs ? p->numArgs : p->vNumArgs;
    std::vector<unsigned char>& c1 = isFs ? p->c1 : p->vc1;
    bool& hasC1 = isFs ? p->hasC1 : p->vHasC1;
    __strong id<MTLBuffer>* const c1Buffer =
        isFs ? &p->c1Buffer : &p->vc1Buffer;
    numArgs = nvngfx_shader_copy_args(blob, args, NVNGFX_MAX_ARGS);
    c1.clear();
    hasC1 = false;
    for (unsigned i = 0; i < numArgs; ++i)
        if (args[i].kind == NVNGFX_ARG_C1) hasC1 = true;
    if (blob && blob->c1 && blob->c1_size)
        c1.assign(blob->c1, blob->c1 + blob->c1_size);
    /* New shaders (or a re-applied sidecar) invalidate the old copy.  A draw
     * that still needs c1 recreates it from the Program's own bytes. */
    *c1Buffer = nil;
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
    /* Each stage's [[id(k)]] layout travels with its MSL in the same blob,
     * and each stage gets its own copy here.  The vertex layout used to be
     * ignored because buffer(0) was the first vertex stream; M3.87 moves the
     * streams to NVNGFX_VERTEX_STREAM_BASE, so buffer(0) is the vertex
     * argument buffer, exactly as the translator emits it. */
    p->numArgs = 0;
    p->c1.clear();
    p->hasC1 = false;
    p->c1Buffer = nil;
    p->vNumArgs = 0;
    p->vc1.clear();
    p->vHasC1 = false;
    p->vc1Buffer = nil;
    p->hasArgBuffer = false;
    p->vHasArgBuffer = false;
    p->directBindings = false;
    for (unsigned st = 0; st < 2; ++st) {
        if (!p->key[st][0]) continue;
        const nvngfx_shader_blob* sb = nvngfx_shader_lookup_blob(
            p->key[st], static_cast<nvngfx_stage>(st), nullptr);
        if (sb) prog_apply_arg_layout(p, sb, static_cast<nvngfx_stage>(st));
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
        if (b->enc && b->hasViewport) [b->enc setViewport:b->viewport];
        if (b->enc && b->hasScissor) [b->enc setScissorRect:b->scissor];
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
    b->encoded = 0;
    b->numColor = 0;
    b->depth = 0;
    b->pendingClear = false;
    b->argbuf[0] = b->argbuf[1] = nil;
    b->hasViewport = false;
    b->hasScissor = false;
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
    /* A texture that has been rendered into holds this backend's pixels, not
     * the guest's, so the upload path must leave it alone from now on. */
    for (unsigned i = 0; i < b->numColor; ++i) {
        if (Tex* t = find<Tex>(b->color[i])) t->rendered = true;
    }
    if (Tex* dt = find<Tex>(b->depth)) dt->rendered = true;
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

/* NVNshaderStage from nxnative's nvn.h: VERTEX=0, FRAGMENT=1, GEOMETRY=2,
 * TESS_CONTROL=3, TESS_EVALUATION=4, COMPUTE=5. */

/* nvnCommandBufferBindUniformBuffer(NVNcommandBuffer*, NVNshaderStage,
 *                                  int index, NVNbufferAddress, size_t) */
void cmdbuf_bind_uniform_buffer(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    const unsigned stage = static_cast<unsigned>(x[1]);
    const int index = static_cast<int>(x[2]);
    if (stage_index(x[1]) < 0) {
        log_once("nvnCommandBufferBindUniformBuffer.stage", stage);
        return;
    }
    if (index < 0 || index >= NVNGFX_MAX_UBO_SLOTS) {
        log_once("nvnCommandBufferBindUniformBuffer.index",
                 static_cast<unsigned>(index));
        return;
    }
    const int si = stage_index(x[1]);
    b->ubo[si][index] = x[3];
    b->uboSize[si][index] = x[4];
    b->argbuf[si] = nil;    /* contents changed */
}

/* nvnCommandBufferBindUniformBuffers(NVNcommandBuffer*, NVNshaderStage,
 *     int first, int count, const NVNbufferRange*) */
void cmdbuf_bind_uniform_buffers(nvngfx_ctx* ctx, uint64_t* x) {
    const int n = static_cast<int>(x[3]);
    const uint64_t* rg = guest_ptr<const uint64_t>(x[4]);
    if (n < 0 || n > NVNGFX_MAX_UBO_SLOTS || (n > 0 && !rg)) {
        log_once("nvnCommandBufferBindUniformBuffers.count",
                 static_cast<unsigned>(n));
        return;
    }
    for (int k = 0; k < n; ++k) {
        uint64_t y[8] = {};
        y[0] = x[0];
        y[1] = x[1];
        y[2] = static_cast<uint64_t>(static_cast<int>(x[2]) + k);
        y[3] = rg[2 * k];
        y[4] = rg[2 * k + 1];
        cmdbuf_bind_uniform_buffer(ctx, y);
    }
}

/* True when this stage of the recording has anything that lives in its
 * argument buffer: constants, a combined texture (which carries its
 * sampler), or a separate texture/sampler.  `si` is 0 for vertex, 1 for
 * fragment. */
bool has_arguments(const CmdBuf* b, int si) {
    for (unsigned i = 0; i < NVNGFX_MAX_UBO_SLOTS; ++i)
        if (b->ubo[si][i]) return true;
    for (unsigned i = 0; i < NVNGFX_MAX_TEXTURE_SLOTS; ++i)
        if (b->texHandle[si][i] || b->sepTexHandle[si][i]) return true;
    for (unsigned i = 0; i < NVNGFX_MAX_SAMPLER_SLOTS; ++i)
        if (b->sepSampHandle[si][i]) return true;
    return false;
}

/* Reads one 32-bit bindless handle from a bound uniform buffer.
 *
 * The guest address is wrapped by a Pool's MTLBuffer, so reading its contents
 * is the same memory the GPU would read.  A missing binding, a range outside
 * the declared buffer, or memory outside a pool is a hard failure: filling the
 * argument word with zero would silently sample a dummy handle. */
bool read_ubo_u32(const CmdBuf* b, int si, unsigned slot, unsigned offset,
                  uint32_t* out) {
    if (!out || !b || si < 0 || si > 1 || slot >= NVNGFX_MAX_UBO_SLOTS ||
        !b->ubo[si][slot])
        return false;
    const uint64_t size = b->uboSize[si][slot];
    if (size == 0 || static_cast<uint64_t>(offset) + 4u > size) return false;
    id<MTLBuffer> src = nil;
    size_t base = 0;
    if (!pool_span(b->ubo[si][slot], static_cast<size_t>(size), &src, &base))
        return false;
    if (![src contents]) return false;
    std::memcpy(out, static_cast<const unsigned char*>([src contents]) + base + offset, 4);
    return true;
}

/* Copies a texture's guest bytes into its MTLTexture, once.
 *
 * The guest fills texture storage inside a memory pool -- decompressed mip
 * chains included -- while the MTLTexture created at nvnTextureInitialize is
 * host allocated and starts empty.  Without this, a sampled texture reads
 * zeros however correct the binding is.
 *
 * One blit per level and slice, from a shared staging buffer that
 * nvngfx_tex_deswizzle() filled out of the pool's MTLBuffer (the pool wraps
 * the guest's own bytes, so this is a copy of the same memory the GPU would
 * have read).  A texture this path cannot handle (multisampled, LINEAR,
 * BUFFER, unknown format, storage outside the pool, unsupported layer count)
 * is reported once and left as it was; a render target is never uploaded at
 * all, because this backend is its only writer.
 *
 * The blits go into the draw's own command buffer before its render pass is
 * opened.  End an earlier pass first if one is still open: allocating a second
 * command buffer and waiting for it from inside an encoder deadlocks the
 * simulator, and a blit must not be encoded while a render encoder is active.
 * The caller reopens the pass, with viewport/scissor restored by
 * ensure_encoder().
 *
 * Returns false only when the caller must not sample the texture. */
bool tex_upload(Ctx* c, CmdBuf* b, Tex* t) {
    if (!b || !b->cb) return false;
    if (!t || !t->mtl) return false;
    if (t->uploaded || t->uploadTried) return t->uploaded;
    t->uploadTried = true;
    if (t->rendered) {
        t->uploaded = true;
        return true;
    }
    if (!t->desc.pool || t->desc.offset < 0) {
        /* Host-only texture (the self-test's, or one the guest never gave
         * storage).  Nothing to copy; the host already owns the pixels. */
        t->uploaded = true;
        return true;
    }
    Pool* p = find<Pool>(t->desc.pool);
    if (!p || !p->buffer || !p->storage) {
        log_once("nvnTextureUpload.pool", static_cast<unsigned>(t->desc.pool));
        return false;
    }
    nvngfx_texlayout L;
    if (nvngfx_tex_layout(&t->desc, &L) != 0) {
        log_once("nvnTextureUpload.layout", static_cast<unsigned>(t->desc.format));
        return false;
    }
    const unsigned long long off = static_cast<unsigned long long>(t->desc.offset);
    if (off > p->size || L.size > p->size - off) {
        log_once("nvnTextureUpload.range", static_cast<unsigned>(L.size));
        return false;
    }
    if (![p->buffer contents]) return false;
    const unsigned char* const base =
        static_cast<const unsigned char*>([p->buffer contents]) + off;

    const unsigned levels = t->desc.levels ? static_cast<unsigned>(t->desc.levels) : 1u;
    /* The largest level decides the staging buffer. */
    size_t staging = 0;
    for (unsigned l = 0; l < levels; ++l) {
        unsigned rb = 0, rows = 0, slices = 0;
        if (nvngfx_tex_level_geometry(&t->desc, l, 0, &rb, &rows, &slices,
                                      nullptr, nullptr, nullptr) != 0) {
            log_once("nvnTextureUpload.geometry", l);
            break;
        }
        const size_t need = static_cast<size_t>(rb) * rows * slices;
        if (need > staging) staging = need;
    }
    if (staging == 0) return false;
    std::fprintf(stderr,
                 "[nvn] texture upload: pool=0x%llx offset=%lld levels=%u "
                 "staging=%zu bytes\n",
                 static_cast<unsigned long long>(t->desc.pool),
                 t->desc.offset, levels, staging);
    std::fflush(stderr);
    id<MTLBuffer> stg = [c->device newBufferWithLength:staging
                                               options:MTLResourceStorageModeShared];
    if (!stg) {
        log_once("nvnTextureUpload.staging", 1);
        return false;
    }
    if (b->enc) {
        end_encoder(b);
        /* useResource: is encoder state, so a table built for the previous
         * pass must be rebuilt for the pass that follows this upload. */
        b->argbuf[0] = b->argbuf[1] = nil;
    }
    id<MTLBlitCommandEncoder> blit = [b->cb blitCommandEncoder];
    if (!blit) {
        log_once("nvnTextureUpload.encoder", 1);
        return false;
    }

    unsigned done = 0;
    const unsigned layers = L.layers ? L.layers : 1u;
    for (unsigned l = 0; l < levels; ++l) {
        unsigned rb = 0, rows = 0, slices = 0;
        if (nvngfx_tex_level_geometry(&t->desc, l, 0, &rb, &rows, &slices,
                                      nullptr, nullptr, nullptr) != 0) {
            log_once("nvnTextureUpload.geometry", l);
            break;
        }
        /* A compressed level's last block may cover fewer texels than its
         * storage does; the copy size stays the level's real size, while the
         * source row is allowed to carry the whole block. */
        const unsigned bw = L.block_width ? L.block_width : 1u;
        const unsigned bh = L.block_height ? L.block_height : 1u;
        const unsigned texelsWide = rb / (L.bytes_per_block ? L.bytes_per_block : 1u);
        const unsigned lw = (t->width >> l) ? (t->width >> l) : 1u;
        const unsigned lh = (t->height >> l) ? (t->height >> l) : 1u;
        if (texelsWide * bw < lw || rows * bh < lh) {
            log_once("nvnTextureUpload.blockEdge", l);
            break;
        }
        bool levelComplete = true;
        for (unsigned layer = 0; layer < layers; ++layer) {
            if (nvngfx_tex_deswizzle(&t->desc, l, layer, base, [stg contents],
                                     rb) != 0) {
                log_once("nvnTextureUpload.deswizzle", l);
                levelComplete = false;
                break;
            }
            [blit copyFromBuffer:stg
                    sourceOffset:0
               sourceBytesPerRow:rb
             sourceBytesPerImage:static_cast<NSUInteger>(rb) * rows
                      sourceSize:MTLSizeMake(lw, lh, slices)
                       toTexture:t->mtl
                destinationSlice:layer
                destinationLevel:l
               destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
        if (!levelComplete) break;
        ++done;
    }
    [blit endEncoding];
    t->uploadedLevels = done;
    std::fprintf(stderr, "[nvn] texture upload encoded: %u level(s)\n", done);
    std::fflush(stderr);
    /* Partial copies are not a usable texture: sampling a missing mip would
     * return whatever Metal left there.  Keep the draw dropped instead. */
    t->uploaded = done == levels;
    return t->uploaded;
}

/* Resolves a bound texture that prepare_textures() has already filled.  The
 * argument-buffer path runs with a render encoder open, so it must not try to
 * start an upload there; a missing upload is a dropped draw, never a zero
 * texture. */
Tex* texture_for_sampling(Ctx*, const CmdBuf* b, uint64_t handle) {
    Tex* t = const_cast<Tex*>(bound_texture(b, handle));
    if (!t || !t->mtl) return nullptr;
    if (!t->uploaded) {
        log_once("nvnCommandBufferDraw.textureNotPrepared", 1);
        return nullptr;
    }
    return t;
}

/* Fills every texture this draw can sample, before any render encoder opens.
 * Bound slots cover the direct and argument-buffer paths; bindless handles are
 * read from their uniform buffers here for the same reason argument_buffer()
 * reads them later.  A resolution/upload failure is left for the binding path
 * to report only if the shader actually uses that resource. */
bool prepare_textures(Ctx* c, CmdBuf* b) {
    if (!b || !b->cb) return false;
    auto upload = [c, b](uint64_t handle) {
        Tex* t = const_cast<Tex*>(bound_texture(b, handle));
        if (t && t->mtl) (void)tex_upload(c, b, t);
    };
    for (int si = 0; si < 2; ++si) {
        for (unsigned slot = 0; slot < NVNGFX_MAX_TEXTURE_SLOTS; ++slot) {
            const uint64_t h = b->texHandle[si][slot]
                                   ? b->texHandle[si][slot]
                                   : b->sepTexHandle[si][slot];
            if (h) upload(h);
        }
    }
    Program* p = find<Program>(b->program);
    if (!p) return true;
    for (int si = 0; si < 2; ++si) {
        const nvngfx_shader_arg* args = si == 0 ? p->vargs : p->args;
        const unsigned n = si == 0 ? p->vNumArgs : p->numArgs;
        for (unsigned i = 0; i < n; ++i) {
            const nvngfx_shader_arg& e = args[i];
            if (e.kind != NVNGFX_ARG_TEXTURE_IN_UBO) continue;
            uint32_t handle = 0;
            if (!read_ubo_u32(b, si, e.slot, e.offset, &handle)) continue;
            const unsigned tid = handle & NVNGFX_POOL_INDEX_MASK;
            if (tid)
                upload(nvngfx_texture_handle(tid, handle >> 20));
        }
    }
    return true;
}

/* Builds one stage's argument buffer for this recording.
 *
 * Metal packs an argument buffer by member declaration order, not by
 * [[id(k)]].  Program::args / vargs therefore carry `packed`
 * (0..numArgs-1), and each member is one 64-bit word at byte 8*packed: a
 * buffer writes its gpuAddress + offset, a texture or sampler writes
 * gpuResourceID.  This fixed ABI is what d2r-mobile's translated MSL
 * expects, and it is also the only option where MTLArgumentEncoder is
 * unusable (the iOS Simulator reports encodedLength == 0).
 *
 * Vertex and pixel stages have separate layouts and separate tables: a
 * shader translation puts both at buffer(0) of its own stage, and NVN lets
 * the two stages bind different resources to the same index.
 *
 * A bindless texture/sampler reads its 32-bit NVN handle from the bound
 * uniform buffer first: low 20 bits name the texture, high 12 the sampler.
 * A missing UBO, unreadable handle, or unregistered pool ID drops the draw.
 *
 * Returns 1 when *out holds the table (fresh or cached), 0 when this stage
 * declares no argument buffer at all, and -1 when a bound resource cannot be
 * resolved -- the caller then drops the draw instead of encoding zeros. */
int argument_buffer(Ctx* c, CmdBuf* b, nvngfx_stage stage, id<MTLBuffer>* out) {
    const int si = stage_index(static_cast<uint64_t>(stage));
    if (si < 0 || !out) return -1;
    *out = nil;
    if (b->argbuf[si]) {
        *out = b->argbuf[si];
        return 1;
    }
    Program* p = find<Program>(b->program);
    if (!p) return 0;
    const bool isFs = si == 1;
    const nvngfx_shader_arg* const layout = isFs ? p->args : p->vargs;
    const unsigned numLayout = isFs ? p->numArgs : p->vNumArgs;
    const bool declaresTable = isFs ? p->hasArgBuffer : p->vHasArgBuffer;
    const bool hasC1 = isFs ? p->hasC1 : p->vHasC1;
    std::vector<unsigned char>& c1 = isFs ? p->c1 : p->vc1;
    __strong id<MTLBuffer>* const c1Buffer =
        isFs ? &p->c1Buffer : &p->vc1Buffer;
    const MTLRenderStages stageFlag =
        isFs ? MTLRenderStageFragment : MTLRenderStageVertex;

    /* c1 is not a command-buffer binding: it belongs to the shader.  A
     * c1-only shader therefore needs buffer(0) even though has_arguments()
     * sees no UBO, texture or sampler on the command buffer. */
    if (!has_arguments(b, si) && !hasC1) return 0;
    if (!declaresTable) {
        /* The sidecar names resources but the MSL has no table to put them
         * in: the two halves of the blob disagree, so nothing is bindable. */
        log_once("nvnCommandBufferDraw.argumentLayout", p ? 1u : 0u);
        return -1;
    }

    nvngfx_shader_arg legacy[NVNGFX_MAX_ARGS] = {};
    const nvngfx_shader_arg* entries = nullptr;
    unsigned numEntries = 0;
    if (numLayout == 0) {
        /* No sidecar: only uniform buffers have an id.  A bound texture
         * cannot be placed, so drop the draw instead of sampling slot 0. */
        for (unsigned i = 0; i < NVNGFX_MAX_TEXTURE_SLOTS; ++i) {
            if (b->texHandle[si][i] || b->sepTexHandle[si][i] ||
                b->sepSampHandle[si][i]) {
                log_once("nvnCommandBufferDraw.textureLayout", i);
                return -1;
            }
        }
        for (unsigned i = 0; i < NVNGFX_MAX_UBO_SLOTS; ++i) {
            if (!b->ubo[si][i]) continue;
            legacy[numEntries].id = static_cast<unsigned char>(i);
            legacy[numEntries].kind = NVNGFX_ARG_UBO;
            legacy[numEntries].slot = static_cast<signed char>(i);
            legacy[numEntries].packed = static_cast<unsigned char>(numEntries);
            ++numEntries;
        }
        entries = legacy;
    } else {
        entries = layout;
        numEntries = numLayout;
    }
    if (numEntries == 0) return 0;

    const size_t bytes = static_cast<size_t>(numEntries) * sizeof(uint64_t);
    id<MTLBuffer> ab = [c->device newBufferWithLength:bytes
                                              options:MTLResourceStorageModeShared];
    if (!ab) {
        log_once("nvnCommandBufferDraw.argumentBuffer", 1);
        return -1;
    }
    uint64_t* slots = static_cast<uint64_t*>([ab contents]);
    std::memset(slots, 0, bytes);

    for (unsigned k = 0; k < numEntries; ++k) {
        const nvngfx_shader_arg& e = entries[k];
        const unsigned slot = static_cast<unsigned>(e.slot);
        switch (e.kind) {
        case NVNGFX_ARG_UBO: {
            /* The layout lists what the shader reads, so an unbound slot is a
             * missing binding, not an optional one: a zero pointer here would
             * fault on the GPU or read the wrong memory. */
            if (!b->ubo[si][slot]) {
                log_once("nvnCommandBufferDraw.missingUniformBuffer", slot);
                return -1;
            }
            id<MTLBuffer> src = nil;
            size_t off = 0;
            const size_t len = static_cast<size_t>(
                b->uboSize[si][slot] ? b->uboSize[si][slot] : 1);
            if (!pool_span(b->ubo[si][slot], len, &src, &off)) {
                log_once("nvnCommandBufferDraw.uniformBuffer", slot);
                return -1;
            }
            slots[e.packed] = static_cast<uint64_t>([src gpuAddress]) + off;
            [b->enc useResource:src usage:MTLResourceUsageRead stages:stageFlag];
        } break;
        case NVNGFX_ARG_TEXTURE:
        case NVNGFX_ARG_SEPARATE_TEXTURE: {
            const uint64_t h = e.kind == NVNGFX_ARG_TEXTURE
                                   ? b->texHandle[si][slot]
                                   : b->sepTexHandle[si][slot];
            const Tex* t = texture_for_sampling(c, b, h);
            if (!t || !t->mtl) {
                log_once("nvnCommandBufferDraw.texture", slot);
                return -1;
            }
            slots[e.packed] = static_cast<uint64_t>(t->mtl.gpuResourceID._impl);
            [b->enc useResource:t->mtl usage:MTLResourceUsageRead stages:stageFlag];
        } break;
        case NVNGFX_ARG_SAMPLER_OF_TEXTURE:
        case NVNGFX_ARG_SEPARATE_SAMPLER: {
            const uint64_t h = e.kind == NVNGFX_ARG_SAMPLER_OF_TEXTURE
                                   ? b->texHandle[si][slot]
                                   : b->sepSampHandle[si][slot];
            const PooledSampler* smp = bound_sampler(b, h);
            if (!smp || !smp->mtl) {
                log_once("nvnCommandBufferDraw.sampler", slot);
                return -1;
            }
            slots[e.packed] = static_cast<uint64_t>(smp->mtl.gpuResourceID._impl);
        } break;
        case NVNGFX_ARG_C1: {
            if (!hasC1) {
                log_once("nvnCommandBufferDraw.missingC1", e.id);
                return -1;
            }
            if (!*c1Buffer) {
                /* Match the shared-bank layout nxnative uses: data rounded
                 * up to 256 bytes, then a 256-byte zero gap.  An explicit
                 * empty c1 still gets that zero span, never a zero-length
                 * MTLBuffer (which Metal may reject). */
                const size_t data = c1.size();
                const size_t need = ((data + 255u) & ~size_t(255u)) + 256u;
                *c1Buffer = [c->device newBufferWithLength:need
                                                   options:MTLResourceStorageModeShared];
                if (!*c1Buffer) {
                    log_once("nvnCommandBufferDraw.c1Buffer", 1);
                    return -1;
                }
                std::memset([*c1Buffer contents], 0, need);
                if (data) std::memcpy([*c1Buffer contents], c1.data(), data);
            }
            slots[e.packed] = static_cast<uint64_t>([*c1Buffer gpuAddress]);
            [b->enc useResource:*c1Buffer usage:MTLResourceUsageRead stages:stageFlag];
        } break;
        case NVNGFX_ARG_TEXTURE_IN_UBO:
        case NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE:
        case NVNGFX_ARG_SAMPLER_IN_UBO: {
            uint32_t handle = 0;
            if (!read_ubo_u32(b, si, slot, e.offset, &handle)) {
                log_once("nvnCommandBufferDraw.bindlessHandle", e.packed);
                return -1;
            }
            if (e.kind == NVNGFX_ARG_TEXTURE_IN_UBO) {
                const unsigned tid = handle & NVNGFX_POOL_INDEX_MASK;
                const Tex* t =
                    tid ? texture_for_sampling(
                              c, b, nvngfx_texture_handle(tid, handle >> 20))
                        : nullptr;
                if (!t || !t->mtl) {
                    log_once("nvnCommandBufferDraw.bindlessTexture", e.packed);
                    return -1;
                }
                slots[e.packed] = static_cast<uint64_t>(t->mtl.gpuResourceID._impl);
                [b->enc useResource:t->mtl usage:MTLResourceUsageRead
                              stages:stageFlag];
            } else {
                const unsigned sid = handle >> NVNGFX_POOL_INDEX_BITS;
                const PooledSampler* smp = sid
                    ? bound_sampler(b, nvngfx_separate_sampler_handle(sid))
                    : nullptr;
                if (!smp || !smp->mtl) {
                    log_once("nvnCommandBufferDraw.bindlessSampler", e.packed);
                    return -1;
                }
                slots[e.packed] = static_cast<uint64_t>(smp->mtl.gpuResourceID._impl);
            }
        } break;
        default:
            /* The driver tables, an image, a typed buffer or a type-3
             * constant sampler: this layer cannot fill that word, and Metal
             * has no way to report an unfinished argument buffer. */
            log_once("nvnCommandBufferDraw.argUnsupported",
                     static_cast<unsigned>(e.id));
            return -1;
        }
    }
    b->argbuf[si] = ab;
    *out = ab;
    return 1;
}

/* Direct texture/sampler bindings for shaders that name resources with
 * [[texture(k)]] / [[sampler(k)]].  This is the non-Metal3 fallback: it
 * never needs gpuResourceID, so the simulator can still exercise the pool,
 * handle, upload and sampler-state layers. */
bool bind_direct_arguments(Ctx* c, CmdBuf* b) {
    bool bound = false;
    const int si = 1;   /* the direct path is the pixel stage's fallback */
    for (unsigned i = 0; i < NVNGFX_MAX_TEXTURE_SLOTS; ++i) {
        if (i < NVNGFX_MAX_UBO_SLOTS && b->ubo[si][i]) {
            id<MTLBuffer> src = nil;
            size_t off = 0;
            const size_t len =
                static_cast<size_t>(b->uboSize[si][i] ? b->uboSize[si][i] : 1);
            if (!pool_span(b->ubo[si][i], len, &src, &off)) {
                log_once("nvnCommandBufferDraw.directUniformBuffer", i);
                return false;
            }
            [b->enc setFragmentBuffer:src offset:off atIndex:i];
            bound = true;
        }
        const uint64_t texHandle = b->texHandle[si][i]
                                       ? b->texHandle[si][i]
                                       : b->sepTexHandle[si][i];
        if (texHandle) {
            const Tex* t = texture_for_sampling(c, b, texHandle);
            if (!t || !t->mtl) {
                log_once("nvnCommandBufferDraw.directTexture", i);
                return false;
            }
            [b->enc setFragmentTexture:t->mtl atIndex:i];
            bound = true;
        }
        const uint64_t sampHandle = b->texHandle[si][i]
                                        ? b->texHandle[si][i]
                                        : b->sepSampHandle[si][i];
        if (sampHandle) {
            const PooledSampler* s = bound_sampler(b, sampHandle);
            if (!s || !s->mtl) {
                log_once("nvnCommandBufferDraw.directSampler", i);
                return false;
            }
            [b->enc setFragmentSamplerState:s->mtl atIndex:i];
            bound = true;
        }
    }
    return bound;
}

/* Binds both stages' resources before a draw.  The vertex table goes to
 * buffer(0) of the vertex stage and the pixel table to buffer(0) of the
 * fragment stage -- the same indices a spirv-cross translation gives
 * `spvDescriptorSetBuffer0`.  Vertex streams live at
 * NVNGFX_VERTEX_STREAM_BASE + stream instead, so they do not collide with
 * the vertex table.
 *
 * True when there is nothing to bind or the binding succeeded; false means
 * the draw must not be encoded, because a shader that reads constants or
 * samples a texture would read the wrong thing. */
bool bind_arguments(Ctx* c, CmdBuf* b) {
    Program* p = find<Program>(b->program);
    id<MTLBuffer> ab = nil;
    const int vr = argument_buffer(c, b, NVNGFX_STAGE_VERTEX, &ab);
    if (vr < 0) return false;
    if (vr > 0) [b->enc setVertexBuffer:ab offset:0 atIndex:0];

    if (p && p->directBindings) return bind_direct_arguments(c, b);
    const int fr = argument_buffer(c, b, NVNGFX_STAGE_FRAGMENT, &ab);
    if (fr < 0) return false;
    if (fr > 0) [b->enc setFragmentBuffer:ab offset:0 atIndex:0];
    return true;
}

/* nvnCommandBufferDrawArrays(NVNcommandBuffer*, NVNdrawPrimitive, int first, int count) */
void cmdbuf_draw_arrays(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    b->draws++;
    if (!ensure_pipeline(c, b)) {
        /* The honest line: there is no translated shader, so there is no
         * pipeline, so this draw is dropped.  Once per process. */
        log_once("nvnCommandBufferDrawArrays.noPipeline", 1);
        return;
    }
    if (!prepare_textures(c, b)) return;
    /* Opening the pass is what makes a recorded ClearColor happen. */
    if (!ensure_encoder(c, b)) return;
    const uint32_t primitive = static_cast<uint32_t>(x[1]);
    const unsigned first = static_cast<unsigned>(x[2]);
    const unsigned count = static_cast<unsigned>(x[3]);
    [b->enc setRenderPipelineState:b->pso];
    if (!bind_arguments(c, b)) return;
    /* No attribute state bound (the self-test's [[vertex_id]] program) means
     * no vertex descriptor and nothing to bind. */
    if (b->numAttrib > 0 && !bind_vertex_streams(b)) return;
    [b->enc drawPrimitives:to_metal_primitive(primitive)
               vertexStart:first
               vertexCount:count];
    b->encoded++;
}

/* nvnCommandBufferDrawElements(NVNcommandBuffer*, NVNdrawPrimitive,
 *                              NVNindexType, int count, NVNbufferAddress)
 *
 * NVNindexType from nxnative's nvn.h: UNSIGNED_BYTE=0, UNSIGNED_SHORT=1,
 * UNSIGNED_INT=2.  That is a reimplementation's header, not Nintendo's SDK.
 * UNSIGNED_BYTE is not wired yet; this backend accepts only 1/2 and drops an
 * unknown type with a log line instead of guessing. */
void cmdbuf_draw_elements(nvngfx_ctx* ctx, uint64_t* x) {
    Ctx* c = ctx_of(ctx);
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    b->draws++;
    if (!ensure_pipeline(c, b)) {
        log_once("nvnCommandBufferDrawElements.noPipeline", 1);
        return;
    }
    if (!prepare_textures(c, b)) return;
    if (!ensure_encoder(c, b)) return;
    const uint32_t primitive = static_cast<uint32_t>(x[1]);
    const uint32_t indexType = static_cast<uint32_t>(x[2]);
    const unsigned count = static_cast<unsigned>(x[3]);
    const unsigned indexSize = indexType == 1 ? 2u : indexType == 2 ? 4u : 0u;
    if (indexSize == 0) {
        log_once("nvnCommandBufferDrawElements.indexType", indexType);
        return;
    }
    id<MTLBuffer> ibuf = nil;
    size_t ioff = 0;
    if (!pool_span(x[4], static_cast<size_t>(count) * indexSize, &ibuf, &ioff)) {
        log_once("nvnCommandBufferDrawElements.indexBuffer", indexType);
        return;
    }
    [b->enc setRenderPipelineState:b->pso];
    if (!bind_arguments(c, b)) return;
    if (b->numAttrib > 0 && !bind_vertex_streams(b)) return;
    [b->enc drawIndexedPrimitives:to_metal_primitive(primitive)
                       indexCount:count
                        indexType:indexSize == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                      indexBuffer:ibuf
                indexBufferOffset:ioff];
    b->encoded++;
}

/* nvnCommandBufferSetViewport(NVNcommandBuffer*, int x, int y, int w, int h) */
void cmdbuf_set_viewport(nvngfx_ctx* ctx, uint64_t* x) {
    (void)ctx;
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    b->viewport = {static_cast<double>(static_cast<int>(x[1])),
                   static_cast<double>(static_cast<int>(x[2])),
                   static_cast<double>(static_cast<int>(x[3])),
                   static_cast<double>(static_cast<int>(x[4])), 0.0, 1.0};
    b->hasViewport = true;
    if (b->enc) [b->enc setViewport:b->viewport];
}

/* nvnCommandBufferSetScissor(NVNcommandBuffer*, int x, int y, int w, int h) */
void cmdbuf_set_scissor(nvngfx_ctx* ctx, uint64_t* x) {
    (void)ctx;
    CmdBuf* b = find<CmdBuf>(x[0]);
    if (!b) return;
    b->scissor = {static_cast<NSUInteger>(static_cast<int>(x[1])),
                  static_cast<NSUInteger>(static_cast<int>(x[2])),
                  static_cast<NSUInteger>(static_cast<int>(x[3])),
                  static_cast<NSUInteger>(static_cast<int>(x[4]))};
    b->hasScissor = true;
    if (b->enc) [b->enc setScissorRect:b->scissor];
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
    BIND("nvnDeviceGetTextureHandle", dev_get_texture_handle),
    BIND("nvnDeviceGetTexelFetchHandle", dev_get_texel_fetch_handle),
    BIND("nvnDeviceGetSeparateTextureHandle", dev_get_separate_texture_handle),
    BIND("nvnDeviceGetSeparateSamplerHandle", dev_get_separate_sampler_handle),
    BIND("nvnDeviceGetImageHandle", dev_get_image_handle),

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

    BIND("nvnTexturePoolInitialize", tex_pool_initialize),
    BIND("nvnTexturePoolFinalize", tex_pool_finalize),
    BIND("nvnTexturePoolGetMemoryOffset", tex_pool_get_memory_offset),
    BIND("nvnTexturePoolGetSize", tex_pool_get_size),
    BIND("nvnTexturePoolSetDebugLabel", tex_pool_set_debug_label),
    BIND("nvnTexturePoolRegisterTexture", tex_pool_register_texture),
    BIND("nvnTexturePoolRegisterImage", tex_pool_register_image),

    BIND("nvnSamplerBuilderSetDefaults", sampler_builder_set_defaults),
    BIND("nvnSamplerBuilderSetDevice", sampler_builder_set_device),
    BIND("nvnSamplerBuilderSetMinMagFilter", sampler_builder_set_minmag),
    BIND("nvnSamplerBuilderSetWrapMode", sampler_builder_set_wrap),
    BIND("nvnSamplerBuilderSetLodClamp", sampler_builder_set_lod_clamp),
    BIND("nvnSamplerBuilderSetLodBias", sampler_builder_set_lod_bias),
    BIND("nvnSamplerBuilderSetCompare", sampler_builder_set_compare),
    BIND("nvnSamplerBuilderSetBorderColor", sampler_builder_set_border),
    BIND("nvnSamplerBuilderSetBorderColori", sampler_builder_set_border_i),
    BIND("nvnSamplerBuilderSetBorderColorui", sampler_builder_set_border_ui),
    BIND("nvnSamplerBuilderSetMaxAnisotropy", sampler_builder_set_max_aniso),
    BIND("nvnSamplerBuilderSetReductionFilter", sampler_builder_set_reduction),
    BIND("nvnSamplerBuilderSetLodSnap", sampler_builder_set_lod_snap),
    BIND("nvnSamplerBuilderGetMinMagFilter", sampler_builder_get_minmag),
    BIND("nvnSamplerBuilderGetWrapMode", sampler_builder_get_wrap),
    BIND("nvnSamplerBuilderGetLodClamp", sampler_builder_get_lod_clamp),
    BIND("nvnSamplerBuilderGetLodBias", sampler_builder_get_lod_bias),
    BIND("nvnSamplerBuilderGetCompare", sampler_builder_get_compare),
    BIND("nvnSamplerBuilderGetBorderColor", sampler_builder_get_border),
    BIND("nvnSamplerBuilderGetBorderColori", sampler_builder_get_border_i),
    BIND("nvnSamplerBuilderGetBorderColorui", sampler_builder_get_border_ui),
    BIND("nvnSamplerBuilderGetMaxAnisotropy", sampler_builder_get_max_aniso),
    BIND("nvnSamplerBuilderGetReductionFilter", sampler_builder_get_reduction),
    BIND("nvnSamplerBuilderGetLodSnap", sampler_builder_get_lod_snap),
    BIND("nvnSamplerInitialize", sampler_initialize),
    BIND("nvnSamplerFinalize", sampler_finalize),
    BIND("nvnSamplerSetDebugLabel", sampler_set_debug_label),
    BIND("nvnSamplerGetMinMagFilter", sampler_get_minmag),
    BIND("nvnSamplerGetWrapMode", sampler_get_wrap),
    BIND("nvnSamplerGetLodClamp", sampler_get_lod_clamp),
    BIND("nvnSamplerGetLodBias", sampler_get_lod_bias),
    BIND("nvnSamplerGetCompare", sampler_get_compare),
    BIND("nvnSamplerGetBorderColor", sampler_get_border),
    BIND("nvnSamplerGetBorderColori", sampler_get_border_i),
    BIND("nvnSamplerGetBorderColorui", sampler_get_border_ui),
    BIND("nvnSamplerGetMaxAnisotropy", sampler_get_max_aniso),
    BIND("nvnSamplerGetReductionFilter", sampler_get_reduction),
    BIND("nvnSamplerCompare", sampler_compare),

    BIND("nvnSamplerPoolInitialize", samp_pool_initialize),
    BIND("nvnSamplerPoolFinalize", samp_pool_finalize),
    BIND("nvnSamplerPoolGetMemoryOffset", samp_pool_get_memory_offset),
    BIND("nvnSamplerPoolGetSize", samp_pool_get_size),
    BIND("nvnSamplerPoolSetDebugLabel", samp_pool_set_debug_label),
    BIND("nvnSamplerPoolRegisterSampler", samp_pool_register_sampler),
    BIND("nvnSamplerPoolRegisterSamplerBuilder", samp_pool_register_builder),

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
    BIND("nvnCommandBufferBindUniformBuffer", cmdbuf_bind_uniform_buffer),
    BIND("nvnCommandBufferBindUniformBuffers", cmdbuf_bind_uniform_buffers),
    BIND("nvnCommandBufferSetTexturePool", cmdbuf_set_texture_pool),
    BIND("nvnCommandBufferSetSamplerPool", cmdbuf_set_sampler_pool),
    BIND("nvnCommandBufferBindTexture", cmdbuf_bind_texture),
    BIND("nvnCommandBufferBindTextures", cmdbuf_bind_textures),
    BIND("nvnCommandBufferBindSeparateTexture", cmdbuf_bind_separate_texture),
    BIND("nvnCommandBufferBindSeparateTextures", cmdbuf_bind_separate_textures),
    BIND("nvnCommandBufferBindSeparateSampler", cmdbuf_bind_separate_sampler),
    BIND("nvnCommandBufferBindSeparateSamplers", cmdbuf_bind_separate_samplers),
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
bool read_pixel_at(id<MTLDevice> dev, id<MTLCommandQueue> queue,
                   id<MTLTexture> tex, NSUInteger side, NSUInteger x, NSUInteger y,
                   unsigned char px[4]) {
    if (!dev || !queue || !tex || x >= side || y >= side) return false;
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
                             y * bpr + x * 4;
    for (int i = 0; i < 4; ++i) px[i] = p[i];
    return true;
}

bool read_center_pixel(id<MTLDevice> dev, id<MTLCommandQueue> queue,
                       id<MTLTexture> tex, NSUInteger side, unsigned char px[4]) {
    return read_pixel_at(dev, queue, tex, side, side / 2, side / 2, px);
}

/* Uploads RGBA8 rows into a private MTLTexture through a shared staging
 * buffer.  The self-test uses this because the guest-upload path does not
 * exist yet; a real upload copies from the guest pool instead of from `px`,
 * but the blit itself is the same. */
bool upload_rgba8(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLTexture> tex,
                  const unsigned char* px, NSUInteger w, NSUInteger h) {
    if (!dev || !queue || !tex || !px || w == 0 || h == 0) return false;
    const NSUInteger bpr = w * 4;
    id<MTLBuffer> src = [dev newBufferWithLength:bpr * h
                                        options:MTLResourceStorageModeShared];
    if (!src) return false;
    std::memcpy([src contents], px, bpr * h);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:src
            sourceOffset:0
       sourceBytesPerRow:bpr
     sourceBytesPerImage:bpr * h
              sourceSize:MTLSizeMake(w, h, 1)
               toTexture:tex
        destinationSlice:0
        destinationLevel:0
       destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb.status != MTLCommandBufferStatusError;
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
    alignas(16) unsigned char samplerBuilder[0x60] = {};
    alignas(16) unsigned char samplerObject[0x30] = {};
    alignas(16) unsigned char samplerObject2[0x30] = {};
    /* Backing store for the textured draw: a 2x2 source texture, its builder,
     * a texture pool, a sampler pool and a builder-registered sampler. */
    alignas(16) unsigned char texSrcObject[0x80] = {};
    alignas(16) unsigned char texSrcBuilder[0x80] = {};
    alignas(16) unsigned char texPoolObject[0x20] = {};
    alignas(16) unsigned char sampPoolObject[0x20] = {};
    alignas(16) unsigned char samplerBuilder5[0x60] = {};
    alignas(16) unsigned char samplerObject5[0x30] = {};
    const uint64_t kTex = reinterpret_cast<uint64_t>(texObject);
    const uint64_t kTexBuilder = reinterpret_cast<uint64_t>(texBuilder);
    const uint64_t kSamplerBuilder = reinterpret_cast<uint64_t>(samplerBuilder);
    const uint64_t kSampler = reinterpret_cast<uint64_t>(samplerObject);
    const uint64_t kSampler2 = reinterpret_cast<uint64_t>(samplerObject2);
    const uint64_t kTexSrc = reinterpret_cast<uint64_t>(texSrcObject);
    const uint64_t kTexSrcBuilder = reinterpret_cast<uint64_t>(texSrcBuilder);
    const uint64_t kTexPool = reinterpret_cast<uint64_t>(texPoolObject);
    const uint64_t kSampPool = reinterpret_cast<uint64_t>(sampPoolObject);
    const uint64_t kSamplerBuilder5 = reinterpret_cast<uint64_t>(samplerBuilder5);
    const uint64_t kSampler5 = reinterpret_cast<uint64_t>(samplerObject5);
    const uint64_t kCb = 0x5200;
    const uint64_t kProg = 0x5300;
    const NSUInteger kSide = 64;
    /* Backing store for the second draw's memory pool; freed at `done`. */
    void* poolRaw = nullptr;

    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            std::fprintf(stderr, "[selftest] no Metal device\n");
            return 1;
        }
        std::fprintf(stderr,
                     "[selftest] metal device: apple7=%d metal3=%d "
                     "unifiedMemory=%d\n",
                     [dev supportsFamily:MTLGPUFamilyApple7] ? 1 : 0,
                     [dev supportsFamily:MTLGPUFamilyMetal3] ? 1 : 0,
                     dev.hasUnifiedMemory ? 1 : 0);
        std::fflush(stderr);

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
            /* --- Sampler state, end to end --------------------------------
             * This is a real MTLSamplerState creation, not just the portable
             * state test: setters, getters, and the object the argument buffer
             * will later reference.  No draw is needed for this check. */
            nvngfx_args a{};
            const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            a.x[0] = kSamplerBuilder;
            sampler_builder_set_defaults(ctx, a.x);
            a.x[0] = kSamplerBuilder; a.x[1] = 4; a.x[2] = 1;
            sampler_builder_set_minmag(ctx, a.x);      /* nearest mip-linear, linear */
            a.x[0] = kSamplerBuilder; a.x[1] = 4; a.x[2] = 5; a.x[3] = 6;
            sampler_builder_set_wrap(ctx, a.x);        /* mirror-border, border, mirror */
            a.x[0] = kSamplerBuilder;
            nvngfx_arg_set_f32(&a, 0, 0.0f);
            nvngfx_arg_set_f32(&a, 1, 8.0f);
            sampler_builder_set_lod_clamp(ctx, a.x);
            nvngfx_arg_set_f32(&a, 0, 0.5f);
            sampler_builder_set_lod_bias(ctx, a.x);
            a.x[0] = kSamplerBuilder; a.x[1] = 1; a.x[2] = 4;
            sampler_builder_set_compare(ctx, a.x);     /* compare R, LEQUAL */
            a.x[0] = kSamplerBuilder; a.x[1] = reinterpret_cast<uint64_t>(white);
            sampler_builder_set_border(ctx, a.x);
            nvngfx_arg_set_f32(&a, 0, 8.0f);
            sampler_builder_set_max_aniso(ctx, a.x);
            a.x[0] = kSamplerBuilder; a.x[1] = 1;
            sampler_builder_set_reduction(ctx, a.x);   /* min */

            uint32_t got_min = 99, got_mag = 99, got_ws = 99, got_wt = 99, got_wr = 99;
            uint32_t got_cmp_mode = 99, got_cmp_func = 99;
            float got_lo = 0.0f, got_hi = 0.0f;
            nvngfx_args g{};
            g.x[0] = kSamplerBuilder;
            g.x[1] = reinterpret_cast<uint64_t>(&got_min);
            g.x[2] = reinterpret_cast<uint64_t>(&got_mag);
            sampler_builder_get_minmag(ctx, g.x);
            g.x[1] = reinterpret_cast<uint64_t>(&got_ws);
            g.x[2] = reinterpret_cast<uint64_t>(&got_wt);
            g.x[3] = reinterpret_cast<uint64_t>(&got_wr);
            sampler_builder_get_wrap(ctx, g.x);
            g.x[1] = reinterpret_cast<uint64_t>(&got_lo);
            g.x[2] = reinterpret_cast<uint64_t>(&got_hi);
            sampler_builder_get_lod_clamp(ctx, g.x);
            g.x[1] = reinterpret_cast<uint64_t>(&got_cmp_mode);
            g.x[2] = reinterpret_cast<uint64_t>(&got_cmp_func);
            sampler_builder_get_compare(ctx, g.x);
            nvngfx_args lb{};
            lb.x[0] = kSamplerBuilder;
            sampler_builder_get_lod_bias(ctx, lb.x);
            nvngfx_args an{};
            an.x[0] = kSamplerBuilder;
            sampler_builder_get_max_aniso(ctx, an.x);
            g.x[0] = kSamplerBuilder;
            sampler_builder_get_reduction(ctx, g.x);
            const uint64_t got_reduction = g.x[0];
            if (got_min != 4 || got_mag != 1 || got_ws != 4 || got_wt != 5 ||
                got_wr != 6 || got_lo != 0.0f || got_hi != 8.0f ||
                got_cmp_mode != 1 || got_cmp_func != 4 || got_reduction != 1 ||
                nvngfx_arg_f32(&lb, 0) != 0.5f ||
                nvngfx_arg_f32(&an, 0) != 8.0f) {
                std::fprintf(stderr, "[selftest] sampler builder getters disagree\n");
                rc = 16384;
                goto done;
            }

            a.x[0] = kSampler; a.x[1] = kSamplerBuilder;
            sampler_initialize(ctx, a.x);
            Sampler* smp = find<Sampler>(kSampler);
            if (a.x[0] != 1 || !smp || !smp->mtl) {
                std::fprintf(stderr, "[selftest] nvnSamplerInitialize failed\n");
                rc = 16384;
                goto done;
            }
            uint32_t obj_min = 99, obj_mag = 99;
            g = nvngfx_args{};
            g.x[0] = kSampler;
            g.x[1] = reinterpret_cast<uint64_t>(&obj_min);
            g.x[2] = reinterpret_cast<uint64_t>(&obj_mag);
            sampler_get_minmag(ctx, g.x);
            nvngfx_args of{};
            of.x[0] = kSampler;
            sampler_get_lod_bias(ctx, of.x);
            bool same = false;
            if (smp) same = nvngfx_sampler_equals(&smp->state,
                                                  &sampler_spec_or_default(kSamplerBuilder).state) != 0;
            if (obj_min != 4 || obj_mag != 1 || nvngfx_arg_f32(&of, 0) != 0.5f || !same) {
                std::fprintf(stderr, "[selftest] sampler object getters disagree\n");
                rc = 16384;
                goto done;
            }
            a.x[0] = kSampler2; a.x[1] = kSamplerBuilder;
            sampler_initialize(ctx, a.x);
            g = nvngfx_args{};
            g.x[0] = kSampler;
            g.x[1] = kSampler2;
            sampler_compare(ctx, g.x);
            if (a.x[0] != 1 || g.x[0] != 1) {
                std::fprintf(stderr, "[selftest] nvnSamplerCompare disagreed\n");
                rc = 16384;
                goto done;
            }
            std::fprintf(stderr, "[selftest] sampler state created, compared and read back\n");
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
            /* The pool wraps one page, and the triangle lives at offset 0.
             * 16 KiB because that is the page size on Apple silicon: Metal
             * returns nil from newBufferWithBytesNoCopy when the pointer or
             * the length is not a page multiple, and 4 KiB is only right on
             * Intel Macs.  Allocated at run time because a static array
             * cannot ask for more than 8 KiB of alignment portably. */
            /* malloc + manual rounding: std::aligned_alloc is not declared by
             * MSVC's C library, and this file also has to type-check there. */
            /* One constant, because the three places that need the size
             * (allocation, memset, builder) must agree.  Using
             * sizeof(vertexPool) here was a bug: vertexPool is a
             * pointer, so the pool was 8 bytes and every span check
             * rejected the 24-byte vertex range. */
            const size_t kPoolSize = 16384;
            poolRaw = std::malloc(kPoolSize + kPoolSize);
            if (!poolRaw) {
                std::fprintf(stderr, "[selftest] could not allocate the pool page\n");
                rc = 256;
                goto done;
            }
            const uintptr_t poolBase = reinterpret_cast<uintptr_t>(poolRaw);
            const uintptr_t poolAligned = (poolBase + 16383u) & ~static_cast<uintptr_t>(16383u);
            unsigned char* const vertexPool = reinterpret_cast<unsigned char*>(poolAligned);
            std::memset(vertexPool, 0, kPoolSize);
            {
                float* v = reinterpret_cast<float*>(vertexPool);
                v[0] = -1.0f; v[1] = -1.0f;
                v[2] = 3.0f;  v[3] = -1.0f;
                v[4] = -1.0f; v[5] = 3.0f;
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
            x[2] = kPoolSize;
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
                /* b2 was scoped to the recording block; the counts still live
                 * on the command buffer, so look it up again. */
                CmdBuf* const b2s = find<CmdBuf>(kCb2);
                const unsigned b2draws = b2s ? b2s->draws : 0u;
                const unsigned b2encoded = b2s ? b2s->encoded : 0u;
                if (!read_center_pixel(dev, c->queue, t->mtl, kSide, px)) {
                    std::fprintf(stderr, "[selftest] draw 2 readback failed\n");
                    rc = 2048;
                    goto done;
                }
                if (!pixels_near(px, want, 2)) {
                    std::fprintf(stderr,
                                 "[selftest] draw 2 centre pixel is (%u,%u,%u,%u), "
                                 "want (%d,%d,%d,%d); draws asked %u, encoded %u\n",
                                 px[0], px[1], px[2], px[3], want[0], want[1], want[2],
                                 want[3], b2draws, b2encoded);
                    rc = 2048;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] draw 2 ([[attribute(0)]] from a pool) produced "
                             "the expected pixel\n");
            }
        }

        {
            /* --- Third draw: the same vertices, indexed -----------------
             * Unity (which is what this title runs on) issues almost every
             * draw as DrawElements, so this path matters more than
             * DrawArrays.  The state bound for draw 2 -- program, attributes,
             * stream, vertex buffer -- is still in the command buffer, which
             * is exactly NVN's rule: state lives on the command buffer, not
             * on the pass. */
            const uint64_t kCb2 = 0x5700;
            const uintptr_t poolBase = reinterpret_cast<uintptr_t>(poolRaw);
            const uintptr_t poolAligned =
                (poolBase + 16383u) & ~static_cast<uintptr_t>(16383u);
            uint16_t* const indices =
                reinterpret_cast<uint16_t*>(poolAligned + 8192);
            indices[0] = 0;
            indices[1] = 1;
            indices[2] = 2;

            const uint64_t target = kTex;
            const float clear3[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            x[0] = kCb2;
            cmdbuf_begin_recording(ctx, x);
            x[0] = kCb2; x[1] = 1; x[2] = reinterpret_cast<uint64_t>(&target);
            x[3] = 0; x[4] = 0; x[5] = 0;
            cmdbuf_set_render_targets(ctx, x);
            x[0] = kCb2; x[1] = 0; x[2] = reinterpret_cast<uint64_t>(clear3);
            x[3] = 0xF;
            cmdbuf_clear_color(ctx, x);
            /* primitive 0 (triangles), index type 1 (16-bit), 3, index address */
            x[0] = kCb2; x[1] = 0; x[2] = 1; x[3] = 3;
            x[4] = reinterpret_cast<uint64_t>(indices);
            cmdbuf_draw_elements(ctx, x);
            x[0] = kCb2;
            cmdbuf_end_recording(ctx, x);

            CmdBuf* b3 = find<CmdBuf>(kCb2);
            if (!b3 || !b3->last) {
                std::fprintf(stderr, "[selftest] draw 3 committed nothing\n");
                rc = 4096;
                goto done;
            }
            [b3->last waitUntilCompleted];
            if (b3->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr, "[selftest] draw 3 command buffer error: %s\n",
                             b3->last.error
                                 ? [[b3->last.error localizedDescription] UTF8String]
                                 : "(no description)");
                rc = 4096;
                goto done;
            }

            unsigned char px[4] = {0, 0, 0, 0};
            const int want[4] = {0, 255, 0, 255};
            if (!read_center_pixel(dev, c->queue, t->mtl, kSide, px) ||
                !pixels_near(px, want, 2)) {
                std::fprintf(stderr,
                             "[selftest] draw 3 centre pixel is (%u,%u,%u,%u), "
                             "want (%d,%d,%d,%d); draws asked %u, encoded %u\n",
                             px[0], px[1], px[2], px[3], want[0], want[1], want[2],
                             want[3], b3->draws, b3->encoded);
                rc = 4096;
                goto done;
            }
            std::fprintf(stderr,
                         "[selftest] draw 3 (DrawElements, 16-bit indices) produced "
                         "the expected pixel\n");
        }

        {
            /* --- Fourth draw: the argument buffer ------------------
             * A spirv-cross shader reads every constant and texture
             * through `constant spvDescriptorSetBuffer0& [[buffer(0)]]`,
             * whose entries are `constant _16* fp_cN [[id(k)]]`.  This
             * draw proves the entry encoding -- an eight-byte GPU
             * address -- and the binding, driven by the guest's own
             * nvnCommandBufferBindUniformBuffer. */
            const uint64_t kProg4 = 0x5900;
            const uint64_t kCb4 = 0x5A00;
            const uintptr_t poolBase4 =
                (reinterpret_cast<uintptr_t>(poolRaw) + 16383u) &
                ~static_cast<uintptr_t>(16383u);
            const uintptr_t blueAt = poolBase4 + 1024;
            {
                float* f = reinterpret_cast<float*>(blueAt);
                f[0] = 0.0f; f[1] = 0.0f; f[2] = 1.0f; f[3] = 1.0f;
            }
            {
                struct { uint64_t data; uint64_t control; } sd4[2];
                sd4[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                    nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 2)));
                sd4[0].control = 0;
                sd4[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                    nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 2)));
                sd4[1].control = 0;
                x[0] = kProg4; x[1] = 0;
                prog_initialize(ctx, x);
                x[0] = kProg4; x[1] = 2; x[2] = reinterpret_cast<uint64_t>(&sd4[0]);
                prog_set_shaders(ctx, x);
                if (x[0] != 1) {
                    std::fprintf(stderr, "[selftest] program 4: SetShaders failed\n");
                    rc = 8192;  /* draw 4: shader setup */
                    goto done;
                }
            }
            {
                const uint64_t target = kTex;
                const float clear4[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                x[0] = kCb4;
                cmdbuf_begin_recording(ctx, x);
                x[0] = kCb4; x[1] = 1; x[2] = reinterpret_cast<uint64_t>(&target);
                x[3] = 0; x[4] = 0; x[5] = 0;
                cmdbuf_set_render_targets(ctx, x);
                x[0] = kCb4; x[1] = 0; x[2] = reinterpret_cast<uint64_t>(clear4);
                x[3] = 0xF;
                cmdbuf_clear_color(ctx, x);
                /* pixel stage (1), slot 0, 16 bytes of constants */
                x[0] = kCb4; x[1] = 1; x[2] = 0;
                x[3] = static_cast<uint64_t>(blueAt); x[4] = 16;
                cmdbuf_bind_uniform_buffer(ctx, x);
                x[0] = kCb4; x[1] = kProg4; x[2] = 0;
                cmdbuf_bind_program(ctx, x);
                x[0] = kCb4; x[1] = 0; x[2] = 0; x[3] = 3;
                cmdbuf_draw_arrays(ctx, x);
                x[0] = kCb4;
                cmdbuf_end_recording(ctx, x);
            }
            CmdBuf* b4 = find<CmdBuf>(kCb4);
            if (!b4 || !b4->last) {
                std::fprintf(stderr, "[selftest] draw 4 committed nothing\n");
                rc = 8193;  /* draw 4: no command buffer */
                goto done;
            }
            if (b4->encoded != 1) {
                Program* p4 = find<Program>(kProg4);
                std::fprintf(stderr,
                             "[selftest] draw 4 not encoded: pso=%d argbuf=%d "
                             "hasArgBuffer=%d numArgs=%u ubo0=0x%llx\n",
                             b4->pso ? 1 : 0, b4->argbuf[1] ? 1 : 0,
                             p4 && p4->hasArgBuffer ? 1 : 0,
                             p4 ? p4->numArgs : 0u,
                             static_cast<unsigned long long>(b4->ubo[1][0]));
            }
            [b4->last waitUntilCompleted];
            if (b4->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr, "[selftest] draw 4 command buffer error: %s\n",
                             b4->last.error
                                 ? [[b4->last.error localizedDescription] UTF8String]
                                 : "(no description)");
                rc = 8194;  /* draw 4: GPU command buffer error */
                goto done;
            }
            {
                unsigned char px[4] = {0, 0, 0, 0};
                const int want[4] = {0, 0, 255, 255};
                if (!read_center_pixel(dev, c->queue, t->mtl, kSide, px) ||
                    !pixels_near(px, want, 2)) {
                    std::fprintf(stderr,
                                 "[selftest] draw 4 centre pixel is (%u,%u,%u,%u), "
                                 "want (%d,%d,%d,%d); draws asked %u, encoded %u\n",
                                 px[0], px[1], px[2], px[3], want[0], want[1],
                                 want[2], want[3], b4->draws, b4->encoded);
                    rc = 8195;  /* draw 4: wrong pixel */
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] draw 4 (argument buffer) produced the "
                             "expected pixel\n");
            }
        }

        {
            /* --- Fifth draw: a texture and a sampler from the pools -------
             * This is the whole binding path a real draw needs, short of a
             * translated shader:
             *   builder -> texture -> pool id 5
             *   builder -> sampler -> pool id 9
             *   SetTexturePool / SetSamplerPool
             *   nvnDeviceGetTextureHandle(5, 9) -> BindTexture
             *   argument buffer: id(0) constants, id(1) texture, id(2) sampler
             * The pixels come from a host blit (upload_rgba8).  Draw 9 below
             * repeats the sampling path with the texels built directly in a
             * guest memory pool, so this draw can stay focused on the pool and
             * sampler-state plumbing. */
            const uint64_t kProg5 = 0x5C00;
            const uint64_t kCb5 = 0x5D00;
            x[0] = kTexSrcBuilder;
            tex_builder_set_defaults(ctx, x);
            x[0] = kTexSrcBuilder; x[1] = 0;
            tex_builder_set_target(ctx, x);
            x[0] = kTexSrcBuilder; x[1] = 0x25;
            tex_builder_set_format(ctx, x);          /* RGBA8_UNORM */
            x[0] = kTexSrcBuilder; x[1] = 2; x[2] = 2;
            tex_builder_set_size2d(ctx, x);
            x[0] = kTexSrcBuilder; x[1] = 1;
            tex_builder_set_levels(ctx, x);
            x[0] = kTexSrcBuilder; x[1] = 1;
            tex_builder_set_samples(ctx, x);
            x[0] = kTexSrc; x[1] = kTexSrcBuilder;
            tex_initialize(ctx, x);
            Tex* src = find<Tex>(kTexSrc);
            if (x[0] != 1 || !src || !src->mtl) {
                std::fprintf(stderr, "[selftest] source texture was not created\n");
                rc = 16385;
                goto done;
            }
            {
                /* Row 0: red, green.  Row 1: blue, white. */
                const unsigned char texels[16] = {
                    255, 0,   0,   255,  0,   255, 0,   255,
                    0,   0,   255, 255,  255, 255, 255, 255};
                if (!upload_rgba8(dev, c->queue, src->mtl, texels, 2, 2)) {
                    std::fprintf(stderr, "[selftest] source texture upload failed\n");
                    rc = 16385;
                    goto done;
                }
                /* The upload and the draw use the same queue, so a readback
                 * here proves the source texels are in the private texture
                 * before the sampler is involved. */
                const struct {
                    NSUInteger x, y;
                    const char* corner;
                } srcChecks[4] = {
                    {0, 0, "top-left"}, {1, 0, "top-right"},
                    {0, 1, "bottom-left"}, {1, 1, "bottom-right"}};
                std::fprintf(stderr, "[selftest] source texture readback:");
                for (int i = 0; i < 4; ++i) {
                    unsigned char px[4] = {0, 0, 0, 0};
                    if (!read_pixel_at(dev, c->queue, src->mtl, 2,
                                       srcChecks[i].x, srcChecks[i].y, px)) {
                        std::fprintf(stderr, " %s=read-failed",
                                     srcChecks[i].corner);
                    } else {
                        std::fprintf(stderr, " %s=(%u,%u,%u,%u)",
                                     srcChecks[i].corner, px[0], px[1], px[2],
                                     px[3]);
                    }
                }
                std::fprintf(stderr, "\n");
                std::fflush(stderr);
            }
            /* Nearest filtering and clamp-to-edge, so each corner pixel of
             * the target reads exactly one texel of the 2x2 source. */
            x[0] = kSamplerBuilder5;
            sampler_builder_set_defaults(ctx, x);
            x[0] = kSamplerBuilder5; x[1] = 0; x[2] = 0;
            sampler_builder_set_minmag(ctx, x);
            x[0] = kSamplerBuilder5; x[1] = 0; x[2] = 0; x[3] = 0;
            sampler_builder_set_wrap(ctx, x);

            /* Two pools of 16 entries each; IDs 5 and 9 are inside them. */
            x[0] = kTexPool; x[1] = 0; x[2] = 0; x[3] = 16;
            tex_pool_initialize(ctx, x);
            x[0] = kTexPool; x[1] = 5; x[2] = kTexSrc; x[3] = 0;
            tex_pool_register_texture(ctx, x);
            x[0] = kSampPool; x[1] = 0; x[2] = 0; x[3] = 16;
            samp_pool_initialize(ctx, x);
            x[0] = kSampPool; x[1] = 9; x[2] = kSamplerBuilder5;
            samp_pool_register_builder(ctx, x);
            /* The other registration path: from an NVNsampler object. */
            x[0] = kSampler5; x[1] = kSamplerBuilder5;
            sampler_initialize(ctx, x);
            x[0] = kSampPool; x[1] = 10; x[2] = kSampler5;
            samp_pool_register_sampler(ctx, x);
            {
                TexturePool* tp = find<TexturePool>(kTexPool);
                SamplerPool* sp = find<SamplerPool>(kSampPool);
                x[0] = kTexPool;
                tex_pool_get_size(ctx, x);
                const uint64_t texSize = x[0];
                x[0] = kSampPool;
                samp_pool_get_memory_offset(ctx, x);
                const uint64_t sampOffset = x[0];
                if (!tp || tp->count != 16 || texSize != 16 || !sp ||
                    sp->count != 16 || sampOffset != 0 ||
                    sp->samplers.find(sp->base + 9) == sp->samplers.end() ||
                    sp->samplers.find(sp->base + 10) == sp->samplers.end()) {
                    std::fprintf(stderr,
                                 "[selftest] descriptor pool registration failed\n");
                    rc = 16385;
                    goto done;
                }
            }

            /* A Metal 3 device gives a texture a non-zero gpuResourceID,
             * which is what an argument-buffer texture entry stores.  The
             * x86_64 simulator reports neither Apple7 nor Metal3 and returns
             * zero here; use direct bindings there instead of sampling a
             * null resource ID. */
            bool samplerHasResourceId = false;
            {
                const SamplerPool* sp5 = find<SamplerPool>(kSampPool);
                if (sp5) {
                    auto it = sp5->samplers.find(sp5->base + 9);
                    if (it != sp5->samplers.end() && it->second.mtl) {
                        samplerHasResourceId =
                            it->second.mtl.gpuResourceID._impl != 0;
                    }
                }
            }
            const bool textureHasResourceId =
                src->mtl.gpuResourceID._impl != 0;
            const bool useDirect = !textureHasResourceId || !samplerHasResourceId;
            std::fprintf(stderr,
                         "[selftest] textured draw path: %s "
                         "(textureResourceId=%d samplerResourceId=%d)\n",
                         useDirect ? "direct bindings" : "argument buffer",
                         textureHasResourceId ? 1 : 0,
                         samplerHasResourceId ? 1 : 0);
            std::fflush(stderr);

            {
                const int variant = useDirect ? 4 : 5;
                struct { uint64_t data; uint64_t control; } sd5[2];
                sd5[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                    nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX,
                                                       variant)));
                sd5[0].control = 0;
                sd5[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                    nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT,
                                                       variant)));
                sd5[1].control = 0;
                x[0] = kProg5; x[1] = 0;
                prog_initialize(ctx, x);
                x[0] = kProg5; x[1] = 2;
                x[2] = reinterpret_cast<uint64_t>(&sd5[0]);
                prog_set_shaders(ctx, x);
                if (x[0] != 1) {
                    std::fprintf(stderr, "[selftest] program 5: SetShaders failed\n");
                    rc = 16387;
                    goto done;
                }
            }
            /* The layout sidecar path, executed on every device: a blob table
             * installed for the moment, looked up by key, then copied into the
             * program.  The ids are 5 / 9 / 12, which is what variant 5
             * declares, so a correct draw on Metal 3 hardware also proves the
             * table's stride and holes -- not just "three words in a row". */
            const nvngfx_shader_arg testArgs[] = {
                {5, NVNGFX_ARG_UBO, 0, 0, 0},
                {9, NVNGFX_ARG_TEXTURE, 0, 1, 0},
                {12, NVNGFX_ARG_SAMPLER_OF_TEXTURE, 0, 2, 0},
            };
            const char* fsText5 = nvngfx_shader_selftest_msl_variant(
                NVNGFX_STAGE_FRAGMENT, useDirect ? 4 : 5);
            const nvngfx_shader_blob testBlob[] = {
                {"00000000000000a5", NVNGFX_STAGE_FRAGMENT, fsText5, testArgs, 3,
                 nullptr, 0},
            };
            size_t savedCount = 0;
            const nvngfx_shader_blob* saved = nvngfx_shader_blob_table(&savedCount);
            if (nvngfx_shader_set_blobs(testBlob, 1) != 0) {
                std::fprintf(stderr, "[selftest] test blob table was refused\n");
                rc = 16389;  /* draw 5: layout sidecar */
                goto done;
            }
            {
                const nvngfx_shader_blob* found = nvngfx_shader_lookup_blob(
                    "00000000000000a5", NVNGFX_STAGE_FRAGMENT, nullptr);
                nvngfx_shader_arg copied[NVNGFX_MAX_ARGS] = {};
                const unsigned n =
                    nvngfx_shader_copy_args(found, copied, NVNGFX_MAX_ARGS);
                if (n != 3 || copied[0].id != 5 || copied[1].id != 9 ||
                    copied[2].id != 12 ||
                    copied[2].kind != NVNGFX_ARG_SAMPLER_OF_TEXTURE ||
                    copied[0].packed != 0 || copied[1].packed != 1 ||
                    copied[2].packed != 2) {
                    std::fprintf(stderr,
                                 "[selftest] blob layout lookup failed (n=%u)\n", n);
                    rc = 16389;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] blob layout: key=%s numArgs=%u "
                             "ids=%u/%u/%u packed=%u/%u/%u\n",
                             found->key, n,
                             static_cast<unsigned>(copied[0].id),
                             static_cast<unsigned>(copied[1].id),
                             static_cast<unsigned>(copied[2].id),
                             static_cast<unsigned>(copied[0].packed),
                             static_cast<unsigned>(copied[1].packed),
                             static_cast<unsigned>(copied[2].packed));
                std::fflush(stderr);
                if (!useDirect) {
                    if (Program* p5 = find<Program>(kProg5))
                        prog_apply_arg_layout(p5, found, NVNGFX_STAGE_FRAGMENT);
                }
            }
            /* Put the real table back before anything else looks a shader up. */
            nvngfx_shader_set_blobs(saved, savedCount);

            const uintptr_t poolBase5 =
                (reinterpret_cast<uintptr_t>(poolRaw) + 16383u) &
                ~static_cast<uintptr_t>(16383u);
            const uintptr_t tintAt = poolBase5 + 2048;
            {
                float* f = reinterpret_cast<float*>(tintAt);
                f[0] = 1.0f; f[1] = 1.0f; f[2] = 1.0f; f[3] = 1.0f;
            }
            {
                const uint64_t target = kTex;
                const float clear5[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                x[0] = kCb5;
                cmdbuf_begin_recording(ctx, x);
                x[0] = kCb5; x[1] = 1;
                x[2] = reinterpret_cast<uint64_t>(&target);
                x[3] = 0; x[4] = 0; x[5] = 0;
                cmdbuf_set_render_targets(ctx, x);
                x[0] = kCb5; x[1] = 0;
                x[2] = reinterpret_cast<uint64_t>(clear5);
                x[3] = 0xF;
                cmdbuf_clear_color(ctx, x);
                x[0] = kCb5; x[1] = kTexPool;
                cmdbuf_set_texture_pool(ctx, x);
                x[0] = kCb5; x[1] = kSampPool;
                cmdbuf_set_sampler_pool(ctx, x);
                x[0] = 0; x[1] = 5; x[2] = 9;
                dev_get_texture_handle(ctx, x);
                const uint64_t handle = x[0];
                x[0] = kCb5; x[1] = 1; x[2] = 0; x[3] = handle;
                cmdbuf_bind_texture(ctx, x);
                x[0] = kCb5; x[1] = 1; x[2] = 0;
                x[3] = static_cast<uint64_t>(tintAt); x[4] = 16;
                cmdbuf_bind_uniform_buffer(ctx, x);
                x[0] = kCb5; x[1] = kProg5; x[2] = 0;
                cmdbuf_bind_program(ctx, x);
                x[0] = kCb5; x[1] = 0; x[2] = 0; x[3] = 3;
                cmdbuf_draw_arrays(ctx, x);
                x[0] = kCb5;
                cmdbuf_end_recording(ctx, x);
            }
            CmdBuf* b5 = find<CmdBuf>(kCb5);
            Program* p5 = find<Program>(kProg5);
            Tex* src5 = find<Tex>(kTexSrc);
            const PooledSampler* ps5 =
                b5 ? bound_sampler(b5, b5->texHandle[1][0]) : nullptr;
            uint64_t words[3] = {0, 0, 0};
            const bool haveTable = b5 && b5->argbuf[1];
            if (haveTable) {
                std::memcpy(words, [b5->argbuf[1] contents], sizeof words);
            }
            std::fprintf(stderr,
                         "[selftest] textured draw state: pso=%d argbuf=%d "
                         "hasArgs=%d encoded=%u draws=%u hasArgBuffer=%d "
                         "direct=%d numArgs=%u ubo0=0x%llx tex0=0x%llx\n",
                         b5 && b5->pso ? 1 : 0, haveTable ? 1 : 0,
                         b5 && has_arguments(b5, 1) ? 1 : 0,
                         b5 ? b5->encoded : 0u, b5 ? b5->draws : 0u,
                         p5 && p5->hasArgBuffer ? 1 : 0,
                         p5 && p5->directBindings ? 1 : 0,
                         p5 ? p5->numArgs : 0u,
                         b5 ? static_cast<unsigned long long>(b5->ubo[1][0]) : 0ull,
                         b5 ? static_cast<unsigned long long>(b5->texHandle[1][0])
                            : 0ull);
            std::fprintf(stderr,
                         "[selftest] textured draw ids: tex=0x%llx samp=0x%llx "
                         "table=%s0x%llx,0x%llx,0x%llx\n",
                         src5 ? static_cast<unsigned long long>(
                                    src5->mtl.gpuResourceID._impl)
                              : 0ull,
                         ps5 ? static_cast<unsigned long long>(
                                   ps5->mtl.gpuResourceID._impl)
                             : 0ull,
                         haveTable ? "" : "(none) ",
                         static_cast<unsigned long long>(words[0]),
                         static_cast<unsigned long long>(words[1]),
                         static_cast<unsigned long long>(words[2]));
            if (p5) {
                std::fprintf(stderr, "[selftest] textured draw args:");
                for (unsigned i = 0; i < p5->numArgs && i < NVNGFX_MAX_ARGS; ++i) {
                    std::fprintf(stderr, " [id=%u kind=%u slot=%d packed=%u off=%u]",
                                 static_cast<unsigned>(p5->args[i].id),
                                 static_cast<unsigned>(p5->args[i].kind),
                                 static_cast<int>(p5->args[i].slot),
                                 static_cast<unsigned>(p5->args[i].packed),
                                 static_cast<unsigned>(p5->args[i].offset));
                }
                std::fprintf(stderr, "\n");
            }
            std::fflush(stderr);
            if (!b5 || !b5->last || b5->encoded != 1) {
                std::fprintf(stderr,
                             "[selftest] textured draw was not encoded (asked %u, "
                             "encoded %u)\n",
                             b5 ? b5->draws : 0u, b5 ? b5->encoded : 0u);
                rc = 16387;
                goto done;
            }
            [b5->last waitUntilCompleted];
            if (b5->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr,
                             "[selftest] textured draw command buffer error: %s\n",
                             b5->last.error
                                 ? [[b5->last.error localizedDescription] UTF8String]
                                 : "(no description)");
                rc = 16387;
                goto done;
            }
            {
                struct {
                    NSUInteger x, y;
                    int want[4];
                    const char* corner;
                } checks[4] = {
                    {1, 1, {255, 0, 0, 255}, "top-left"},
                    {kSide - 2, 1, {0, 255, 0, 255}, "top-right"},
                    {1, kSide - 2, {0, 0, 255, 255}, "bottom-left"},
                    {kSide - 2, kSide - 2, {255, 255, 255, 255}, "bottom-right"}};
                for (int i = 0; i < 4; ++i) {
                    unsigned char px[4] = {0, 0, 0, 0};
                    if (!read_pixel_at(dev, c->queue, t->mtl, kSide, checks[i].x,
                                       checks[i].y, px) ||
                        !pixels_near(px, checks[i].want, 2)) {
                        std::fprintf(stderr,
                                     "[selftest] textured draw %s pixel is "
                                     "(%u,%u,%u,%u), want (%d,%d,%d,%d)\n",
                                     checks[i].corner, px[0], px[1], px[2], px[3],
                                     checks[i].want[0], checks[i].want[1],
                                     checks[i].want[2], checks[i].want[3]);
                        rc = 16388;
                        goto done;
                    }
                }
                std::fprintf(stderr,
                             "[selftest] textured draw sampled all four pool texels\n");
            }
        }

        {
            /* --- Sixth draw: a c1-only fragment shader -------------------
             * The sidecar carries the raw c1 bytes; the MSL names them with
             * one [[id(0)]] pointer.  There is no UBO/texture/sampler binding,
             * so this also proves the backend binds buffer(0) for c1 alone.
             * The local bytes are overwritten after SetShaders: a readback of
             * the original colour proves the Program copied them rather than
             * keeping a pointer to the temporary blob. */
            const uint64_t kProg6 = 0x5E00;
            const uint64_t kCb6 = 0x5F00;
            struct { uint64_t data; uint64_t control; } sd6[2];
            sd6[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 6)));
            sd6[0].control = 0;
            sd6[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 6)));
            sd6[1].control = 0;
            x[0] = kProg6; x[1] = 0;
            prog_initialize(ctx, x);
            x[0] = kProg6; x[1] = 2;
            x[2] = reinterpret_cast<uint64_t>(&sd6[0]);
            prog_set_shaders(ctx, x);
            if (x[0] != 1) {
                std::fprintf(stderr, "[selftest] c1 program: SetShaders failed\n");
                rc = 16390;
                goto done;
            }

            unsigned char c1Bytes[256] = {};
            const float c1Colour[4] = {0.25f, 0.5f, 0.75f, 1.0f};
            std::memcpy(c1Bytes, c1Colour, sizeof c1Colour);
            const nvngfx_shader_arg c1Args[] = {
                {0, NVNGFX_ARG_C1, -1, 0, 0},
            };
            const nvngfx_shader_blob c1Blob[] = {
                {"00000000000000a6", NVNGFX_STAGE_FRAGMENT,
                 nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 6),
                 c1Args, 1, c1Bytes, sizeof c1Bytes},
            };
            size_t c1SavedCount = 0;
            const nvngfx_shader_blob* c1Saved =
                nvngfx_shader_blob_table(&c1SavedCount);
            if (nvngfx_shader_set_blobs(c1Blob, 1) != 0) {
                std::fprintf(stderr, "[selftest] c1 test blob was refused\n");
                rc = 16390;
                goto done;
            }
            {
                const nvngfx_shader_blob* found = nvngfx_shader_lookup_blob(
                    "00000000000000a6", NVNGFX_STAGE_FRAGMENT, nullptr);
                Program* p6 = find<Program>(kProg6);
                if (!found || !p6 || found->c1 != c1Bytes ||
                    found->c1_size != sizeof c1Bytes) {
                    std::fprintf(stderr, "[selftest] c1 blob lookup failed\n");
                    nvngfx_shader_set_blobs(c1Saved, c1SavedCount);
                    rc = 16390;
                    goto done;
                }
                prog_apply_arg_layout(p6, found, NVNGFX_STAGE_FRAGMENT);
                /* The Program must own a copy: poison the source now. */
                std::memset(c1Bytes, 0xAA, sizeof c1Bytes);
                std::fprintf(stderr,
                             "[selftest] c1 sidecar: key=%s bytes=%u id=%u\n",
                             found->key, found->c1_size,
                             static_cast<unsigned>(c1Args[0].id));
            }
            nvngfx_shader_set_blobs(c1Saved, c1SavedCount);

            {
                const uint64_t target = kTex;
                const float clear6[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                x[0] = kCb6;
                cmdbuf_begin_recording(ctx, x);
                x[0] = kCb6; x[1] = 1;
                x[2] = reinterpret_cast<uint64_t>(&target);
                x[3] = 0; x[4] = 0; x[5] = 0;
                cmdbuf_set_render_targets(ctx, x);
                x[0] = kCb6; x[1] = 0;
                x[2] = reinterpret_cast<uint64_t>(clear6);
                x[3] = 0xF;
                cmdbuf_clear_color(ctx, x);
                x[0] = kCb6; x[1] = kProg6; x[2] = 0;
                cmdbuf_bind_program(ctx, x);
                x[0] = kCb6; x[1] = 0; x[2] = 0; x[3] = 3;
                cmdbuf_draw_arrays(ctx, x);
                x[0] = kCb6;
                cmdbuf_end_recording(ctx, x);
            }
            CmdBuf* b6 = find<CmdBuf>(kCb6);
            Program* p6 = find<Program>(kProg6);
            if (!b6 || !b6->last || b6->encoded != 1 || !p6 ||
                !p6->hasC1 || !p6->c1Buffer || p6->c1.size() != sizeof c1Bytes) {
                std::fprintf(stderr,
                             "[selftest] c1 draw not encoded (encoded=%u c1=%d "
                             "buffer=%d bytes=%zu)\n",
                             b6 ? b6->encoded : 0u,
                             p6 && p6->hasC1 ? 1 : 0,
                             p6 && p6->c1Buffer ? 1 : 0,
                             p6 ? p6->c1.size() : 0u);
                rc = 16391;
                goto done;
            }
            std::fprintf(stderr,
                         "[selftest] c1 buffer: gpuAddress=0x%llx bytes=%zu\n",
                         static_cast<unsigned long long>([p6->c1Buffer gpuAddress]),
                         p6->c1.size());
            [b6->last waitUntilCompleted];
            if (b6->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr, "[selftest] c1 draw command buffer error: %s\n",
                             b6->last.error
                                 ? [[b6->last.error localizedDescription] UTF8String]
                                 : "(no description)");
                rc = 16391;
                goto done;
            }
            {
                unsigned char px[4] = {0, 0, 0, 0};
                const int want[4] = {64, 128, 191, 255};
                if (!read_center_pixel(dev, c->queue, t->mtl, kSide, px) ||
                    !pixels_near(px, want, 2)) {
                    std::fprintf(stderr,
                                 "[selftest] c1 draw centre pixel is "
                                 "(%u,%u,%u,%u), want (%d,%d,%d,%d)\n",
                                 px[0], px[1], px[2], px[3],
                                 want[0], want[1], want[2], want[3]);
                    rc = 16392;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] c1 draw read back its copied constants\n");
            }
        }

        {
            /* --- Bindless table probe, not a draw ----------------------
             * The shader names ids 9 / 12 but Metal packs the two members at
             * bytes 0 and 8.  The handle is a 32-bit NVN word in uniform
             * buffer 0 at +0xA8: low 20 bits texture ID 5, high 12 sampler
             * ID 9.  This exercises the UBO read, both pool resolutions and
             * the packed table layout.  It deliberately stops before encoding
             * a draw: an x86_64 simulator texture has gpuResourceID == 0, so a
             * GPU-side sample would test the simulator, not this layer. */
            const uint64_t kProg7 = 0x6000;
            const uint64_t kCb7 = 0x6100;
            struct { uint64_t data; uint64_t control; } sd7[2];
            sd7[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 7)));
            sd7[0].control = 0;
            sd7[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 7)));
            sd7[1].control = 0;
            x[0] = kProg7; x[1] = 0;
            prog_initialize(ctx, x);
            x[0] = kProg7; x[1] = 2;
            x[2] = reinterpret_cast<uint64_t>(&sd7[0]);
            prog_set_shaders(ctx, x);
            if (x[0] != 1) {
                std::fprintf(stderr, "[selftest] bindless program: SetShaders failed\n");
                rc = 16393;
                goto done;
            }

            const nvngfx_shader_arg bindlessArgs[] = {
                {9, NVNGFX_ARG_TEXTURE_IN_UBO, 0, 0, 0xA8},
                {12, NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE, 0, 1, 0xA8},
            };
            const nvngfx_shader_blob bindlessBlob[] = {
                {"00000000000000a7", NVNGFX_STAGE_FRAGMENT,
                 nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 7),
                 bindlessArgs, 2, nullptr, 0},
            };
            size_t saved7Count = 0;
            const nvngfx_shader_blob* saved7 =
                nvngfx_shader_blob_table(&saved7Count);
            if (nvngfx_shader_set_blobs(bindlessBlob, 1) != 0) {
                std::fprintf(stderr, "[selftest] bindless test blob was refused\n");
                rc = 16393;
                goto done;
            }
            {
                const nvngfx_shader_blob* found = nvngfx_shader_lookup_blob(
                    "00000000000000a7", NVNGFX_STAGE_FRAGMENT, nullptr);
                Program* p7 = find<Program>(kProg7);
                if (!found || !p7) {
                    nvngfx_shader_set_blobs(saved7, saved7Count);
                    std::fprintf(stderr, "[selftest] bindless blob lookup failed\n");
                    rc = 16393;
                    goto done;
                }
                prog_apply_arg_layout(p7, found, NVNGFX_STAGE_FRAGMENT);
            }
            nvngfx_shader_set_blobs(saved7, saved7Count);

            const uintptr_t bindlessBase =
                (reinterpret_cast<uintptr_t>(poolRaw) + 16383u) &
                ~static_cast<uintptr_t>(16383u);
            const uint64_t bindlessUbo = bindlessBase + 4096u;
            const uint64_t target7 = kTex;
            {
                unsigned char* p = reinterpret_cast<unsigned char*>(bindlessUbo);
                std::memset(p, 0, 256);
                const uint32_t word =
                    static_cast<uint32_t>(NVNGFX_POOL_INDEX_MASK & 5u) |
                    (9u << NVNGFX_POOL_INDEX_BITS);
                std::memcpy(p + 0xA8, &word, sizeof word);
            }
            x[0] = kCb7;
            cmdbuf_begin_recording(ctx, x);
            x[0] = kCb7; x[1] = 1;
            x[2] = reinterpret_cast<uint64_t>(&target7);
            x[3] = 0; x[4] = 0; x[5] = 0;
            cmdbuf_set_render_targets(ctx, x);
            x[0] = kCb7; x[1] = kTexPool;
            cmdbuf_set_texture_pool(ctx, x);
            x[0] = kCb7; x[1] = kSampPool;
            cmdbuf_set_sampler_pool(ctx, x);
            x[0] = kCb7; x[1] = 1; x[2] = 0;
            x[3] = bindlessUbo; x[4] = 256;
            cmdbuf_bind_uniform_buffer(ctx, x);
            x[0] = kCb7; x[1] = kProg7; x[2] = 0;
            cmdbuf_bind_program(ctx, x);

            CmdBuf* b7 = find<CmdBuf>(kCb7);
            Program* p7 = find<Program>(kProg7);
            if (!b7 || !p7 || !p7->hasArgBuffer || !ensure_encoder(c, b7)) {
                std::fprintf(stderr,
                             "[selftest] bindless setup failed (program=%d argbuf=%d)\n",
                             p7 ? 1 : 0, p7 && p7->hasArgBuffer ? 1 : 0);
                rc = 16394;
                goto done;
            }
            id<MTLBuffer> table7 = nil;
            if (argument_buffer(c, b7, NVNGFX_STAGE_FRAGMENT, &table7) < 0)
                table7 = nil;
            const Tex* tex7 = find<Tex>(kTexSrc);
            const PooledSampler* samp7 = bound_sampler(
                b7, nvngfx_separate_sampler_handle(9));
            uint64_t words7[2] = {0, 0};
            if (table7) std::memcpy(words7, [table7 contents], sizeof words7);
            const uint64_t wantTex =
                tex7 ? static_cast<uint64_t>(tex7->mtl.gpuResourceID._impl) : ~0ull;
            const uint64_t wantSamp =
                samp7 && samp7->mtl
                    ? static_cast<uint64_t>(samp7->mtl.gpuResourceID._impl)
                    : ~0ull;
            if (!table7 || [table7 length] != 16 ||
                words7[0] != wantTex || words7[1] != wantSamp) {
                std::fprintf(stderr,
                             "[selftest] bindless table mismatch: len=%lu "
                             "words=0x%llx/0x%llx want=0x%llx/0x%llx\n",
                             table7 ? static_cast<unsigned long>([table7 length]) : 0ul,
                             static_cast<unsigned long long>(words7[0]),
                             static_cast<unsigned long long>(words7[1]),
                             static_cast<unsigned long long>(wantTex),
                             static_cast<unsigned long long>(wantSamp));
                rc = 16394;
                goto done;
            }
            std::fprintf(stderr,
                         "[selftest] bindless table: packed 0/1 from ids 9/12, "
                         "ubo=+0xA8 tex=0x%llx samp=0x%llx\n",
                         static_cast<unsigned long long>(words7[0]),
                         static_cast<unsigned long long>(words7[1]));
            x[0] = kCb7;
            cmdbuf_end_recording(ctx, x);
            if (!b7->last) {
                std::fprintf(stderr, "[selftest] bindless probe did not commit\n");
                rc = 16394;
                goto done;
            }
            [b7->last waitUntilCompleted];
            if (b7->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr,
                             "[selftest] bindless probe command buffer error: %s\n",
                             b7->last.error
                                 ? [[b7->last.error localizedDescription] UTF8String]
                                 : "(no description)");
                rc = 16394;
                goto done;
            }
        }

        {
            /* --- Draw 8: the vertex stage's argument buffer ------------
             * The vertex shader reads its shift from buffer(0) and its
             * positions through [[stage_in]] from stream 0.  The backend
             * binds the vertex table at buffer(0) and stream 0 at
             * NVNGFX_VERTEX_STREAM_BASE, so neither can be mistaken for the
             * other.  The shift moves the full-screen triangle 1.5 to the
             * left: the left eighth of the target turns magenta and the
             * right eighth keeps the clear colour.  A missing table leaves
             * the triangle centred; a stream bound at index 0 would feed the
             * attribute the argument buffer's own bytes. */
            const uint64_t kProg8 = 0x6200;
            const uint64_t kCb8 = 0x6300;
            struct { uint64_t data; uint64_t control; } sd8[2];
            sd8[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 8)));
            sd8[0].control = 0;
            sd8[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 8)));
            sd8[1].control = 0;
            x[0] = kProg8; x[1] = 0;
            prog_initialize(ctx, x);
            x[0] = kProg8; x[1] = 2;
            x[2] = reinterpret_cast<uint64_t>(&sd8[0]);
            prog_set_shaders(ctx, x);
            if (x[0] != 1) {
                std::fprintf(stderr,
                             "[selftest] vertex-ubo program: SetShaders failed\n");
                rc = 16395;
                goto done;
            }

            /* The pool's aligned base, recomputed here because the earlier
             * block scoped its own pointer. */
            unsigned char* const vpool = reinterpret_cast<unsigned char*>(
                (reinterpret_cast<uintptr_t>(poolRaw) + 16383u) &
                ~static_cast<uintptr_t>(16383u));
            /* A second triangle at +64 and its shift at +3072. */
            {
                float* v = reinterpret_cast<float*>(vpool + 64);
                v[0] = -1.0f; v[1] = -1.0f;
                v[2] = 3.0f;  v[3] = -1.0f;
                v[4] = -1.0f; v[5] = 3.0f;
                float* sh = reinterpret_cast<float*>(vpool + 3072);
                sh[0] = -1.5f; sh[1] = 0.0f; sh[2] = 0.0f; sh[3] = 0.0f;
            }
            /* RG32F attribute 0 at offset 0, fed by stream 0 with stride 8 --
             * the same state draw 2 binds, rebuilt here so this recording
             * does not depend on another command buffer's copy. */
            alignas(16) unsigned char attrib8[8] = {};
            alignas(16) unsigned char stream8[8] = {};
            x[0] = reinterpret_cast<uint64_t>(attrib8);
            vattrib_set_defaults(ctx, x);
            x[0] = reinterpret_cast<uint64_t>(attrib8); x[1] = 0x16; x[2] = 0;
            vattrib_set_format(ctx, x);
            x[0] = reinterpret_cast<uint64_t>(attrib8); x[1] = 0;
            vattrib_set_stream(ctx, x);
            x[0] = reinterpret_cast<uint64_t>(stream8);
            vstream_set_defaults(ctx, x);
            x[0] = reinterpret_cast<uint64_t>(stream8); x[1] = 8;
            vstream_set_stride(ctx, x);

            const uint64_t target8 = kTex;
            const float clear8[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            x[0] = kCb8;
            cmdbuf_begin_recording(ctx, x);
            x[0] = kCb8; x[1] = 1; x[2] = reinterpret_cast<uint64_t>(&target8);
            x[3] = 0; x[4] = 0; x[5] = 0;
            cmdbuf_set_render_targets(ctx, x);
            x[0] = kCb8; x[1] = 0; x[2] = reinterpret_cast<uint64_t>(clear8);
            x[3] = 0xF;
            cmdbuf_clear_color(ctx, x);
            x[0] = kCb8; x[1] = 1;
            x[2] = reinterpret_cast<uint64_t>(&attrib8[0]);
            cmdbuf_bind_vertex_attrib_state(ctx, x);
            x[0] = kCb8; x[1] = 1;
            x[2] = reinterpret_cast<uint64_t>(&stream8[0]);
            cmdbuf_bind_vertex_stream_state(ctx, x);
            x[0] = kCb8; x[1] = 0;
            x[2] = reinterpret_cast<uint64_t>(vpool + 64); x[3] = 24;
            cmdbuf_bind_vertex_buffer(ctx, x);
            x[0] = kCb8; x[1] = 0; x[2] = 0;
            x[3] = reinterpret_cast<uint64_t>(vpool + 3072); x[4] = 16;
            cmdbuf_bind_uniform_buffer(ctx, x);
            x[0] = kCb8; x[1] = kProg8; x[2] = 0;
            cmdbuf_bind_program(ctx, x);
            x[0] = kCb8; x[1] = 0; x[2] = 0; x[3] = 3;
            cmdbuf_draw_arrays(ctx, x);
            x[0] = kCb8;
            cmdbuf_end_recording(ctx, x);

            CmdBuf* b8 = find<CmdBuf>(kCb8);
            Program* p8 = find<Program>(kProg8);
            if (!b8 || !b8->last || !p8) {
                std::fprintf(stderr,
                             "[selftest] vertex-ubo draw did not commit (cb=%d p=%d)\n",
                             b8 ? 1 : 0, p8 ? 1 : 0);
                rc = 16395;
                goto done;
            }
            [b8->last waitUntilCompleted];
            if (b8->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr,
                             "[selftest] vertex-ubo command buffer error: %s\n",
                             b8->last.error
                                 ? [[b8->last.error localizedDescription] UTF8String]
                                 : "(no description)");
                rc = 16395;
                goto done;
            }

            id<MTLBuffer> vtable = b8->argbuf[0];
            uint64_t word8 = 0;
            if (vtable && [vtable length] >= 8)
                std::memcpy(&word8, [vtable contents], 8);
            id<MTLBuffer> uboBuf = nil;
            size_t uboOff = 0;
            const bool uboKnown =
                pool_span(reinterpret_cast<uint64_t>(vpool) + 3072, 16,
                          &uboBuf, &uboOff);
            const uint64_t wantWord =
                uboKnown ? static_cast<uint64_t>([uboBuf gpuAddress]) + uboOff
                         : ~0ull;
            if (!vtable || [vtable length] != 8 || word8 != wantWord ||
                !p8->vHasArgBuffer || b8->encoded != 1) {
                std::fprintf(stderr,
                             "[selftest] vertex table mismatch: len=%lu "
                             "word=0x%llx want=0x%llx hasArg=%d encoded=%u\n",
                             vtable ? static_cast<unsigned long>([vtable length]) : 0ul,
                             static_cast<unsigned long long>(word8),
                             static_cast<unsigned long long>(wantWord),
                             p8->vHasArgBuffer ? 1 : 0, b8->encoded);
                rc = 16395;
                goto done;
            }
            {
                unsigned char left[4] = {0, 0, 0, 0};
                unsigned char right[4] = {0, 0, 0, 0};
                const int wantMagenta[4] = {255, 0, 255, 255};
                const int wantClear[4] = {0, 0, 0, 255};
                if (!read_pixel_at(dev, c->queue, t->mtl, kSide,
                                   kSide / 8, kSide / 2, left) ||
                    !read_pixel_at(dev, c->queue, t->mtl, kSide,
                                   (7 * kSide) / 8, kSide / 2, right)) {
                    std::fprintf(stderr, "[selftest] vertex-ubo readback failed\n");
                    rc = 16395;
                    goto done;
                }
                if (!pixels_near(left, wantMagenta, 2) ||
                    !pixels_near(right, wantClear, 2)) {
                    std::fprintf(stderr,
                                 "[selftest] vertex-ubo pixels (%u,%u,%u,%u) / "
                                 "(%u,%u,%u,%u), want (%d,%d,%d,%d) / (%d,%d,%d,%d)\n",
                                 left[0], left[1], left[2], left[3],
                                 right[0], right[1], right[2], right[3],
                                 wantMagenta[0], wantMagenta[1], wantMagenta[2],
                                 wantMagenta[3], wantClear[0], wantClear[1],
                                 wantClear[2], wantClear[3]);
                    rc = 16395;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] vertex table: buffer(0) word=0x%llx, "
                             "stream at %d, shifted pixels read back\n",
                             static_cast<unsigned long long>(word8),
                             NVNGFX_VERTEX_STREAM_BASE);
            }
        }

        {
            /* --- Draw 9: a texture whose pixels come from guest memory ----
             * Every earlier draw got its texels from a host blit.  This one
             * builds a 2x2 RGBA8 texture whose storage is inside the memory
             * pool, writes the block-linear bytes the way the guest's decoder
             * would, and lets the draw path copy them out: tex_upload() calls
             * nvngfx_tex_deswizzle() and blits the result into the private
             * MTLTexture before the pipeline samples it.  The texels are one
             * colour on purpose -- a pixel that is not the clear colour can
             * only come from a table that reached the GPU. */
            /* nvnTextureBuilderSetDefaults writes 0x80 bytes through the
             * builder pointer, so draw 9's builder needs real backing store.
             * The texture, program and command buffer stay table keys. */
            alignas(16) unsigned char guestTexBuilder[0x80] = {};
            const uint64_t kTexGuest = 0x6400;
            const uint64_t kProg9 = 0x6500;
            const uint64_t kCb9 = 0x6600;
            const uint64_t kB = reinterpret_cast<uint64_t>(guestTexBuilder);
            const unsigned kGuestOffset = 8192;
            unsigned char* const vpool9 = reinterpret_cast<unsigned char*>(
                (reinterpret_cast<uintptr_t>(poolRaw) + 16383u) &
                ~static_cast<uintptr_t>(16383u));

            nvngfx_texdesc gd;
            nvngfx_texdesc_defaults(&gd);
            gd.target = 0;
            gd.format = 0x25;                    /* RGBA8_UNORM */
            gd.width = 2;
            gd.height = 2;
            gd.levels = 1;
            gd.samples = 1;
            gd.pool = 0x5400;   /* the pool object the earlier draws built */
            gd.offset = kGuestOffset;
            {
                /* Row 0: cyan, cyan.  Row 1: cyan, cyan. */
                const unsigned char texels[16] = {
                    0, 255, 255, 255, 0, 255, 255, 255,
                    0, 255, 255, 255, 0, 255, 255, 255};
                nvngfx_texlayout gl;
                if (nvngfx_tex_layout(&gd, &gl) != 0 || gl.size > 1024 ||
                    nvngfx_tex_swizzle(&gd, 0, 0, texels, 8,
                                       vpool9 + kGuestOffset) != 0) {
                    std::fprintf(stderr,
                                 "[selftest] guest texture layout failed\n");
                    rc = 16396;
                    goto done;
                }
            }
            {
                /* tex_initialize reads the builder state, so replay the
                 * builder calls that describe this texture.  kB is real
                 * storage: SetDefaults memsets through it. */
                x[0] = kB;
                tex_builder_set_defaults(ctx, x);
                x[0] = kB; x[1] = 0;
                tex_builder_set_target(ctx, x);
                x[0] = kB; x[1] = 0x25;
                tex_builder_set_format(ctx, x);
                x[0] = kB; x[1] = 2; x[2] = 2;
                tex_builder_set_size2d(ctx, x);
                x[0] = kB; x[1] = 1;
                tex_builder_set_levels(ctx, x);
                x[0] = kB; x[1] = 1;
                tex_builder_set_samples(ctx, x);
                x[0] = kB; x[1] = 0x5400; x[2] = kGuestOffset;
                tex_builder_set_storage(ctx, x);
                x[0] = kTexGuest; x[1] = kB;
                tex_initialize(ctx, x);
            }
            Tex* guestTex = find<Tex>(kTexGuest);
            if (x[0] != 1 || !guestTex || !guestTex->mtl) {
                std::fprintf(stderr, "[selftest] guest texture was not created\n");
                rc = 16396;
                goto done;
            }

            struct { uint64_t data; uint64_t control; } sd9[2];
            sd9[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 4)));
            sd9[0].control = 0;
            sd9[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 4)));
            sd9[1].control = 0;
            x[0] = kProg9; x[1] = 0;
            prog_initialize(ctx, x);
            x[0] = kProg9; x[1] = 2;
            x[2] = reinterpret_cast<uint64_t>(&sd9[0]);
            prog_set_shaders(ctx, x);
            if (x[0] != 1) {
                std::fprintf(stderr, "[selftest] guest texture: SetShaders failed\n");
                rc = 16396;
                goto done;
            }
            x[0] = kTexPool; x[1] = 6; x[2] = kTexGuest; x[3] = 0;
            tex_pool_register_texture(ctx, x);

            const uint64_t target9 = kTex;
            const float clear9[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            x[0] = kCb9;
            cmdbuf_begin_recording(ctx, x);
            x[0] = kCb9; x[1] = 1; x[2] = reinterpret_cast<uint64_t>(&target9);
            x[3] = 0; x[4] = 0; x[5] = 0;
            cmdbuf_set_render_targets(ctx, x);
            x[0] = kCb9; x[1] = 0; x[2] = reinterpret_cast<uint64_t>(clear9);
            x[3] = 0xF;
            cmdbuf_clear_color(ctx, x);
            x[0] = kCb9; x[1] = kTexPool;
            cmdbuf_set_texture_pool(ctx, x);
            x[0] = kCb9; x[1] = kSampPool;
            cmdbuf_set_sampler_pool(ctx, x);
            x[0] = 0; x[1] = 6; x[2] = 9;
            dev_get_texture_handle(ctx, x);
            const uint64_t ghandle = x[0];
            x[0] = kCb9; x[1] = 1; x[2] = 0; x[3] = ghandle;
            cmdbuf_bind_texture(ctx, x);
            x[0] = kCb9; x[1] = kProg9; x[2] = 0;
            cmdbuf_bind_program(ctx, x);
            x[0] = kCb9; x[1] = 0; x[2] = 0; x[3] = 3;
            cmdbuf_draw_arrays(ctx, x);
            x[0] = kCb9;
            cmdbuf_end_recording(ctx, x);

            CmdBuf* b9 = find<CmdBuf>(kCb9);
            if (!b9 || !b9->last) {
                std::fprintf(stderr, "[selftest] guest texture draw did not commit\n");
                rc = 16396;
                goto done;
            }
            std::fprintf(stderr,
                         "[selftest] guest texture: draw committed, waiting "
                         "for the GPU\n");
            std::fflush(stderr);
            [b9->last waitUntilCompleted];
            if (b9->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr,
                             "[selftest] guest texture command buffer error: %s\n",
                             b9->last.error
                                 ? [[b9->last.error localizedDescription] UTF8String]
                                 : "(no description)");
                rc = 16396;
                goto done;
            }
            if (!guestTex->uploaded || guestTex->uploadedLevels != 1 ||
                b9->encoded != 1) {
                std::fprintf(stderr,
                             "[selftest] guest texture not used: uploaded=%d "
                             "levels=%u encoded=%u\n",
                             guestTex->uploaded ? 1 : 0,
                             guestTex->uploadedLevels, b9->encoded);
                rc = 16396;
                goto done;
            }
            {
                unsigned char px[4] = {0, 0, 0, 0};
                const int wantCyan[4] = {0, 255, 255, 255};
                if (!read_center_pixel(dev, c->queue, t->mtl, kSide, px)) {
                    std::fprintf(stderr, "[selftest] guest texture readback failed\n");
                    rc = 16396;
                    goto done;
                }
                if (!pixels_near(px, wantCyan, 2)) {
                    std::fprintf(stderr,
                                 "[selftest] guest texture centre is "
                                 "(%u,%u,%u,%u), want (%d,%d,%d,%d)\n",
                                 px[0], px[1], px[2], px[3], wantCyan[0],
                                 wantCyan[1], wantCyan[2], wantCyan[3]);
                    rc = 16396;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] guest texture: %u level(s) deswizzled "
                             "from the pool and sampled back\n",
                             guestTex->uploadedLevels);
            }
        }

    done:
        nvngfx_shader_set_msl_passthrough(0);
        forget<CmdBuf>(kCb);
        forget<Tex>(kTex);
        forget<Sampler>(kSampler);
        forget<Sampler>(kSampler2);
        forget<Program>(kProg);
        forget<Program>(0x5600);
        forget<CmdBuf>(0x5700);
        forget<Pool>(0x5400);   /* releases the MTLBuffer wrapping the page */
        g_pool_specs.erase(0x5500);
        std::free(poolRaw);
        g_tex_specs.erase(kTexBuilder);
        g_sampler_specs.erase(kSamplerBuilder);
        forget<Tex>(kTexSrc);
        forget<TexturePool>(kTexPool);
        forget<SamplerPool>(kSampPool);
        forget<Sampler>(kSampler5);
        forget<Program>(0x5C00);
        forget<CmdBuf>(0x5D00);
        forget<Program>(0x5E00);
        forget<CmdBuf>(0x5F00);
        forget<Program>(0x6000);
        forget<CmdBuf>(0x6100);
        forget<Program>(0x6200);
        forget<CmdBuf>(0x6300);
        forget<Tex>(0x6400);
        forget<Program>(0x6500);
        forget<CmdBuf>(0x6600);
        g_tex_specs.erase(kTexSrcBuilder);
        g_tex_specs.erase(0x6450);
        g_sampler_specs.erase(kSamplerBuilder5);
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
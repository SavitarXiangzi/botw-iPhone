/* The MSVC CRT marks getenv() deprecated; the portable spelling is what this
 * translation unit uses, and Apple's clang ignores this define. */
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

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
 *         nvngfx_metal_selftest() in CI's iOS Simulator: thirteen draws, a
 *         bindless table probe, and a vertex-stage argument-buffer draw whose
 *         shifted pixels are read back.  The ninth builds a block-linear
 *         texture in a guest memory pool and proves that the sampled pixels
 *         came from that storage, not from a host-side upload.  The tenth
 *         computes a guest shader key, loads its MSL from <app>/shaders and
 *         reads back orange.  The eleventh uploads a two-layer 2D array and
 *         reads red from layer 0 and yellow from layer 1.  A sampler state
 *         object is also created, compared and read back.
 *         MRT / depth attachment / texture views
 *         streaming texture updates, texture views, compute, sync/fence
 *         BC6H/BC7 and signed BCn (unsigned BC1-BC5 decode to RGBA8)
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
#include "nvn_bcn.h"
#include "nvn_metal.h"
#include "nvn_pool.h"
#include "nvn_sampler.h"
#include "nvn_pipeline.h"
#include "nvn_state.h"
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
    /* The guest stores a BCn format that iOS Metal cannot create.  This
     * texture's MTLTexture is therefore RGBA8 and tex_upload() decodes every
     * block while it copies the guest image. */
    bool transcoded = false;
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
    /* Set when the guest rewrote this texture's storage after the first
     * upload (nvnTextureWriteTexels) or said its CPU writes are visible
     * (nvnTextureFlushTexels).  The next draw re-runs tex_upload(). */
    bool dirty = false;
};

struct Sampler {
    nvngfx_sampler state = {};
    uint64_t device = 0;
    std::string label;
    id<MTLSamplerState> mtl = nil;
};

/* NVNcopyRegion is six int32 fields, at offsets 0, 4, ... 20. */
struct CopyRegion {
    int32_t xoffset, yoffset, zoffset, width, height, depth;
};
static_assert(sizeof(CopyRegion) == 24, "NVNcopyRegion measured as 24 bytes");

/* NVNtextureView is 16 bytes in the newer SDK.  This is nxnative's measured
 * layout (dm/gfx/nvn/nvn_obj.h, MIT): bit 0 set_levels, bit 1 set_layers,
 * bit 2 set_format, bit 3 set_target, bit 4 set_swizzle, bit 5 set_dsmode.
 * The base level and min layer fields follow the first four bytes.  Only the
 * level/layer selection and the format/target consistency checks are used
 * here; swizzle and depth-stencil mode do not move storage bytes. */
struct TexView16 {
    uint8_t bits;
    uint8_t target;
    uint8_t baseLevel;
    uint8_t numLevels;
    uint16_t format;
    uint16_t minLayer;
    uint16_t numLayers;
    uint8_t swizzle[4];
    uint16_t reserved;
};
static_assert(sizeof(TexView16) == 16, "NVNtextureView measured as 16 bytes");

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
    /* Which attachments that pipeline declared.  Binding a depth or stencil
     * test to a pass without the matching attachment is a Metal error, so the
     * encoder state is sanitised against these flags. */
    bool psoHasDepthStencil = false;
    bool psoHasDepth = false;
    bool psoHasStencil = false;
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
    /* M4 fixed-function state.  The guest's setters write these exact bit
     * layouts into its own 4/8/10-byte objects; a bind copies the decoded
     * fields here so a later pipeline can be built from the state that was
     * current at the draw, not from whatever the guest reused later. */
    /* One blend state per colour target: NVN's blend object names its own
     * target (nvnBlendStateSetBlendTarget), so a bind lands in slot
     * state.target instead of always in slot 0. */
    nvngfx_blend_state blendState[NVNGFX_MAX_COLOR_TARGETS] = {};
    nvngfx_color_state colorState = {};
    nvngfx_channel_mask_state channelState = {};
    nvngfx_multisample_state multisampleState = {};
    nvngfx_polygon_state polygonState = {};
    nvngfx_depth_stencil_state depthState = {};
    bool hasBlendState[NVNGFX_MAX_COLOR_TARGETS] = {};
    bool hasColorState = false;
    bool hasChannelState = false;
    bool hasMultisampleState = false;
    bool hasPolygonState = false;
    bool hasDepthState = false;
    /* MTLDepthStencilState built from depthState; dropped when the state
     * object is bound again, because the guest may have rewritten it. */
    id<MTLDepthStencilState> dss = nil;
    unsigned long long dssKey = 0;
    bool dssValid = false;
    float blendColor[4] = {0, 0, 0, 0};
    bool hasBlendColor = false;
    float alphaRef = 0.0f;
    bool hasAlphaRef = false;
    uint32_t renderEnable = 1;
    bool hasRenderEnable = false;
    /* Constants from nvnCommandBufferSetPolygonOffsetClamp.  The polygon
     * state decides whether the FILL enable is on; point/line enables have
     * no Metal equivalent and are reported by nvngfx_pipeline_set_polygon(). */
    float polyOffsetFactor = 0.0f;
    float polyOffsetUnits = 0.0f;
    float polyOffsetClamp = 0.0f;
    bool hasPolyOffset = false;
    float coverageTable[16] = {};
    bool hasCoverageTable = false;
    double depthNear = 0.0, depthFar = 1.0;
    /* Compute / transform-feedback and image bindings are recorded for the
     * compute path; the render path does not consume them yet. */
    uint64_t storage[6][NVNGFX_MAX_UBO_SLOTS] = {};
    uint64_t storageSize[6][NVNGFX_MAX_UBO_SLOTS] = {};
    uint64_t image[6][NVNGFX_MAX_TEXTURE_SLOTS] = {};
    uint64_t tfBuffer[16] = {};
    uint64_t tfSize[16] = {};
    uint32_t viewportSwizzle[4] = {};
    bool hasViewportSwizzle = false;
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

/* Same "guest memory is host memory" assumption, with the two checks that
 * keep a corrupt pointer from wrapping or reading the unmapped first 64 KiB.
 * Used for the NVNcopyRegion / NVNtextureView structs and the source buffer
 * of WriteTexels; the length of the source is the guest's responsibility. */
bool guest_range(uint64_t addr, size_t n) {
    return addr >= 0x10000u && n <= UINTPTR_MAX - addr;
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

/* NVN's target carries the array / cube shape that `rank` alone cannot
 * express (see nvngfx_tex_layout's shape()).  Build the matching Metal view so
 * layer 1 of a 2D array, or a cube face, is addressable as a slice instead of
 * silently being another `MTLTextureType2D` of the same dimensions. */
MTLTextureType to_metal_texture_type(const nvngfx_texdesc& d) {
    const int t = d.target;
    /* Samples describe the storage, not the shape.  Only the two NVN
     * multisample targets can become a Metal multisample type; treating a
     * cube array with samples > 1 as a 2D multisample array would silently
     * drop four faces. */
    if (d.samples > 1 && t == 6) return MTLTextureType2DMultisampleArray;
    if (d.samples > 1 && t == 5) return MTLTextureType2DMultisample;
    if (t == 10) return MTLTextureTypeTextureBuffer;
    if (t == 3) return MTLTextureType1DArray;
    if (t == 4) return MTLTextureType2DArray;
    if (t == 8) return MTLTextureTypeCube;
    if (t == 9) return MTLTextureTypeCubeArray;
    /* `rank` is authoritative when the builder used size2d but left target at
     * its 1D default (the self-test does exactly this). */
    if (t == 2 || d.rank == 3) return MTLTextureType3D;
    if (d.rank == 1) return MTLTextureType1D;
    return MTLTextureType2D;
}

/* Metal's arrayLength counts arrays, cubes and 2D MS arrays differently from
 * NVN's raw depth/height: a cube is one entry (six implicit slices) and a cube
 * array is `depth` cubes (depth * 6 slices).  The upload loop, in contrast,
 * addresses every slice, so it uses nvngfx_texlayout::layers. */
NSUInteger metal_array_length(const nvngfx_texdesc& d) {
    const int t = d.target;
    if (t == 3) return static_cast<NSUInteger>(d.height > 0 ? d.height : 1);
    if (t == 4 || t == 6 || t == 9)
        return static_cast<NSUInteger>(d.depth > 0 ? d.depth : 1);
    return 1;
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

    /* iOS Metal has no BCn pixel formats.  Decode to RGBA8 instead of asking
     * for a descriptor the device will reject (or, on a Mac host, silently
     * use a format the iOS build cannot ship).  sRGB variants keep their
     * transfer function so sampling is not double-converted. */
    const bool bcn = nvngfx_bcn_supported(d.format) != 0;
    int lossy = 0;
    unsigned pf = 0;
    if (bcn) {
        pf = nvngfx_bcn_is_srgb(d.format)
                 ? static_cast<unsigned>(MTLPixelFormatRGBA8Unorm_sRGB)
                 : static_cast<unsigned>(MTLPixelFormatRGBA8Unorm);
        log_once("nvnTextureInitialize.bcnTranscode",
                 static_cast<unsigned>(d.format));
    } else {
        pf = nvngfx_format_to_mtl(d.format, &lossy);
    }
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
        nvngfx_texlayout L = {};
        unsigned layers = 1;
        if (nvngfx_tex_layout(&d, &L) == 0 && L.layers)
            layers = L.layers;
        else
            log_once("nvnTextureInitialize.layout", static_cast<unsigned>(d.format));
        const MTLTextureType type = to_metal_texture_type(d);
        const bool is3d = type == MTLTextureType3D;
        const bool is1d = type == MTLTextureType1D ||
                          type == MTLTextureType1DArray;
        const bool isMultisample = type == MTLTextureType2DMultisample ||
                                   type == MTLTextureType2DMultisampleArray;

        MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
        td.pixelFormat = static_cast<MTLPixelFormat>(pf);
        td.width = static_cast<NSUInteger>(d.width);
        /* NVN stores a 1D array's layer count in height; Metal keeps the
         * height 1 and puts the layers in arrayLength. */
        td.height = is1d ? 1 : static_cast<NSUInteger>(d.height);
        td.depth = is3d ? static_cast<NSUInteger>(d.depth > 0 ? d.depth : 1) : 1;
        td.arrayLength = metal_array_length(d);
        td.mipmapLevelCount = isMultisample
                                  ? 1
                                  : (d.levels > 1
                                         ? static_cast<NSUInteger>(d.levels)
                                         : 1);
        td.sampleCount = isMultisample
                             ? static_cast<NSUInteger>(d.samples > 1 ? d.samples : 1)
                             : 1;
        td.textureType = type;
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
        t->transcoded = bcn;
        t->pool = d.pool;
        t->offset = static_cast<ptrdiff_t>(d.offset);
        t->desc = d;
        t->layers = layers;
        t->uploadTried = false;
        t->uploaded = false;
        t->rendered = false;
        t->uploadedLevels = 0;
        t->dirty = false;
        /* A descriptor Metal rejects throws an NSException rather than
         * returning nil; uncaught, that aborts the process with no clue in
         * the self-test log (which is exactly how draw 13 died).  Turn it
         * into a logged failure and let the caller drop the draw. */
        @try {
            t->mtl = [c->device newTextureWithDescriptor:td];
        } @catch (NSException* e) {
            std::fprintf(stderr,
                         "[nvn] texture create threw for target %d: %s\n",
                         d.target,
                         [e reason] ? [[e reason] UTF8String] : "?");
            t->mtl = nil;
        }
        x[0] = t->mtl ? 1u : 0u;
    }
}

/* nvnTextureWriteTexels(NVNtexture*, const NVNtextureView*, const
 * NVNcopyRegion*, const void*) and its Strided variant.
 *
 * The guest is allowed to rewrite a texture after it was first sampled (the
 * boot trace calls this 27 times).  Our MTLTexture is private and a one-shot
 * copy, so the bytes are written into the guest's own storage with the same
 * block-linear/LINEAR layout the upload path reads, and the texture is marked
 * dirty.  The next draw's prepare_textures() re-runs tex_upload(). */
void tex_write_texels_common(nvngfx_ctx* ctx, uint64_t* x, bool strided) {
    (void)ctx;
    Tex* t = find<Tex>(x[0]);
    if (!t || !t->desc.pool) {
        log_once_str("nvnTextureWriteTexels",
                     "texture has no guest storage (or is not initialized)");
        return;
    }
    if (!guest_range(x[2], sizeof(CopyRegion)) || !guest_range(x[3], 1)) {
        log_once_str("nvnTextureWriteTexels",
                     "region or source pointer is not guest memory");
        return;
    }
    CopyRegion r = {};
    std::memcpy(&r, guest_ptr<const void>(x[2]), sizeof r);
    if (r.xoffset < 0 || r.yoffset < 0 || r.zoffset < 0 ||
        r.width <= 0 || r.height <= 0 || r.depth < 0) {
        log_once_str("nvnTextureWriteTexels", "region is empty or negative");
        return;
    }

    unsigned baseLevel = 0, minLayer = 0;
    if (x[1]) {
        if (!guest_range(x[1], sizeof(TexView16))) {
            log_once_str("nvnTextureWriteTexels",
                         "texture view pointer is not guest memory");
            return;
        }
        TexView16 v = {};
        std::memcpy(&v, guest_ptr<const void>(x[1]), sizeof v);
        /* A view may reinterpret the pixels in another format or as another
         * target.  The storage layout would no longer be the one this
         * descriptor describes, so refuse instead of writing the wrong
         * bytes; swizzle and depth-stencil mode do not move storage. */
        if ((v.bits & 0x04u) && v.format != t->desc.format) {
            log_once_str("nvnTextureWriteTexels",
                         "texture view changes the format; region write refused");
            return;
        }
        if ((v.bits & 0x08u) && v.target != t->desc.target) {
            log_once_str("nvnTextureWriteTexels",
                         "texture view changes the target; region write refused");
            return;
        }
        if (v.bits & 0x01u) baseLevel = v.baseLevel;
        if (v.bits & 0x02u) minLayer = v.minLayer;
    }
    const unsigned levels =
        t->desc.levels ? static_cast<unsigned>(t->desc.levels) : 1u;
    if (baseLevel >= levels) {
        log_once_str("nvnTextureWriteTexels",
                     "texture view names a level past the descriptor");
        return;
    }

    Pool* p = find<Pool>(t->desc.pool);
    if (!p || !p->storage || !p->size) {
        log_once_str("nvnTextureWriteTexels", "texture pool is not initialized");
        return;
    }
    nvngfx_texlayout L = {};
    if (nvngfx_tex_layout(&t->desc, &L) != 0) {
        log_once_str("nvnTextureWriteTexels", "texture layout is unknown");
        return;
    }
    const unsigned long long off =
        static_cast<unsigned long long>(t->desc.offset);
    if (t->desc.offset < 0 || off > p->size || L.size > p->size - off) {
        log_once_str("nvnTextureWriteTexels",
                     "texture storage is outside its pool");
        return;
    }
    unsigned char* const storage =
        static_cast<unsigned char*>(p->storage) + off;

    unsigned bpb = 0, bw = 1, bh = 1;
    if (!nvngfx_tex_format_block(t->desc.format, &bpb, &bw, &bh) || !bpb) {
        log_once_str("nvnTextureWriteTexels", "texture format has no block shape");
        return;
    }
    const unsigned wblocks = (static_cast<unsigned>(r.width) + bw - 1u) / bw;
    const unsigned hblocks = (static_cast<unsigned>(r.height) + bh - 1u) / bh;
    const size_t tightRow = static_cast<size_t>(wblocks) * bpb;
    if (hblocks && tightRow > SIZE_MAX / hblocks) {
        log_once_str("nvnTextureWriteTexels", "region size overflows");
        return;
    }
    const size_t tightImage = tightRow * hblocks;

    const int64_t rowArg = strided ? static_cast<int64_t>(x[4]) : 0;
    const int64_t imgArg = strided ? static_cast<int64_t>(x[5]) : 0;
    if (rowArg < 0 || imgArg < 0) {
        log_once_str("nvnTextureWriteTexels", "negative source stride");
        return;
    }
    const size_t row = rowArg ? static_cast<size_t>(rowArg) : tightRow;
    const size_t image = imgArg ? static_cast<size_t>(imgArg) : tightImage;
    if (row < tightRow || image < tightImage) {
        log_once_str("nvnTextureWriteTexels",
                     "source stride is narrower than a tightly packed region");
        return;
    }

    const unsigned depth = r.depth ? static_cast<unsigned>(r.depth) : 1u;
    const unsigned zoff = static_cast<unsigned>(r.zoffset);
    if (zoff > 0x7FFFFFFFu - depth) {
        log_once_str("nvnTextureWriteTexels", "region depth overflows");
        return;
    }
    const bool is3d = t->desc.target == 2 || t->desc.rank == 3;
    unsigned slices = 1;
    if (is3d &&
        nvngfx_tex_level_geometry(&t->desc, baseLevel, 0, nullptr, nullptr,
                                  &slices, nullptr, nullptr, nullptr) != 0) {
        log_once_str("nvnTextureWriteTexels", "3D level geometry is unknown");
        return;
    }
    if (is3d) {
        if (static_cast<unsigned long long>(zoff) + depth > slices) {
            log_once_str("nvnTextureWriteTexels",
                         "region runs past the 3D slices");
            return;
        }
    } else if (static_cast<unsigned long long>(minLayer) + zoff + depth >
               L.layers) {
        log_once_str("nvnTextureWriteTexels",
                     "region runs past the array layers");
        return;
    }
    if (depth > 1 &&
        image > (UINTPTR_MAX - x[3]) / static_cast<size_t>(depth - 1u)) {
        log_once_str("nvnTextureWriteTexels", "source range overflows");
        return;
    }

    bool wrote = false;
    for (unsigned i = 0; i < depth; ++i) {
        const unsigned layer = is3d ? 0u : minLayer + zoff + i;
        const int z = is3d ? static_cast<int>(zoff + i) : 0;
        const unsigned char* const src =
            guest_ptr<const unsigned char>(x[3]) +
            static_cast<size_t>(i) * image;
        if (nvngfx_tex_write_region(&t->desc, baseLevel, layer,
                                    r.xoffset, r.yoffset, z,
                                    r.width, r.height, src, row, image,
                                    storage) != 0) {
            log_once_str("nvnTextureWriteTexels",
                         "region refused (level, layer, block alignment or bounds)");
            break;
        }
        wrote = true;
    }
    if (!wrote) return;

    t->dirty = true;
    t->uploaded = false;
    t->uploadTried = false;
    if (t->rendered) {
        log_once_str("nvnTextureWriteTexels",
                     "guest write to a host-rendered texture is stored but not re-uploaded");
    } else {
        log_once_str("nvnTextureWriteTexels",
                     "guest region write will be re-uploaded on the next sample");
    }
}

void tex_write_texels(nvngfx_ctx* ctx, uint64_t* x) {
    tex_write_texels_common(ctx, x, false);
}

void tex_write_texels_strided(nvngfx_ctx* ctx, uint64_t* x) {
    tex_write_texels_common(ctx, x, true);
}

/* nvnTextureFlushTexels(NVNtexture*, const NVNtextureView*,
 * const NVNcopyRegion*).  The guest uses this to publish CPU writes made
 * directly into mapped texture storage.  We have no dirty-page tracking, so
 * invalidate the one-shot upload and let the next draw copy the whole level
 * chain again.  That is deliberately conservative: a missed update samples
 * stale pixels, a needless update only costs bandwidth. */
void tex_flush_texels(nvngfx_ctx*, uint64_t* x) {
    Tex* t = find<Tex>(x[0]);
    if (!t || !t->desc.pool) return;
    if (t->rendered) return;
    t->dirty = true;
    t->uploaded = false;
    t->uploadTried = false;
    log_once_str("nvnTextureFlushTexels",
                 "guest-visible texture writes will be re-uploaded on the next sample");
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
        /* Fixed-function state that was current at the draw.  A pipeline is
         * only reusable when the whole state matches, not just the format. */
        unsigned long long stateKey = 0;
        /* Attachments are baked into the pipeline too.  A depth-only and a
         * depth+stencil pipeline can share the colour format, so the cache
         * must not key on the colour format alone. */
        MTLPixelFormat depthFormat = static_cast<MTLPixelFormat>(0);
        MTLPixelFormat stencilFormat = static_cast<MTLPixelFormat>(0);
        unsigned sampleCount = 1;
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

/* The cached pipeline for the complete (colour/depth/stencil format, sample
 * count, vertex layout, fixed-function state) key, or nil.  Also records the
 * hit in slot 0 so the next draw with the same key finds it first. */
id<MTLRenderPipelineState> pso_lookup(Program* p, MTLPixelFormat fmt,
                                     unsigned long long layout, bool hasLayout,
                                     unsigned long long stateKey,
                                     MTLPixelFormat depthFormat,
                                     MTLPixelFormat stencilFormat,
                                     unsigned sampleCount) {
    for (unsigned i = 0; i < 4; ++i) {
        Program::PsoSlot& s = p->slots[i];
        if (s.pso && s.format == fmt && s.layout == layout && s.hasLayout == hasLayout &&
            s.stateKey == stateKey && s.depthFormat == depthFormat &&
            s.stencilFormat == stencilFormat && s.sampleCount == sampleCount) {
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
               unsigned long long layout, bool hasLayout,
               unsigned long long stateKey, MTLPixelFormat depthFormat,
               MTLPixelFormat stencilFormat, unsigned sampleCount) {
    /* Shift down; slot 3 falls out.  Releasing the object it held is fine:
     * the command buffers that used it keep their own reference. */
    for (unsigned i = 3; i > 0; --i) p->slots[i] = p->slots[i - 1];
    p->slots[0].pso = pso;
    p->slots[0].format = fmt;
    p->slots[0].layout = layout;
    p->slots[0].hasLayout = hasLayout;
    p->slots[0].stateKey = stateKey;
    p->slots[0].depthFormat = depthFormat;
    p->slots[0].stencilFormat = stencilFormat;
    p->slots[0].sampleCount = sampleCount;
}

void pso_drop(Program* p) {
    for (unsigned i = 0; i < 4; ++i) {
        p->slots[i].pso = nil;
        p->slots[i].format = static_cast<MTLPixelFormat>(0);
        p->slots[i].layout = 0;
        p->slots[i].hasLayout = false;
        p->slots[i].stateKey = 0;
        p->slots[i].depthFormat = static_cast<MTLPixelFormat>(0);
        p->slots[i].stencilFormat = static_cast<MTLPixelFormat>(0);
        p->slots[i].sampleCount = 1;
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

/* ---- fixed-function state -> pipeline and encoder --------------------- */

/* Layers the six state objects the command buffer currently holds into one
 * description.  An absent object contributes NVN's defaults, which is what
 * the guest would see if it had never touched that object. */
void pipeline_state_of(const CmdBuf* b, nvngfx_pipeline_state* st) {
    nvngfx_pipeline_defaults(st);
    for (unsigned i = 0; i < NVNGFX_MAX_COLOR_TARGETS; ++i)
        if (b->hasBlendState[i]) nvngfx_pipeline_set_blend(st, &b->blendState[i]);
    if (b->hasColorState) nvngfx_pipeline_set_color(st, &b->colorState);
    if (b->hasChannelState) nvngfx_pipeline_set_channel_mask(st, &b->channelState);
    if (b->hasMultisampleState) nvngfx_pipeline_set_multisample(st, &b->multisampleState);
    if (b->hasPolygonState) nvngfx_pipeline_set_polygon(st, &b->polygonState);
    if (b->hasDepthState) nvngfx_pipeline_set_depth_stencil(st, &b->depthState);
}

/* One line per feature that cannot be honoured.  Silently ignoring these is
 * how a backend ends up "mostly right" and impossible to debug. */
void report_unsupported(const nvngfx_pipeline_state& st) {
    const unsigned f = st.unsupported;
    if (f & NVNGFX_UNSUP_BLEND_FACTOR) log_once("nvnPipeline.blendFactor", 1);
    if (f & NVNGFX_UNSUP_BLEND_EQUATION) log_once("nvnPipeline.blendEquation", 1);
    if (f & NVNGFX_UNSUP_DEPTH_FUNC) log_once("nvnPipeline.depthFunc", 1);
    if (f & NVNGFX_UNSUP_STENCIL_FUNC) log_once("nvnPipeline.stencilFunc", 1);
    if (f & NVNGFX_UNSUP_STENCIL_OP) log_once("nvnPipeline.stencilOp", 1);
    if (f & NVNGFX_UNSUP_CULL) log_once("nvnPipeline.cullFrontAndBack", 1);
    if (f & NVNGFX_UNSUP_FILL) log_once("nvnPipeline.pointFill", 1);
    if (f & NVNGFX_UNSUP_LOGIC_OP) log_once("nvnPipeline.logicOp", 1);
    if (f & NVNGFX_UNSUP_ALPHA_TEST) log_once("nvnPipeline.alphaTest", 1);
    if (f & NVNGFX_UNSUP_MULTISAMPLE) log_once("nvnPipeline.samples", 1);
    if (f & NVNGFX_UNSUP_COVERAGE) log_once("nvnPipeline.coverage", 1);
    if (f & NVNGFX_UNSUP_POLYGON_OFFSET) log_once("nvnPipeline.polygonOffsetModes", 1);
}

MTLBlendFactor to_mtl_blend(unsigned f) {
    switch (f) {
    case NVNGFX_BF_ZERO: return MTLBlendFactorZero;
    case NVNGFX_BF_ONE: return MTLBlendFactorOne;
    case NVNGFX_BF_SRC_COLOR: return MTLBlendFactorSourceColor;
    case NVNGFX_BF_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case NVNGFX_BF_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case NVNGFX_BF_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case NVNGFX_BF_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case NVNGFX_BF_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case NVNGFX_BF_DST_COLOR: return MTLBlendFactorDestinationColor;
    case NVNGFX_BF_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case NVNGFX_BF_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
    /* NVN's "constant" is Metal's blend colour, which the encoder carries
     * from nvnCommandBufferSetBlendColor. */
    case NVNGFX_BF_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
    case NVNGFX_BF_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
    case NVNGFX_BF_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
    case NVNGFX_BF_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
    case NVNGFX_BF_SRC1_COLOR: return MTLBlendFactorSource1Color;
    case NVNGFX_BF_ONE_MINUS_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
    case NVNGFX_BF_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
    case NVNGFX_BF_ONE_MINUS_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;   /* already reported as unsupported */
    }
}

MTLBlendOperation to_mtl_blend_op(unsigned e) {
    switch (e) {
    case NVNGFX_EQ_ADD: return MTLBlendOperationAdd;
    case NVNGFX_EQ_SUB: return MTLBlendOperationSubtract;
    case NVNGFX_EQ_REVERSE_SUB: return MTLBlendOperationReverseSubtract;
    case NVNGFX_EQ_MIN: return MTLBlendOperationMin;
    case NVNGFX_EQ_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLCompareFunction to_mtl_compare(unsigned c) {
    switch (c) {
    case NVNGFX_CMP_NEVER: return MTLCompareFunctionNever;
    case NVNGFX_CMP_LESS: return MTLCompareFunctionLess;
    case NVNGFX_CMP_EQUAL: return MTLCompareFunctionEqual;
    case NVNGFX_CMP_LEQUAL: return MTLCompareFunctionLessEqual;
    case NVNGFX_CMP_GREATER: return MTLCompareFunctionGreater;
    case NVNGFX_CMP_NOTEQUAL: return MTLCompareFunctionNotEqual;
    case NVNGFX_CMP_GEQUAL: return MTLCompareFunctionGreaterEqual;
    case NVNGFX_CMP_ALWAYS: return MTLCompareFunctionAlways;
    default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation to_mtl_stencil(unsigned op) {
    switch (op) {
    case NVNGFX_STENCIL_KEEP: return MTLStencilOperationKeep;
    case NVNGFX_STENCIL_ZERO: return MTLStencilOperationZero;
    case NVNGFX_STENCIL_REPLACE: return MTLStencilOperationReplace;
    case NVNGFX_STENCIL_INCR: return MTLStencilOperationIncrementClamp;
    case NVNGFX_STENCIL_DECR: return MTLStencilOperationDecrementClamp;
    case NVNGFX_STENCIL_INVERT: return MTLStencilOperationInvert;
    case NVNGFX_STENCIL_INCR_WRAP: return MTLStencilOperationIncrementWrap;
    case NVNGFX_STENCIL_DECR_WRAP: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

MTLCullMode to_mtl_cull(unsigned c) {
    switch (c) {
    case NVNGFX_CULL_FRONT: return MTLCullModeFront;
    case NVNGFX_CULL_BACK: return MTLCullModeBack;
    default: return MTLCullModeNone;
    }
}

MTLWinding to_mtl_winding(unsigned w) {
    return w == NVNGFX_WINDING_CW ? MTLWindingClockwise : MTLWindingCounterClockwise;
}

MTLTriangleFillMode to_mtl_fill(unsigned f) {
    return f == NVNGFX_FILL_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
}

/* Metal requires the pipeline's stencil format to name a format that actually
 * has stencil bits; declaring one for Depth32Float fails pipeline creation. */
/* The depth-stencil object only depends on the depth/stencil fields, so it is
 * cached on that key alone: a blend change must not rebuild it.  Those fields
 * are declared consecutively in nvngfx_pipeline_state on purpose. */
unsigned long long depth_stencil_key(const nvngfx_pipeline_state& st) {
    const unsigned char* p = &st.depth_test;
    const unsigned n = 12;   /* test, write, func, stencil test, 2 funcs, 6 ops */
    unsigned long long h = 14695981039346656037ull;
    for (unsigned i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool mtl_format_has_depth(MTLPixelFormat f) {
    return f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float ||
           f == MTLPixelFormatDepth32Float_Stencil8;
}

bool mtl_format_has_stencil(MTLPixelFormat f) {
    return f == MTLPixelFormatStencil8 || f == MTLPixelFormatDepth32Float_Stencil8;
}

id<MTLDepthStencilState> build_depth_stencil(Ctx* c, const nvngfx_pipeline_state& st) {
    if (!c || !c->device) return nil;
    @autoreleasepool {
        MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
        /* NVN semantics: with the depth test off there is neither a test nor
         * a write, whatever depthWriteEnable still says. */
        dd.depthCompareFunction =
            st.depth_test ? to_mtl_compare(st.depth_func) : MTLCompareFunctionAlways;
        dd.depthWriteEnabled = (st.depth_test && st.depth_write) ? YES : NO;
        if (st.stencil_test) {
            MTLStencilDescriptor* front = [[MTLStencilDescriptor alloc] init];
            front.stencilCompareFunction = to_mtl_compare(st.stencil_func[0]);
            front.stencilFailureOperation = to_mtl_stencil(st.stencil_op[0][0]);
            front.depthFailureOperation = to_mtl_stencil(st.stencil_op[0][1]);
            front.depthStencilPassOperation = to_mtl_stencil(st.stencil_op[0][2]);
            front.readMask = 0xFFu;
            front.writeMask = 0xFFu;
            MTLStencilDescriptor* back = [[MTLStencilDescriptor alloc] init];
            back.stencilCompareFunction = to_mtl_compare(st.stencil_func[1]);
            back.stencilFailureOperation = to_mtl_stencil(st.stencil_op[1][0]);
            back.depthFailureOperation = to_mtl_stencil(st.stencil_op[1][1]);
            back.depthStencilPassOperation = to_mtl_stencil(st.stencil_op[1][2]);
            back.readMask = 0xFFu;
            back.writeMask = 0xFFu;
            dd.frontFaceStencil = front;
            dd.backFaceStencil = back;
        }
        return [c->device newDepthStencilStateWithDescriptor:dd];
    }
}

id<MTLRenderPipelineState> program_pipeline(Ctx* c, Program* p, MTLPixelFormat fmt,
                                            MTLPixelFormat depthFmt,
                                            MTLPixelFormat stencilFmt,
                                            unsigned colorSamples,
                                            const nvngfx_pipeline_state& st,
                                            const CmdBuf* b, const char** why) {
    const bool hasLayout = b && b->numAttrib > 0;
    unsigned long long layout = 0;
    MTLVertexDescriptor* vd = nil;
    if (hasLayout) {
        vd = vertex_descriptor(b, &layout, why);
        if (!vd) return nil;
    }
    /* A pipeline's sample count must match the attachments it renders into.
     * The guest's request wins only when it can actually be honoured. */
    unsigned raster = st.samples ? st.samples : 1;
    if (colorSamples && raster != colorSamples) {
        log_once("nvnPipeline.sampleCountMismatch", raster);
        raster = colorSamples;
    }

    const unsigned long long stateKey = nvngfx_pipeline_key(&st);
    if (id<MTLRenderPipelineState> hit =
            pso_lookup(p, fmt, layout, hasLayout, stateKey, depthFmt, stencilFmt, raster))
        return hit;

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
        /* Every field the guest actually set lands here.  Anything without a
         * Metal equivalent was already reported by report_unsupported(), so
         * this descriptor is an honest description of what can be honoured. */
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[0];
        ca.pixelFormat = fmt;
        ca.writeMask = static_cast<MTLColorWriteMask>(st.write_mask[0]);
        const bool blend = st.blend_enable[0] != 0;
        ca.blendingEnabled = blend ? YES : NO;
        if (blend) {
            ca.sourceRGBBlendFactor = to_mtl_blend(st.src_rgb[0]);
            ca.destinationRGBBlendFactor = to_mtl_blend(st.dst_rgb[0]);
            ca.rgbBlendOperation = to_mtl_blend_op(st.eq_rgb[0]);
            ca.sourceAlphaBlendFactor = to_mtl_blend(st.src_a[0]);
            ca.destinationAlphaBlendFactor = to_mtl_blend(st.dst_a[0]);
            ca.alphaBlendOperation = to_mtl_blend_op(st.eq_a[0]);
        }
        /* Alpha-to-coverage is only defined for attachment 0, and Metal
         * rejects it when the pass is not multisampled.  If the guest asked
         * for it on a single-sample target, disabling it is the only valid
         * description; report instead of making pipeline creation fail. */
        bool a2c = st.alpha_to_coverage != 0;
        if (a2c && raster < 2) {
            log_once("nvnPipeline.alphaToCoverageWithoutMSAA", 1);
            a2c = false;
        }
        pd.alphaToCoverageEnabled = a2c ? YES : NO;
        if (depthFmt) pd.depthAttachmentPixelFormat = depthFmt;
        else if (st.depth_test) log_once("nvnPipeline.depthWithoutAttachment", 1);
        if (stencilFmt) pd.stencilAttachmentPixelFormat = stencilFmt;
        else if (st.stencil_test) log_once("nvnPipeline.stencilWithoutFormat", 1);
        pd.rasterSampleCount = raster;
        if (vd) pd.vertexDescriptor = vd;

        err = nil;
        id<MTLRenderPipelineState> pso =
            [c->device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) {
            log_once_str("nvnProgram.pipeline.state",
                         err ? [[err localizedDescription] UTF8String] : "nil pipeline");
            return nil;
        }
        pso_store(p, pso, fmt, layout, hasLayout, stateKey, depthFmt, stencilFmt, raster);
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
    nvngfx_pipeline_state st;
    pipeline_state_of(b, &st);
    report_unsupported(st);
    const Tex* dt = b->depth ? find<Tex>(b->depth) : nullptr;
    const MTLPixelFormat attachmentFmt =
        (dt && dt->mtl) ? dt->mtl.pixelFormat : static_cast<MTLPixelFormat>(0);
    const MTLPixelFormat depthFmt =
        mtl_format_has_depth(attachmentFmt) ? attachmentFmt : static_cast<MTLPixelFormat>(0);
    const MTLPixelFormat stencilFmt =
        mtl_format_has_stencil(attachmentFmt) ? attachmentFmt : static_cast<MTLPixelFormat>(0);
    const char* why = nullptr;
    b->pso = program_pipeline(c, p, t->mtl.pixelFormat, depthFmt, stencilFmt,
                              static_cast<unsigned>(t->mtl.sampleCount), st, b, &why);
    b->psoHasDepthStencil = b->pso != nil && (depthFmt || stencilFmt);
    b->psoHasDepth = b->pso != nil && depthFmt != 0;
    b->psoHasStencil = b->pso != nil && stencilFmt != 0;
    if (!b->pso) log_once_str("nvnCommandBufferDraw.pipeline", why ? why : "unknown");
    return b->pso != nil;
}

/* Applies the state that lives on the encoder rather than in the pipeline:
 * cull/winding/fill mode and the depth-stencil object.  Called per draw
 * because a bind can happen between two draws of the same pass. */
void apply_fixed_state(Ctx* c, CmdBuf* b) {
    if (!b || !b->enc) return;
    nvngfx_pipeline_state st;
    pipeline_state_of(b, &st);
    [b->enc setCullMode:to_mtl_cull(st.cull)];
    [b->enc setFrontFacingWinding:to_mtl_winding(st.winding)];
    [b->enc setTriangleFillMode:to_mtl_fill(st.fill)];
    if (b->psoHasDepth) {
        if (st.polygon_offset && b->hasPolyOffset) {
            [b->enc setDepthBias:b->polyOffsetUnits
                        slopeScale:b->polyOffsetFactor
                             clamp:b->polyOffsetClamp];
        } else {
            /* Reset, not merely "leave alone": the previous draw in this pass
             * may have enabled a bias, and encoder state persists.  If the
             * guest enabled offset without sending constants, zero is the
             * only safe value but still deserves a report. */
            [b->enc setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
            if (st.polygon_offset) log_once("nvnPipeline.polygonOffsetWithoutClamp", 1);
        }
    }
    /* NVN's defaults still apply when the guest never bound a state object.
     * With a depth attachment present Metal requires a depth-stencil state,
     * so build it from the layered description rather than skipping here. */
    if (!b->psoHasDepthStencil) return;
    nvngfx_pipeline_state ds = st;
    if (!b->psoHasDepth) {
        ds.depth_test = 0;
        ds.depth_write = 0;
    }
    if (!b->psoHasStencil) ds.stencil_test = 0;
    const unsigned long long key = depth_stencil_key(ds);
    if (!b->dssValid || b->dssKey != key) {
        b->dss = build_depth_stencil(c, ds);
        b->dssKey = key;
        b->dssValid = b->dss != nil;
        if (!b->dss) log_once("nvnPipeline.depthStencilState", 1);
    }
    if (b->dss) [b->enc setDepthStencilState:b->dss];
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


/* ------------------------------------------------------------------ */
/* fixed-function state objects (M4)                                   */
/* ------------------------------------------------------------------ */

/* State objects live in the guest's own 4/8/10-byte storage.  These helpers
 * are the only place that crosses between those bytes and the portable
 * nvngfx_*_state structs; the setters below never keep an address-keyed
 * side table, so a guest memcpy of a state object cannot go stale. */
bool read_blend(uint64_t a, nvngfx_blend_state* s) {
    const unsigned long long* w = guest_ptr<const unsigned long long>(a);
    if (!w) return false;
    nvngfx_blend_unpack(s, *w);
    return true;
}
void write_blend(uint64_t a, const nvngfx_blend_state* s) {
    unsigned long long* w = guest_ptr<unsigned long long>(a);
    if (w) *w = nvngfx_blend_pack(s);
}
bool read_color(uint64_t a, nvngfx_color_state* s) {
    const unsigned* w = guest_ptr<const unsigned>(a);
    if (!w) return false;
    nvngfx_color_unpack(s, *w);
    return true;
}
void write_color(uint64_t a, const nvngfx_color_state* s) {
    unsigned* w = guest_ptr<unsigned>(a);
    if (w) *w = nvngfx_color_pack(s);
}
bool read_channel(uint64_t a, nvngfx_channel_mask_state* s) {
    const unsigned* w = guest_ptr<const unsigned>(a);
    if (!w) return false;
    nvngfx_channel_mask_unpack(s, *w);
    return true;
}
void write_channel(uint64_t a, const nvngfx_channel_mask_state* s) {
    unsigned* w = guest_ptr<unsigned>(a);
    if (w) *w = nvngfx_channel_mask_pack(s);
}
bool read_multisample(uint64_t a, nvngfx_multisample_state* s) {
    const unsigned char* w = guest_ptr<const unsigned char>(a);
    if (!w) return false;
    nvngfx_multisample_unpack(s, w);
    return true;
}
void write_multisample(uint64_t a, const nvngfx_multisample_state* s) {
    unsigned char* w = guest_ptr<unsigned char>(a);
    if (w) nvngfx_multisample_pack(s, w);
}
bool read_polygon(uint64_t a, nvngfx_polygon_state* s) {
    const unsigned* w = guest_ptr<const unsigned>(a);
    if (!w) return false;
    nvngfx_polygon_unpack(s, *w);
    return true;
}
void write_polygon(uint64_t a, const nvngfx_polygon_state* s) {
    unsigned* w = guest_ptr<unsigned>(a);
    if (w) *w = nvngfx_polygon_pack(s);
}
bool read_depth(uint64_t a, nvngfx_depth_stencil_state* s) {
    const unsigned long long* w = guest_ptr<const unsigned long long>(a);
    if (!w) return false;
    nvngfx_depth_stencil_unpack(s, *w);
    return true;
}
void write_depth(uint64_t a, const nvngfx_depth_stencil_state* s) {
    unsigned long long* w = guest_ptr<unsigned long long>(a);
    if (w) *w = nvngfx_depth_stencil_pack(s);
}

/* nvnBlendStateSetDefaults / Target / Func / Equation */
void blend_set_defaults(nvngfx_ctx*, uint64_t* x) {
    nvngfx_blend_state s;
    nvngfx_blend_defaults(&s);
    write_blend(x[0], &s);
}
void blend_set_target(nvngfx_ctx*, uint64_t* x) {
    nvngfx_blend_state s;
    if (!read_blend(x[0], &s)) return;
    s.target = static_cast<unsigned>(x[1]);
    write_blend(x[0], &s);
}
void blend_set_func(nvngfx_ctx*, uint64_t* x) {
    nvngfx_blend_state s;
    if (!read_blend(x[0], &s)) return;
    s.src_rgb = static_cast<unsigned>(x[1]);
    s.dst_rgb = static_cast<unsigned>(x[2]);
    s.src_a = static_cast<unsigned>(x[3]);
    s.dst_a = static_cast<unsigned>(x[4]);
    write_blend(x[0], &s);
}
void blend_set_equation(nvngfx_ctx*, uint64_t* x) {
    nvngfx_blend_state s;
    if (!read_blend(x[0], &s)) return;
    s.eq_rgb = static_cast<unsigned>(x[1]);
    s.eq_a = static_cast<unsigned>(x[2]);
    write_blend(x[0], &s);
}

/* nvnColorStateSetDefaults / BlendEnable / LogicOp / AlphaTest */
void color_set_defaults(nvngfx_ctx*, uint64_t* x) {
    nvngfx_color_state s;
    nvngfx_color_defaults(&s);
    write_color(x[0], &s);
}
void color_set_blend_enable(nvngfx_ctx*, uint64_t* x) {
    nvngfx_color_state s;
    if (!read_color(x[0], &s)) return;
    const unsigned i = static_cast<unsigned>(x[1]);
    if (i >= 8) { log_once("nvnColorStateSetBlendEnable.target", i); return; }
    const unsigned mask = 1u << i;
    if (x[2] & 0xFFu) s.blend_en |= mask; else s.blend_en &= ~mask;
    write_color(x[0], &s);
}
void color_set_logic_op(nvngfx_ctx*, uint64_t* x) {
    nvngfx_color_state s;
    if (!read_color(x[0], &s)) return;
    s.logic_op = static_cast<unsigned>(x[1]);
    write_color(x[0], &s);
}
void color_set_alpha_test(nvngfx_ctx*, uint64_t* x) {
    nvngfx_color_state s;
    if (!read_color(x[0], &s)) return;
    s.alpha_func = static_cast<unsigned>(x[1]);
    write_color(x[0], &s);
}

/* nvnChannelMaskStateSetDefaults / SetChannelMask */
void channel_set_defaults(nvngfx_ctx*, uint64_t* x) {
    nvngfx_channel_mask_state s;
    nvngfx_channel_mask_defaults(&s);
    write_channel(x[0], &s);
}
void channel_set_mask(nvngfx_ctx*, uint64_t* x) {
    nvngfx_channel_mask_state s;
    if (!read_channel(x[0], &s)) return;
    const unsigned i = static_cast<unsigned>(x[1]);
    if (i >= 8) { log_once("nvnChannelMaskStateSetChannelMask.target", i); return; }
    const unsigned nib = ((x[2] & 0xFFu) ? 1u : 0u) |
                         ((x[3] & 0xFFu) ? 2u : 0u) |
                         ((x[4] & 0xFFu) ? 4u : 0u) |
                         ((x[5] & 0xFFu) ? 8u : 0u);
    const unsigned sh = 4u * i;
    s.mask = (s.mask & ~(0xFu << sh)) | (nib << sh);
    write_channel(x[0], &s);
}

/* nvnMultisampleStateSet*: ten one-byte fields in declaration order. */
void multisample_set_defaults(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    nvngfx_multisample_defaults(&s);
    write_multisample(x[0], &s);
}
void multisample_set_enable(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    if (!read_multisample(x[0], &s)) return;
    s.enable = static_cast<unsigned char>(x[1] & 0xFFu);
    write_multisample(x[0], &s);
}
void multisample_set_samples(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    if (!read_multisample(x[0], &s)) return;
    s.samples = static_cast<unsigned char>(x[1] & 0xFFu);
    write_multisample(x[0], &s);
}
void multisample_set_a2c(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    if (!read_multisample(x[0], &s)) return;
    s.a2c = static_cast<unsigned char>(x[1] & 0xFFu);
    write_multisample(x[0], &s);
}
void multisample_set_dither(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    if (!read_multisample(x[0], &s)) return;
    s.a2c_dither = static_cast<unsigned char>(x[1] & 0xFFu);
    write_multisample(x[0], &s);
}
void multisample_set_raster_samples(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    if (!read_multisample(x[0], &s)) return;
    s.raster_samples = static_cast<unsigned char>(x[1] & 0xFFu);
    write_multisample(x[0], &s);
}
void multisample_set_cov_mod(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    if (!read_multisample(x[0], &s)) return;
    s.cov_mod = static_cast<unsigned char>(x[1] & 0xFFu);
    write_multisample(x[0], &s);
}
void multisample_set_cov2color_en(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    if (!read_multisample(x[0], &s)) return;
    s.cov2color_en = static_cast<unsigned char>(x[1] & 0xFFu);
    write_multisample(x[0], &s);
}
void multisample_set_cov2color_out(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    if (!read_multisample(x[0], &s)) return;
    s.cov2color_out = static_cast<unsigned char>(x[1] & 0xFFu);
    write_multisample(x[0], &s);
}
void multisample_set_sl_enable(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    if (!read_multisample(x[0], &s)) return;
    s.sl_en = static_cast<unsigned char>(x[1] & 0xFFu);
    write_multisample(x[0], &s);
}
void multisample_set_sl_grid(nvngfx_ctx*, uint64_t* x) {
    nvngfx_multisample_state s;
    if (!read_multisample(x[0], &s)) return;
    s.sl_grid = static_cast<unsigned char>(x[1] & 0xFFu);
    write_multisample(x[0], &s);
}
void multisample_set_sample_locations(nvngfx_ctx*, uint64_t* x) {
    /* The locations are not part of the 10-byte bindable state.  Recording
     * them would require a side table keyed by a guest address that the
     * guest may immediately copy; report once instead of pretending. */
    log_once("nvnMultisampleStateSetSampleLocations.ignored",
             static_cast<unsigned>(x[2]));
}

/* nvnPolygonStateSetDefaults / CullFace / FrontFace / Mode / OffsetEnables */
void polygon_set_defaults(nvngfx_ctx*, uint64_t* x) {
    nvngfx_polygon_state s;
    nvngfx_polygon_defaults(&s);
    write_polygon(x[0], &s);
}
void polygon_set_cull(nvngfx_ctx*, uint64_t* x) {
    nvngfx_polygon_state s;
    if (!read_polygon(x[0], &s)) return;
    s.cull = static_cast<unsigned>(x[1]);
    write_polygon(x[0], &s);
}
void polygon_set_front(nvngfx_ctx*, uint64_t* x) {
    nvngfx_polygon_state s;
    if (!read_polygon(x[0], &s)) return;
    s.front = static_cast<unsigned>(x[1]);
    write_polygon(x[0], &s);
}
void polygon_set_mode(nvngfx_ctx*, uint64_t* x) {
    nvngfx_polygon_state s;
    if (!read_polygon(x[0], &s)) return;
    s.mode = static_cast<unsigned>(x[1]);
    write_polygon(x[0], &s);
}
void polygon_set_offset(nvngfx_ctx*, uint64_t* x) {
    nvngfx_polygon_state s;
    if (!read_polygon(x[0], &s)) return;
    s.offset_en = static_cast<unsigned>(x[1]);
    write_polygon(x[0], &s);
}

/* nvnDepthStencilStateSet* */
void depth_set_defaults(nvngfx_ctx*, uint64_t* x) {
    nvngfx_depth_stencil_state s;
    nvngfx_depth_stencil_defaults(&s);
    write_depth(x[0], &s);
}
void depth_set_test(nvngfx_ctx*, uint64_t* x) {
    nvngfx_depth_stencil_state s;
    if (!read_depth(x[0], &s)) return;
    s.dt = (x[1] & 0xFFu) ? 1u : 0u;
    write_depth(x[0], &s);
}
void depth_set_write(nvngfx_ctx*, uint64_t* x) {
    nvngfx_depth_stencil_state s;
    if (!read_depth(x[0], &s)) return;
    s.dw = (x[1] & 0xFFu) ? 1u : 0u;
    write_depth(x[0], &s);
}
void depth_set_func(nvngfx_ctx*, uint64_t* x) {
    nvngfx_depth_stencil_state s;
    if (!read_depth(x[0], &s)) return;
    s.df = static_cast<unsigned>(x[1]);
    write_depth(x[0], &s);
}
void depth_set_stencil_test(nvngfx_ctx*, uint64_t* x) {
    nvngfx_depth_stencil_state s;
    if (!read_depth(x[0], &s)) return;
    s.st = (x[1] & 0xFFu) ? 1u : 0u;
    write_depth(x[0], &s);
}
void depth_set_stencil_func(nvngfx_ctx*, uint64_t* x) {
    nvngfx_depth_stencil_state s;
    if (!read_depth(x[0], &s)) return;
    const unsigned face = static_cast<unsigned>(x[1]);
    if (face & 1u) s.sf_f = static_cast<unsigned>(x[2]);
    if (face & 2u) s.sf_b = static_cast<unsigned>(x[2]);
    write_depth(x[0], &s);
}
void depth_set_stencil_op(nvngfx_ctx*, uint64_t* x) {
    nvngfx_depth_stencil_state s;
    if (!read_depth(x[0], &s)) return;
    const unsigned face = static_cast<unsigned>(x[1]);
    if (face & 1u) {
        s.fail_f = static_cast<unsigned>(x[2]);
        s.zfail_f = static_cast<unsigned>(x[3]);
        s.zpass_f = static_cast<unsigned>(x[4]);
    }
    if (face & 2u) {
        s.fail_b = static_cast<unsigned>(x[2]);
        s.zfail_b = static_cast<unsigned>(x[3]);
        s.zpass_b = static_cast<unsigned>(x[4]);
    }
    write_depth(x[0], &s);
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
        /* The pipeline declares attachment formats only when a depth/stencil
         * texture is bound, so the pass has to attach the same texture(s) or
         * the two disagree.  There is no ClearDepthStencil path yet, hence
         * Load: an uncleared pass keeps whatever was in the texture. */
        Tex* dt = b->depth ? find<Tex>(b->depth) : nullptr;
        if (dt && dt->mtl) {
            const MTLPixelFormat df = dt->mtl.pixelFormat;
            if (mtl_format_has_depth(df)) {
                rpd.depthAttachment.texture = dt->mtl;
                rpd.depthAttachment.loadAction = MTLLoadActionLoad;
                rpd.depthAttachment.storeAction = MTLStoreActionStore;
            }
            if (mtl_format_has_stencil(df)) {
                rpd.stencilAttachment.texture = dt->mtl;
                rpd.stencilAttachment.loadAction = MTLLoadActionLoad;
                rpd.stencilAttachment.storeAction = MTLStoreActionStore;
            }
        }
        b->enc = [b->cb renderCommandEncoderWithDescriptor:rpd];
        if (b->enc && b->hasViewport) [b->enc setViewport:b->viewport];
        if (b->enc && b->hasScissor) [b->enc setScissorRect:b->scissor];
        if (b->enc && b->hasBlendColor) {
            [b->enc setBlendColorRed:b->blendColor[0]
                                green:b->blendColor[1]
                                 blue:b->blendColor[2]
                                alpha:b->blendColor[3]];
        }
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
    /* The cached pipeline belongs to the state and targets of the previous
     * recording.  Both were just reset, so it must not be reused. */
    b->pso = nil;
    b->psoHasDepthStencil = false;
    b->psoHasDepth = false;
    b->psoHasStencil = false;
    b->pendingClear = false;
    b->argbuf[0] = b->argbuf[1] = nil;
    b->hasViewport = false;
    b->hasScissor = false;
    for (unsigned i = 0; i < NVNGFX_MAX_COLOR_TARGETS; ++i) b->hasBlendState[i] = false;
    b->hasColorState = b->hasChannelState = false;
    b->dss = nil;
    b->dssKey = 0;
    b->dssValid = false;
    b->hasMultisampleState = b->hasPolygonState = b->hasDepthState = false;
    b->hasBlendColor = false;
    b->hasAlphaRef = false;
    b->hasRenderEnable = false;
    b->hasCoverageTable = false;
    b->hasViewportSwizzle = false;
    b->hasPolyOffset = false;
    b->depthNear = 0.0;
    b->depthFar = 1.0;
    for (unsigned si = 0; si < 6; ++si) {
        for (unsigned i = 0; i < NVNGFX_MAX_UBO_SLOTS; ++i) {
            b->storage[si][i] = 0;
            b->storageSize[si][i] = 0;
        }
        for (unsigned i = 0; i < NVNGFX_MAX_TEXTURE_SLOTS; ++i)
            b->image[si][i] = 0;
    }
    for (unsigned i = 0; i < 16; ++i) {
        b->tfBuffer[i] = 0;
        b->tfSize[i] = 0;
    }
    for (unsigned i = 0; i < 4; ++i) b->viewportSwizzle[i] = 0;
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
    b->psoHasDepthStencil = false;
    b->psoHasDepth = false;
    b->psoHasStencil = false;
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


/* A raw 0..5 NVNshaderStage for tables that are recorded but not consumed by
 * the two live render stages yet.  Keep this separate from stage_index():
 * accepting compute here must not make the render path think it can use it. */
int record_stage_index(uint64_t stage) {
    return stage < 6 ? static_cast<int>(stage) : -1;
}

/* Fixed-function state binds.  Snapshot at call time: the guest reuses these
 * objects, and a pipeline built from them must see the values at the draw,
 * not the final values after recording. */
void cmdbuf_bind_blend_state(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    nvngfx_blend_state s = {};
    if (!b || !read_blend(x[1], &s)) return;
    const unsigned slot = s.target & (NVNGFX_MAX_COLOR_TARGETS - 1);
    s.target = slot;
    b->blendState[slot] = s;
    b->hasBlendState[slot] = true;
    b->pso = nil;
}
void cmdbuf_bind_color_state(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b || !read_color(x[1], &b->colorState)) return;
    b->hasColorState = true;
    b->pso = nil;
}
void cmdbuf_bind_channel_state(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b || !read_channel(x[1], &b->channelState)) return;
    b->hasChannelState = true;
    b->pso = nil;
}
void cmdbuf_bind_multisample_state(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b || !read_multisample(x[1], &b->multisampleState)) return;
    b->hasMultisampleState = true;
    b->pso = nil;
}
void cmdbuf_bind_polygon_state(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b || !read_polygon(x[1], &b->polygonState)) return;
    b->hasPolygonState = true;
    b->pso = nil;
}
void cmdbuf_bind_depth_state(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b || !read_depth(x[1], &b->depthState)) return;
    b->hasDepthState = true;
    b->dssValid = false;
    b->pso = nil;
}

/* nvnCommandBufferBindCoverageModulationTable(NVNcommandBuffer*, const float*) */
void cmdbuf_bind_coverage_table(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    const float* t = guest_ptr<const float>(x[1]);
    if (!b || !t) return;
    for (unsigned i = 0; i < 16; ++i) b->coverageTable[i] = t[i];
    b->hasCoverageTable = true;
    b->pso = nil;
}

/* nvnCommandBufferBindImage(s): recorded, but Metal image blocks / UAVs are
 * not wired into the argument buffer yet.  The handle is still kept so the
 * compute path can use it without another guest call. */
void cmdbuf_bind_image(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    const int si = record_stage_index(x[1]);
    const unsigned slot = static_cast<unsigned>(x[2]);
    if (si < 0 || slot >= NVNGFX_MAX_TEXTURE_SLOTS) {
        log_once("nvnCommandBufferBindImage.slot", static_cast<unsigned>(x[2]));
        return;
    }
    b->image[si][slot] = x[3];
    log_once("nvnCommandBufferBindImage.unsupported", static_cast<unsigned>(x[1]));
}
void cmdbuf_bind_images(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    const int si = record_stage_index(x[1]);
    const int first = static_cast<int>(x[2]);
    const int n = static_cast<int>(x[3]);
    const uint64_t* hs = guest_ptr<const uint64_t>(x[4]);
    if (si < 0 || first < 0 || n < 0 || first + n > NVNGFX_MAX_TEXTURE_SLOTS ||
        (n > 0 && !hs)) {
        log_once("nvnCommandBufferBindImages.range", static_cast<unsigned>(n));
        return;
    }
    for (int i = 0; i < n; ++i) b->image[si][first + i] = hs[i];
    log_once("nvnCommandBufferBindImages.unsupported", static_cast<unsigned>(x[1]));
}

/* nvnCommandBufferBindStorageBuffer(s): recorded for compute. */
void cmdbuf_bind_storage_buffer(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    const int si = record_stage_index(x[1]);
    const unsigned slot = static_cast<unsigned>(x[2]);
    if (si < 0 || slot >= NVNGFX_MAX_UBO_SLOTS) {
        log_once("nvnCommandBufferBindStorageBuffer.slot", static_cast<unsigned>(x[2]));
        return;
    }
    b->storage[si][slot] = x[3];
    b->storageSize[si][slot] = x[4];
    log_once("nvnCommandBufferBindStorageBuffer.unsupported", static_cast<unsigned>(x[1]));
}
void cmdbuf_bind_storage_buffers(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    const int si = record_stage_index(x[1]);
    const int first = static_cast<int>(x[2]);
    const int n = static_cast<int>(x[3]);
    const uint64_t* rg = guest_ptr<const uint64_t>(x[4]);
    if (si < 0 || first < 0 || n < 0 || first + n > NVNGFX_MAX_UBO_SLOTS ||
        (n > 0 && !rg)) {
        log_once("nvnCommandBufferBindStorageBuffers.range", static_cast<unsigned>(n));
        return;
    }
    for (int i = 0; i < n; ++i) {
        b->storage[si][first + i] = rg[2 * i];
        b->storageSize[si][first + i] = rg[2 * i + 1];
    }
    log_once("nvnCommandBufferBindStorageBuffers.unsupported", static_cast<unsigned>(x[1]));
}

/* nvnCommandBufferBindTransformFeedbackBuffer(s): recorded for the later
 * transform-feedback path. */
void cmdbuf_bind_tf_buffer(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    const int slot = static_cast<int>(x[1]);
    if (!b || slot < 0 || slot >= 16) {
        log_once("nvnCommandBufferBindTransformFeedbackBuffer.slot",
                 static_cast<unsigned>(x[1]));
        return;
    }
    b->tfBuffer[slot] = x[2];
    b->tfSize[slot] = x[3];
}
void cmdbuf_bind_tf_buffers(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    const int first = static_cast<int>(x[1]);
    const int n = static_cast<int>(x[2]);
    const uint64_t* rg = guest_ptr<const uint64_t>(x[3]);
    if (!b || first < 0 || n < 0 || first + n > 16 || (n > 0 && !rg)) {
        log_once("nvnCommandBufferBindTransformFeedbackBuffers.range",
                 static_cast<unsigned>(n));
        return;
    }
    for (int i = 0; i < n; ++i) {
        b->tfBuffer[first + i] = rg[2 * i];
        b->tfSize[first + i] = rg[2 * i + 1];
    }
}

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
    /* A dirty texture ignores the one-shot flags: the guest rewrote it after
     * the previous upload.  Clear dirty before attempting the copy so a
     * failure is retried only after the next guest write, not on every draw. */
    if (!t->dirty && (t->uploaded || t->uploadTried)) return t->uploaded;
    t->dirty = false;
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
    const unsigned layers = L.layers ? L.layers : 1u;
    const bool transcoded = t->transcoded;
    const unsigned format = static_cast<unsigned>(t->desc.format);

    /* Mip dimensions are floor halved with a floor of one texel.  They are
     * only needed for BCn, whose guest image is block aligned but destination
     * RGBA8 texture is not. */
    auto mip_extent = [](unsigned base, unsigned level) {
        unsigned v = base ? base : 1u;
        for (unsigned i = 0; i < level && v > 1u; ++i) v >>= 1;
        return v;
    };
    auto size3 = [](size_t a, size_t b, size_t c, size_t* out) {
        if (a != 0 && b > SIZE_MAX / a) return false;
        const size_t ab = a * b;
        if (c != 0 && ab > SIZE_MAX / c) return false;
        *out = ab * c;
        return true;
    };

    /* Every (level, layer) needs its own staging region.  The GPU reads these
     * bytes when the command buffer executes, so reusing one region would make
     * every mip/slice observe whichever copy wrote it last.  The price is a
     * full CPU-side copy of this texture for the lifetime of the command
     * buffer; a GPU deswizzle path is future work, not an optimisation to
     * hide here.  A BCn texture stages RGBA8, not its smaller compressed
     * guest bytes, because that is what the blit destination holds. */
    size_t stagingTotal = 0, stagingLargest = 0, bcnPackedLargest = 0;
    for (unsigned l = 0; l < levels; ++l) {
        unsigned rb = 0, rows = 0, slices = 0;
        if (nvngfx_tex_level_geometry(&t->desc, l, 0, &rb, &rows, &slices,
                                      nullptr, nullptr, nullptr) != 0) {
            log_once("nvnTextureUpload.geometry", l);
            stagingTotal = 0;
            break;
        }
        size_t srcLevelBytes = 0, dstLevelBytes = 0;
        if (!size3(rb, rows, slices, &srcLevelBytes)) {
            log_once("nvnTextureUpload.stagingOverflow", l);
            stagingTotal = 0;
            break;
        }
        if (transcoded) {
            const unsigned w = mip_extent(static_cast<unsigned>(t->desc.width), l);
            const unsigned h = mip_extent(static_cast<unsigned>(t->desc.height), l);
            size_t rowBytes = 0;
            if (w == 0 || h == 0 ||
                !size3(static_cast<size_t>(w), 4u, 1u, &rowBytes) ||
                !size3(rowBytes, h, slices, &dstLevelBytes)) {
                log_once("nvnTextureUpload.stagingOverflow", l);
                stagingTotal = 0;
                break;
            }
            if (srcLevelBytes > bcnPackedLargest) bcnPackedLargest = srcLevelBytes;
        } else {
            dstLevelBytes = srcLevelBytes;
        }
        if (dstLevelBytes > stagingLargest) stagingLargest = dstLevelBytes;
        if (dstLevelBytes > SIZE_MAX / layers) {
            log_once("nvnTextureUpload.stagingOverflow", l);
            stagingTotal = 0;
            break;
        }
        const size_t layerTotal = dstLevelBytes * layers;
        if (layerTotal > SIZE_MAX - stagingTotal) {
            log_once("nvnTextureUpload.stagingOverflow", l);
            stagingTotal = 0;
            break;
        }
        stagingTotal += layerTotal;
    }
    if (stagingTotal == 0) return false;
    std::fprintf(stderr,
                 "[nvn] texture upload: pool=0x%llx offset=%lld levels=%u "
                 "layers=%u staging=%zu bytes (largest slice=%zu)\n",
                 static_cast<unsigned long long>(t->desc.pool),
                 t->desc.offset, levels, layers, stagingTotal, stagingLargest);
    if (transcoded) {
        std::fprintf(stderr,
                     "[nvn] texture upload transcode: NVNformat 0x%X BCn -> "
                     "RGBA8\n",
                     format);
    }
    std::fflush(stderr);
    id<MTLBuffer> stg = [c->device newBufferWithLength:stagingTotal
                                               options:MTLResourceStorageModeShared];
    if (!stg) {
        log_once("nvnTextureUpload.staging", 1);
        return false;
    }
    /* The deswizzle still produces the guest's packed BCn rows; decode into
     * the shared staging buffer afterwards.  One scratch region is enough
     * because levels and layers are processed sequentially. */
    std::vector<unsigned char> bcnPacked;
    if (transcoded) {
        bcnPacked.resize(bcnPackedLargest);
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
    size_t levelStage = 0;
    for (unsigned l = 0; l < levels; ++l) {
        unsigned rb = 0, rows = 0, slices = 0;
        if (nvngfx_tex_level_geometry(&t->desc, l, 0, &rb, &rows, &slices,
                                      nullptr, nullptr, nullptr) != 0) {
            log_once("nvnTextureUpload.geometry", l);
            break;
        }
        const unsigned bpb = L.bytes_per_block ? L.bytes_per_block : 1u;
        const unsigned bw = L.block_width ? L.block_width : 1u;
        const unsigned bh = L.block_height ? L.block_height : 1u;
        unsigned lw = 0, lh = 0;
        size_t rowBytes = 0, dstLevelBytes = 0, srcLevelBytes = 0;
        if (!size3(rb, rows, slices, &srcLevelBytes)) {
            log_once("nvnTextureUpload.stagingOverflow", l);
            break;
        }
        if (transcoded) {
            lw = mip_extent(static_cast<unsigned>(t->desc.width), l);
            lh = mip_extent(static_cast<unsigned>(t->desc.height), l);
            if (lw == 0 || lh == 0 ||
                !size3(static_cast<size_t>(lw), 4u, 1u, &rowBytes) ||
                !size3(rowBytes, lh, slices, &dstLevelBytes)) {
                log_once("nvnTextureUpload.blockEdge", l);
                break;
            }
            if (srcLevelBytes > bcnPacked.size()) {
                log_once("nvnTextureUpload.bcnScratch", l);
                break;
            }
        } else {
            /* Derive the copy extent from the layout, not from the raw NVN
             * width/height.  For a 1D array the raw height is the layer count;
             * for ASTC the texel extent must be block aligned; and a cube
             * array has six times as many slices as its depth.  Row/block
             * counts already include all three rules. */
            lw = (rb / bpb) * bw;
            lh = rows * bh;
            rowBytes = rb;
            dstLevelBytes = srcLevelBytes;
        }
        if (lw == 0 || lh == 0) {
            log_once("nvnTextureUpload.blockEdge", l);
            break;
        }
        bool levelComplete = true;
        for (unsigned layer = 0; layer < layers; ++layer) {
            const size_t at = levelStage + static_cast<size_t>(layer) * dstLevelBytes;
            unsigned char* const layerBytes =
                static_cast<unsigned char*>([stg contents]) + at;
            if (transcoded) {
                if (nvngfx_tex_deswizzle(&t->desc, l, layer, base,
                                         bcnPacked.data(), rb) != 0) {
                    log_once("nvnTextureUpload.deswizzle", l);
                    levelComplete = false;
                    break;
                }
                const size_t srcSlice = static_cast<size_t>(rb) * rows;
                const size_t dstSlice = rowBytes * lh;
                for (unsigned z = 0; z < slices; ++z) {
                    if (nvngfx_bcn_decode_image(
                            t->desc.format, bcnPacked.data() +
                                                 static_cast<size_t>(z) * srcSlice,
                            rb, lw, lh,
                            layerBytes + static_cast<size_t>(z) * dstSlice,
                            rowBytes) != 0) {
                        log_once("nvnTextureUpload.bcnDecode", l);
                        levelComplete = false;
                        break;
                    }
                }
                if (!levelComplete) break;
            } else if (nvngfx_tex_deswizzle(&t->desc, l, layer, base,
                                            layerBytes, rb) != 0) {
                log_once("nvnTextureUpload.deswizzle", l);
                levelComplete = false;
                break;
            }
            [blit copyFromBuffer:stg
                    sourceOffset:static_cast<NSUInteger>(at)
               sourceBytesPerRow:rowBytes
             sourceBytesPerImage:rowBytes * lh
                      sourceSize:MTLSizeMake(lw, lh, slices)
                       toTexture:t->mtl
                destinationSlice:layer
                destinationLevel:l
               destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
        if (!levelComplete) break;
        levelStage += dstLevelBytes * layers;
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
    apply_fixed_state(c, b);
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
    apply_fixed_state(c, b);
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
                   static_cast<double>(static_cast<int>(x[4])),
                   b->depthNear, b->depthFar};
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
/* remaining M4 command-buffer state                                   */
/* ------------------------------------------------------------------ */

/* nvnCommandBufferClearColori / ClearColorui.
 *
 * Metal's load-action clear takes doubles, not integer components.  Applying
 * an integer clear through that path would depend on the attachment's pixel
 * format and would quietly reinterpret the bits.  These two calls therefore
 * record that a clear happened and report once; the float entry point above
 * remains the implemented clear. */
void cmdbuf_clear_color_i(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    log_once("nvnCommandBufferClearColori.unsupported", static_cast<unsigned>(x[1]));
}
void cmdbuf_clear_color_ui(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    log_once("nvnCommandBufferClearColorui.unsupported", static_cast<unsigned>(x[1]));
}

/* nvnCommandBufferClearDepthStencil(cb, float depth, uint32_t depthMask,
 *                                  int stencil, int stencilMask)
 * The current pass has no depth attachment, so a clear cannot be encoded. */
void cmdbuf_clear_depth_stencil(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    nvngfx_args* a = reinterpret_cast<nvngfx_args*>(x);
    log_once("nvnCommandBufferClearDepthStencil.unsupported",
             static_cast<unsigned>(nvngfx_arg_f32(a, 0) * 1000.0f));
    (void)x[1]; (void)x[2]; (void)x[3];
}

/* nvnCommandBufferClearBuffer(cb, address, size, uint32_t value) */
void cmdbuf_clear_buffer(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    log_once("nvnCommandBufferClearBuffer.unsupported", static_cast<unsigned>(x[3]));
}

/* nvnCommandBufferClearTexture{,i,ui}: the texture clear path is not wired
 * to a Metal blit yet.  Keep the call visible instead of silently dropping. */
void cmdbuf_clear_texture(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    log_once("nvnCommandBufferClearTexture.unsupported", static_cast<unsigned>(x[1] != 0));
}
void cmdbuf_clear_texture_i(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    log_once("nvnCommandBufferClearTexturei.unsupported", static_cast<unsigned>(x[1] != 0));
}
void cmdbuf_clear_texture_ui(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    log_once("nvnCommandBufferClearTextureui.unsupported", static_cast<unsigned>(x[1] != 0));
}

/* nvnCommandBufferSetAlphaRef(cb, float) */
void cmdbuf_set_alpha_ref(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    nvngfx_args* a = reinterpret_cast<nvngfx_args*>(x);
    b->alphaRef = nvngfx_arg_f32(a, 0);
    b->hasAlphaRef = true;
    log_once("nvnCommandBufferSetAlphaRef.unconsumed",
             static_cast<unsigned>(b->alphaRef * 1000.0f));
}

/* nvnCommandBufferSetBlendColor(cb, const float rgba[4]) */
void cmdbuf_set_blend_color(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    const float* rgba = guest_ptr<const float>(x[1]);
    if (!b || !rgba) return;
    for (unsigned i = 0; i < 4; ++i) b->blendColor[i] = rgba[i];
    b->hasBlendColor = true;
    if (b->enc) {
        [b->enc setBlendColorRed:b->blendColor[0]
                            green:b->blendColor[1]
                             blue:b->blendColor[2]
                            alpha:b->blendColor[3]];
    }
}

/* nvnCommandBufferSetPolygonOffsetClamp(cb, float factor, float units,
 *     float clamp).  Metal's setDepthBias takes the same three numbers; the
 *     units are the constant bias, the factor the slope scale. */
void cmdbuf_set_polygon_offset(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    nvngfx_args* a = reinterpret_cast<nvngfx_args*>(x);
    b->polyOffsetFactor = nvngfx_arg_f32(a, 0);
    b->polyOffsetUnits = nvngfx_arg_f32(a, 1);
    b->polyOffsetClamp = nvngfx_arg_f32(a, 2);
    b->hasPolyOffset = true;
}

/* nvnCommandBufferSetDepthRange(cb, float near, float far) */
void cmdbuf_set_depth_range(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    nvngfx_args* a = reinterpret_cast<nvngfx_args*>(x);
    b->depthNear = static_cast<double>(nvngfx_arg_f32(a, 0));
    b->depthFar = static_cast<double>(nvngfx_arg_f32(a, 1));
    if (b->hasViewport) {
        b->viewport.znear = b->depthNear;
        b->viewport.zfar = b->depthFar;
        if (b->enc) [b->enc setViewport:b->viewport];
    }
}

/* nvnCommandBufferSetRenderEnable(cb, uint32_t) */
void cmdbuf_set_render_enable(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    if (!b) return;
    b->renderEnable = static_cast<uint32_t>(x[1]);
    b->hasRenderEnable = true;
    log_once("nvnCommandBufferSetRenderEnable.recorded", b->renderEnable);
}

/* nvnCommandBufferSetScissors(cb, int first, int count, const int* rects)
 * NVNrectangle is four int32 fields.  The current pass has one scissor, so
 * the first rectangle is applied and a multi-rect call is reported. */
void cmdbuf_set_scissors(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    const int first = static_cast<int>(x[1]);
    const int n = static_cast<int>(x[2]);
    const int* r = guest_ptr<const int>(x[3]);
    if (!b || !r || n <= 0) return;
    if (first != 0 || n > 1) {
        log_once("nvnCommandBufferSetScissors.multi",
                 static_cast<unsigned>(first) * 32u + static_cast<unsigned>(n));
    }
    const int rx = r[0], ry = r[1], rw = r[2], rh = r[3];
    b->scissor = {static_cast<NSUInteger>(rx < 0 ? 0 : rx),
                  static_cast<NSUInteger>(ry < 0 ? 0 : ry),
                  static_cast<NSUInteger>(rw < 0 ? 0 : rw),
                  static_cast<NSUInteger>(rh < 0 ? 0 : rh)};
    b->hasScissor = true;
    if (b->enc) [b->enc setScissorRect:b->scissor];
}

/* nvnCommandBufferSetViewports(cb, int first, int count, const float* v)
 * Each NVNviewport is four floats (x, y, w, h).  Apply the first one and
 * report a multi-viewport call; the render path has one viewport. */
void cmdbuf_set_viewports(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    const int first = static_cast<int>(x[1]);
    const int n = static_cast<int>(x[2]);
    const float* v = guest_ptr<const float>(x[3]);
    if (!b || !v || n <= 0) return;
    if (first != 0 || n > 1) {
        log_once("nvnCommandBufferSetViewports.multi",
                 static_cast<unsigned>(first) * 32u + static_cast<unsigned>(n));
    }
    b->viewport = {static_cast<double>(v[0]), static_cast<double>(v[1]),
                   static_cast<double>(v[2]), static_cast<double>(v[3]),
                   b->depthNear, b->depthFar};
    b->hasViewport = true;
    if (b->enc) [b->enc setViewport:b->viewport];
}

/* nvnCommandBufferSetViewportSwizzles(cb, int first, int count,
 *                                     const NVNviewportSwizzle*)
 * Metal has no per-viewport axis swizzle; record the first one and report. */
void cmdbuf_set_viewport_swizzles(nvngfx_ctx*, uint64_t* x) {
    CmdBuf* b = find_or_add<CmdBuf>(x[0]);
    const int n = static_cast<int>(x[2]);
    const unsigned* v = guest_ptr<const unsigned>(x[3]);
    if (!b || !v || n <= 0) return;
    for (unsigned i = 0; i < 4; ++i) b->viewportSwizzle[i] = v[i];
    b->hasViewportSwizzle = true;
    log_once("nvnCommandBufferSetViewportSwizzles.unsupported", b->viewportSwizzle[0]);
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
    BIND("nvnTextureWriteTexels", tex_write_texels),
    BIND("nvnTextureWriteTexelsStrided", tex_write_texels_strided),
    BIND("nvnTextureFlushTexels", tex_flush_texels),

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

    BIND("nvnBlendStateSetDefaults", blend_set_defaults),
    BIND("nvnBlendStateSetBlendTarget", blend_set_target),
    BIND("nvnBlendStateSetBlendFunc", blend_set_func),
    BIND("nvnBlendStateSetBlendEquation", blend_set_equation),
    BIND("nvnColorStateSetDefaults", color_set_defaults),
    BIND("nvnColorStateSetBlendEnable", color_set_blend_enable),
    BIND("nvnColorStateSetLogicOp", color_set_logic_op),
    BIND("nvnColorStateSetAlphaTest", color_set_alpha_test),
    BIND("nvnChannelMaskStateSetDefaults", channel_set_defaults),
    BIND("nvnChannelMaskStateSetChannelMask", channel_set_mask),
    BIND("nvnMultisampleStateSetDefaults", multisample_set_defaults),
    BIND("nvnMultisampleStateSetMultisampleEnable", multisample_set_enable),
    BIND("nvnMultisampleStateSetSamples", multisample_set_samples),
    BIND("nvnMultisampleStateSetAlphaToCoverageEnable", multisample_set_a2c),
    BIND("nvnMultisampleStateSetAlphaToCoverageDither", multisample_set_dither),
    BIND("nvnMultisampleStateSetRasterSamples", multisample_set_raster_samples),
    BIND("nvnMultisampleStateSetCoverageModulationMode", multisample_set_cov_mod),
    BIND("nvnMultisampleStateSetCoverageToColorEnable", multisample_set_cov2color_en),
    BIND("nvnMultisampleStateSetCoverageToColorOutput", multisample_set_cov2color_out),
    BIND("nvnMultisampleStateSetSampleLocationsEnable", multisample_set_sl_enable),
    BIND("nvnMultisampleStateSetSampleLocationsGridEnable", multisample_set_sl_grid),
    BIND("nvnMultisampleStateSetSampleLocations", multisample_set_sample_locations),
    BIND("nvnPolygonStateSetDefaults", polygon_set_defaults),
    BIND("nvnPolygonStateSetCullFace", polygon_set_cull),
    BIND("nvnPolygonStateSetFrontFace", polygon_set_front),
    BIND("nvnPolygonStateSetPolygonMode", polygon_set_mode),
    BIND("nvnPolygonStateSetPolygonOffsetEnables", polygon_set_offset),
    BIND("nvnDepthStencilStateSetDefaults", depth_set_defaults),
    BIND("nvnDepthStencilStateSetDepthTestEnable", depth_set_test),
    BIND("nvnDepthStencilStateSetDepthWriteEnable", depth_set_write),
    BIND("nvnDepthStencilStateSetDepthFunc", depth_set_func),
    BIND("nvnDepthStencilStateSetStencilTestEnable", depth_set_stencil_test),
    BIND("nvnDepthStencilStateSetStencilFunc", depth_set_stencil_func),
    BIND("nvnDepthStencilStateSetStencilOp", depth_set_stencil_op),

    BIND("nvnCommandBufferBindBlendState", cmdbuf_bind_blend_state),
    BIND("nvnCommandBufferBindColorState", cmdbuf_bind_color_state),
    BIND("nvnCommandBufferBindChannelMaskState", cmdbuf_bind_channel_state),
    BIND("nvnCommandBufferBindMultisampleState", cmdbuf_bind_multisample_state),
    BIND("nvnCommandBufferBindPolygonState", cmdbuf_bind_polygon_state),
    BIND("nvnCommandBufferBindDepthStencilState", cmdbuf_bind_depth_state),
    BIND("nvnCommandBufferBindCoverageModulationTable", cmdbuf_bind_coverage_table),
    BIND("nvnCommandBufferBindStorageBuffer", cmdbuf_bind_storage_buffer),
    BIND("nvnCommandBufferBindStorageBuffers", cmdbuf_bind_storage_buffers),
    BIND("nvnCommandBufferBindImage", cmdbuf_bind_image),
    BIND("nvnCommandBufferBindImages", cmdbuf_bind_images),
    BIND("nvnCommandBufferBindTransformFeedbackBuffer", cmdbuf_bind_tf_buffer),
    BIND("nvnCommandBufferBindTransformFeedbackBuffers", cmdbuf_bind_tf_buffers),
    BIND("nvnCommandBufferClearColori", cmdbuf_clear_color_i),
    BIND("nvnCommandBufferClearColorui", cmdbuf_clear_color_ui),
    BIND("nvnCommandBufferClearDepthStencil", cmdbuf_clear_depth_stencil),
    BIND("nvnCommandBufferClearBuffer", cmdbuf_clear_buffer),
    BIND("nvnCommandBufferClearTexture", cmdbuf_clear_texture),
    BIND("nvnCommandBufferClearTexturei", cmdbuf_clear_texture_i),
    BIND("nvnCommandBufferClearTextureui", cmdbuf_clear_texture_ui),
    BIND("nvnCommandBufferSetAlphaRef", cmdbuf_set_alpha_ref),
    BIND("nvnCommandBufferSetBlendColor", cmdbuf_set_blend_color),
    BIND("nvnCommandBufferSetPolygonOffsetClamp", cmdbuf_set_polygon_offset),
    BIND("nvnCommandBufferSetDepthRange", cmdbuf_set_depth_range),
    BIND("nvnCommandBufferSetRenderEnable", cmdbuf_set_render_enable),
    BIND("nvnCommandBufferSetScissors", cmdbuf_set_scissors),
    BIND("nvnCommandBufferSetViewports", cmdbuf_set_viewports),
    BIND("nvnCommandBufferSetViewportSwizzles", cmdbuf_set_viewport_swizzles),

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
    /* The BCn draw's guest texture and builder.  The builder must be real
     * memory because tex_builder_set_defaults() writes it. */
    alignas(16) unsigned char texBcnObject[0x80] = {};
    alignas(16) unsigned char texBcnBuilder[0x80] = {};
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
    const uint64_t kTexBcn = reinterpret_cast<uint64_t>(texBcnObject);
    const uint64_t kTexBcnBuilder = reinterpret_cast<uint64_t>(texBcnBuilder);
    const uint64_t kTexPool = reinterpret_cast<uint64_t>(texPoolObject);
    const uint64_t kSampPool = reinterpret_cast<uint64_t>(sampPoolObject);
    const uint64_t kSamplerBuilder5 = reinterpret_cast<uint64_t>(samplerBuilder5);
    const uint64_t kSampler5 = reinterpret_cast<uint64_t>(samplerObject5);
    const uint64_t kCb = 0x5200;
    const uint64_t kProg = 0x5300;
    const NSUInteger kSide = 64;
    /* Backing store for the second draw's memory pool; freed at `done`. */
    void* poolRaw = nullptr;
    /* The cube draws need 3072 + 6144 bytes of block-aligned layers, more
     * than the last 4 KiB of the first page, so they get their own page and
     * their own NVN memory pool (0x7300).  Growing the first pool to 64 KiB
     * instead made the simulator app exit on the next draw (cause not
     * established); two 16 KiB pages keep every no-copy buffer at the size
     * that already worked. */
    void* poolRaw2 = nullptr;
    const uint64_t kCubePool = 0x7300;

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

        {
            /* --- Draw 10: MSL that came from a file ----------------------
             * Every draw so far used MSL compiled into this binary.  This one
             * follows the path a translated title shader will use: the
             * control+code of a "guest" shader hash to a key, that key names a
             * blob loaded from <app>/shaders/manifest.tsv, and the blob's MSL
             * builds the pipeline.  The fixture is synthetic (ios/nvn/shaders),
             * so no title data is involved. */
            if (nvngfx_shader_bundle_count() == 0) {
                const char* require = std::getenv("BOTW_REQUIRE_SHADER_BUNDLE");
                if (require && std::strcmp(require, "1") == 0) {
                    std::fprintf(stderr,
                                 "[selftest] draw 10: no MSL bundle installed "
                                 "(BOTW_REQUIRE_SHADER_BUNDLE=1)\n");
                    rc = 16397;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] draw 10 skipped: no MSL bundle installed\n");
            } else {
                alignas(16) unsigned char ctl10[0x880] = {};
                unsigned char code10[0x40];
                const uint32_t own10 = 0x880, ncode10 = 0x40;
                const uint64_t kProg10 = 0x6700;
                const uint64_t kCb10 = 0x6800;
                char key10[17] = {0};
                for (unsigned i = 0; i < sizeof code10; ++i)
                    code10[i] = static_cast<unsigned char>(i);
                std::memcpy(ctl10 + 0x20, &own10, sizeof own10);
                std::memcpy(ctl10 + 0x704, &ncode10, sizeof ncode10);
                if (nvngfx_shader_key_from_guest(ctl10, code10, key10) != 0) {
                    std::fprintf(stderr,
                                 "[selftest] draw 10: key computation failed\n");
                    rc = 16397;
                    goto done;
                }
                {
                    const char* why10 = nullptr;
                    if (!nvngfx_shader_lookup_blob(key10, NVNGFX_STAGE_VERTEX,
                                                   &why10) ||
                        !nvngfx_shader_lookup_blob(key10, NVNGFX_STAGE_FRAGMENT,
                                                   &why10)) {
                        std::fprintf(stderr,
                                     "[selftest] draw 10: no bundle blob for key "
                                     "%s (%s)\n",
                                     key10, why10 ? why10 : "?");
                        rc = 16397;
                        goto done;
                    }
                }
                struct { uint64_t data; uint64_t control; } sd10[2];
                sd10[0].data = reinterpret_cast<uint64_t>(code10);
                sd10[0].control = reinterpret_cast<uint64_t>(ctl10);
                sd10[1] = sd10[0];
                x[0] = kProg10; x[1] = 0;
                prog_initialize(ctx, x);
                x[0] = kProg10; x[1] = 2;
                x[2] = reinterpret_cast<uint64_t>(&sd10[0]);
                prog_set_shaders(ctx, x);
                if (x[0] != 1) {
                    std::fprintf(stderr, "[selftest] draw 10: SetShaders failed\n");
                    rc = 16397;
                    goto done;
                }
                {
                    Program* p10 = find<Program>(kProg10);
                    if (!p10 || std::strcmp(p10->key[0], key10) != 0 ||
                        std::strcmp(p10->key[1], key10) != 0) {
                        std::fprintf(stderr,
                                     "[selftest] draw 10: program key is not the "
                                     "guest key %s\n",
                                     key10);
                        rc = 16397;
                        goto done;
                    }
                }
                {
                    const uint64_t target10 = kTex;
                    const float clear10[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                    x[0] = kCb10;
                    cmdbuf_begin_recording(ctx, x);
                    x[0] = kCb10; x[1] = 1;
                    x[2] = reinterpret_cast<uint64_t>(&target10);
                    x[3] = 0; x[4] = 0; x[5] = 0;
                    cmdbuf_set_render_targets(ctx, x);
                    x[0] = kCb10; x[1] = 0;
                    x[2] = reinterpret_cast<uint64_t>(clear10);
                    x[3] = 0xF;
                    cmdbuf_clear_color(ctx, x);
                    x[0] = kCb10; x[1] = kProg10; x[2] = 0;
                    cmdbuf_bind_program(ctx, x);
                    x[0] = kCb10; x[1] = 0; x[2] = 0; x[3] = 3;
                    cmdbuf_draw_arrays(ctx, x);
                    x[0] = kCb10;
                    cmdbuf_end_recording(ctx, x);
                }
                CmdBuf* b10 = find<CmdBuf>(kCb10);
                if (!b10 || !b10->last) {
                    std::fprintf(stderr,
                                 "[selftest] draw 10: nothing was committed\n");
                    rc = 16398;
                    goto done;
                }
                [b10->last waitUntilCompleted];
                if (b10->last.status == MTLCommandBufferStatusError) {
                    std::fprintf(stderr,
                                 "[selftest] draw 10 command buffer error: %s\n",
                                 b10->last.error
                                     ? [[b10->last.error localizedDescription]
                                           UTF8String]
                                     : "(no description)");
                    rc = 16398;
                    goto done;
                }
                {
                    unsigned char px[4] = {0, 0, 0, 0};
                    const int wantOrange[4] = {255, 128, 0, 255};
                    if (!read_center_pixel(dev, c->queue, t->mtl, kSide, px) ||
                        !pixels_near(px, wantOrange, 2)) {
                        std::fprintf(stderr,
                                     "[selftest] draw 10 centre pixel is "
                                     "(%u,%u,%u,%u), want (%d,%d,%d,%d); the "
                                     "bundle MSL never reached the GPU\n",
                                     px[0], px[1], px[2], px[3], wantOrange[0],
                                     wantOrange[1], wantOrange[2], wantOrange[3]);
                        rc = 16398;
                        goto done;
                    }
                    std::fprintf(stderr,
                                 "[selftest] draw 10: MSL loaded from the bundle "
                                 "drew key %s\n",
                                 key10);
                }
            }
        }
        {
            /* --- Draw 11: a layered texture from guest memory -------------
             * The previous guest-texture draw was a single 2D image.  This
             * one is a two-layer RGBA8 2D array: layer 0 is red, layer 1 is
             * yellow, and the fragment shader explicitly samples layer 1.
             * Passing requires the descriptor to be MTLTextureType2DArray,
             * arrayLength to be 2, both guest slices to be deswizzled, and
             * Metal to accept destinationSlice:1 during upload. */
            alignas(16) unsigned char guestArrayBuilder[0x80] = {};
            const uint64_t kTexGuestArray = 0x6900;
            const uint64_t kProg11 = 0x6A00;
            const uint64_t kCb11 = 0x6B00;
            const uint64_t kB11 = reinterpret_cast<uint64_t>(guestArrayBuilder);
            const unsigned kArrayOffset = 12288;
            unsigned char* const vpool11 = reinterpret_cast<unsigned char*>(
                (reinterpret_cast<uintptr_t>(poolRaw) + 16383u) &
                ~static_cast<uintptr_t>(16383u));

            nvngfx_texdesc ad;
            nvngfx_texdesc_defaults(&ad);
            ad.target = 4;                       /* NVN_TEXTURE_TARGET_2D_ARRAY */
            ad.format = 0x25;                    /* RGBA8_UNORM */
            ad.width = 2;
            ad.height = 2;
            ad.depth = 2;
            ad.levels = 1;
            ad.samples = 1;
            ad.pool = 0x5400;
            ad.offset = kArrayOffset;
            {
                const unsigned char red[16] = {
                    255, 0, 0, 255, 255, 0, 0, 255,
                    255, 0, 0, 255, 255, 0, 0, 255};
                const unsigned char yellow[16] = {
                    255, 255, 0, 255, 255, 255, 0, 255,
                    255, 255, 0, 255, 255, 255, 0, 255};
                nvngfx_texlayout al;
                if (nvngfx_tex_layout(&ad, &al) != 0 || al.layers != 2 ||
                    al.size > 1024 ||
                    nvngfx_tex_swizzle(&ad, 0, 0, red, 8,
                                       vpool11 + kArrayOffset) != 0 ||
                    nvngfx_tex_swizzle(&ad, 0, 1, yellow, 8,
                                       vpool11 + kArrayOffset) != 0) {
                    std::fprintf(stderr,
                                 "[selftest] draw 11: guest array layout failed "
                                 "(layers=%u size=%llu)\n",
                                 al.layers,
                                 static_cast<unsigned long long>(al.size));
                    rc = 16399;
                    goto done;
                }
            }
            {
                x[0] = kB11;
                tex_builder_set_defaults(ctx, x);
                x[0] = kB11; x[1] = 4;
                tex_builder_set_target(ctx, x);
                x[0] = kB11; x[1] = 0x25;
                tex_builder_set_format(ctx, x);
                x[0] = kB11; x[1] = 2; x[2] = 2;
                tex_builder_set_size2d(ctx, x);
                x[0] = kB11; x[1] = 2;
                tex_builder_set_depth(ctx, x);
                x[0] = kB11; x[1] = 1;
                tex_builder_set_levels(ctx, x);
                x[0] = kB11; x[1] = 1;
                tex_builder_set_samples(ctx, x);
                x[0] = kB11; x[1] = 0x5400; x[2] = kArrayOffset;
                tex_builder_set_storage(ctx, x);
                x[0] = kTexGuestArray; x[1] = kB11;
                tex_initialize(ctx, x);
            }
            Tex* arrayTex = find<Tex>(kTexGuestArray);
            if (x[0] != 1 || !arrayTex || !arrayTex->mtl ||
                arrayTex->mtl.textureType != MTLTextureType2DArray ||
                arrayTex->mtl.arrayLength != 2 ||
                arrayTex->layers != 2) {
                std::fprintf(stderr,
                             "[selftest] draw 11: array texture is not "
                             "2DArray x2 (type=%lu array=%lu layers=%u)\n",
                             arrayTex
                                 ? static_cast<unsigned long>(
                                       arrayTex->mtl.textureType)
                                 : 0ul,
                             arrayTex
                                 ? static_cast<unsigned long>(
                                       arrayTex->mtl.arrayLength)
                                 : 0ul,
                             arrayTex ? arrayTex->layers : 0u);
                rc = 16399;
                goto done;
            }

            struct { uint64_t data; uint64_t control; } sd11[2];
            sd11[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 9)));
            sd11[0].control = 0;
            sd11[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 9)));
            sd11[1].control = 0;
            x[0] = kProg11; x[1] = 0;
            prog_initialize(ctx, x);
            x[0] = kProg11; x[1] = 2;
            x[2] = reinterpret_cast<uint64_t>(&sd11[0]);
            prog_set_shaders(ctx, x);
            if (x[0] != 1) {
                std::fprintf(stderr,
                             "[selftest] draw 11: SetShaders failed\n");
                rc = 16399;
                goto done;
            }
            x[0] = kTexPool; x[1] = 7; x[2] = kTexGuestArray; x[3] = 0;
            tex_pool_register_texture(ctx, x);

            const uint64_t target11 = kTex;
            const float clear11[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            x[0] = kCb11;
            cmdbuf_begin_recording(ctx, x);
            x[0] = kCb11; x[1] = 1;
            x[2] = reinterpret_cast<uint64_t>(&target11);
            x[3] = 0; x[4] = 0; x[5] = 0;
            cmdbuf_set_render_targets(ctx, x);
            x[0] = kCb11; x[1] = 0;
            x[2] = reinterpret_cast<uint64_t>(clear11);
            x[3] = 0xF;
            cmdbuf_clear_color(ctx, x);
            x[0] = kCb11; x[1] = kTexPool;
            cmdbuf_set_texture_pool(ctx, x);
            x[0] = kCb11; x[1] = kSampPool;
            cmdbuf_set_sampler_pool(ctx, x);
            x[0] = 0; x[1] = 7; x[2] = 9;
            dev_get_texture_handle(ctx, x);
            const uint64_t ahandle = x[0];
            x[0] = kCb11; x[1] = 1; x[2] = 0; x[3] = ahandle;
            cmdbuf_bind_texture(ctx, x);
            x[0] = kCb11; x[1] = kProg11; x[2] = 0;
            cmdbuf_bind_program(ctx, x);
            x[0] = kCb11; x[1] = 0; x[2] = 0; x[3] = 3;
            cmdbuf_draw_arrays(ctx, x);
            x[0] = kCb11;
            cmdbuf_end_recording(ctx, x);

            CmdBuf* b11 = find<CmdBuf>(kCb11);
            if (!b11 || !b11->last) {
                std::fprintf(stderr,
                             "[selftest] draw 11: nothing committed\n");
                rc = 16399;
                goto done;
            }
            [b11->last waitUntilCompleted];
            if (b11->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr,
                             "[selftest] draw 11 command buffer error: %s\n",
                             b11->last.error
                                 ? [[b11->last.error localizedDescription]
                                       UTF8String]
                                 : "(no description)");
                rc = 16399;
                goto done;
            }
            if (!arrayTex->uploaded || arrayTex->uploadedLevels != 1 ||
                b11->encoded != 1) {
                std::fprintf(stderr,
                             "[selftest] draw 11: array not used "
                             "(uploaded=%d levels=%u encoded=%u)\n",
                             arrayTex->uploaded ? 1 : 0,
                             arrayTex->uploadedLevels, b11->encoded);
                rc = 16399;
                goto done;
            }
            {
                unsigned char left[4] = {0, 0, 0, 0};
                unsigned char right[4] = {0, 0, 0, 0};
                const int wantRed[4] = {255, 0, 0, 255};
                const int wantYellow[4] = {255, 255, 0, 255};
                if (!read_pixel_at(dev, c->queue, t->mtl, kSide,
                                   kSide / 8, kSide / 2, left) ||
                    !read_pixel_at(dev, c->queue, t->mtl, kSide,
                                   (7 * kSide) / 8, kSide / 2, right)) {
                    std::fprintf(stderr,
                                 "[selftest] draw 11 readback failed\n");
                    rc = 16400;
                    goto done;
                }
                if (!pixels_near(left, wantRed, 2) ||
                    !pixels_near(right, wantYellow, 2)) {
                    std::fprintf(stderr,
                                 "[selftest] draw 11 layers are "
                                 "(%u,%u,%u,%u) / (%u,%u,%u,%u), want "
                                 "(%d,%d,%d,%d) / (%d,%d,%d,%d); both slices "
                                 "did not reach the shader\n",
                                 left[0], left[1], left[2], left[3],
                                 right[0], right[1], right[2], right[3],
                                 wantRed[0], wantRed[1], wantRed[2], wantRed[3],
                                 wantYellow[0], wantYellow[1], wantYellow[2],
                                 wantYellow[3]);
                    rc = 16400;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] draw 11: 2D array layers 0/1 sampled "
                             "from guest memory\n");
            }
        }

        {
            /* --- Draw 12: a cube map from guest memory --------------------
             * NVN's face order is +X, -X, +Y, -Y, +Z, -Z, so slices 4 and 5
             * are the two Z faces.  Metal's cube has one array entry and six
             * implicit slices, but the upload still has to name slice 4 and
             * slice 5; the other four faces are green so a wrong face is
             * visible instead of passing. */
            alignas(16) unsigned char guestCubeBuilder[0x80] = {};
            const uint64_t kTexGuestCube = 0x6C00;
            const uint64_t kProg12 = 0x6D00;
            const uint64_t kCb12 = 0x6E00;
            const uint64_t kB12 =
                reinterpret_cast<uint64_t>(guestCubeBuilder);
            const unsigned kCubeOffset = 0;
            /* Filled in after the malloc below: computing it from the still
             * null poolRaw2 produced a null page and memset() crashed the
             * app, which is what run 41077027673 hit right after draw 11. */
            unsigned char* vpool12 = nullptr;

            nvngfx_texdesc cd;
            nvngfx_texdesc_defaults(&cd);
            cd.target = 8;                       /* NVN_TEXTURE_TARGET_CUBEMAP */
            cd.format = 0x25;                    /* RGBA8_UNORM */
            cd.width = 2;
            cd.height = 2;
            cd.depth = 1;
            cd.levels = 1;
            cd.samples = 1;
            poolRaw2 = std::malloc(16384 + 16384);
            if (!poolRaw2) {
                std::fprintf(stderr,
                             "[selftest] draw 12: could not allocate the "
                             "cube pool page\n");
                rc = 16401;
                goto done;
            }
            vpool12 = reinterpret_cast<unsigned char*>(
                (reinterpret_cast<uintptr_t>(poolRaw2) + 16383u) &
                ~static_cast<uintptr_t>(16383u));
            std::memset(vpool12, 0, 16384);
            {
                alignas(16) unsigned char cubePoolBuilder[0x40] = {};
                const uint64_t kCubePoolB =
                    reinterpret_cast<uint64_t>(cubePoolBuilder);
                x[0] = kCubePoolB;
                pool_builder_set_defaults(ctx, x);
                x[0] = kCubePoolB;
                x[1] = reinterpret_cast<uint64_t>(vpool12);
                x[2] = 16384;
                pool_builder_set_storage(ctx, x);
                x[0] = kCubePool;
                x[1] = kCubePoolB;
                pool_initialize(ctx, x);
                if (x[0] != 1) {
                    std::fprintf(stderr,
                                 "[selftest] draw 12: cube pool init failed\n");
                    rc = 16401;
                    goto done;
                }
            }
            std::fprintf(stderr, "[selftest] trace: draw 12 pool ready\n");
            std::fflush(stderr);
            cd.pool = kCubePool;
            cd.offset = kCubeOffset;
            {
                const unsigned char red[16] = {
                    255, 0, 0, 255, 255, 0, 0, 255,
                    255, 0, 0, 255, 255, 0, 0, 255};
                const unsigned char yellow[16] = {
                    255, 255, 0, 255, 255, 255, 0, 255,
                    255, 255, 0, 255, 255, 255, 0, 255};
                const unsigned char green[16] = {
                    0, 255, 0, 255, 0, 255, 0, 255,
                    0, 255, 0, 255, 0, 255, 0, 255};
                nvngfx_texlayout cl = {};
                /* A layer is block-aligned, so a 2x2 RGBA8 face still
                 * occupies a full 512-byte layer: 6 * 512 for the cube. */
                bool ok = nvngfx_tex_layout(&cd, &cl) == 0 &&
                          cl.layers == 6 && cl.size == 512ull * cl.layers;
                for (unsigned face = 0; ok && face < 6; ++face) {
                    const unsigned char* const src =
                        face == 4 ? red : (face == 5 ? yellow : green);
                    ok = nvngfx_tex_swizzle(&cd, 0, face, src, 8,
                                            vpool12 + kCubeOffset) == 0;
                }
                if (!ok) {
                    std::fprintf(stderr,
                                 "[selftest] draw 12: guest cube layout failed "
                                 "(layers=%u size=%llu)\n",
                                 cl.layers,
                                 static_cast<unsigned long long>(cl.size));
                    rc = 16401;
                    goto done;
                }
            }
            {
                x[0] = kB12;
                tex_builder_set_defaults(ctx, x);
                x[0] = kB12; x[1] = 8;
                tex_builder_set_target(ctx, x);
                x[0] = kB12; x[1] = 0x25;
                tex_builder_set_format(ctx, x);
                x[0] = kB12; x[1] = 2; x[2] = 2;
                tex_builder_set_size2d(ctx, x);
                x[0] = kB12; x[1] = 1;
                tex_builder_set_depth(ctx, x);
                x[0] = kB12; x[1] = 1;
                tex_builder_set_levels(ctx, x);
                x[0] = kB12; x[1] = 1;
                tex_builder_set_samples(ctx, x);
                x[0] = kB12; x[1] = kCubePool; x[2] = kCubeOffset;
                tex_builder_set_storage(ctx, x);
                x[0] = kTexGuestCube; x[1] = kB12;
                tex_initialize(ctx, x);
            }
            std::fprintf(stderr, "[selftest] trace: draw 12 texture initialized\n");
            std::fflush(stderr);
            Tex* cubeTex = find<Tex>(kTexGuestCube);
            if (x[0] != 1 || !cubeTex || !cubeTex->mtl ||
                cubeTex->mtl.textureType != MTLTextureTypeCube ||
                cubeTex->mtl.arrayLength != 1 || cubeTex->layers != 6) {
                std::fprintf(stderr,
                             "[selftest] draw 12: cube texture is not "
                             "Cube x1 / 6 slices (type=%lu array=%lu "
                             "layers=%u)\n",
                             cubeTex
                                 ? static_cast<unsigned long>(
                                       cubeTex->mtl.textureType)
                                 : 0ul,
                             cubeTex
                                 ? static_cast<unsigned long>(
                                       cubeTex->mtl.arrayLength)
                                 : 0ul,
                             cubeTex ? cubeTex->layers : 0u);
                rc = 16401;
                goto done;
            }

            struct { uint64_t data; uint64_t control; } sd12[2];
            sd12[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 10)));
            sd12[0].control = 0;
            sd12[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 10)));
            sd12[1].control = 0;
            x[0] = kProg12; x[1] = 0;
            prog_initialize(ctx, x);
            x[0] = kProg12; x[1] = 2;
            x[2] = reinterpret_cast<uint64_t>(&sd12[0]);
            prog_set_shaders(ctx, x);
            if (x[0] != 1) {
                std::fprintf(stderr,
                             "[selftest] draw 12: SetShaders failed\n");
                rc = 16401;
                goto done;
            }
            std::fprintf(stderr, "[selftest] trace: draw 12 program ready\n");
            std::fflush(stderr);
            x[0] = kTexPool; x[1] = 6; x[2] = kTexGuestCube; x[3] = 0;
            tex_pool_register_texture(ctx, x);

            const uint64_t target12 = kTex;
            const float clear12[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            x[0] = kCb12;
            cmdbuf_begin_recording(ctx, x);
            x[0] = kCb12; x[1] = 1;
            x[2] = reinterpret_cast<uint64_t>(&target12);
            x[3] = 0; x[4] = 0; x[5] = 0;
            cmdbuf_set_render_targets(ctx, x);
            x[0] = kCb12; x[1] = 0;
            x[2] = reinterpret_cast<uint64_t>(clear12);
            x[3] = 0xF;
            cmdbuf_clear_color(ctx, x);
            x[0] = kCb12; x[1] = kTexPool;
            cmdbuf_set_texture_pool(ctx, x);
            x[0] = kCb12; x[1] = kSampPool;
            cmdbuf_set_sampler_pool(ctx, x);
            x[0] = 0; x[1] = 6; x[2] = 9;
            dev_get_texture_handle(ctx, x);
            const uint64_t chandle = x[0];
            x[0] = kCb12; x[1] = 1; x[2] = 0; x[3] = chandle;
            cmdbuf_bind_texture(ctx, x);
            x[0] = kCb12; x[1] = kProg12; x[2] = 0;
            cmdbuf_bind_program(ctx, x);
            x[0] = kCb12; x[1] = 0; x[2] = 0; x[3] = 3;
            cmdbuf_draw_arrays(ctx, x);
            x[0] = kCb12;
            cmdbuf_end_recording(ctx, x);

            std::fprintf(stderr, "[selftest] trace: draw 12 recorded\n");
            std::fflush(stderr);
            CmdBuf* b12 = find<CmdBuf>(kCb12);
            if (!b12 || !b12->last) {
                std::fprintf(stderr,
                             "[selftest] draw 12: nothing committed\n");
                rc = 16401;
                goto done;
            }
            std::fprintf(stderr, "[selftest] trace: draw 12 waiting\n");
            std::fflush(stderr);
            [b12->last waitUntilCompleted];
            std::fprintf(stderr, "[selftest] trace: draw 12 completed\n");
            std::fflush(stderr);
            if (b12->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr,
                             "[selftest] draw 12 command buffer error: %s\n",
                             b12->last.error
                                 ? [[b12->last.error localizedDescription]
                                       UTF8String]
                                 : "(no description)");
                rc = 16401;
                goto done;
            }
            if (!cubeTex->uploaded || cubeTex->uploadedLevels != 1 ||
                b12->encoded != 1) {
                std::fprintf(stderr,
                             "[selftest] draw 12: cube not used "
                             "(uploaded=%d levels=%u encoded=%u)\n",
                             cubeTex->uploaded ? 1 : 0,
                             cubeTex->uploadedLevels, b12->encoded);
                rc = 16401;
                goto done;
            }
            {
                unsigned char left[4] = {0, 0, 0, 0};
                unsigned char right[4] = {0, 0, 0, 0};
                const int wantRed[4] = {255, 0, 0, 255};
                const int wantYellow[4] = {255, 255, 0, 255};
                if (!read_pixel_at(dev, c->queue, t->mtl, kSide,
                                   kSide / 8, kSide / 2, left) ||
                    !read_pixel_at(dev, c->queue, t->mtl, kSide,
                                   (7 * kSide) / 8, kSide / 2, right)) {
                    std::fprintf(stderr,
                                 "[selftest] draw 12 readback failed\n");
                    rc = 16402;
                    goto done;
                }
                if (!pixels_near(left, wantRed, 2) ||
                    !pixels_near(right, wantYellow, 2)) {
                    std::fprintf(stderr,
                                 "[selftest] draw 12 +Z/-Z are "
                                 "(%u,%u,%u,%u) / (%u,%u,%u,%u), want "
                                 "(%d,%d,%d,%d) / (%d,%d,%d,%d); the cube "
                                 "faces did not reach the shader\n",
                                 left[0], left[1], left[2], left[3],
                                 right[0], right[1], right[2], right[3],
                                 wantRed[0], wantRed[1], wantRed[2], wantRed[3],
                                 wantYellow[0], wantYellow[1], wantYellow[2],
                                 wantYellow[3]);
                    rc = 16402;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] draw 12: cube faces 4/5 sampled "
                             "from guest memory\n");
            }
        }

        {
            /* --- Draw 13: a cube array from guest memory ------------------
             * Six slices per cube: `arrayLength` is 2 but the upload must
             * reach slice 11, the -Z face of cube 1.  Reading +Z of cube 0
             * and -Z of cube 1 on the two halves catches both a wrong
             * arrayLength and an upload that stopped after depth * 1. */
            alignas(16) unsigned char guestCubeArrayBuilder[0x80] = {};
            const uint64_t kTexGuestCubeArray = 0x6F00;
            const uint64_t kProg13 = 0x7000;
            const uint64_t kCb13 = 0x7100;
            const uint64_t kB13 =
                reinterpret_cast<uint64_t>(guestCubeArrayBuilder);
            const unsigned kCubeArrayOffset = 4096;
            std::fprintf(stderr, "[selftest] trace: draw 13 start\n");
            std::fflush(stderr);
            unsigned char* vpool13 = nullptr;
            if (!poolRaw2) {
                std::fprintf(stderr,
                             "[selftest] draw 13: the cube pool is missing\n");
                rc = 16403;
                goto done;
            }
            vpool13 = reinterpret_cast<unsigned char*>(
                (reinterpret_cast<uintptr_t>(poolRaw2) + 16383u) &
                ~static_cast<uintptr_t>(16383u));

            /* Creating an MTLTextureTypeCubeArray aborts this simulator's
             * Metal outright: run 41079561654 died inside
             * newTextureWithDescriptor with no NSException the @try in
             * tex_initialize could catch, and no crash report.  The layout and
             * the shader for this path are still covered on Windows by
             * nvn_texlayout_test / nvn_shader_test, so make the draw opt-in and
             * keep the simulator job honest instead of red. */
            const char* const cubeArrayEnv =
                std::getenv("BOTW_SELFTEST_CUBE_ARRAY");
            if (!cubeArrayEnv || *cubeArrayEnv == '\0' ||
                *cubeArrayEnv == '0') {
                std::fprintf(stderr,
                             "[selftest] draw 13 skipped: MTLTextureTypeCubeArray "
                             "aborts the iOS Simulator's Metal (layout and "
                             "shader are covered by the portable tests); set "
                             "BOTW_SELFTEST_CUBE_ARRAY=1 on a device to run "
                             "it\n");
            } else {

            nvngfx_texdesc ad13;
            nvngfx_texdesc_defaults(&ad13);
            ad13.target = 9;                  /* NVN_TEXTURE_TARGET_CUBEMAP_ARRAY */
            ad13.format = 0x25;               /* RGBA8_UNORM */
            ad13.width = 2;
            ad13.height = 2;
            ad13.depth = 2;
            ad13.levels = 1;
            ad13.samples = 1;
            ad13.pool = kCubePool;
            ad13.offset = kCubeArrayOffset;
            {
                const unsigned char red[16] = {
                    255, 0, 0, 255, 255, 0, 0, 255,
                    255, 0, 0, 255, 255, 0, 0, 255};
                const unsigned char yellow[16] = {
                    255, 255, 0, 255, 255, 255, 0, 255,
                    255, 255, 0, 255, 255, 255, 0, 255};
                const unsigned char green[16] = {
                    0, 255, 0, 255, 0, 255, 0, 255,
                    0, 255, 0, 255, 0, 255, 0, 255};
                nvngfx_texlayout al13 = {};
                bool ok = nvngfx_tex_layout(&ad13, &al13) == 0 &&
                          al13.layers == 12 &&
                          al13.size == 512ull * al13.layers;
                for (unsigned face = 0; ok && face < 12; ++face) {
                    const unsigned char* const src =
                        face == 4 ? red : (face == 11 ? yellow : green);
                    ok = nvngfx_tex_swizzle(&ad13, 0, face, src, 8,
                                            vpool13 + kCubeArrayOffset) == 0;
                }
                if (!ok) {
                    std::fprintf(stderr,
                                 "[selftest] draw 13: guest cube array layout "
                                 "failed (layers=%u size=%llu)\n",
                                 al13.layers,
                                 static_cast<unsigned long long>(al13.size));
                    rc = 16403;
                    goto done;
                }
            }
            std::fprintf(stderr, "[selftest] trace: draw 13 guest bytes ready\n");
            std::fflush(stderr);
            {
                x[0] = kB13;
                tex_builder_set_defaults(ctx, x);
                x[0] = kB13; x[1] = 9;
                tex_builder_set_target(ctx, x);
                x[0] = kB13; x[1] = 0x25;
                tex_builder_set_format(ctx, x);
                x[0] = kB13; x[1] = 2; x[2] = 2;
                tex_builder_set_size2d(ctx, x);
                x[0] = kB13; x[1] = 2;
                tex_builder_set_depth(ctx, x);
                x[0] = kB13; x[1] = 1;
                tex_builder_set_levels(ctx, x);
                x[0] = kB13; x[1] = 1;
                tex_builder_set_samples(ctx, x);
                x[0] = kB13; x[1] = kCubePool; x[2] = kCubeArrayOffset;
                tex_builder_set_storage(ctx, x);
                x[0] = kTexGuestCubeArray; x[1] = kB13;
                tex_initialize(ctx, x);
            }
            std::fprintf(stderr, "[selftest] trace: draw 13 texture initialized\n");
            std::fflush(stderr);
            Tex* cubeArrayTex = find<Tex>(kTexGuestCubeArray);
            std::fprintf(stderr,
                         "[selftest] trace: draw 13 texture type=%lu "
                         "array=%lu layers=%u\n",
                         cubeArrayTex
                             ? static_cast<unsigned long>(
                                   cubeArrayTex->mtl.textureType)
                             : 0ul,
                         cubeArrayTex
                             ? static_cast<unsigned long>(
                                   cubeArrayTex->mtl.arrayLength)
                             : 0ul,
                         cubeArrayTex ? cubeArrayTex->layers : 0u);
            std::fflush(stderr);
            if (x[0] != 1 || !cubeArrayTex || !cubeArrayTex->mtl ||
                cubeArrayTex->mtl.textureType != MTLTextureTypeCubeArray ||
                cubeArrayTex->mtl.arrayLength != 2 ||
                cubeArrayTex->layers != 12) {
                std::fprintf(stderr,
                             "[selftest] draw 13: cube array texture is not "
                             "CubeArray x2 / 12 slices (type=%lu array=%lu "
                             "layers=%u)\n",
                             cubeArrayTex
                                 ? static_cast<unsigned long>(
                                       cubeArrayTex->mtl.textureType)
                                 : 0ul,
                             cubeArrayTex
                                 ? static_cast<unsigned long>(
                                       cubeArrayTex->mtl.arrayLength)
                                 : 0ul,
                             cubeArrayTex ? cubeArrayTex->layers : 0u);
                rc = 16403;
                goto done;
            }

            struct { uint64_t data; uint64_t control; } sd13[2];
            sd13[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 11)));
            sd13[0].control = 0;
            sd13[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 11)));
            sd13[1].control = 0;
            x[0] = kProg13; x[1] = 0;
            prog_initialize(ctx, x);
            x[0] = kProg13; x[1] = 2;
            x[2] = reinterpret_cast<uint64_t>(&sd13[0]);
            prog_set_shaders(ctx, x);
            if (x[0] != 1) {
                std::fprintf(stderr,
                             "[selftest] draw 13: SetShaders failed\n");
                rc = 16403;
                goto done;
            }
            std::fprintf(stderr, "[selftest] trace: draw 13 program ready\n");
            std::fflush(stderr);
            x[0] = kTexPool; x[1] = 8; x[2] = kTexGuestCubeArray; x[3] = 0;
            tex_pool_register_texture(ctx, x);

            const uint64_t target13 = kTex;
            const float clear13[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            x[0] = kCb13;
            cmdbuf_begin_recording(ctx, x);
            x[0] = kCb13; x[1] = 1;
            x[2] = reinterpret_cast<uint64_t>(&target13);
            x[3] = 0; x[4] = 0; x[5] = 0;
            cmdbuf_set_render_targets(ctx, x);
            x[0] = kCb13; x[1] = 0;
            x[2] = reinterpret_cast<uint64_t>(clear13);
            x[3] = 0xF;
            cmdbuf_clear_color(ctx, x);
            x[0] = kCb13; x[1] = kTexPool;
            cmdbuf_set_texture_pool(ctx, x);
            x[0] = kCb13; x[1] = kSampPool;
            cmdbuf_set_sampler_pool(ctx, x);
            x[0] = 0; x[1] = 8; x[2] = 9;
            dev_get_texture_handle(ctx, x);
            const uint64_t cahandle = x[0];
            x[0] = kCb13; x[1] = 1; x[2] = 0; x[3] = cahandle;
            cmdbuf_bind_texture(ctx, x);
            x[0] = kCb13; x[1] = kProg13; x[2] = 0;
            cmdbuf_bind_program(ctx, x);
            x[0] = kCb13; x[1] = 0; x[2] = 0; x[3] = 3;
            cmdbuf_draw_arrays(ctx, x);
            x[0] = kCb13;
            cmdbuf_end_recording(ctx, x);

            std::fprintf(stderr, "[selftest] trace: draw 13 recorded\n");
            std::fflush(stderr);
            CmdBuf* b13 = find<CmdBuf>(kCb13);
            if (!b13 || !b13->last) {
                std::fprintf(stderr,
                             "[selftest] draw 13: nothing committed\n");
                rc = 16403;
                goto done;
            }
            std::fprintf(stderr, "[selftest] trace: draw 13 waiting\n");
            std::fflush(stderr);
            [b13->last waitUntilCompleted];
            std::fprintf(stderr, "[selftest] trace: draw 13 completed\n");
            std::fflush(stderr);
            if (b13->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr,
                             "[selftest] draw 13 command buffer error: %s\n",
                             b13->last.error
                                 ? [[b13->last.error localizedDescription]
                                       UTF8String]
                                 : "(no description)");
                rc = 16403;
                goto done;
            }
            if (!cubeArrayTex->uploaded || cubeArrayTex->uploadedLevels != 1 ||
                b13->encoded != 1) {
                std::fprintf(stderr,
                             "[selftest] draw 13: cube array not used "
                             "(uploaded=%d levels=%u encoded=%u)\n",
                             cubeArrayTex->uploaded ? 1 : 0,
                             cubeArrayTex->uploadedLevels, b13->encoded);
                rc = 16403;
                goto done;
            }
            {
                unsigned char left[4] = {0, 0, 0, 0};
                unsigned char right[4] = {0, 0, 0, 0};
                const int wantRed[4] = {255, 0, 0, 255};
                const int wantYellow[4] = {255, 255, 0, 255};
                if (!read_pixel_at(dev, c->queue, t->mtl, kSide,
                                   kSide / 8, kSide / 2, left) ||
                    !read_pixel_at(dev, c->queue, t->mtl, kSide,
                                   (7 * kSide) / 8, kSide / 2, right)) {
                    std::fprintf(stderr,
                                 "[selftest] draw 13 readback failed\n");
                    rc = 16404;
                    goto done;
                }
                if (!pixels_near(left, wantRed, 2) ||
                    !pixels_near(right, wantYellow, 2)) {
                    std::fprintf(stderr,
                                 "[selftest] draw 13 (cube0 +Z, cube1 -Z) are "
                                 "(%u,%u,%u,%u) / (%u,%u,%u,%u), want "
                                 "(%d,%d,%d,%d) / (%d,%d,%d,%d); slice 11 did "
                                 "not reach the shader\n",
                                 left[0], left[1], left[2], left[3],
                                 right[0], right[1], right[2], right[3],
                                 wantRed[0], wantRed[1], wantRed[2], wantRed[3],
                                 wantYellow[0], wantYellow[1], wantYellow[2],
                                 wantYellow[3]);
                    rc = 16404;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] draw 13: cube array layer 0 face 4 and "
                             "layer 1 face 5 sampled from guest memory\n");
            }
            }   /* BOTW_SELFTEST_CUBE_ARRAY */
        }

        {
            /* --- Draw 14: a LINEAR texture from guest memory --------------
             * Linear storage has no GOBs: row 1 starts at the descriptor's
             * stride, not right after row 0.  The guest bytes carry 16 bytes
             * of padding per 8-byte row, so a copy that ignored the stride
             * would read zeros for the second row and the bottom-left pixel
             * would come back black instead of blue. */
            alignas(16) unsigned char guestLinearBuilder[0x80] = {};
            const uint64_t kTexGuestLinear = 0x7500;
            const uint64_t kProg14 = 0x7600;
            const uint64_t kCb14 = 0x7700;
            const uint64_t kB14 = reinterpret_cast<uint64_t>(guestLinearBuilder);
            const unsigned kLinearOffset = 12288;
            unsigned char* const vpool14 =
                poolRaw2 ? reinterpret_cast<unsigned char*>(
                               (reinterpret_cast<uintptr_t>(poolRaw2) + 16383u) &
                               ~static_cast<uintptr_t>(16383u))
                         : nullptr;

            if (!vpool14) {
                std::fprintf(stderr,
                             "[selftest] draw 14: the cube pool is missing\n");
                rc = 16405;
                goto done;
            }

            nvngfx_texdesc ld;
            nvngfx_texdesc_defaults(&ld);
            ld.target = 0;                       /* plain 2D */
            ld.format = 0x25;                    /* RGBA8_UNORM */
            ld.width = 2;
            ld.height = 2;
            ld.depth = 1;
            ld.levels = 1;
            ld.samples = 1;
            ld.flags = 0x10;                     /* LINEAR */
            ld.stride = 32;                      /* 8 real bytes + 24 padding */
            ld.pool = kCubePool;
            ld.offset = kLinearOffset;
            {
                const unsigned char top[8] = {255, 0, 0, 255, 0, 255, 0, 255};
                const unsigned char bottom[8] = {0, 0, 255, 255,
                                                 255, 255, 255, 255};
                nvngfx_texlayout ll = {};
                bool ok = nvngfx_tex_layout(&ld, &ll) == 0 &&
                          ll.layers == 1 && ll.layer_size == 64;
                unsigned char* const guest =
                    vpool14 + kLinearOffset;
                if (ok) {
                    std::memset(guest, 0, 64);
                    std::memcpy(guest, top, sizeof top);
                    std::memcpy(guest + 32, bottom, sizeof bottom);
                }
                if (!ok) {
                    std::fprintf(stderr,
                                 "[selftest] draw 14: linear layout failed "
                                 "(layers=%u size=%llu)\n",
                                 ll.layers,
                                 static_cast<unsigned long long>(ll.size));
                    rc = 16405;
                    goto done;
                }
            }
            {
                x[0] = kB14;
                tex_builder_set_defaults(ctx, x);
                x[0] = kB14; x[1] = 2; x[2] = 2;
                tex_builder_set_size2d(ctx, x);
                x[0] = kB14; x[1] = 0x25;
                tex_builder_set_format(ctx, x);
                x[0] = kB14; x[1] = 0x10;
                tex_builder_set_flags(ctx, x);
                x[0] = kB14; x[1] = 32;
                tex_builder_set_stride(ctx, x);
                x[0] = kB14; x[1] = 1;
                tex_builder_set_levels(ctx, x);
                x[0] = kB14; x[1] = 1;
                tex_builder_set_samples(ctx, x);
                x[0] = kB14; x[1] = kCubePool; x[2] = kLinearOffset;
                tex_builder_set_storage(ctx, x);
                x[0] = kTexGuestLinear; x[1] = kB14;
                tex_initialize(ctx, x);
            }
            Tex* linearTex = find<Tex>(kTexGuestLinear);
            if (x[0] != 1 || !linearTex || !linearTex->mtl ||
                linearTex->mtl.textureType != MTLTextureType2D ||
                linearTex->layers != 1) {
                std::fprintf(stderr,
                             "[selftest] draw 14: linear texture was not "
                             "created (type=%lu layers=%u)\n",
                             linearTex
                                 ? static_cast<unsigned long>(
                                       linearTex->mtl.textureType)
                                 : 0ul,
                             linearTex ? linearTex->layers : 0u);
                rc = 16405;
                goto done;
            }

            struct { uint64_t data; uint64_t control; } sd14[2];
            sd14[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 4)));
            sd14[0].control = 0;
            sd14[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT, 4)));
            sd14[1].control = 0;
            x[0] = kProg14; x[1] = 0;
            prog_initialize(ctx, x);
            x[0] = kProg14; x[1] = 2;
            x[2] = reinterpret_cast<uint64_t>(&sd14[0]);
            prog_set_shaders(ctx, x);
            if (x[0] != 1) {
                std::fprintf(stderr,
                             "[selftest] draw 14: SetShaders failed\n");
                rc = 16405;
                goto done;
            }
            x[0] = kTexPool; x[1] = 11; x[2] = kTexGuestLinear; x[3] = 0;
            tex_pool_register_texture(ctx, x);

            const uint64_t target14 = kTex;
            const float clear14[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            x[0] = kCb14;
            cmdbuf_begin_recording(ctx, x);
            x[0] = kCb14; x[1] = 1;
            x[2] = reinterpret_cast<uint64_t>(&target14);
            x[3] = 0; x[4] = 0; x[5] = 0;
            cmdbuf_set_render_targets(ctx, x);
            x[0] = kCb14; x[1] = 0;
            x[2] = reinterpret_cast<uint64_t>(clear14);
            x[3] = 0xF;
            cmdbuf_clear_color(ctx, x);
            x[0] = kCb14; x[1] = kTexPool;
            cmdbuf_set_texture_pool(ctx, x);
            x[0] = kCb14; x[1] = kSampPool;
            cmdbuf_set_sampler_pool(ctx, x);
            x[0] = 0; x[1] = 11; x[2] = 9;
            dev_get_texture_handle(ctx, x);
            const uint64_t lhandle = x[0];
            x[0] = kCb14; x[1] = 1; x[2] = 0; x[3] = lhandle;
            cmdbuf_bind_texture(ctx, x);
            x[0] = kCb14; x[1] = kProg14; x[2] = 0;
            cmdbuf_bind_program(ctx, x);
            x[0] = kCb14; x[1] = 0; x[2] = 0; x[3] = 3;
            cmdbuf_draw_arrays(ctx, x);
            x[0] = kCb14;
            cmdbuf_end_recording(ctx, x);

            CmdBuf* b14 = find<CmdBuf>(kCb14);
            if (!b14 || !b14->last) {
                std::fprintf(stderr,
                             "[selftest] draw 14: nothing committed\n");
                rc = 16405;
                goto done;
            }
            [b14->last waitUntilCompleted];
            if (b14->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr,
                             "[selftest] draw 14 command buffer error: %s\n",
                             b14->last.error
                                 ? [[b14->last.error localizedDescription]
                                       UTF8String]
                                 : "(no description)");
                rc = 16405;
                goto done;
            }
            if (!linearTex->uploaded || linearTex->uploadedLevels != 1 ||
                b14->encoded != 1) {
                std::fprintf(stderr,
                             "[selftest] draw 14: linear texture not used "
                             "(uploaded=%d levels=%u encoded=%u)\n",
                             linearTex->uploaded ? 1 : 0,
                             linearTex->uploadedLevels, b14->encoded);
                rc = 16405;
                goto done;
            }
            {
                struct {
                    NSUInteger x, y;
                    int want[4];
                    const char* corner;
                } checks14[2] = {
                    {1, 1, {255, 0, 0, 255}, "top-left"},
                    {1, kSide - 2, {0, 0, 255, 255}, "bottom-left"}};
                for (int i = 0; i < 2; ++i) {
                    unsigned char px[4] = {0, 0, 0, 0};
                    if (!read_pixel_at(dev, c->queue, t->mtl, kSide,
                                       checks14[i].x, checks14[i].y, px) ||
                        !pixels_near(px, checks14[i].want, 2)) {
                        std::fprintf(stderr,
                                     "[selftest] draw 14 %s pixel is "
                                     "(%u,%u,%u,%u), want (%d,%d,%d,%d); the "
                                     "linear stride was ignored\n",
                                     checks14[i].corner, px[0], px[1], px[2],
                                     px[3], checks14[i].want[0],
                                     checks14[i].want[1], checks14[i].want[2],
                                     checks14[i].want[3]);
                        rc = 16406;
                        goto done;
                    }
                }
                std::fprintf(stderr,
                             "[selftest] draw 14: LINEAR texture rows sampled "
                             "from guest memory at a 32-byte stride\n");
            }
        }

        {
            /* --- Draw 15: a BCn texture from guest memory -----------------
             * iOS Metal has no BC1 pixel format.  tex_initialize() must create
             * an RGBA8 texture instead, and tex_upload() must decode the
             * block before blitting.  A 4x4 BC1 block holds red in its left
             * half and blue in its right half; the shader then samples the
             * two halves.  If the block is copied as if it were RGBA8, or the
             * decoder is skipped, the sampling coordinates no longer address
             * the same colours. */
            const uint64_t kProg15 = 0x7900;
            const uint64_t kCb15 = 0x7A00;
            const unsigned kBcnOffset = 14336;
            unsigned char* const vpool15 =
                poolRaw2 ? reinterpret_cast<unsigned char*>(
                               (reinterpret_cast<uintptr_t>(poolRaw2) + 16383u) &
                               ~static_cast<uintptr_t>(16383u))
                         : nullptr;
            if (!vpool15) {
                std::fprintf(stderr,
                             "[selftest] draw 15: the cube pool is missing\n");
                rc = 16407;
                goto done;
            }

            nvngfx_texdesc bd;
            nvngfx_texdesc_defaults(&bd);
            bd.target = 0;                    /* plain 2D */
            bd.format = 0x42;                 /* RGBA_DXT1 / BC1 */
            bd.width = 4;
            bd.height = 4;
            bd.depth = 1;
            bd.levels = 1;
            bd.samples = 1;
            bd.pool = kCubePool;
            bd.offset = kBcnOffset;
            {
                /* c0=red > c1=blue selects BC1's four-colour mode.  The
                 * 2-bit indices are 0,0,1,1 on every row. */
                const unsigned char block[8] = {
                    0x00, 0xF8, 0x1F, 0x00, 0x44, 0x44, 0x44, 0x44};
                nvngfx_texlayout bl = {};
                bool ok = nvngfx_tex_layout(&bd, &bl) == 0 &&
                          bl.size <= 16384u - kBcnOffset;
                if (ok) {
                    unsigned char* const guest = vpool15 + kBcnOffset;
                    std::memset(guest, 0, static_cast<size_t>(bl.size));
                    ok = nvngfx_tex_swizzle(&bd, 0, 0, block, 8, guest) == 0;
                }
                if (!ok) {
                    std::fprintf(stderr,
                                 "[selftest] draw 15: BC1 layout failed "
                                 "(layers=%u size=%llu)\n",
                                 bl.layers,
                                 static_cast<unsigned long long>(bl.size));
                    rc = 16407;
                    goto done;
                }
            }
            {
                x[0] = kTexBcnBuilder;
                tex_builder_set_defaults(ctx, x);
                x[0] = kTexBcnBuilder; x[1] = 4; x[2] = 4;
                tex_builder_set_size2d(ctx, x);
                x[0] = kTexBcnBuilder; x[1] = 0x42;
                tex_builder_set_format(ctx, x);
                x[0] = kTexBcnBuilder; x[1] = 1;
                tex_builder_set_levels(ctx, x);
                x[0] = kTexBcnBuilder; x[1] = 1;
                tex_builder_set_samples(ctx, x);
                x[0] = kTexBcnBuilder; x[1] = kCubePool; x[2] = kBcnOffset;
                tex_builder_set_storage(ctx, x);
                x[0] = kTexBcn; x[1] = kTexBcnBuilder;
                tex_initialize(ctx, x);
            }
            Tex* bcnTex = find<Tex>(kTexBcn);
            if (x[0] != 1 || !bcnTex || !bcnTex->mtl ||
                bcnTex->mtl.textureType != MTLTextureType2D ||
                bcnTex->mtl.pixelFormat != MTLPixelFormatRGBA8Unorm ||
                !bcnTex->transcoded) {
                std::fprintf(stderr,
                             "[selftest] draw 15: BC1 texture was not "
                             "transcoded to RGBA8 (ok=%llu type=%lu fmt=%lu)\n",
                             static_cast<unsigned long long>(x[0]),
                             bcnTex
                                 ? static_cast<unsigned long>(
                                       bcnTex->mtl.textureType)
                                 : 0ul,
                             bcnTex
                                 ? static_cast<unsigned long>(
                                       bcnTex->mtl.pixelFormat)
                                 : 0ul);
                rc = 16407;
                goto done;
            }

            {
                struct { uint64_t data; uint64_t control; } sd15[2];
                sd15[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                    nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX,
                                                       4)));
                sd15[0].control = 0;
                sd15[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                    nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT,
                                                       4)));
                sd15[1].control = 0;
                x[0] = kProg15; x[1] = 0;
                prog_initialize(ctx, x);
                x[0] = kProg15; x[1] = 2;
                x[2] = reinterpret_cast<uint64_t>(&sd15[0]);
                prog_set_shaders(ctx, x);
                if (x[0] != 1) {
                    std::fprintf(stderr,
                                 "[selftest] draw 15: SetShaders failed\n");
                    rc = 16407;
                    goto done;
                }
            }
            x[0] = kTexPool; x[1] = 12; x[2] = kTexBcn; x[3] = 0;
            tex_pool_register_texture(ctx, x);

            const uint64_t target15 = kTex;
            const float clear15[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            x[0] = kCb15;
            cmdbuf_begin_recording(ctx, x);
            x[0] = kCb15; x[1] = 1;
            x[2] = reinterpret_cast<uint64_t>(&target15);
            x[3] = 0; x[4] = 0; x[5] = 0;
            cmdbuf_set_render_targets(ctx, x);
            x[0] = kCb15; x[1] = 0;
            x[2] = reinterpret_cast<uint64_t>(clear15);
            x[3] = 0xF;
            cmdbuf_clear_color(ctx, x);
            x[0] = kCb15; x[1] = kTexPool;
            cmdbuf_set_texture_pool(ctx, x);
            x[0] = kCb15; x[1] = kSampPool;
            cmdbuf_set_sampler_pool(ctx, x);
            x[0] = 0; x[1] = 12; x[2] = 9;
            dev_get_texture_handle(ctx, x);
            const uint64_t bhandle = x[0];
            x[0] = kCb15; x[1] = 1; x[2] = 0; x[3] = bhandle;
            cmdbuf_bind_texture(ctx, x);
            x[0] = kCb15; x[1] = kProg15; x[2] = 0;
            cmdbuf_bind_program(ctx, x);
            x[0] = kCb15; x[1] = 0; x[2] = 0; x[3] = 3;
            cmdbuf_draw_arrays(ctx, x);
            x[0] = kCb15;
            cmdbuf_end_recording(ctx, x);

            CmdBuf* b15 = find<CmdBuf>(kCb15);
            if (!b15 || !b15->last) {
                std::fprintf(stderr,
                             "[selftest] draw 15: nothing committed\n");
                rc = 16407;
                goto done;
            }
            [b15->last waitUntilCompleted];
            if (b15->last.status == MTLCommandBufferStatusError) {
                std::fprintf(stderr,
                             "[selftest] draw 15 command buffer error: %s\n",
                             b15->last.error
                                 ? [[b15->last.error localizedDescription]
                                       UTF8String]
                                 : "(no description)");
                rc = 16407;
                goto done;
            }
            if (!bcnTex->uploaded || bcnTex->uploadedLevels != 1 ||
                b15->encoded != 1) {
                std::fprintf(stderr,
                             "[selftest] draw 15: BC1 texture not used "
                             "(uploaded=%d levels=%u encoded=%u)\n",
                             bcnTex->uploaded ? 1 : 0,
                             bcnTex->uploadedLevels, b15->encoded);
                rc = 16407;
                goto done;
            }
            {
                unsigned char left[4] = {0, 0, 0, 0};
                unsigned char right[4] = {0, 0, 0, 0};
                const int wantRed[4] = {255, 0, 0, 255};
                const int wantBlue[4] = {0, 0, 255, 255};
                if (!read_pixel_at(dev, c->queue, t->mtl, kSide,
                                   kSide / 8, kSide / 2, left) ||
                    !read_pixel_at(dev, c->queue, t->mtl, kSide,
                                   (7 * kSide) / 8, kSide / 2, right) ||
                    !pixels_near(left, wantRed, 2) ||
                    !pixels_near(right, wantBlue, 2)) {
                    std::fprintf(stderr,
                                 "[selftest] draw 15 halves are "
                                 "(%u,%u,%u,%u) / (%u,%u,%u,%u), want "
                                 "(%d,%d,%d,%d) / (%d,%d,%d,%d); BC1 "
                                 "decoding did not reach the sampler\n",
                                 left[0], left[1], left[2], left[3],
                                 right[0], right[1], right[2], right[3],
                                 wantRed[0], wantRed[1], wantRed[2],
                                 wantRed[3], wantBlue[0], wantBlue[1],
                                 wantBlue[2], wantBlue[3]);
                    rc = 16408;
                    goto done;
                }
                std::fprintf(stderr,
                             "[selftest] draw 15: BC1 texture decoded to RGBA8 "
                             "and sampled from guest memory\n");
            }
        }

        {
            /* --- Draw 16: guest writes after the first sample -------------
             * nvnTextureWriteTexels writes into the guest's storage and marks
             * the texture dirty; the next draw must re-upload.  Then a direct
             * CPU write into the same storage plus nvnTextureFlushTexels must
             * do the same.  Both paths sample a 2x2 RGBA8 texture and check
             * all four quadrants, so a one-shot upload that ignores the
             * second write fails visibly. */
            alignas(16) unsigned char guestStreamBuilder[0x80] = {};
            const uint64_t kTexStream = 0x7B00;
            const uint64_t kProg16 = 0x7C00;
            const uint64_t kCb16 = 0x7D00;
            const uint64_t kCb16b = 0x7E00;
            const uint64_t kCb16c = 0x7F00;
            const uint64_t kB16 =
                reinterpret_cast<uint64_t>(guestStreamBuilder);
            const unsigned kStreamOffset = 15360;
            unsigned char* const vpool16 =
                poolRaw2 ? reinterpret_cast<unsigned char*>(
                               (reinterpret_cast<uintptr_t>(poolRaw2) + 16383u) &
                               ~static_cast<uintptr_t>(16383u))
                         : nullptr;
            if (!vpool16) {
                std::fprintf(stderr,
                             "[selftest] draw 16: the cube pool is missing\n");
                rc = 16409;
                goto done;
            }
            nvngfx_texdesc sd;
            nvngfx_texdesc_defaults(&sd);
            sd.target = 0;                    /* plain 2D */
            sd.format = 0x25;                 /* RGBA8_UNORM */
            sd.width = 2;
            sd.height = 2;
            sd.depth = 1;
            sd.levels = 1;
            sd.samples = 1;
            sd.pool = kCubePool;
            sd.offset = kStreamOffset;
            nvngfx_texlayout L16 = {};
            if (nvngfx_tex_layout(&sd, &L16) != 0 ||
                L16.size > 16384u - kStreamOffset) {
                std::fprintf(stderr,
                             "[selftest] draw 16: stream layout failed "
                             "(size=%llu)\n",
                             static_cast<unsigned long long>(L16.size));
                rc = 16409;
                goto done;
            }
            const unsigned char initial[16] = {
                255, 0, 0, 255, 0, 255, 0, 255,
                0, 0, 255, 255, 255, 255, 255, 255};
            std::memset(vpool16 + kStreamOffset, 0, 1024);
            if (nvngfx_tex_swizzle(&sd, 0, 0, initial, 8,
                                   vpool16 + kStreamOffset) != 0) {
                std::fprintf(stderr,
                             "[selftest] draw 16: initial swizzle failed\n");
                rc = 16409;
                goto done;
            }
            x[0] = kB16;
            tex_builder_set_defaults(ctx, x);
            x[0] = kB16; x[1] = 2; x[2] = 2;
            tex_builder_set_size2d(ctx, x);
            x[0] = kB16; x[1] = 0x25;
            tex_builder_set_format(ctx, x);
            x[0] = kB16; x[1] = 1;
            tex_builder_set_levels(ctx, x);
            x[0] = kB16; x[1] = 1;
            tex_builder_set_samples(ctx, x);
            x[0] = kB16; x[1] = kCubePool; x[2] = kStreamOffset;
            tex_builder_set_storage(ctx, x);
            x[0] = kTexStream; x[1] = kB16;
            tex_initialize(ctx, x);
            Tex* streamTex = find<Tex>(kTexStream);
            if (x[0] != 1 || !streamTex || !streamTex->mtl ||
                streamTex->mtl.textureType != MTLTextureType2D ||
                streamTex->transcoded) {
                std::fprintf(stderr,
                             "[selftest] draw 16: stream texture was not "
                             "created (ok=%llu type=%lu)\n",
                             static_cast<unsigned long long>(x[0]),
                             streamTex
                                 ? static_cast<unsigned long>(
                                       streamTex->mtl.textureType)
                                 : 0ul);
                rc = 16409;
                goto done;
            }

            {
                struct { uint64_t data; uint64_t control; } sd16[2];
                sd16[0].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                    nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX,
                                                       4)));
                sd16[0].control = 0;
                sd16[1].data = reinterpret_cast<uint64_t>(const_cast<char*>(
                    nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_FRAGMENT,
                                                       4)));
                sd16[1].control = 0;
                x[0] = kProg16; x[1] = 0;
                prog_initialize(ctx, x);
                x[0] = kProg16; x[1] = 2;
                x[2] = reinterpret_cast<uint64_t>(&sd16[0]);
                prog_set_shaders(ctx, x);
                if (x[0] != 1) {
                    std::fprintf(stderr,
                                 "[selftest] draw 16: SetShaders failed\n");
                    rc = 16409;
                    goto done;
                }
            }
            x[0] = kTexPool; x[1] = 13; x[2] = kTexStream; x[3] = 0;
            tex_pool_register_texture(ctx, x);

            auto run16 = [&](uint64_t cbId) -> CmdBuf* {
                const uint64_t target16 = kTex;
                const float clear16[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                x[0] = cbId;
                cmdbuf_begin_recording(ctx, x);
                x[0] = cbId; x[1] = 1;
                x[2] = reinterpret_cast<uint64_t>(&target16);
                x[3] = 0; x[4] = 0; x[5] = 0;
                cmdbuf_set_render_targets(ctx, x);
                x[0] = cbId; x[1] = 0;
                x[2] = reinterpret_cast<uint64_t>(clear16);
                x[3] = 0xF;
                cmdbuf_clear_color(ctx, x);
                x[0] = cbId; x[1] = kTexPool;
                cmdbuf_set_texture_pool(ctx, x);
                x[0] = cbId; x[1] = kSampPool;
                cmdbuf_set_sampler_pool(ctx, x);
                x[0] = 0; x[1] = 13; x[2] = 9;
                dev_get_texture_handle(ctx, x);
                const uint64_t streamHandle = x[0];
                x[0] = cbId; x[1] = 1; x[2] = 0; x[3] = streamHandle;
                cmdbuf_bind_texture(ctx, x);
                x[0] = cbId; x[1] = kProg16; x[2] = 0;
                cmdbuf_bind_program(ctx, x);
                x[0] = cbId; x[1] = 0; x[2] = 0; x[3] = 3;
                cmdbuf_draw_arrays(ctx, x);
                x[0] = cbId;
                cmdbuf_end_recording(ctx, x);
                CmdBuf* b = find<CmdBuf>(cbId);
                if (!b || !b->last) return nullptr;
                [b->last waitUntilCompleted];
                if (b->last.status == MTLCommandBufferStatusError)
                    return nullptr;
                return b;
            };

            struct Check16 {
                int x, y;
                int want[4];
                const char* name;
            };
            auto check16 = [&](const char* what, const Check16* checks,
                               unsigned fail) -> bool {
                for (int i = 0; i < 4; ++i) {
                    unsigned char px[4] = {0, 0, 0, 0};
                    if (!read_pixel_at(dev, c->queue, t->mtl, kSide,
                                       checks[i].x, checks[i].y, px) ||
                        !pixels_near(px, checks[i].want, 2)) {
                        std::fprintf(stderr,
                                     "[selftest] draw 16 %s %s is "
                                     "(%u,%u,%u,%u), want (%d,%d,%d,%d)\n",
                                     what, checks[i].name,
                                     px[0], px[1], px[2], px[3],
                                     checks[i].want[0], checks[i].want[1],
                                     checks[i].want[2], checks[i].want[3]);
                        rc = fail;
                        return false;
                    }
                }
                return true;
            };

            const Check16 initialChecks[4] = {
                {kSide / 4, kSide / 4, {255, 0, 0, 255}, "top-left red"},
                {3 * kSide / 4, kSide / 4, {0, 255, 0, 255}, "top-right green"},
                {kSide / 4, 3 * kSide / 4, {0, 0, 255, 255}, "bottom-left blue"},
                {3 * kSide / 4, 3 * kSide / 4, {255, 255, 255, 255},
                 "bottom-right white"}};
            CmdBuf* b16 = run16(kCb16);
            if (!b16 || b16->encoded != 1 || !streamTex->uploaded) {
                std::fprintf(stderr,
                             "[selftest] draw 16: initial draw failed "
                             "(encoded=%u uploaded=%d)\n",
                             b16 ? b16->encoded : 0u,
                             streamTex->uploaded ? 1 : 0);
                rc = 16409;
                goto done;
            }
            if (!check16("initial", initialChecks, 16409)) goto done;

            const unsigned char updated[16] = {
                255, 255, 0, 255, 0, 255, 255, 255,
                255, 0, 255, 255, 0, 0, 0, 255};
            struct {
                int32_t xoffset, yoffset, zoffset, width, height, depth;
            } region16 = {0, 0, 0, 2, 2, 1};
            x[0] = kTexStream; x[1] = 0;
            x[2] = reinterpret_cast<uint64_t>(&region16);
            x[3] = reinterpret_cast<uint64_t>(updated);
            tex_write_texels(ctx, x);
            if (!streamTex->dirty) {
                std::fprintf(stderr,
                             "[selftest] draw 16: WriteTexels did not mark "
                             "the texture dirty\n");
                rc = 16410;
                goto done;
            }
            const Check16 updatedChecks[4] = {
                {kSide / 4, kSide / 4, {255, 255, 0, 255}, "top-left yellow"},
                {3 * kSide / 4, kSide / 4, {0, 255, 255, 255}, "top-right cyan"},
                {kSide / 4, 3 * kSide / 4, {255, 0, 255, 255},
                 "bottom-left magenta"},
                {3 * kSide / 4, 3 * kSide / 4, {0, 0, 0, 255},
                 "bottom-right black"}};
            CmdBuf* b16b = run16(kCb16b);
            if (!b16b || b16b->encoded != 1 || !streamTex->uploaded ||
                streamTex->dirty) {
                std::fprintf(stderr,
                             "[selftest] draw 16: WriteTexels redraw failed "
                             "(encoded=%u uploaded=%d dirty=%d)\n",
                             b16b ? b16b->encoded : 0u,
                             streamTex->uploaded ? 1 : 0,
                             streamTex->dirty ? 1 : 0);
                rc = 16410;
                goto done;
            }
            if (!check16("WriteTexels", updatedChecks, 16410)) goto done;

            const unsigned char flushed[16] = {
                128, 64, 32, 255, 32, 64, 128, 255,
                255, 128, 0, 255, 0, 128, 255, 255};
            if (nvngfx_tex_swizzle(&sd, 0, 0, flushed, 8,
                                   vpool16 + kStreamOffset) != 0) {
                std::fprintf(stderr,
                             "[selftest] draw 16: flush image swizzle failed\n");
                rc = 16411;
                goto done;
            }
            x[0] = kTexStream; x[1] = 0; x[2] = 0;
            tex_flush_texels(ctx, x);
            if (!streamTex->dirty) {
                std::fprintf(stderr,
                             "[selftest] draw 16: FlushTexels did not mark "
                             "the texture dirty\n");
                rc = 16411;
                goto done;
            }
            const Check16 flushedChecks[4] = {
                {kSide / 4, kSide / 4, {128, 64, 32, 255},
                 "top-left brown"},
                {3 * kSide / 4, kSide / 4, {32, 64, 128, 255},
                 "top-right steel"},
                {kSide / 4, 3 * kSide / 4, {255, 128, 0, 255},
                 "bottom-left orange"},
                {3 * kSide / 4, 3 * kSide / 4, {0, 128, 255, 255},
                 "bottom-right azure"}};
            CmdBuf* b16c = run16(kCb16c);
            if (!b16c || b16c->encoded != 1 || !streamTex->uploaded ||
                streamTex->dirty) {
                std::fprintf(stderr,
                             "[selftest] draw 16: FlushTexels redraw failed "
                             "(encoded=%u uploaded=%d dirty=%d)\n",
                             b16c ? b16c->encoded : 0u,
                             streamTex->uploaded ? 1 : 0,
                             streamTex->dirty ? 1 : 0);
                rc = 16411;
                goto done;
            }
            if (!check16("FlushTexels", flushedChecks, 16411)) goto done;
            std::fprintf(stderr,
                         "[selftest] draw 16: WriteTexels and FlushTexels "
                         "both re-uploaded a sampled texture\n");
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
        forget<Tex>(0x6900);
        forget<Program>(0x6A00);
        forget<CmdBuf>(0x6B00);
        forget<Tex>(0x6C00);
        forget<Program>(0x6D00);
        forget<CmdBuf>(0x6E00);
        forget<Tex>(0x6F00);
        forget<Program>(0x7000);
        forget<CmdBuf>(0x7100);
        forget<Tex>(0x7500);
        forget<Program>(0x7600);
        forget<CmdBuf>(0x7700);
        forget<Tex>(kTexBcn);
        forget<Program>(0x7900);
        forget<CmdBuf>(0x7A00);
        forget<Tex>(0x7B00);
        forget<Program>(0x7C00);
        forget<CmdBuf>(0x7D00);
        forget<CmdBuf>(0x7E00);
        forget<CmdBuf>(0x7F00);
        forget<Pool>(0x7300);
        g_pool_specs.erase(0x7400);
        std::free(poolRaw2);
        g_tex_specs.erase(kTexSrcBuilder);
        g_tex_specs.erase(kTexBcnBuilder);
        g_tex_specs.erase(0x6450);
        g_sampler_specs.erase(kSamplerBuilder5);
        nvngfx_metal_destroy(ctx);
        return rc;
    }
}

extern "C" const nvngfx_backend* nvngfx_metal_backend(void) {
    return &k_metal;
}

/* Offline MSL as files: <app>/shaders/manifest.tsv, or NVNGFX_SHADER_DIR.
 *
 * The generated C table is for a handful of shaders; a translated title is
 * thousands of files, and those must be replaceable without a rebuild.  A
 * missing directory is the normal state of this repository (the committed
 * table is empty), so this logs and continues rather than failing the device.
 * The fixture under ios/nvn/shaders is what the self-test's tenth draw uses. */
static void install_shader_bundle(void) {
    char why[256] = {0};
    const char* env = std::getenv("NVNGFX_SHADER_DIR");
    NSString* dir = nil;
    if (env && env[0]) {
        dir = [NSString stringWithUTF8String:env];
    } else {
        NSString* res = [[NSBundle mainBundle] resourcePath];
        if (res) dir = [res stringByAppendingPathComponent:@"shaders"];
    }
    if (!dir) return;
    const char* path = [dir fileSystemRepresentation];
    if (nvngfx_shader_load_bundle(path, why, sizeof why) == 0) {
        std::fprintf(stderr, "[nvn] shader bundle: %zu blob(s) from %s\n",
                     nvngfx_shader_bundle_count(), path);
    } else {
        std::fprintf(stderr, "[nvn] shader bundle: %s (%s)\n", path, why);
    }
    std::fflush(stderr);
}

/* Lifetime is owned by the app: create once, install, destroy at exit. */
extern "C" nvngfx_ctx* nvngfx_metal_create(void) {
    /* The offline chain's output, if this build has any: the table lives in
     * nvn_shader_blobs.gen.c and is empty in the committed tree.  Installing
     * it here means a program's key finds its MSL without any further setup. */
    nvngfx_shader_install_offline_blobs();
    install_shader_bundle();
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

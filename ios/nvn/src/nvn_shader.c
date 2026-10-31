/* Shader seam.  Portable C: no Apple headers, so it is compiled and tested on
 * Windows/Linux and on the macOS runner.
 *
 * There is no translator *on the phone*: Maxwell SASS -> SPIR-V -> MSL happens
 * offline on a computer (docs/SHADER_CHAIN.md), and its output reaches the
 * backend through the blob table below, keyed by the shader key.  This file
 * exists so that the rest of the backend can be written, executed and tested
 * against a known program, and so that "where does MSL come from" has exactly
 * one answer. */
#include "nvn_shader.h"

#include <stddef.h>
#include <string.h>

static const char* const kNoTranslator =
    "no translator: Maxwell SASS -> MSL is not implemented "
    "(see docs/IOS_PORTING.md, section 8)";

static const char* const kNoCode = "null shader code address";

static int g_passthrough = 0;

/* A full-screen triangle from [[vertex_id]] needs no vertex buffer, which is
 * what lets the self-test draw before vertex state exists.  The fragment
 * output is a constant so the readback can compare exact 8-bit values. */
static const char* const kVs =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; };\n"
    "\n"
    "vertex NvngfxVaryings nvngfx_main_vs(uint vid [[vertex_id]]) {\n"
    "    const float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
    "    NvngfxVaryings v;\n"
    "    v.position = float4(p[vid], 0.0, 1.0);\n"
    "    return v;\n"
    "}\n";

/* The compute self-test: one dispatch fills a 16x16 uint buffer and a 16x16
 * RGBA8 image.  The comment is the threadgroup-size sidecar the backend reads;
 * it is deliberately machine-readable and lives in the MSL so a translated
 * compute blob can carry the same information later. */
static const char* const kCs =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "// nvngfx-threadgroup 16 1 1\n"
    "kernel void nvngfx_main_cs(device const uint* src [[buffer(16)]],\n"
    "                           device uint* dst [[buffer(17)]],\n"
    "                           texture2d<float, access::write> image [[texture(0)]],\n"
    "                           uint2 gid [[thread_position_in_grid]]) {\n"
    "    if (gid.x >= 16 || gid.y >= 16) return;\n"
    "    dst[gid.y * 16 + gid.x] = src[0] + gid.x + gid.y;\n"
    "    image.write(float4(float(gid.x) / 15.0,\n"
    "                       float(gid.y) / 15.0, 0.75, 1.0), gid);\n"
    "}\n";

static const char* const kFs =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "fragment float4 nvngfx_main_fs() {\n"
    "    return float4(0.25, 0.5, 0.75, 1.0);\n"
    "}\n";

/* Depth-only fixtures.  The vertex shader puts the triangle at z=0.5; the
 * fragment shader writes no colour, which is what a pass with zero colour
 * attachments needs.  The matching colour shader is used by the follow-up
 * EQUAL test to prove the depth-only pass really wrote the buffer. */
static const char* const kVsDepthHalf =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; };\n"
    "\n"
    "vertex NvngfxVaryings nvngfx_main_vs(uint vid [[vertex_id]]) {\n"
    "    const float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
    "    NvngfxVaryings v;\n"
    "    v.position = float4(p[vid], 0.5, 1.0);\n"
    "    return v;\n"
    "}\n";

static const char* const kFsDepthOnly =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "fragment void nvngfx_main_fs() {}\n";

/* MRT fixture: two colour outputs with distinct, exact 8-bit values.  The
 * render-pass self-test reads both attachments back, so a pass that merely
 * declares a second attachment but never routes the shader output to it
 * fails instead of looking successful. */
static const char* const kFsMrt =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxMrtOut {\n"
    "    float4 color0 [[color(0)]];\n"
    "    float4 color1 [[color(1)]];\n"
    "};\n"
    "\n"
    "fragment NvngfxMrtOut nvngfx_main_fs() {\n"
    "    NvngfxMrtOut o;\n"
    "    o.color0 = float4(1.0, 0.0, 0.0, 1.0);\n"
    "    o.color1 = float4(0.0, 0.0, 1.0, 1.0);\n"
    "    return o;\n"
    "}\n";

/* Variant 1: the positions come from [[attribute(0)]] instead of
 * [[vertex_id]].  The fragment colour differs from variant 0, so a readback
 * shows which program drew the pixel.
 *
 * The shape is the one spirv-cross emits -- one `[[stage_in]]` struct whose
 * members carry the attribute locations -- rather than a bare
 * `[[attribute(0)]]` parameter.  Both are legal MSL, but the test exists to
 * exercise the shape the translator actually produces. */
static const char* const kVsAttr =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; };\n"
    "\n"
    "struct NvngfxVertexIn {\n"
    "    float2 position [[attribute(0)]];\n"
    "};\n"
    "\n"
    "vertex NvngfxVaryings nvngfx_main_vs(NvngfxVertexIn in [[stage_in]]) {\n"
    "    NvngfxVaryings v;\n"
    "    v.position = float4(in.position, 0.0, 1.0);\n"
    "    return v;\n"
    "}\n";

/* Variant 2: everything the fragment shader knows comes through the
 * argument buffer at buffer(0), which is the shape spirv-cross emits for
 * a real program (`constant spvDescriptorSetBuffer0& spvDescriptorSet0
 * [[buffer(0)]]`, with `[[id(k)]]` members).  One constant pointer is
 * enough to prove the entry encoding and the binding. */
static const char* const kFsUbo =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct spvDescriptorSetBuffer0 { constant float4* tint [[id(0)]]; };\n"
    "\n"
    "fragment float4 nvngfx_main_fs(constant spvDescriptorSetBuffer0& s0 [[buffer(0)]]) {\n"
    "    return *s0.tint;\n"
    "}\n";

static const char* const kFsGreen =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "fragment float4 nvngfx_main_fs() {\n"
    "    return float4(0.0, 1.0, 0.0, 1.0);\n"
    "}\n";

/* Variant 3: the first textured draw.  The fragment function samples the
 * texture and the sampler the guest bound through an argument buffer, at the
 * [[id(1)]] and [[id(2)]] the sidecar will one day name.  The vertex stage
 * passes the texture coordinate as a varying so every pixel reads a known
 * texel of a 2x2 texture. */
static const char* const kVsUv =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; float2 uv; };\n"
    "\n"
    "vertex NvngfxVaryings nvngfx_main_vs(uint vid [[vertex_id]]) {\n"
    "    const float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
    "    NvngfxVaryings v;\n"
    "    v.position = float4(p[vid], 0.0, 1.0);\n"
    "    v.uv = p[vid] * 0.5 + 0.5;\n"
    "    /* Metal's texture origin is top-left while NDC +y is up. */\n"
    "    v.uv.y = 1.0 - v.uv.y;\n"
    "    return v;\n"
    "}\n";

static const char* const kFsTexture =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; float2 uv; };\n"
    "\n"
    "struct spvDescriptorSetBuffer0 {\n"
    "    constant float4* tint [[id(0)]];\n"
    "    texture2d<float> tex [[id(1)]];\n"
    "    sampler smp [[id(2)]];\n"
    "};\n"
    "\n"
    "fragment float4 nvngfx_main_fs(NvngfxVaryings in [[stage_in]],\n"
    "                               constant spvDescriptorSetBuffer0& s0 [[buffer(0)]]) {\n"
    "    return s0.tex.sample(s0.smp, in.uv) * (*s0.tint);\n"
    "}\n";

/* Variant 4: the same textured draw without an argument buffer.  A device
 * that cannot provide Metal 3 resource IDs (the x86_64 simulator, for
 * example) returns 0 from MTLTexture.gpuResourceID, so an argument-buffer
 * texture entry cannot name it.  Direct texture/sampler bindings exercise the
 * pool, handle, upload and sampler state on those devices; real translated
 * shaders still use variant 3 on an Apple7/Metal3 device. */
static const char* const kFsTextureDirect =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; float2 uv; };\n"
    "\n"
    "fragment float4 nvngfx_main_fs(NvngfxVaryings in [[stage_in]],\n"
    "                               texture2d<float> tex [[texture(0)]],\n"
    "                               sampler smp [[sampler(0)]]) {\n"
    "    return tex.sample(smp, in.uv);\n"
    "}\n";

/* Variant 9: sample both layers of a two-layer 2D array and place layer 0 on
 * the left half of the target and layer 1 on the right half.  The texture
 * type is what makes this different from variant 4: Metal has to see
 * MTLTextureType2DArray and arrayLength 2 before destinationSlice:1 during the
 * guest upload becomes visible to the shader.  Reading both halves also
 * catches a staging buffer reused for every slice -- then both halves would
 * contain the last layer's colour. */
static const char* const kFsTextureArray =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; float2 uv; };\n"
    "\n"
    "fragment float4 nvngfx_main_fs(NvngfxVaryings in [[stage_in]],\n"
    "                               texture2d_array<float> tex [[texture(0)]],\n"
    "                               sampler smp [[sampler(0)]]) {\n"
    "    float4 lo = tex.sample(smp, in.uv, 0);\n"
    "    float4 hi = tex.sample(smp, in.uv, 1);\n"
    "    return in.uv.x < 0.5 ? lo : hi;\n"
    "}\n";


/* Variant 10: a cube map.  NVN face order is +X, -X, +Y, -Y, +Z, -Z, so the
 * two Z faces are slices 4 and 5 of the very same upload.  Metal hides the six
 * slices behind one array entry, but destinationSlice:4/5 during the upload
 * still has to reach the right face.  Sampling both directions and showing
 * them on the two halves means a wrong face shows green instead of failing to
 * compile. */
static const char* const kFsTextureCube =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; float2 uv; };\n"
    "\n"
    "fragment float4 nvngfx_main_fs(NvngfxVaryings in [[stage_in]],\n"
    "                               texturecube<float> tex [[texture(0)]],\n"
    "                               sampler smp [[sampler(0)]]) {\n"
    "    float4 plusZ = tex.sample(smp, float3(0.0, 0.0, 1.0));\n"
    "    float4 minusZ = tex.sample(smp, float3(0.0, 0.0, -1.0));\n"
    "    return in.uv.x < 0.5 ? plusZ : minusZ;\n"
    "}\n";

/* Variant 11: a cube array.  `arrayLength` counts cubes while the upload
 * addresses all depth * 6 slices, so layer 1 face 5 is slice 11.  Sampling
 * (0,0,1) from cube 0 and (0,0,-1) from cube 1 forces Metal to keep both
 * counts apart; if the backend recorded the cube as a plain 2D image, or
 * stored 6 slices instead of 12, one half cannot be the expected colour. */
static const char* const kFsTextureCubeArray =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; float2 uv; };\n"
    "\n"
    "fragment float4 nvngfx_main_fs(NvngfxVaryings in [[stage_in]],\n"
    "                               texturecube_array<float> tex [[texture(0)]],\n"
    "                               sampler smp [[sampler(0)]]) {\n"
    "    float4 lo = tex.sample(smp, float3(0.0, 0.0, 1.0), 0);\n"
    "    float4 hi = tex.sample(smp, float3(0.0, 0.0, -1.0), 1);\n"
    "    return in.uv.x < 0.5 ? lo : hi;\n"
    "}\n";

/* The same textured draw, but the argument-buffer ids are 5 / 9 / 12 instead
 * of 0 / 1 / 2.  The self-test uses this variant to prove that the fixed
 * 8-byte table's stride and holes follow the shader's layout sidecar and not
 * an assumed ordering. */
static const char* const kFsTextureBlob =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; float2 uv; };\n"
    "\n"
    "struct spvDescriptorSetBuffer0 {\n"
    "    constant float4* tint [[id(5)]];\n"
    "    texture2d<float> tex [[id(9)]];\n"
    "    sampler smp [[id(12)]];\n"
    "};\n"
    "\n"
    "fragment float4 nvngfx_main_fs(NvngfxVaryings in [[stage_in]],\n"
    "                               constant spvDescriptorSetBuffer0& s0 [[buffer(0)]]) {\n"
    "    return s0.tex.sample(s0.smp, in.uv) * (*s0.tint);\n"
    "}\n";

/* Variant 7: two bindless resources.  Their ids are sparse (9 / 12), while
 * their packed positions are 0 / 1, and both handles live in one uniform
 * buffer word.  The self-test uses this to prove the raw table is written by
 * declaration order and that the UBO handle is split into pool IDs.  It does
 * not claim a GPU sample: the x86_64 simulator returns zero for a texture's
 * gpuResourceID, so the table probe stops before encoding a draw. */
static const char* const kFsBindless =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; float2 uv; };\n"
    "\n"
    "struct spvDescriptorSetBuffer0 {\n"
    "    texture2d<float> tex [[id(9)]];\n"
    "    sampler smp [[id(12)]];\n"
    "};\n"
    "\n"
    "fragment float4 nvngfx_main_fs(NvngfxVaryings in [[stage_in]],\n"
    "                               constant spvDescriptorSetBuffer0& s0 [[buffer(0)]]) {\n"
    "    return s0.tex.sample(s0.smp, in.uv);\n"
    "}\n";

/* Variant 8: the vertex stage's own argument buffer.  The constants live at
 * buffer(0) -- the same index the translator gives spvDescriptorSetBuffer0 --
 * while the positions arrive through [[stage_in]] from a vertex stream the
 * backend binds at NVNGFX_VERTEX_STREAM_BASE.  The fragment stage is a
 * constant colour, so a wrong index shows up as a triangle in the wrong
 * place rather than as a compile error. */
static const char* const kVsVertexUbo =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct spvDescriptorSetBuffer0 { constant float4* shift [[id(0)]]; };\n"
    "\n"
    "struct NvngfxVertexIn {\n"
    "    float2 position [[attribute(0)]];\n"
    "};\n"
    "\n"
    "struct NvngfxVaryings { float4 position [[position]]; };\n"
    "\n"
    "vertex NvngfxVaryings nvngfx_main_vs(\n"
    "    NvngfxVertexIn in [[stage_in]],\n"
    "    constant spvDescriptorSetBuffer0& s0 [[buffer(0)]]) {\n"
    "    NvngfxVaryings v;\n"
    "    v.position = float4(in.position + (*s0.shift).xy, 0.0, 1.0);\n"
    "    return v;\n"
    "}\n";

static const char* const kFsMagenta =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "fragment float4 nvngfx_main_fs() {\n"
    "    return float4(1.0, 0.0, 1.0, 1.0);\n"
    "}\n";

/* Variant 6: a c1-only fragment shader.  The argument buffer has one pointer
 * at [[id(0)]], and the bytes behind it come from the sidecar, not from any
 * command-buffer binding.  The self-test fills them with a known colour so a
 * readback proves the c1 buffer was created and its address was written.
 *
 * The id must be 0 here, not 2: Metal lays an argument buffer out in member
 * declaration order (8 bytes per scalar resource), while [[id(k)]] only names
 * the encoder binding.  A lone member declared as [[id(2)]] still lives at
 * byte 0, so a raw id*8 table would read zeros.  Real translated shaders have
 * contiguous ids (0..41 in the measured BOTW fragment shader), where the two
 * orders coincide. */
static const char* const kFsC1 =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct spvDescriptorSetBuffer0 { constant float4* c1 [[id(0)]]; };\n"
    "\n"
    "fragment float4 nvngfx_main_fs(constant spvDescriptorSetBuffer0& s0 [[buffer(0)]]) {\n"
    "    return *s0.c1;\n"
    "}\n";

/* ------------------------------------------------------------------------- *
 * Offline MSL blobs
 *
 * A linear table, because it is installed once at start-up and read once per
 * pipeline compilation.  With the whole title (19,592 shaders) it is 19,592
 * pointer comparisons per miss, which is nothing next to a Metal pipeline
 * build; sorting it would only hide the fact that a miss means the offline
 * build did not cover the shader.
 * ------------------------------------------------------------------------- */

#define NVNGFX_KEY_LEN 16

static const nvngfx_shader_blob* g_blobs = NULL;
static size_t g_blob_count = 0;

/* Lowercased hex digit, or 0 when the character is not one. */
static char hex_lower(char c) {
    if (c >= '0' && c <= '9') return c;
    if (c >= 'a' && c <= 'f') return c;
    if (c >= 'A' && c <= 'F') return (char)(c - 'A' + 'a');
    return 0;
}

static int key_valid(const char* k) {
    int i;
    if (!k) return 0;
    for (i = 0; i < NVNGFX_KEY_LEN; ++i)
        if (!hex_lower(k[i])) return 0;
    return k[NVNGFX_KEY_LEN] == '\0';
}

static int key_eq(const char* a, const char* b) {
    int i;
    for (i = 0; i < NVNGFX_KEY_LEN; ++i)
        if (hex_lower(a[i]) != hex_lower(b[i])) return 0;
    return 1;
}

/* A layout must be usable without a second validation pass in the backend:
 * ascending unique ids for deterministic lookup and diagnostics; packed slots
 * forming a permutation of 0..num_args-1 because that is the byte layout
 * Metal derives from declaration order; a kind whose slot fits the table it
 * names; and -1 for everything unsupported.  A shader whose layout cannot be
 * validated is rejected here rather than dropped later, so a bad generator
 * shows up in nvngfx_shader_set_blobs' return value. */
static int args_valid(const nvngfx_shader_blob* b) {
    unsigned i, has_c1 = 0;
    unsigned char packed_seen[NVNGFX_MAX_ARGS] = {0};
    if (b->c1_size > NVNGFX_MAX_C1_BYTES) return 0;
    if (b->c1_size != 0 && b->c1 == NULL) return 0;
    if (b->num_args == 0) {
        if (b->args != NULL) return 0;
    } else {
        if (b->args == NULL || b->num_args > NVNGFX_MAX_ARGS) return 0;
        for (i = 0; i < b->num_args; ++i) {
            const nvngfx_shader_arg* a = &b->args[i];
            if (a->id > NVNGFX_MAX_ARG_ID) return 0;
            if (i > 0 && b->args[i - 1].id >= a->id) return 0;
            if (a->packed >= b->num_args || packed_seen[a->packed]) return 0;
            packed_seen[a->packed] = 1;
            if (a->kind < NVNGFX_ARG_UBO ||
                a->kind > NVNGFX_ARG_SAMPLER_IN_UBO)
                return 0;
            if (a->kind == NVNGFX_ARG_C1) {
                if (a->slot != -1 || a->offset != 0) return 0;
                has_c1 = 1;
                continue;
            }
            if (a->kind == NVNGFX_ARG_UNSUPPORTED) {
                if (a->slot != -1 || a->offset != 0) return 0;
                continue;
            }
            if (a->slot < 0) return 0;
            switch (a->kind) {
            case NVNGFX_ARG_UBO:
                if (a->slot >= NVNGFX_MAX_UBO_SLOTS || a->offset != 0) return 0;
                break;
            case NVNGFX_ARG_TEXTURE_IN_UBO:
            case NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE:
            case NVNGFX_ARG_SAMPLER_IN_UBO:
                if (a->slot >= NVNGFX_MAX_UBO_SLOTS) return 0;
                if ((a->offset & 3u) != 0) return 0;
                break;
            case NVNGFX_ARG_TEXTURE:
            case NVNGFX_ARG_SAMPLER_OF_TEXTURE:
            case NVNGFX_ARG_SEPARATE_TEXTURE:
                if (a->slot >= NVNGFX_MAX_TEXTURE_SLOTS || a->offset != 0)
                    return 0;
                break;
            case NVNGFX_ARG_SEPARATE_SAMPLER:
                if (a->slot >= NVNGFX_MAX_SAMPLER_SLOTS || a->offset != 0)
                    return 0;
                break;
            default:
                return 0;
            }
        }
    }
    if (has_c1 && b->c1 == NULL) return 0;
    return 1;
}

int nvngfx_shader_set_blobs(const nvngfx_shader_blob* blobs, size_t count) {
    size_t i, j;
    if (count != 0 && blobs == NULL) return -1;
    for (i = 0; i < count; ++i) {
        const nvngfx_shader_blob* b = &blobs[i];
        if (!key_valid(b->key)) return -(int)(i + 1);
        if (b->stage != NVNGFX_STAGE_VERTEX &&
            b->stage != NVNGFX_STAGE_FRAGMENT &&
            b->stage != NVNGFX_STAGE_COMPUTE)
            return -(int)(i + 1);
        if (b->msl == NULL || b->msl[0] == '\0') return -(int)(i + 1);
        if (!args_valid(b)) return -(int)(i + 1);
        for (j = 0; j < i; ++j)
            if (blobs[j].stage == b->stage && key_eq(blobs[j].key, b->key))
                return -(int)(i + 1);
    }
    /* Only now does anything change: a rejected table leaves the installed one
     * exactly as it was. */
    g_blobs = blobs;
    g_blob_count = count;
    return 0;
}

void nvngfx_shader_clear_blobs(void) {
    g_blobs = NULL;
    g_blob_count = 0;
}

size_t nvngfx_shader_blob_count(void) { return g_blob_count; }

const nvngfx_shader_blob* nvngfx_shader_lookup_blob(const char* key,
                                                    nvngfx_stage stage,
                                                    const char** why) {
    size_t i;
    if (!key_valid(key)) {
        if (why) *why = "shader key is not 16 hex digits";
        return NULL;
    }
    if (g_blob_count == 0) {
        if (why) *why = "no offline MSL blob table is installed";
        return NULL;
    }
    for (i = 0; i < g_blob_count; ++i) {
        if (g_blobs[i].stage != (int)stage) continue;
        if (!key_eq(g_blobs[i].key, key)) continue;
        if (why) *why = "offline MSL blob";
        return &g_blobs[i];
    }
    if (why) *why = "no offline MSL blob for this key";
    return NULL;
}

const char* nvngfx_shader_lookup(const char* key, nvngfx_stage stage,
                                 const char** why) {
    const nvngfx_shader_blob* b = nvngfx_shader_lookup_blob(key, stage, why);
    return b ? b->msl : NULL;
}

const nvngfx_shader_blob* nvngfx_shader_blob_table(size_t* count) {
    if (count) *count = g_blob_count;
    return g_blobs;
}

unsigned nvngfx_shader_copy_args(const nvngfx_shader_blob* blob,
                                 nvngfx_shader_arg* out, unsigned cap) {
    unsigned i;
    if (!blob || !blob->args || blob->num_args == 0 || !out) return 0;
    if (cap < blob->num_args) return 0;
    for (i = 0; i < blob->num_args; ++i) out[i] = blob->args[i];
    return blob->num_args;
}

int nvngfx_shader_threadgroup_size(const char* msl, unsigned out[3]) {
    static const char marker[] = "nvngfx-threadgroup";
    if (!msl || !out) return -1;
    const char* p = strstr(msl, marker);
    if (!p) return -1;
    p += sizeof marker - 1;
    for (int i = 0; i < 3; ++i) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (*p < '0' || *p > '9') return -1;
        unsigned v = 0;
        while (*p >= '0' && *p <= '9') {
            const unsigned digit = (unsigned)(*p - '0');
            if (v > 1024u / 10u || v * 10u + digit > 1024u) return -1;
            v = v * 10u + digit;
            ++p;
        }
        if (!v) return -1;
        out[i] = v;
    }
    if (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
        *p != '/')
        return -1;
    if (out[0] > 1024u / out[1]) return -1;
    if (out[0] * out[1] > 1024u / out[2]) return -1;
    return 0;
}

void nvngfx_shader_set_msl_passthrough(int on) { g_passthrough = on ? 1 : 0; }

int nvngfx_shader_msl_passthrough(void) { return g_passthrough; }

const char* nvngfx_shader_selftest_msl_variant(nvngfx_stage stage, int variant) {
    if (variant == 1) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsAttr;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsGreen;
        return NULL;
    }
    if (variant == 2) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVs;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsUbo;
        return NULL;
    }
    if (variant == 3) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsUv;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsTexture;
        return NULL;
    }
    if (variant == 4) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsUv;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsTextureDirect;
        return NULL;
    }
    if (variant == 5) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsUv;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsTextureBlob;
        return NULL;
    }
    if (variant == 6) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVs;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsC1;
        return NULL;
    }
    if (variant == 7) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsUv;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsBindless;
        return NULL;
    }
    if (variant == 8) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsVertexUbo;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsMagenta;
        return NULL;
    }
    if (variant == 9) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsUv;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsTextureArray;
        return NULL;
    }
    if (variant == 10) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsUv;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsTextureCube;
        return NULL;
    }
    if (variant == 11) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsUv;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsTextureCubeArray;
        return NULL;
    }
    if (variant == 12) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVs;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsMrt;
        return NULL;
    }
    if (variant == 13) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsDepthHalf;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsDepthOnly;
        return NULL;
    }
    if (variant == 14) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsDepthHalf;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsGreen;
        return NULL;
    }
    if (stage == NVNGFX_STAGE_VERTEX) return kVs;
    if (stage == NVNGFX_STAGE_FRAGMENT) return kFs;
    if (stage == NVNGFX_STAGE_COMPUTE) return kCs;
    return NULL;
}

const char* nvngfx_shader_selftest_msl(nvngfx_stage stage) {
    return nvngfx_shader_selftest_msl_variant(stage, 0);
}

const char* nvngfx_shader_to_msl(const void* code, unsigned long long size,
                                 nvngfx_stage stage, const char** why) {
    (void)stage;
    if (!code) {
        if (why) *why = kNoCode;
        return NULL;
    }
    if (!g_passthrough) {
        if (why) *why = kNoTranslator;
        return NULL;
    }
    /* Passthrough still has to be a C string: the Metal compiler takes one.
     * A size of 0 means "trust the caller that it is NUL-terminated". */
    if (size != 0) {
        const char* s = (const char*)code;
        size_t i = 0;
        while (i < (size_t)size && s[i]) ++i;
        if (i == (size_t)size) {
            if (why) *why = "passthrough buffer is not NUL-terminated within its size";
            return NULL;
        }
    }
    if (why) *why = "passthrough";
    return (const char*)code;
}
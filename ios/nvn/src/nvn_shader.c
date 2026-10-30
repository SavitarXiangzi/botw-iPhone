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

static const char* const kFs =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "fragment float4 nvngfx_main_fs() {\n"
    "    return float4(0.25, 0.5, 0.75, 1.0);\n"
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

static const char* const kFsGreen =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "fragment float4 nvngfx_main_fs() {\n"
    "    return float4(0.0, 1.0, 0.0, 1.0);\n"
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

int nvngfx_shader_set_blobs(const nvngfx_shader_blob* blobs, size_t count) {
    size_t i, j;
    if (count != 0 && blobs == NULL) return -1;
    for (i = 0; i < count; ++i) {
        const nvngfx_shader_blob* b = &blobs[i];
        if (!key_valid(b->key)) return -(int)(i + 1);
        if (b->stage != NVNGFX_STAGE_VERTEX && b->stage != NVNGFX_STAGE_FRAGMENT)
            return -(int)(i + 1);
        if (b->msl == NULL || b->msl[0] == '\0') return -(int)(i + 1);
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

const char* nvngfx_shader_lookup(const char* key, nvngfx_stage stage,
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
        return g_blobs[i].msl;
    }
    if (why) *why = "no offline MSL blob for this key";
    return NULL;
}

void nvngfx_shader_set_msl_passthrough(int on) { g_passthrough = on ? 1 : 0; }

int nvngfx_shader_msl_passthrough(void) { return g_passthrough; }

const char* nvngfx_shader_selftest_msl_variant(nvngfx_stage stage, int variant) {
    if (variant == 1) {
        if (stage == NVNGFX_STAGE_VERTEX) return kVsAttr;
        if (stage == NVNGFX_STAGE_FRAGMENT) return kFsGreen;
        return NULL;
    }
    if (stage == NVNGFX_STAGE_VERTEX) return kVs;
    if (stage == NVNGFX_STAGE_FRAGMENT) return kFs;
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
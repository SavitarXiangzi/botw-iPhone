/* Shader seam.  Portable C: no Apple headers, so it is compiled and tested on
 * Windows/Linux and on the macOS runner.
 *
 * The translator does not exist yet.  This file exists so that the rest of the
 * backend can be written, executed and tested against a known program, and so
 * that "where does Maxwell SASS turn into MSL" has exactly one answer. */
#include "nvn_shader.h"

#include <stddef.h>

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

void nvngfx_shader_set_msl_passthrough(int on) { g_passthrough = on ? 1 : 0; }

int nvngfx_shader_msl_passthrough(void) { return g_passthrough; }

const char* nvngfx_shader_selftest_msl(nvngfx_stage stage) {
    if (stage == NVNGFX_STAGE_VERTEX) return kVs;
    if (stage == NVNGFX_STAGE_FRAGMENT) return kFs;
    return NULL;
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
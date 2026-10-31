/* NVN fixed-function state -> a backend-neutral pipeline description.
 *
 * Why this file exists: the six NVN state objects are decoded in nvn_state.c,
 * but turning them into a pipeline needs a second step that must be testable
 * without Metal and without a Mac.  That step lives here.
 *
 * Two rules:
 *
 *   1. Nothing here names a Metal type.  A value that NVN defines is decoded
 *      into an NVNGFX_* value; the backend owns the hop to MTLBlendFactor and
 *      friends, because that is the hop an SDK can reject.
 *   2. Anything that cannot be honoured is reported in `unsupported` instead
 *      of being silently dropped.  A silent drop here reads as "the game did
 *      not ask for it", which is exactly the class of bug this project keeps
 *      finding in its own backends.
 *
 * The NVN numeric values come from the public nvn.h header shipped with the
 * d2r-mobile analysis tree (dm/gfx/nvn/include/nvn.h), not from guessing.
 */
#ifndef NVN_PIPELINE_H
#define NVN_PIPELINE_H

#include "nvn_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVNGFX_MAX_COLOR_TARGETS 8

/* NVNblendFunc, decoded.  NVNGFX_BF_UNSUPPORTED is the "not a value NVN
 * defines" answer, never a valid NVN enum. */
enum {
    NVNGFX_BF_UNSUPPORTED = 0,
    NVNGFX_BF_ZERO,
    NVNGFX_BF_ONE,
    NVNGFX_BF_SRC_COLOR,
    NVNGFX_BF_ONE_MINUS_SRC_COLOR,
    NVNGFX_BF_SRC_ALPHA,
    NVNGFX_BF_ONE_MINUS_SRC_ALPHA,
    NVNGFX_BF_DST_ALPHA,
    NVNGFX_BF_ONE_MINUS_DST_ALPHA,
    NVNGFX_BF_DST_COLOR,
    NVNGFX_BF_ONE_MINUS_DST_COLOR,
    NVNGFX_BF_SRC_ALPHA_SATURATE,
    NVNGFX_BF_SRC1_COLOR,
    NVNGFX_BF_ONE_MINUS_SRC1_COLOR,
    NVNGFX_BF_SRC1_ALPHA,
    NVNGFX_BF_ONE_MINUS_SRC1_ALPHA,
    NVNGFX_BF_CONSTANT_COLOR,
    NVNGFX_BF_ONE_MINUS_CONSTANT_COLOR,
    NVNGFX_BF_CONSTANT_ALPHA,
    NVNGFX_BF_ONE_MINUS_CONSTANT_ALPHA
};

/* NVNblendEquation, decoded. */
enum {
    NVNGFX_EQ_UNSUPPORTED = 0,
    NVNGFX_EQ_ADD,
    NVNGFX_EQ_SUB,
    NVNGFX_EQ_REVERSE_SUB,
    NVNGFX_EQ_MIN,
    NVNGFX_EQ_MAX
};

/* NVNdepthFunc / NVNstencilFunc share one ordering in NVN. */
enum {
    NVNGFX_CMP_UNSUPPORTED = 0,
    NVNGFX_CMP_NEVER,
    NVNGFX_CMP_LESS,
    NVNGFX_CMP_EQUAL,
    NVNGFX_CMP_LEQUAL,
    NVNGFX_CMP_GREATER,
    NVNGFX_CMP_NOTEQUAL,
    NVNGFX_CMP_GEQUAL,
    NVNGFX_CMP_ALWAYS
};

/* NVNstencilOp, decoded. */
enum {
    NVNGFX_STENCIL_UNSUPPORTED = 0,
    NVNGFX_STENCIL_KEEP,
    NVNGFX_STENCIL_ZERO,
    NVNGFX_STENCIL_REPLACE,
    NVNGFX_STENCIL_INCR,
    NVNGFX_STENCIL_DECR,
    NVNGFX_STENCIL_INVERT,
    NVNGFX_STENCIL_INCR_WRAP,
    NVNGFX_STENCIL_DECR_WRAP
};

/* NVNface as a cull mode.  FRONT_AND_BACK has no single Metal value. */
enum {
    NVNGFX_CULL_NONE = 0,
    NVNGFX_CULL_FRONT,
    NVNGFX_CULL_BACK,
    NVNGFX_CULL_UNSUPPORTED
};

enum { NVNGFX_WINDING_CW = 0, NVNGFX_WINDING_CCW = 1 };

enum {
    NVNGFX_FILL_FILL = 0,
    NVNGFX_FILL_LINE = 1,
    NVNGFX_FILL_UNSUPPORTED = 2
};

/* One bit per feature the caller must report rather than honour. */
enum {
    NVNGFX_UNSUP_BLEND_FACTOR   = 1u << 0,
    NVNGFX_UNSUP_BLEND_EQUATION = 1u << 1,
    NVNGFX_UNSUP_DEPTH_FUNC     = 1u << 2,
    NVNGFX_UNSUP_STENCIL_FUNC   = 1u << 3,
    NVNGFX_UNSUP_STENCIL_OP     = 1u << 4,
    NVNGFX_UNSUP_CULL           = 1u << 5,
    NVNGFX_UNSUP_FILL           = 1u << 6,
    NVNGFX_UNSUP_LOGIC_OP       = 1u << 7,
    NVNGFX_UNSUP_ALPHA_TEST     = 1u << 8,
    NVNGFX_UNSUP_MULTISAMPLE    = 1u << 10,
    NVNGFX_UNSUP_COVERAGE       = 1u << 11,
    NVNGFX_UNSUP_POLYGON_OFFSET = 1u << 12
};

typedef struct {
    /* Per colour target.  Blend enable comes from NVNcolorState's 8-bit
     * field (bit i = target i); factors come from the blend state whose own
     * `target` field selected slot i. */
    unsigned char blend_enable[NVNGFX_MAX_COLOR_TARGETS];
    unsigned char src_rgb[NVNGFX_MAX_COLOR_TARGETS];
    unsigned char dst_rgb[NVNGFX_MAX_COLOR_TARGETS];
    unsigned char src_a[NVNGFX_MAX_COLOR_TARGETS];
    unsigned char dst_a[NVNGFX_MAX_COLOR_TARGETS];
    unsigned char eq_rgb[NVNGFX_MAX_COLOR_TARGETS];
    unsigned char eq_a[NVNGFX_MAX_COLOR_TARGETS];
    /* Already in Metal's bit order: red = 8, green = 4, blue = 2, alpha = 1.
     * NVN stores the opposite order (r=1,g=2,b=4,a=8), so the reversal happens
     * in nvngfx_write_mask() and is covered by the unit test. */
    unsigned char write_mask[NVNGFX_MAX_COLOR_TARGETS];

    unsigned char depth_test, depth_write, depth_func;
    unsigned char stencil_test;
    unsigned char stencil_func[2];      /* 0 = front, 1 = back */
    unsigned char stencil_op[2][3];     /* fail, z-fail, z-pass */

    unsigned char cull, winding, fill;
    /* Only the FILL bit (0x4) maps to Metal's triangle depth bias.  Point and
     * line offset modes have no Metal equivalent and are reported separately. */
    unsigned char polygon_offset;
    unsigned char samples;              /* 1 when multisample is off */
    unsigned char alpha_to_coverage;

    unsigned unsupported;
} nvngfx_pipeline_state;

/* Decoders.  Each takes an NVN enum value and returns the NVNGFX_* form. */
unsigned nvngfx_blend_factor(unsigned nvn);
unsigned nvngfx_blend_equation(unsigned nvn);
unsigned nvngfx_compare_func(unsigned nvn);
unsigned nvngfx_stencil_op(unsigned nvn);
unsigned nvngfx_write_mask(unsigned nvn4);
unsigned nvngfx_cull(int nvn_face);
unsigned nvngfx_winding(int nvn_front);
unsigned nvngfx_fill(int nvn_mode);

/* Defaults are NVN's own: blending off, every channel written, depth test off
 * (write on, LESS), stencil off (ALWAYS/KEEP), no culling, CCW front, fill. */
void nvngfx_pipeline_defaults(nvngfx_pipeline_state* st);

/* Layering one state object on top of another.  Calling a setter twice for
 * the same object is harmless; each call fully describes its own fields. */
void nvngfx_pipeline_set_blend(nvngfx_pipeline_state* st, const nvngfx_blend_state* b);
void nvngfx_pipeline_set_color(nvngfx_pipeline_state* st, const nvngfx_color_state* c);
void nvngfx_pipeline_set_channel_mask(nvngfx_pipeline_state* st,
                                      const nvngfx_channel_mask_state* m);
void nvngfx_pipeline_set_multisample(nvngfx_pipeline_state* st,
                                     const nvngfx_multisample_state* m);
void nvngfx_pipeline_set_polygon(nvngfx_pipeline_state* st, const nvngfx_polygon_state* p);
void nvngfx_pipeline_set_depth_stencil(nvngfx_pipeline_state* st,
                                       const nvngfx_depth_stencil_state* d);

/* FNV-1a over the whole structure.  Only meaningful after
 * nvngfx_pipeline_defaults() (which zeroes every byte, padding included). */
unsigned long long nvngfx_pipeline_key(const nvngfx_pipeline_state* st);

#ifdef __cplusplus
}
#endif

#endif /* NVN_PIPELINE_H */

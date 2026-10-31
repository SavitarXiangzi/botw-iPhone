#include "nvn_pipeline.h"

#include <string.h>

/* NVN enum values, from dm/gfx/nvn/include/nvn.h.  They are spelled out here
 * rather than included so that this file stays free of the NVN headers and
 * can be compiled and tested on any host. */
enum {
    NVN_BLEND_FUNC_ZERO = 0x1,
    NVN_BLEND_FUNC_ONE = 0x2,
    NVN_BLEND_FUNC_SRC_COLOR = 0x3,
    NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR = 0x4,
    NVN_BLEND_FUNC_SRC_ALPHA = 0x5,
    NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA = 0x6,
    NVN_BLEND_FUNC_DST_ALPHA = 0x7,
    NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA = 0x8,
    NVN_BLEND_FUNC_DST_COLOR = 0x9,
    NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR = 0xA,
    NVN_BLEND_FUNC_SRC_ALPHA_SATURATE = 0xB,
    NVN_BLEND_FUNC_SRC1_COLOR = 0x10,
    NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR = 0x11,
    NVN_BLEND_FUNC_SRC1_ALPHA = 0x12,
    NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA = 0x13,
    NVN_BLEND_FUNC_CONSTANT_COLOR = 0x61,
    NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR = 0x62,
    NVN_BLEND_FUNC_CONSTANT_ALPHA = 0x63,
    NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA = 0x64,

    NVN_BLEND_EQUATION_ADD = 0x1,
    NVN_BLEND_EQUATION_SUB = 0x2,
    NVN_BLEND_EQUATION_REVERSE_SUB = 0x3,
    NVN_BLEND_EQUATION_MIN = 0x4,
    NVN_BLEND_EQUATION_MAX = 0x5,

    NVN_DEPTH_FUNC_NEVER = 0x1,
    NVN_DEPTH_FUNC_LESS = 0x2,
    NVN_DEPTH_FUNC_EQUAL = 0x3,
    NVN_DEPTH_FUNC_LEQUAL = 0x4,
    NVN_DEPTH_FUNC_GREATER = 0x5,
    NVN_DEPTH_FUNC_NOTEQUAL = 0x6,
    NVN_DEPTH_FUNC_GEQUAL = 0x7,
    NVN_DEPTH_FUNC_ALWAYS = 0x8,

    NVN_STENCIL_OP_KEEP = 0x1,
    NVN_STENCIL_OP_ZERO = 0x2,
    NVN_STENCIL_OP_REPLACE = 0x3,
    NVN_STENCIL_OP_INCR = 0x4,
    NVN_STENCIL_OP_DECR = 0x5,
    NVN_STENCIL_OP_INVERT = 0x6,
    NVN_STENCIL_OP_INCR_WRAP = 0x7,
    NVN_STENCIL_OP_DECR_WRAP = 0x8,

    NVN_FACE_NONE = 0x0,
    NVN_FACE_FRONT = 0x1,
    NVN_FACE_BACK = 0x2,
    NVN_FACE_FRONT_AND_BACK = 0x3,

    NVN_LOGIC_OP_COPY = 0x3,
    NVN_ALPHA_FUNC_ALWAYS = 0x8
};

unsigned nvngfx_blend_factor(unsigned nvn) {
    switch (nvn) {
    case NVN_BLEND_FUNC_ZERO: return NVNGFX_BF_ZERO;
    case NVN_BLEND_FUNC_ONE: return NVNGFX_BF_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return NVNGFX_BF_SRC_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return NVNGFX_BF_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return NVNGFX_BF_SRC_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return NVNGFX_BF_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return NVNGFX_BF_DST_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return NVNGFX_BF_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return NVNGFX_BF_DST_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return NVNGFX_BF_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return NVNGFX_BF_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR: return NVNGFX_BF_SRC1_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return NVNGFX_BF_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return NVNGFX_BF_SRC1_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return NVNGFX_BF_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return NVNGFX_BF_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return NVNGFX_BF_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return NVNGFX_BF_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return NVNGFX_BF_ONE_MINUS_CONSTANT_ALPHA;
    default: return NVNGFX_BF_UNSUPPORTED;
    }
}

unsigned nvngfx_blend_equation(unsigned nvn) {
    switch (nvn) {
    case NVN_BLEND_EQUATION_ADD: return NVNGFX_EQ_ADD;
    case NVN_BLEND_EQUATION_SUB: return NVNGFX_EQ_SUB;
    case NVN_BLEND_EQUATION_REVERSE_SUB: return NVNGFX_EQ_REVERSE_SUB;
    case NVN_BLEND_EQUATION_MIN: return NVNGFX_EQ_MIN;
    case NVN_BLEND_EQUATION_MAX: return NVNGFX_EQ_MAX;
    default: return NVNGFX_EQ_UNSUPPORTED;
    }
}

unsigned nvngfx_compare_func(unsigned nvn) {
    switch (nvn) {
    case NVN_DEPTH_FUNC_NEVER: return NVNGFX_CMP_NEVER;
    case NVN_DEPTH_FUNC_LESS: return NVNGFX_CMP_LESS;
    case NVN_DEPTH_FUNC_EQUAL: return NVNGFX_CMP_EQUAL;
    case NVN_DEPTH_FUNC_LEQUAL: return NVNGFX_CMP_LEQUAL;
    case NVN_DEPTH_FUNC_GREATER: return NVNGFX_CMP_GREATER;
    case NVN_DEPTH_FUNC_NOTEQUAL: return NVNGFX_CMP_NOTEQUAL;
    case NVN_DEPTH_FUNC_GEQUAL: return NVNGFX_CMP_GEQUAL;
    case NVN_DEPTH_FUNC_ALWAYS: return NVNGFX_CMP_ALWAYS;
    default: return NVNGFX_CMP_UNSUPPORTED;
    }
}

unsigned nvngfx_stencil_op(unsigned nvn) {
    switch (nvn) {
    case NVN_STENCIL_OP_KEEP: return NVNGFX_STENCIL_KEEP;
    case NVN_STENCIL_OP_ZERO: return NVNGFX_STENCIL_ZERO;
    case NVN_STENCIL_OP_REPLACE: return NVNGFX_STENCIL_REPLACE;
    case NVN_STENCIL_OP_INCR: return NVNGFX_STENCIL_INCR;
    case NVN_STENCIL_OP_DECR: return NVNGFX_STENCIL_DECR;
    case NVN_STENCIL_OP_INVERT: return NVNGFX_STENCIL_INVERT;
    case NVN_STENCIL_OP_INCR_WRAP: return NVNGFX_STENCIL_INCR_WRAP;
    case NVN_STENCIL_OP_DECR_WRAP: return NVNGFX_STENCIL_DECR_WRAP;
    default: return NVNGFX_STENCIL_UNSUPPORTED;
    }
}

/* NVN: r = 1, g = 2, b = 4, a = 8 (bit set = that channel is written).
 * Metal: red = 8, green = 4, blue = 2, alpha = 1 -- the opposite order.
 * A straight copy would swap red and alpha, which looks almost right on a
 * grey test image and wrong on everything else. */
unsigned nvngfx_write_mask(unsigned nvn4) {
    const unsigned m = nvn4 & 0xFu;
    return ((m & 0x1u) << 3) | ((m & 0x2u) << 1) | ((m & 0x4u) >> 1) | ((m & 0x8u) >> 3);
}

unsigned nvngfx_cull(int nvn_face) {
    switch (nvn_face) {
    case NVN_FACE_NONE: return NVNGFX_CULL_NONE;
    case NVN_FACE_FRONT: return NVNGFX_CULL_FRONT;
    case NVN_FACE_BACK: return NVNGFX_CULL_BACK;
    default: return NVNGFX_CULL_UNSUPPORTED;
    }
}

unsigned nvngfx_winding(int nvn_front) {
    return nvn_front == 0x0 ? NVNGFX_WINDING_CW : NVNGFX_WINDING_CCW;
}

unsigned nvngfx_fill(int nvn_mode) {
    switch (nvn_mode) {
    case 0x0: return NVNGFX_FILL_UNSUPPORTED;  /* POINT has no Metal equivalent */
    case 0x1: return NVNGFX_FILL_LINE;
    case 0x2: return NVNGFX_FILL_FILL;
    default: return NVNGFX_FILL_UNSUPPORTED;
    }
}

void nvngfx_pipeline_defaults(nvngfx_pipeline_state* st) {
    unsigned i;
    if (!st) return;
    memset(st, 0, sizeof(*st));
    for (i = 0; i < NVNGFX_MAX_COLOR_TARGETS; ++i) {
        st->src_rgb[i] = (unsigned char)NVNGFX_BF_ONE;
        st->dst_rgb[i] = (unsigned char)NVNGFX_BF_ZERO;
        st->src_a[i] = (unsigned char)NVNGFX_BF_ONE;
        st->dst_a[i] = (unsigned char)NVNGFX_BF_ZERO;
        st->eq_rgb[i] = (unsigned char)NVNGFX_EQ_ADD;
        st->eq_a[i] = (unsigned char)NVNGFX_EQ_ADD;
        st->write_mask[i] = 0xFu;   /* Metal order: r|g|b|a */
    }
    st->depth_test = 0;
    st->depth_write = 1;
    st->depth_func = (unsigned char)NVNGFX_CMP_LESS;
    st->stencil_test = 0;
    st->stencil_func[0] = st->stencil_func[1] = (unsigned char)NVNGFX_CMP_ALWAYS;
    for (i = 0; i < 3; ++i) {
        st->stencil_op[0][i] = (unsigned char)NVNGFX_STENCIL_KEEP;
        st->stencil_op[1][i] = (unsigned char)NVNGFX_STENCIL_KEEP;
    }
    st->cull = (unsigned char)NVNGFX_CULL_NONE;
    st->winding = (unsigned char)NVNGFX_WINDING_CCW;
    st->fill = (unsigned char)NVNGFX_FILL_FILL;
    st->polygon_offset = 0;
    st->samples = 1;
    st->alpha_to_coverage = 0;
}

void nvngfx_pipeline_set_blend(nvngfx_pipeline_state* st, const nvngfx_blend_state* b) {
    unsigned slot, f[4], e[2], i;
    if (!st || !b) return;
    /* NVN's blend object names its own colour target; the command buffer keeps
     * one blend state per target.  Applying every bind to target 0 is a real
     * bug for a title that blends into an MRT. */
    slot = b->target & (NVNGFX_MAX_COLOR_TARGETS - 1);
    f[0] = nvngfx_blend_factor(b->src_rgb);
    f[1] = nvngfx_blend_factor(b->dst_rgb);
    f[2] = nvngfx_blend_factor(b->src_a);
    f[3] = nvngfx_blend_factor(b->dst_a);
    e[0] = nvngfx_blend_equation(b->eq_rgb);
    e[1] = nvngfx_blend_equation(b->eq_a);
    for (i = 0; i < 4; ++i)
        if (f[i] == NVNGFX_BF_UNSUPPORTED) st->unsupported |= NVNGFX_UNSUP_BLEND_FACTOR;
    for (i = 0; i < 2; ++i)
        if (e[i] == NVNGFX_EQ_UNSUPPORTED) st->unsupported |= NVNGFX_UNSUP_BLEND_EQUATION;
    st->src_rgb[slot] = (unsigned char)(f[0] ? f[0] : NVNGFX_BF_ONE);
    st->dst_rgb[slot] = (unsigned char)(f[1] ? f[1] : NVNGFX_BF_ZERO);
    st->src_a[slot] = (unsigned char)(f[2] ? f[2] : NVNGFX_BF_ONE);
    st->dst_a[slot] = (unsigned char)(f[3] ? f[3] : NVNGFX_BF_ZERO);
    st->eq_rgb[slot] = (unsigned char)(e[0] ? e[0] : NVNGFX_EQ_ADD);
    st->eq_a[slot] = (unsigned char)(e[1] ? e[1] : NVNGFX_EQ_ADD);
}

void nvngfx_pipeline_set_color(nvngfx_pipeline_state* st, const nvngfx_color_state* c) {
    unsigned i;
    if (!st || !c) return;
    for (i = 0; i < NVNGFX_MAX_COLOR_TARGETS; ++i)
        st->blend_enable[i] = (unsigned char)((c->blend_en >> i) & 1u);
    /* NVN logic ops and the fixed-function alpha test have no Metal
     * equivalent: Metal needs a shader for either.  Report, do not fake. */
    if (c->logic_op != NVN_LOGIC_OP_COPY) st->unsupported |= NVNGFX_UNSUP_LOGIC_OP;
    if (c->alpha_func != NVN_ALPHA_FUNC_ALWAYS) st->unsupported |= NVNGFX_UNSUP_ALPHA_TEST;
}

void nvngfx_pipeline_set_channel_mask(nvngfx_pipeline_state* st,
                                      const nvngfx_channel_mask_state* m) {
    unsigned i;
    if (!st || !m) return;
    for (i = 0; i < NVNGFX_MAX_COLOR_TARGETS; ++i)
        st->write_mask[i] = (unsigned char)nvngfx_write_mask((m->mask >> (4 * i)) & 0xFu);
}

static int pow2_ge1(unsigned v) { return v && (v & (v - 1)) == 0; }

void nvngfx_pipeline_set_multisample(nvngfx_pipeline_state* st,
                                     const nvngfx_multisample_state* m) {
    if (!st || !m) return;
    if (m->enable && m->samples > 1 && pow2_ge1(m->samples)) {
        st->samples = m->samples;
    } else {
        st->samples = 1;
        if (m->enable && m->samples > 1) st->unsupported |= NVNGFX_UNSUP_MULTISAMPLE;
        if (!m->enable && m->samples > 1) st->unsupported |= NVNGFX_UNSUP_MULTISAMPLE;
    }
    st->alpha_to_coverage = m->a2c ? 1 : 0;
    if (m->a2c_dither || m->cov_mod || m->cov2color_en || m->sl_en || m->sl_grid)
        st->unsupported |= NVNGFX_UNSUP_COVERAGE;
    if (m->raster_samples) st->unsupported |= NVNGFX_UNSUP_MULTISAMPLE;
}

void nvngfx_pipeline_set_polygon(nvngfx_pipeline_state* st, const nvngfx_polygon_state* p) {
    unsigned c, f;
    if (!st || !p) return;
    c = nvngfx_cull((int)p->cull);
    if (c == NVNGFX_CULL_UNSUPPORTED) {
        st->unsupported |= NVNGFX_UNSUP_CULL;
        st->cull = (unsigned char)NVNGFX_CULL_NONE;   /* keep the geometry visible */
    } else {
        st->cull = (unsigned char)c;
    }
    st->winding = (unsigned char)nvngfx_winding((int)p->front);
    f = nvngfx_fill((int)p->mode);
    if (f == NVNGFX_FILL_UNSUPPORTED) {
        st->unsupported |= NVNGFX_UNSUP_FILL;
        st->fill = (unsigned char)NVNGFX_FILL_FILL;
    } else {
        st->fill = (unsigned char)f;
    }
    /* NVN has separate POINT/LINE/FILL offset enables (1/2/4).  Metal's
     * setDepthBias only affects filled triangles, so honour FILL and report
     * the other two instead of applying their bias to triangles. */
    st->polygon_offset = (p->offset_en & 0x4u) ? 1 : 0;
    if (p->offset_en & ~0x4u) st->unsupported |= NVNGFX_UNSUP_POLYGON_OFFSET;
}

void nvngfx_pipeline_set_depth_stencil(nvngfx_pipeline_state* st,
                                       const nvngfx_depth_stencil_state* d) {
    unsigned i, f;
    if (!st || !d) return;
    f = nvngfx_compare_func(d->df);
    if (f == NVNGFX_CMP_UNSUPPORTED) {
        st->unsupported |= NVNGFX_UNSUP_DEPTH_FUNC;
        f = NVNGFX_CMP_LESS;
    }
    st->depth_test = d->dt ? 1 : 0;
    st->depth_write = d->dw ? 1 : 0;
    st->depth_func = (unsigned char)f;

    st->stencil_test = d->st ? 1 : 0;
    {
        const unsigned fn[2] = {d->sf_f, d->sf_b};
        const unsigned ops[2][3] = {
            {d->fail_f, d->zfail_f, d->zpass_f},
            {d->fail_b, d->zfail_b, d->zpass_b}
        };
        for (i = 0; i < 2; ++i) {
            unsigned cf = nvngfx_compare_func(fn[i]);
            unsigned j;
            if (cf == NVNGFX_CMP_UNSUPPORTED) {
                st->unsupported |= NVNGFX_UNSUP_STENCIL_FUNC;
                cf = NVNGFX_CMP_ALWAYS;
            }
            st->stencil_func[i] = (unsigned char)cf;
            for (j = 0; j < 3; ++j) {
                unsigned op = nvngfx_stencil_op(ops[i][j]);
                if (op == NVNGFX_STENCIL_UNSUPPORTED) {
                    st->unsupported |= NVNGFX_UNSUP_STENCIL_OP;
                    op = NVNGFX_STENCIL_KEEP;
                }
                st->stencil_op[i][j] = (unsigned char)op;
            }
        }
    }
}

unsigned long long nvngfx_pipeline_key(const nvngfx_pipeline_state* st) {
    const unsigned char* p = (const unsigned char*)st;
    unsigned long long h = 14695981039346656037ull;
    size_t i;
    if (!st) return 0;
    for (i = 0; i < sizeof(*st); ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

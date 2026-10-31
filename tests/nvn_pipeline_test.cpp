/* Portable test: NVN fixed-function state -> backend-neutral pipeline.
 *
 * Everything here runs on a host without Metal.  The point is that the two
 * conversions most likely to look plausible and be wrong are pinned down by
 * value, not by inspection:
 *
 *   - NVN's colour write mask is r=1,g=2,b=4,a=8 while Metal's is the exact
 *     reverse (r=8,g=4,b=2,a=1);
 *   - an NVN blend state names its own colour target, so a bind must land in
 *     slot `target`, not slot 0.
 */
#include "nvn_pipeline.h"

#include <cstdio>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}

static void check_u(unsigned got, unsigned want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %u want %u\n", what, got, want);
        ++failures;
    }
}

struct FactorRow { unsigned nvn; unsigned ours; };

int main() {
    nvngfx_pipeline_state st;

    /* ---- defaults ---- */
    nvngfx_pipeline_defaults(&st);
    for (unsigned i = 0; i < NVNGFX_MAX_COLOR_TARGETS; ++i) {
        check(st.blend_enable[i] == 0, "default blend disabled");
        check_u(st.write_mask[i], 0xFu, "default write mask");
        check_u(st.src_rgb[i], NVNGFX_BF_ONE, "default src rgb");
        check_u(st.dst_rgb[i], NVNGFX_BF_ZERO, "default dst rgb");
        check_u(st.eq_rgb[i], NVNGFX_EQ_ADD, "default eq");
    }
    check_u(st.depth_test, 0, "default depth test");
    check_u(st.depth_write, 1, "default depth write");
    check_u(st.depth_func, NVNGFX_CMP_LESS, "default depth func");
    check_u(st.stencil_test, 0, "default stencil test");
    check_u(st.stencil_func[0], NVNGFX_CMP_ALWAYS, "default stencil func");
    check_u(st.stencil_op[1][2], NVNGFX_STENCIL_KEEP, "default stencil zpass");
    check_u(st.cull, NVNGFX_CULL_NONE, "default cull");
    check_u(st.winding, NVNGFX_WINDING_CCW, "default winding");
    check_u(st.fill, NVNGFX_FILL_FILL, "default fill");
    check_u(st.polygon_offset, 0, "default polygon offset off");
    check_u(st.samples, 1, "default samples");
    check_u(st.unsupported, 0u, "defaults report nothing unsupported");

    /* ---- NVNblendFunc, every value nvn.h defines ---- */
    static const FactorRow factors[] = {
        {0x1,  NVNGFX_BF_ZERO},
        {0x2,  NVNGFX_BF_ONE},
        {0x3,  NVNGFX_BF_SRC_COLOR},
        {0x4,  NVNGFX_BF_ONE_MINUS_SRC_COLOR},
        {0x5,  NVNGFX_BF_SRC_ALPHA},
        {0x6,  NVNGFX_BF_ONE_MINUS_SRC_ALPHA},
        {0x7,  NVNGFX_BF_DST_ALPHA},
        {0x8,  NVNGFX_BF_ONE_MINUS_DST_ALPHA},
        {0x9,  NVNGFX_BF_DST_COLOR},
        {0xA,  NVNGFX_BF_ONE_MINUS_DST_COLOR},
        {0xB,  NVNGFX_BF_SRC_ALPHA_SATURATE},
        {0x10, NVNGFX_BF_SRC1_COLOR},
        {0x11, NVNGFX_BF_ONE_MINUS_SRC1_COLOR},
        {0x12, NVNGFX_BF_SRC1_ALPHA},
        {0x13, NVNGFX_BF_ONE_MINUS_SRC1_ALPHA},
        {0x61, NVNGFX_BF_CONSTANT_COLOR},
        {0x62, NVNGFX_BF_ONE_MINUS_CONSTANT_COLOR},
        {0x63, NVNGFX_BF_CONSTANT_ALPHA},
        {0x64, NVNGFX_BF_ONE_MINUS_CONSTANT_ALPHA},
    };
    for (const FactorRow& r : factors)
        check_u(nvngfx_blend_factor(r.nvn), r.ours, "blend factor");
    check_u(nvngfx_blend_factor(0), NVNGFX_BF_UNSUPPORTED, "factor 0 is not an NVN value");
    check_u(nvngfx_blend_factor(0x7FFFFFFFu), NVNGFX_BF_UNSUPPORTED, "NVN_LARGE factor");
    check_u(nvngfx_blend_factor(0x14), NVNGFX_BF_UNSUPPORTED, "gap after SRC1_ALPHA");

    /* ---- equations ---- */
    check_u(nvngfx_blend_equation(1), NVNGFX_EQ_ADD, "eq add");
    check_u(nvngfx_blend_equation(2), NVNGFX_EQ_SUB, "eq sub");
    check_u(nvngfx_blend_equation(3), NVNGFX_EQ_REVERSE_SUB, "eq reverse sub");
    check_u(nvngfx_blend_equation(4), NVNGFX_EQ_MIN, "eq min");
    check_u(nvngfx_blend_equation(5), NVNGFX_EQ_MAX, "eq max");
    check_u(nvngfx_blend_equation(0), NVNGFX_EQ_UNSUPPORTED, "eq 0");
    check_u(nvngfx_blend_equation(6), NVNGFX_EQ_UNSUPPORTED, "eq 6");

    /* ---- depth / stencil comparison, shared ordering ---- */
    for (unsigned v = 1; v <= 8; ++v)
        check_u(nvngfx_compare_func(v), v, "compare func is 1:1 with nvn");

    /* ---- stencil ops ---- */
    check_u(nvngfx_stencil_op(1), NVNGFX_STENCIL_KEEP, "keep");
    check_u(nvngfx_stencil_op(2), NVNGFX_STENCIL_ZERO, "zero");
    check_u(nvngfx_stencil_op(3), NVNGFX_STENCIL_REPLACE, "replace");
    check_u(nvngfx_stencil_op(4), NVNGFX_STENCIL_INCR, "incr clamp");
    check_u(nvngfx_stencil_op(5), NVNGFX_STENCIL_DECR, "decr clamp");
    check_u(nvngfx_stencil_op(6), NVNGFX_STENCIL_INVERT, "invert");
    check_u(nvngfx_stencil_op(7), NVNGFX_STENCIL_INCR_WRAP, "incr wrap");
    check_u(nvngfx_stencil_op(8), NVNGFX_STENCIL_DECR_WRAP, "decr wrap");
    check_u(nvngfx_stencil_op(9), NVNGFX_STENCIL_UNSUPPORTED, "op 9");

    /* ---- colour write mask: NVN order -> Metal order ---- */
    check_u(nvngfx_write_mask(0xF), 0xF, "mask r+g+b+a");
    check_u(nvngfx_write_mask(0x1), 0x8, "NVN red -> Metal red bit 3");
    check_u(nvngfx_write_mask(0x2), 0x4, "NVN green -> Metal green bit 2");
    check_u(nvngfx_write_mask(0x4), 0x2, "NVN blue -> Metal blue bit 1");
    check_u(nvngfx_write_mask(0x8), 0x1, "NVN alpha -> Metal alpha bit 0");
    check_u(nvngfx_write_mask(0x5), 0xA, "NVN red+blue -> Metal 0xA");

    /* ---- blend state binds to its own target ---- */
    nvngfx_blend_state b;
    nvngfx_blend_defaults(&b);
    b.target = 2;
    b.src_rgb = 5;      /* SRC_ALPHA */
    b.dst_rgb = 6;      /* ONE_MINUS_SRC_ALPHA */
    b.eq_rgb = 2;       /* SUB */
    nvngfx_pipeline_set_blend(&st, &b);
    check_u(st.src_rgb[0], NVNGFX_BF_ONE, "target 0 untouched");
    check_u(st.src_rgb[2], NVNGFX_BF_SRC_ALPHA, "target 2 src");
    check_u(st.dst_rgb[2], NVNGFX_BF_ONE_MINUS_SRC_ALPHA, "target 2 dst");
    check_u(st.eq_rgb[2], NVNGFX_EQ_SUB, "target 2 eq");
    check_u(st.unsupported, 0u, "known blend values report nothing");

    nvngfx_pipeline_defaults(&st);
    b.target = 0;
    b.dst_rgb = 0x7FFFFFFFu;     /* NVN_BLEND_FUNC_LARGE */
    nvngfx_pipeline_set_blend(&st, &b);
    check((st.unsupported & NVNGFX_UNSUP_BLEND_FACTOR) != 0, "unknown factor is reported");
    check_u(st.dst_rgb[0], NVNGFX_BF_ZERO, "unknown factor falls back to a no-op");

    /* ---- colour state ---- */
    nvngfx_pipeline_defaults(&st);
    nvngfx_color_state c;
    nvngfx_color_defaults(&c);
    c.blend_en = 0x0F;
    nvngfx_pipeline_set_color(&st, &c);
    check_u(st.blend_enable[0], 1, "target 0 blend on");
    check_u(st.blend_enable[3], 1, "target 3 blend on");
    check_u(st.blend_enable[4], 0, "target 4 blend off");
    check_u(st.unsupported, 0u, "COPY/ALWAYS is fully supported");

    c.logic_op = 6;              /* XOR */
    nvngfx_pipeline_set_color(&st, &c);
    check((st.unsupported & NVNGFX_UNSUP_LOGIC_OP) != 0, "logic op is reported");
    c.logic_op = 3;
    c.alpha_func = 4;            /* LEQUAL: alpha test */
    nvngfx_pipeline_set_color(&st, &c);
    check((st.unsupported & NVNGFX_UNSUP_ALPHA_TEST) != 0, "alpha test is reported");

    /* ---- channel mask, per target ---- */
    nvngfx_pipeline_defaults(&st);
    nvngfx_channel_mask_state cm;
    nvngfx_channel_mask_defaults(&cm);
    cm.mask = 0x12345678u;
    nvngfx_pipeline_set_channel_mask(&st, &cm);
    check_u(st.write_mask[0], 0x1, "target 0 nibble 8 -> alpha only");
    check_u(st.write_mask[1], 0xE, "target 1 nibble 7 -> rgb");
    check_u(st.write_mask[2], 0x6, "target 2 nibble 6 -> gb");
    check_u(st.write_mask[3], 0xA, "target 3 nibble 5 -> rb");
    check_u(st.write_mask[7], 0x8, "target 7 nibble 1 -> red only");

    /* ---- multisample ---- */
    nvngfx_pipeline_defaults(&st);
    nvngfx_multisample_state m;
    nvngfx_multisample_defaults(&m);
    m.samples = 4;
    nvngfx_pipeline_set_multisample(&st, &m);
    check_u(st.samples, 4, "4x multisample kept");
    check_u(st.unsupported, 0u, "plain 4x needs no report");

    nvngfx_pipeline_defaults(&st);
    m.samples = 3;               /* not a power of two */
    nvngfx_pipeline_set_multisample(&st, &m);
    check_u(st.samples, 1, "3x is refused");
    check((st.unsupported & NVNGFX_UNSUP_MULTISAMPLE) != 0, "3x is reported");

    nvngfx_pipeline_defaults(&st);
    nvngfx_multisample_defaults(&m);
    m.samples = 2; m.a2c = 1; m.cov_mod = 1;
    nvngfx_pipeline_set_multisample(&st, &m);
    check_u(st.alpha_to_coverage, 1, "alpha to coverage kept");
    check((st.unsupported & NVNGFX_UNSUP_COVERAGE) != 0, "coverage modulation reported");

    /* ---- polygon ---- */
    nvngfx_pipeline_defaults(&st);
    nvngfx_polygon_state p;
    nvngfx_polygon_defaults(&p);
    p.cull = 2;                  /* NVN_FACE_BACK */
    p.front = 0;                 /* CW */
    p.mode = 1;                  /* LINE */
    nvngfx_pipeline_set_polygon(&st, &p);
    check_u(st.cull, NVNGFX_CULL_BACK, "cull back");
    check_u(st.winding, NVNGFX_WINDING_CW, "clockwise front");
    check_u(st.fill, NVNGFX_FILL_LINE, "line fill");
    check_u(st.unsupported, 0u, "polygon values all map");

    p.cull = 3;                  /* FRONT_AND_BACK: no single Metal mode */
    p.mode = 0;                  /* POINT: no Metal fill mode */
    p.offset_en = 1;
    nvngfx_pipeline_set_polygon(&st, &p);
    check((st.unsupported & NVNGFX_UNSUP_CULL) != 0, "front+back cull reported");
    check((st.unsupported & NVNGFX_UNSUP_FILL) != 0, "point fill reported");
    check((st.unsupported & NVNGFX_UNSUP_POLYGON_OFFSET) != 0,
          "point-only polygon offset reported");
    check_u(st.polygon_offset, 0, "point-only offset is not applied to triangles");
    check_u(st.cull, NVNGFX_CULL_NONE, "unsupported cull keeps geometry visible");
    check_u(st.fill, NVNGFX_FILL_FILL, "unsupported fill falls back to fill");

    nvngfx_pipeline_defaults(&st);
    p.cull = 0;
    p.mode = 2;                  /* FILL */
    p.offset_en = 4;             /* NVN_POLYGON_OFFSET_ENABLE_FILL */
    nvngfx_pipeline_set_polygon(&st, &p);
    check_u(st.polygon_offset, 1, "fill polygon offset enabled");
    check_u(st.unsupported, 0u, "fill-only polygon offset needs no report");

    /* ---- depth / stencil ---- */
    nvngfx_pipeline_defaults(&st);
    nvngfx_depth_stencil_state d;
    nvngfx_depth_stencil_defaults(&d);
    d.dt = 1; d.df = 4;          /* LEQUAL */
    d.st = 1;
    d.sf_f = 3; d.sf_b = 6;      /* EQUAL / NOTEQUAL */
    d.fail_f = 3; d.zfail_f = 4; d.zpass_f = 5;   /* REPLACE / INCR / DECR */
    nvngfx_pipeline_set_depth_stencil(&st, &d);
    check_u(st.depth_test, 1, "depth test on");
    check_u(st.depth_write, 1, "depth write on");
    check_u(st.depth_func, NVNGFX_CMP_LEQUAL, "depth LEQUAL");
    check_u(st.stencil_test, 1, "stencil on");
    check_u(st.stencil_func[0], NVNGFX_CMP_EQUAL, "stencil front EQUAL");
    check_u(st.stencil_func[1], NVNGFX_CMP_NOTEQUAL, "stencil back NOTEQUAL");
    check_u(st.stencil_op[0][0], NVNGFX_STENCIL_REPLACE, "front fail op");
    check_u(st.stencil_op[0][1], NVNGFX_STENCIL_INCR, "front zfail op");
    check_u(st.stencil_op[0][2], NVNGFX_STENCIL_DECR, "front zpass op");
    check_u(st.unsupported, 0u, "depth/stencil values all map");

    d.df = 0x7FFFFFFFu;
    nvngfx_pipeline_set_depth_stencil(&st, &d);
    check((st.unsupported & NVNGFX_UNSUP_DEPTH_FUNC) != 0, "unknown depth func reported");
    check_u(st.depth_func, NVNGFX_CMP_LESS, "unknown depth func falls back to LESS");

    /* ---- the cache key ---- */
    nvngfx_pipeline_state a1, a2;
    nvngfx_pipeline_defaults(&a1);
    nvngfx_pipeline_defaults(&a2);
    check(nvngfx_pipeline_key(&a1) == nvngfx_pipeline_key(&a2), "equal state, equal key");
    a2.cull = NVNGFX_CULL_BACK;
    check(nvngfx_pipeline_key(&a1) != nvngfx_pipeline_key(&a2), "cull changes the key");
    nvngfx_pipeline_defaults(&a2);
    a2.write_mask[3] = 0x1;
    check(nvngfx_pipeline_key(&a1) != nvngfx_pipeline_key(&a2), "write mask changes the key");
    nvngfx_pipeline_defaults(&a2);
    a2.blend_enable[7] = 1;
    check(nvngfx_pipeline_key(&a1) != nvngfx_pipeline_key(&a2), "blend enable changes the key");
    nvngfx_pipeline_defaults(&a2);
    a2.stencil_op[1][1] = NVNGFX_STENCIL_INVERT;
    check(nvngfx_pipeline_key(&a1) != nvngfx_pipeline_key(&a2), "stencil op changes the key");

    if (failures) {
        std::fprintf(stderr, "nvn_pipeline_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("nvn_pipeline_test: ok (defaults, enum tables, write-mask order, "
                "blend targets, unsupported reporting, cache key)\n");
    return 0;
}

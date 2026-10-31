/* Portable test: nvn_state.c's defaults and bit layouts must match the guest.
 * These are the same assertions the Python harness makes in _test_stateobj.py.
 */
#include "nvn_state.h"

#include <cstdio>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}

static unsigned long long f(unsigned long long v, unsigned sh, unsigned w) {
    return (v >> sh) & ((1ull << w) - 1ull);
}

int main() {
    nvngfx_blend_state b;
    nvngfx_blend_defaults(&b);
    unsigned long long bb = nvngfx_blend_pack(&b);
    check(f(bb, 0, 3) == 0 && f(bb, 3, 7) == 2 && f(bb, 10, 7) == 1 &&
          f(bb, 17, 7) == 2 && f(bb, 24, 7) == 1 &&
          f(bb, 31, 3) == 1 && f(bb, 34, 3) == 1 &&
          f(bb, 37, 6) == 0 && f(bb, 43, 1) == 0 && f(bb, 44, 2) == 0,
          "blend defaults");

    b.target = 2; b.src_rgb = 5; b.dst_rgb = 6; b.src_a = 7; b.dst_a = 8;
    b.eq_rgb = 2; b.eq_a = 3; b.adv = 0x16; b.adv_npm = 1; b.adv_ovl = 2;
    nvngfx_blend_state br;
    nvngfx_blend_unpack(&br, nvngfx_blend_pack(&b));
    check(br.target == 2 && br.src_rgb == 5 && br.dst_rgb == 6 &&
          br.src_a == 7 && br.dst_a == 8 && br.eq_rgb == 2 && br.eq_a == 3 &&
          br.adv == 0x16 && br.adv_npm == 1 && br.adv_ovl == 2,
          "blend round trip");

    nvngfx_color_state c;
    nvngfx_color_defaults(&c);
    check(nvngfx_color_pack(&c) == 0x8300u, "color defaults");
    c.blend_en = 0x08; c.logic_op = 6; c.alpha_func = 4;
    nvngfx_color_state cr;
    nvngfx_color_unpack(&cr, nvngfx_color_pack(&c));
    check(cr.blend_en == 8 && cr.logic_op == 6 && cr.alpha_func == 4,
          "color round trip");

    nvngfx_channel_mask_state cm;
    nvngfx_channel_mask_defaults(&cm);
    check(nvngfx_channel_mask_pack(&cm) == 0xFFFFFFFFu, "channel mask defaults");
    cm.mask = 0x12345678u;
    nvngfx_channel_mask_state cmr;
    nvngfx_channel_mask_unpack(&cmr, nvngfx_channel_mask_pack(&cm));
    check(cmr.mask == 0x12345678u, "channel mask round trip");

    nvngfx_multisample_state m;
    nvngfx_multisample_defaults(&m);
    unsigned char mb[10];
    static const unsigned char md[10] = {1,0,0,0,0,0,0,0,0,0};
    nvngfx_multisample_pack(&m, mb);
    check(std::memcmp(mb, md, 10) == 0, "multisample defaults");
    m.enable=0; m.samples=4; m.a2c=1; m.a2c_dither=1; m.raster_samples=2;
    m.cov_mod=3; m.cov2color_en=1; m.cov2color_out=1; m.sl_en=1; m.sl_grid=1;
    nvngfx_multisample_pack(&m, mb);
    static const unsigned char md2[10] = {0,4,1,1,2,3,1,1,1,1};
    check(std::memcmp(mb, md2, 10) == 0, "multisample fields");
    nvngfx_multisample_state mx;
    nvngfx_multisample_unpack(&mx, mb);
    check(mx.sl_grid == 1 && mx.samples == 4 && mx.cov_mod == 3,
          "multisample round trip");

    nvngfx_polygon_state p;
    nvngfx_polygon_defaults(&p);
    check(nvngfx_polygon_pack(&p) == 0x14u, "polygon defaults");
    p.cull=2; p.front=0; p.mode=1; p.offset_en=5;
    nvngfx_polygon_state pr;
    nvngfx_polygon_unpack(&pr, nvngfx_polygon_pack(&p));
    check(pr.cull==2 && pr.front==0 && pr.mode==1 && pr.offset_en==5,
          "polygon round trip");

    nvngfx_depth_stencil_state d;
    nvngfx_depth_stencil_defaults(&d);
    unsigned long long db = nvngfx_depth_stencil_pack(&d);
    check(f(db,0,1)==0 && f(db,1,1)==1 && f(db,2,4)==2 && f(db,6,1)==0 &&
          f(db,7,4)==8 && f(db,11,4)==8 && f(db,15,4)==1 && f(db,19,4)==1 &&
          f(db,23,4)==1 && f(db,27,4)==1 && f(db,31,4)==1 && f(db,35,4)==1,
          "depth stencil defaults");
    d.dt=1; d.dw=0; d.df=5; d.st=1; d.sf_f=3; d.sf_b=6;
    d.fail_f=3; d.zfail_f=2; d.zpass_f=4;
    d.fail_b=6; d.zfail_b=5; d.zpass_b=1;
    nvngfx_depth_stencil_state dr;
    nvngfx_depth_stencil_unpack(&dr, nvngfx_depth_stencil_pack(&d));
    check(dr.dt==1 && dr.dw==0 && dr.df==5 && dr.st==1 && dr.sf_f==3 &&
          dr.sf_b==6 && dr.fail_f==3 && dr.zfail_f==2 && dr.zpass_f==4 &&
          dr.fail_b==6 && dr.zfail_b==5 && dr.zpass_b==1,
          "depth stencil round trip");

    if (failures) return 1;
    std::puts("nvn_state_test: ok (defaults, pack/unpack, round trip)");
    return 0;
}

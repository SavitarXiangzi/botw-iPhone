#include "nvn_state.h"

#include <string.h>

static unsigned long long bf(unsigned long long v, unsigned shift, unsigned width) {
    return (v >> shift) & ((1ull << width) - 1ull);
}

static unsigned long long put(unsigned long long old, unsigned shift, unsigned width,
                              unsigned long long value) {
    const unsigned long long mask = ((1ull << width) - 1ull) << shift;
    return (old & ~mask) | ((value & ((1ull << width) - 1ull)) << shift);
}

/* Blend: NVN defaults are target 0, (ONE, ZERO, ONE, ZERO), ADD/ADD. */
void nvngfx_blend_defaults(nvngfx_blend_state* s) {
    if (!s) return;
    s->target = 0;
    s->src_rgb = 2; s->dst_rgb = 1;
    s->src_a = 2; s->dst_a = 1;
    s->eq_rgb = 1; s->eq_a = 1;
}

unsigned long long nvngfx_blend_pack(const nvngfx_blend_state* s) {
    unsigned long long v = 0;
    if (!s) return 0;
    v = put(v, 0, 3, s->target);
    v = put(v, 3, 7, s->src_rgb);
    v = put(v, 10, 7, s->dst_rgb);
    v = put(v, 17, 7, s->src_a);
    v = put(v, 24, 7, s->dst_a);
    v = put(v, 31, 3, s->eq_rgb);
    v = put(v, 34, 3, s->eq_a);
    return v;
}

void nvngfx_blend_unpack(nvngfx_blend_state* s, unsigned long long v) {
    if (!s) return;
    s->target = (unsigned)bf(v, 0, 3);
    s->src_rgb = (unsigned)bf(v, 3, 7);
    s->dst_rgb = (unsigned)bf(v, 10, 7);
    s->src_a = (unsigned)bf(v, 17, 7);
    s->dst_a = (unsigned)bf(v, 24, 7);
    s->eq_rgb = (unsigned)bf(v, 31, 3);
    s->eq_a = (unsigned)bf(v, 34, 3);
}

/* Color: defaults are blend disabled, COPY, ALWAYS. */
void nvngfx_color_defaults(nvngfx_color_state* s) {
    if (!s) return;
    s->blend_en = 0; s->logic_op = 3; s->alpha_func = 8;
}

unsigned nvngfx_color_pack(const nvngfx_color_state* s) {
    unsigned v = 0;
    if (!s) return 0;
    v = (unsigned)put(v, 0, 8, s->blend_en);
    v = (unsigned)put(v, 8, 4, s->logic_op);
    v = (unsigned)put(v, 12, 4, s->alpha_func);
    return v;
}

void nvngfx_color_unpack(nvngfx_color_state* s, unsigned v) {
    if (!s) return;
    s->blend_en = (unsigned)bf(v, 0, 8);
    s->logic_op = (unsigned)bf(v, 8, 4);
    s->alpha_func = (unsigned)bf(v, 12, 4);
}

/* Channel mask: every target writes every channel by default. */
void nvngfx_channel_mask_defaults(nvngfx_channel_mask_state* s) {
    if (s) s->mask = 0xFFFFFFFFu;
}

unsigned nvngfx_channel_mask_pack(const nvngfx_channel_mask_state* s) {
    return s ? s->mask : 0u;
}

void nvngfx_channel_mask_unpack(nvngfx_channel_mask_state* s, unsigned v) {
    if (s) s->mask = v;
}

/* Multisample: enabled by default, every other byte zero. */
void nvngfx_multisample_defaults(nvngfx_multisample_state* s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->enable = 1;
}

void nvngfx_multisample_pack(const nvngfx_multisample_state* s, unsigned char out[10]) {
    if (!out) return;
    if (!s) { memset(out, 0, 10); return; }
    out[0] = s->enable; out[1] = s->samples; out[2] = s->a2c;
    out[3] = s->a2c_dither; out[4] = s->raster_samples; out[5] = s->cov_mod;
    out[6] = s->cov2color_en; out[7] = s->cov2color_out;
    out[8] = s->sl_en; out[9] = s->sl_grid;
}

void nvngfx_multisample_unpack(nvngfx_multisample_state* s, const unsigned char in[10]) {
    if (!s) return;
    if (!in) { nvngfx_multisample_defaults(s); return; }
    s->enable = in[0]; s->samples = in[1]; s->a2c = in[2];
    s->a2c_dither = in[3]; s->raster_samples = in[4]; s->cov_mod = in[5];
    s->cov2color_en = in[6]; s->cov2color_out = in[7];
    s->sl_en = in[8]; s->sl_grid = in[9];
}

/* Polygon: no culling, CCW = front, fill, no polygon offset. */
void nvngfx_polygon_defaults(nvngfx_polygon_state* s) {
    if (!s) return;
    s->cull = 0; s->front = 1; s->mode = 2; s->offset_en = 0;
}

unsigned nvngfx_polygon_pack(const nvngfx_polygon_state* s) {
    unsigned v = 0;
    if (!s) return 0;
    v = (unsigned)put(v, 0, 2, s->cull);
    v = (unsigned)put(v, 2, 1, s->front);
    v = (unsigned)put(v, 3, 2, s->mode);
    v = (unsigned)put(v, 5, 3, s->offset_en);
    return v;
}

void nvngfx_polygon_unpack(nvngfx_polygon_state* s, unsigned v) {
    if (!s) return;
    s->cull = (unsigned)bf(v, 0, 2);
    s->front = (unsigned)bf(v, 2, 1);
    s->mode = (unsigned)bf(v, 3, 2);
    s->offset_en = (unsigned)bf(v, 5, 3);
}

/* Depth/stencil: depth writes + LESS; stencil disabled, ALWAYS/KEEP. */
void nvngfx_depth_stencil_defaults(nvngfx_depth_stencil_state* s) {
    if (!s) return;
    s->dt = 0; s->dw = 1; s->df = 2; s->st = 0;
    s->sf_f = 8; s->sf_b = 8;
    s->fail_f = 1; s->zfail_f = 1; s->zpass_f = 1;
    s->fail_b = 1; s->zfail_b = 1; s->zpass_b = 1;
}

unsigned long long nvngfx_depth_stencil_pack(const nvngfx_depth_stencil_state* s) {
    unsigned long long v = 0;
    if (!s) return 0;
    v = put(v, 0, 1, s->dt); v = put(v, 1, 1, s->dw);
    v = put(v, 2, 4, s->df); v = put(v, 6, 1, s->st);
    v = put(v, 7, 4, s->sf_f); v = put(v, 11, 4, s->sf_b);
    v = put(v, 15, 4, s->fail_f); v = put(v, 19, 4, s->zfail_f);
    v = put(v, 23, 4, s->zpass_f); v = put(v, 27, 4, s->fail_b);
    v = put(v, 31, 4, s->zfail_b); v = put(v, 35, 4, s->zpass_b);
    return v;
}

void nvngfx_depth_stencil_unpack(nvngfx_depth_stencil_state* s, unsigned long long v) {
    if (!s) return;
    s->dt = (unsigned)bf(v, 0, 1); s->dw = (unsigned)bf(v, 1, 1);
    s->df = (unsigned)bf(v, 2, 4); s->st = (unsigned)bf(v, 6, 1);
    s->sf_f = (unsigned)bf(v, 7, 4); s->sf_b = (unsigned)bf(v, 11, 4);
    s->fail_f = (unsigned)bf(v, 15, 4); s->zfail_f = (unsigned)bf(v, 19, 4);
    s->zpass_f = (unsigned)bf(v, 23, 4); s->fail_b = (unsigned)bf(v, 27, 4);
    s->zfail_b = (unsigned)bf(v, 31, 4); s->zpass_b = (unsigned)bf(v, 35, 4);
}

/* Fixed-function NVN state objects: the exact guest bit layouts and defaults.
 *
 * The layouts are from nxnative's measured nvn_obj.h (MIT), the same source
 * the harness uses.  They are kept in a portable C file so a Windows/Linux
 * test can assert every field without Metal or an Apple SDK.
 *
 * Nothing here translates an NVN enum.  A field is stored exactly as the guest
 * wrote it; the Metal layer decides whether it understands a value.
 */
#ifndef NVN_STATE_H
#define NVN_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* NVNblendState: uint64
 * { target:3, src_rgb:7, dst_rgb:7, src_a:7, dst_a:7, eq_rgb:3, eq_a:3,
 *   adv:6, adv_npm:1, adv_ovl:2 }
 * The advanced fields are nxnative's measured layout (nvn_obj.h, MIT):
 * adv is NVNblendAdvancedMode, adv_npm is the inverse of
 * SetAdvancedPremultipliedSrc, adv_ovl is NVNblendAdvancedOverlap. */
typedef struct {
    unsigned target, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a;
    unsigned adv, adv_npm, adv_ovl;
} nvngfx_blend_state;

/* NVNcolorState: uint32 { blend_en:8, logic_op:4, alpha_func:4 } */
typedef struct {
    unsigned blend_en, logic_op, alpha_func;
} nvngfx_color_state;

/* NVNchannelMaskState: uint32, four write-enable bits per target. */
typedef struct {
    unsigned mask;
} nvngfx_channel_mask_state;

/* NVNmultisampleState: ten bytes, in declaration order. */
typedef struct {
    unsigned char enable, samples, a2c, a2c_dither, raster_samples;
    unsigned char cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid;
} nvngfx_multisample_state;

/* NVNpolygonState: uint32 { cull:2, front:1, mode:2, offset_en:3 } */
typedef struct {
    unsigned cull, front, mode, offset_en;
} nvngfx_polygon_state;

/* NVNdepthStencilState: uint64
 * { dt:1, dw:1, df:4, st:1, sf_f:4, sf_b:4,
 *   fail_f:4, zfail_f:4, zpass_f:4, fail_b:4, zfail_b:4, zpass_b:4 } */
typedef struct {
    unsigned dt, dw, df, st, sf_f, sf_b;
    unsigned fail_f, zfail_f, zpass_f, fail_b, zfail_b, zpass_b;
} nvngfx_depth_stencil_state;

void nvngfx_blend_defaults(nvngfx_blend_state* s);
unsigned long long nvngfx_blend_pack(const nvngfx_blend_state* s);
void nvngfx_blend_unpack(nvngfx_blend_state* s, unsigned long long bits);

void nvngfx_color_defaults(nvngfx_color_state* s);
unsigned nvngfx_color_pack(const nvngfx_color_state* s);
void nvngfx_color_unpack(nvngfx_color_state* s, unsigned bits);

void nvngfx_channel_mask_defaults(nvngfx_channel_mask_state* s);
unsigned nvngfx_channel_mask_pack(const nvngfx_channel_mask_state* s);
void nvngfx_channel_mask_unpack(nvngfx_channel_mask_state* s, unsigned bits);

void nvngfx_multisample_defaults(nvngfx_multisample_state* s);
void nvngfx_multisample_pack(const nvngfx_multisample_state* s, unsigned char out[10]);
void nvngfx_multisample_unpack(nvngfx_multisample_state* s, const unsigned char in[10]);

void nvngfx_polygon_defaults(nvngfx_polygon_state* s);
unsigned nvngfx_polygon_pack(const nvngfx_polygon_state* s);
void nvngfx_polygon_unpack(nvngfx_polygon_state* s, unsigned bits);

void nvngfx_depth_stencil_defaults(nvngfx_depth_stencil_state* s);
unsigned long long nvngfx_depth_stencil_pack(const nvngfx_depth_stencil_state* s);
void nvngfx_depth_stencil_unpack(nvngfx_depth_stencil_state* s, unsigned long long bits);

#ifdef __cplusplus
}
#endif
#endif /* NVN_STATE_H */

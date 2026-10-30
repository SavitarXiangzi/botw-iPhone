/* NVN sampler state -> the handful of values Metal names.
 *
 * No Apple headers here: this file has to compile with MSVC as well, so it
 * produces neutral enums and nvn_metal.mm turns those into MTL constants.
 * The NVN numbers come from nxnative's nvn.h (see nvn_sampler.h).
 */
#include <string.h>

#include "nvn_sampler.h"

void nvngfx_sampler_defaults(nvngfx_sampler* s) {
    if (!s) return;
    /* hle_nvn.c nvnSamplerBuilderSetDefaults: GL defaults.  The border is
     * left as the zeroed struct has it, i.e. transparent black. */
    s->min_filter = NVNGFX_TEXFILTER_NEAREST;
    s->mip_filter = NVNGFX_MIPFILTER_LINEAR;
    s->mag_filter = NVNGFX_TEXFILTER_LINEAR;
    s->wrap_s = NVNGFX_WRAP_REPEAT;
    s->wrap_t = NVNGFX_WRAP_REPEAT;
    s->wrap_r = NVNGFX_WRAP_REPEAT;
    s->nvn_min_filter = 4;      /* NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR */
    s->nvn_mag_filter = 1;      /* NVN_MAG_FILTER_LINEAR */
    s->nvn_wrap_s = s->nvn_wrap_t = s->nvn_wrap_r = 1;  /* NVN_WRAP_MODE_REPEAT */
    s->compare = 0;
    s->reduction = NVNGFX_REDUCTION_AVERAGE;
    s->nvn_compare_mode = 0;    /* NVN_COMPARE_MODE_NONE */
    s->nvn_compare_func = 2;    /* NVN_COMPARE_FUNC_LESS */
    s->nvn_reduction = 0;       /* NVN_SAMPLER_REDUCTION_AVERAGE */
    s->approximate = 0;
    s->reserved[0] = s->reserved[1] = s->reserved[2] = 0;
    s->lod_bias = 0.0f;
    s->lod_min = -1000.0f;
    s->lod_max = 1000.0f;
    s->max_aniso = 1.0f;
    s->border[0] = 0.0f;
    s->border[1] = 0.0f;
    s->border[2] = 0.0f;
    s->border[3] = 0.0f;
}

/* NVNminFilter 0..5 already encodes the mipmap choice:
 *   0 nearest          1 linear
 *   2 nearest, nearest 3 linear, nearest      (mip nearest)
 *   4 nearest, linear  5 linear, linear       (mip linear)
 */
int nvngfx_sampler_set_minmag(nvngfx_sampler* s, unsigned nvn_min, unsigned nvn_mag) {
    unsigned char minf, mipf, magf;
    if (!s) return -1;
    switch (nvn_min) {
    case 0: minf = NVNGFX_TEXFILTER_NEAREST; mipf = NVNGFX_MIPFILTER_NONE; break;
    case 1: minf = NVNGFX_TEXFILTER_LINEAR;  mipf = NVNGFX_MIPFILTER_NONE; break;
    case 2: minf = NVNGFX_TEXFILTER_NEAREST; mipf = NVNGFX_MIPFILTER_NEAREST; break;
    case 3: minf = NVNGFX_TEXFILTER_LINEAR;  mipf = NVNGFX_MIPFILTER_NEAREST; break;
    case 4: minf = NVNGFX_TEXFILTER_NEAREST; mipf = NVNGFX_MIPFILTER_LINEAR; break;
    case 5: minf = NVNGFX_TEXFILTER_LINEAR;  mipf = NVNGFX_MIPFILTER_LINEAR; break;
    default: return -1;
    }
    switch (nvn_mag) {
    case 0: magf = NVNGFX_TEXFILTER_NEAREST; break;
    case 1: magf = NVNGFX_TEXFILTER_LINEAR;  break;
    default: return -1;
    }
    /* Validate both before changing either: a rejected pair must not leave
     * half of the previous sampler state overwritten. */
    s->min_filter = minf;
    s->mip_filter = mipf;
    s->mag_filter = magf;
    s->nvn_min_filter = (unsigned char)nvn_min;
    s->nvn_mag_filter = (unsigned char)nvn_mag;
    return 0;
}

/* NVNwrapMode 0..7 -> the four addressing modes Metal has.  There is no
 * mirrored clamp-to-border, so 4 lands on clamp-to-border and says so. */
static int wrap_map(unsigned w) {
    switch (w) {
    case 0: return NVNGFX_WRAP_CLAMP_TO_EDGE;        /* NVN_WRAP_MODE_CLAMP */
    case 1: return NVNGFX_WRAP_REPEAT;
    case 2: return NVNGFX_WRAP_MIRROR_CLAMP_TO_EDGE; /* NVN_WRAP_MODE_MIRROR_CLAMP */
    case 3: return NVNGFX_WRAP_MIRROR_CLAMP_TO_EDGE;
    case 4: return NVNGFX_WRAP_CLAMP_TO_BORDER;      /* mirror border: approximated */
    case 5: return NVNGFX_WRAP_CLAMP_TO_BORDER;
    case 6: return NVNGFX_WRAP_MIRRORED_REPEAT;
    case 7: return NVNGFX_WRAP_CLAMP_TO_EDGE;
    default: return -1;
    }
}

int nvngfx_sampler_set_wrap(nvngfx_sampler* s, unsigned s_wrap, unsigned t_wrap,
                            unsigned r_wrap) {
    if (!s) return -1;
    const int ws = wrap_map(s_wrap);
    const int wt = wrap_map(t_wrap);
    const int wr = wrap_map(r_wrap);
    if (ws < 0 || wt < 0 || wr < 0) return -1;
    s->wrap_s = (unsigned char)ws;
    s->wrap_t = (unsigned char)wt;
    s->wrap_r = (unsigned char)wr;
    s->nvn_wrap_s = (unsigned char)s_wrap;
    s->nvn_wrap_t = (unsigned char)t_wrap;
    s->nvn_wrap_r = (unsigned char)r_wrap;
    if (s_wrap == 4 || t_wrap == 4 || r_wrap == 4) s->approximate |= NVNGFX_SAMPLER_APPROX_WRAP;
    return 0;
}

/* NVNcompareMode: 0 none, 1 compare R to texture.
 * NVNcompareFunc 1..8 (NEVER..ALWAYS) is Metal's 0..7 plus one. */
int nvngfx_sampler_set_compare(nvngfx_sampler* s, unsigned nvn_mode, unsigned nvn_func) {
    if (!s) return -1;
    if (nvn_mode == 0) {
        s->compare = 0;
        s->nvn_compare_mode = 0;
        s->nvn_compare_func = (unsigned char)nvn_func;
        return 0;
    }
    if (nvn_mode != 1) return -1;
    if (nvn_func < 1 || nvn_func > 8) return -1;
    s->compare = (unsigned char)(nvn_func - 1);
    s->nvn_compare_mode = 1;
    s->nvn_compare_func = (unsigned char)nvn_func;
    return 0;
}

int nvngfx_sampler_set_reduction(nvngfx_sampler* s, unsigned nvn_reduction) {
    if (!s) return -1;
    if (nvn_reduction > 2) return -1;
    s->reduction = (unsigned char)nvn_reduction;
    s->nvn_reduction = (unsigned char)nvn_reduction;
    return 0;
}

/* Keep the guest's exact values: nvnSamplerGetLodClamp must return them.
 * Metal's descriptor needs a non-negative, ordered clamp; create_sampler_state
 * does that mapping and this flag records that it is not a bit-exact copy. */
void nvngfx_sampler_set_lod_clamp(nvngfx_sampler* s, float lo, float hi) {
    if (!s) return;
    s->lod_min = lo;
    s->lod_max = hi;
    if (lo < 0.0f || hi < lo) s->approximate |= NVNGFX_SAMPLER_APPROX_LOD;
}

/* Metal has no LOD bias on a sampler; it is kept for the getter and flagged. */
void nvngfx_sampler_set_lod_bias(nvngfx_sampler* s, float v) {
    if (!s) return;
    s->lod_bias = v;
    if (v != 0.0f) s->approximate |= NVNGFX_SAMPLER_APPROX_BIAS;
}

void nvngfx_sampler_set_anisotropy(nvngfx_sampler* s, float v) {
    if (!s) return;
    s->max_aniso = v;
    if (v < 1.0f || v > 16.0f) s->approximate |= NVNGFX_SAMPLER_APPROX_ANISO;
}

/* Metal's border colour is one of three constants, so only those three
 * values survive exactly; anything else becomes opaque black and says so. */
void nvngfx_sampler_set_border(nvngfx_sampler* s, const float rgba[4]) {
    int i;
    if (!s || !rgba) return;
    for (i = 0; i < 4; ++i) s->border[i] = rgba[i];
    const int transparent = rgba[0] == 0.0f && rgba[1] == 0.0f && rgba[2] == 0.0f && rgba[3] == 0.0f;
    const int opaque_black = rgba[0] == 0.0f && rgba[1] == 0.0f && rgba[2] == 0.0f && rgba[3] == 1.0f;
    const int opaque_white = rgba[0] == 1.0f && rgba[1] == 1.0f && rgba[2] == 1.0f && rgba[3] == 1.0f;
    if (!transparent && !opaque_black && !opaque_white) s->approximate |= NVNGFX_SAMPLER_APPROX_BORDER;
}

/* Same fields nxnative compares: the exact NVN state, not the Metal mapping.
 * Debug labels and approximation flags are host bookkeeping and do not make
 * two guest samplers different. */
int nvngfx_sampler_equals(const nvngfx_sampler* a, const nvngfx_sampler* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->nvn_min_filter != b->nvn_min_filter || a->nvn_mag_filter != b->nvn_mag_filter ||
        a->nvn_wrap_s != b->nvn_wrap_s || a->nvn_wrap_t != b->nvn_wrap_t ||
        a->nvn_wrap_r != b->nvn_wrap_r || a->nvn_compare_mode != b->nvn_compare_mode ||
        a->nvn_compare_func != b->nvn_compare_func || a->nvn_reduction != b->nvn_reduction ||
        a->lod_min != b->lod_min || a->lod_max != b->lod_max ||
        a->lod_bias != b->lod_bias || a->max_aniso != b->max_aniso) {
        return 0;
    }
    /* nxnative compares the border with memcmp, so -0 and +0 are different
     * guest states even though they compare equal as floats. */
    if (memcmp(a->border, b->border, sizeof a->border) != 0) return 0;
    return 1;
}

/* NVN sampler state, translated without naming Metal -- see nvn_sampler.c.
 *
 * Every numeric NVN value in here comes from nxnative's nvn.h
 * (dm/gfx/nvn/include/nvn.h): NVNminFilter 0..5, NVNmagFilter 0..1,
 * NVNwrapMode 0..7, NVNcompareMode 0..1, NVNcompareFunc 1..8,
 * NVNsamplerReduction 0..2.  That header is a reimplementation's header, not
 * Nintendo's SDK; the values are used because they are the only concrete
 * definition this port has, and every one of them is checked here on Windows.
 *
 * Unknown values are reported as -1 instead of being mapped to something
 * plausible: a sampler that silently repeats instead of clamping changes the
 * picture everywhere at once.
 */
#ifndef NVN_SAMPLER_H
#define NVN_SAMPLER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NVNGFX_TEXFILTER_NEAREST = 0,
    NVNGFX_TEXFILTER_LINEAR = 1
} nvngfx_texfilter;

typedef enum {
    NVNGFX_MIPFILTER_NONE = 0,
    NVNGFX_MIPFILTER_NEAREST = 1,
    NVNGFX_MIPFILTER_LINEAR = 2
} nvngfx_mipfilter;

typedef enum {
    NVNGFX_WRAP_CLAMP_TO_EDGE = 0,
    NVNGFX_WRAP_REPEAT = 1,
    NVNGFX_WRAP_MIRROR_CLAMP_TO_EDGE = 2,
    NVNGFX_WRAP_MIRRORED_REPEAT = 3,
    NVNGFX_WRAP_CLAMP_TO_BORDER = 4,
    NVNGFX_WRAP_UNSUPPORTED = 5
} nvngfx_wrap;

typedef enum {
    NVNGFX_REDUCTION_AVERAGE = 0,
    NVNGFX_REDUCTION_MIN = 1,
    NVNGFX_REDUCTION_MAX = 2
} nvngfx_reduction;

/* Bit flags: a value was mapped to something that is not exactly what the
 * guest asked for, so the backend can say so once. */
#define NVNGFX_SAMPLER_APPROX_WRAP   0x01u
#define NVNGFX_SAMPLER_APPROX_BORDER 0x02u
#define NVNGFX_SAMPLER_APPROX_LOD    0x04u
#define NVNGFX_SAMPLER_APPROX_ANISO  0x08u
#define NVNGFX_SAMPLER_APPROX_BIAS   0x10u

typedef struct {
    /* Neutral values the Metal descriptor uses. */
    unsigned char min_filter;   /* nvngfx_texfilter */
    unsigned char mip_filter;   /* nvngfx_mipfilter */
    unsigned char mag_filter;   /* nvngfx_texfilter */
    unsigned char wrap_s;
    unsigned char wrap_t;
    unsigned char wrap_r;       /* nvngfx_wrap */
    unsigned char compare;      /* 0 = none, else the Metal compare function */
    unsigned char reduction;    /* nvngfx_reduction */

    /* The exact NVN numbers.  GetMinMagFilter/GetWrapMode must return these,
     * not the lossy neutral ones: NVN CLAMP (0) and CLAMP_TO_EDGE (7) both
     * become Metal clamp-to-edge, but the guest can tell them apart. */
    unsigned char nvn_min_filter;
    unsigned char nvn_mag_filter;
    unsigned char nvn_wrap_s;
    unsigned char nvn_wrap_t;
    unsigned char nvn_wrap_r;
    unsigned char nvn_compare_mode;
    unsigned char nvn_compare_func;
    unsigned char nvn_reduction;

    unsigned char approximate;  /* NVNGFX_SAMPLER_APPROX_* */
    unsigned char reserved[3];
    float lod_bias;
    float lod_min;
    float lod_max;              /* raw guest values; Metal mapping clamps */
    float max_aniso;            /* raw guest value; Metal mapping clamps */
    float border[4];
} nvngfx_sampler;

void nvngfx_sampler_defaults(nvngfx_sampler* s);
int nvngfx_sampler_set_minmag(nvngfx_sampler* s, unsigned nvn_min, unsigned nvn_mag);
int nvngfx_sampler_set_wrap(nvngfx_sampler* s, unsigned s_wrap, unsigned t_wrap,
                            unsigned r_wrap);
int nvngfx_sampler_set_compare(nvngfx_sampler* s, unsigned nvn_mode,
                               unsigned nvn_func);
int nvngfx_sampler_set_reduction(nvngfx_sampler* s, unsigned nvn_reduction);
void nvngfx_sampler_set_lod_clamp(nvngfx_sampler* s, float lo, float hi);
void nvngfx_sampler_set_lod_bias(nvngfx_sampler* s, float v);
void nvngfx_sampler_set_anisotropy(nvngfx_sampler* s, float v);
void nvngfx_sampler_set_border(nvngfx_sampler* s, const float rgba[4]);
int nvngfx_sampler_equals(const nvngfx_sampler* a, const nvngfx_sampler* b);

#ifdef __cplusplus
}
#endif

#endif

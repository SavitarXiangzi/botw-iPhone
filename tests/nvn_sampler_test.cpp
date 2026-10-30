/* nvn_sampler: the NVN -> neutral-state mapping, checked value by value.
 *
 * Every number here is from nxnative's nvn.h; if a mapping changes, this
 * test is where it has to be justified.
 */
#include <cstdio>
#include "nvn_sampler.h"

static int g_fails = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %d: %s\n", __LINE__, #cond);                \
            ++g_fails;                                                    \
        }                                                                 \
    } while (0)

int main() {
    nvngfx_sampler s;
    nvngfx_sampler_defaults(&s);
    /* nxnative hle_nvn.c nvnSamplerBuilderSetDefaults: GL defaults. */
    CHECK(s.min_filter == NVNGFX_TEXFILTER_NEAREST);
    CHECK(s.mip_filter == NVNGFX_MIPFILTER_LINEAR);
    CHECK(s.mag_filter == NVNGFX_TEXFILTER_LINEAR);
    CHECK(s.wrap_s == NVNGFX_WRAP_REPEAT);
    CHECK(s.wrap_t == NVNGFX_WRAP_REPEAT);
    CHECK(s.wrap_r == NVNGFX_WRAP_REPEAT);
    CHECK(s.compare == 0);
    CHECK(s.reduction == NVNGFX_REDUCTION_AVERAGE);
    CHECK(s.approximate == 0);
    CHECK(s.lod_min == -1000.0f && s.lod_max == 1000.0f);
    CHECK(s.lod_bias == 0.0f);
    CHECK(s.max_aniso == 1.0f);
    CHECK(s.border[0] == 0.0f && s.border[1] == 0.0f);
    CHECK(s.border[2] == 0.0f && s.border[3] == 0.0f);
    CHECK(s.nvn_min_filter == 4);   /* NEAREST_MIPMAP_LINEAR */
    CHECK(s.nvn_mag_filter == 1);   /* LINEAR */
    CHECK(s.nvn_wrap_s == 1 && s.nvn_wrap_t == 1 && s.nvn_wrap_r == 1);
    CHECK(s.nvn_compare_mode == 0 && s.nvn_compare_func == 2);
    CHECK(s.nvn_reduction == 0);

    /* NVNminFilter 0..5 -> (min, mip). */
    {
        static const struct { unsigned nvn, minf, mipf; } kMin[6] = {
            {0, NVNGFX_TEXFILTER_NEAREST, NVNGFX_MIPFILTER_NONE},
            {1, NVNGFX_TEXFILTER_LINEAR,  NVNGFX_MIPFILTER_NONE},
            {2, NVNGFX_TEXFILTER_NEAREST, NVNGFX_MIPFILTER_NEAREST},
            {3, NVNGFX_TEXFILTER_LINEAR,  NVNGFX_MIPFILTER_NEAREST},
            {4, NVNGFX_TEXFILTER_NEAREST, NVNGFX_MIPFILTER_LINEAR},
            {5, NVNGFX_TEXFILTER_LINEAR,  NVNGFX_MIPFILTER_LINEAR},
        };
        for (unsigned i = 0; i < 6; ++i) {
            nvngfx_sampler_defaults(&s);
            CHECK(nvngfx_sampler_set_minmag(&s, kMin[i].nvn, 0) == 0);
            CHECK(s.min_filter == kMin[i].minf);
            CHECK(s.mip_filter == kMin[i].mipf);
            CHECK(s.mag_filter == NVNGFX_TEXFILTER_NEAREST);
            CHECK(s.nvn_min_filter == kMin[i].nvn);
            CHECK(s.nvn_mag_filter == 0);
        }
    }
    CHECK(nvngfx_sampler_set_minmag(&s, 0, 1) == 0);
    CHECK(s.mag_filter == NVNGFX_TEXFILTER_LINEAR);
    CHECK(nvngfx_sampler_set_minmag(&s, 6, 0) == -1);
    CHECK(nvngfx_sampler_set_minmag(&s, 0, 2) == -1);

    /* NVNwrapMode 0..7. */
    {
        static const unsigned kWrap[8] = {
            NVNGFX_WRAP_CLAMP_TO_EDGE,        /* 0 CLAMP */
            NVNGFX_WRAP_REPEAT,               /* 1 */
            NVNGFX_WRAP_MIRROR_CLAMP_TO_EDGE, /* 2 MIRROR_CLAMP */
            NVNGFX_WRAP_MIRROR_CLAMP_TO_EDGE, /* 3 */
            NVNGFX_WRAP_CLAMP_TO_BORDER,      /* 4 MIRROR_CLAMP_TO_BORDER */
            NVNGFX_WRAP_CLAMP_TO_BORDER,      /* 5 */
            NVNGFX_WRAP_MIRRORED_REPEAT,      /* 6 */
            NVNGFX_WRAP_CLAMP_TO_EDGE,        /* 7 CLAMP_TO_EDGE */
        };
        for (unsigned i = 0; i < 8; ++i) {
            nvngfx_sampler_defaults(&s);
            CHECK(nvngfx_sampler_set_wrap(&s, i, i, i) == 0);
            CHECK(s.wrap_s == kWrap[i]);
            CHECK(s.wrap_t == kWrap[i]);
            CHECK(s.wrap_r == kWrap[i]);
            CHECK(s.nvn_wrap_s == i && s.nvn_wrap_t == i && s.nvn_wrap_r == i);
            /* Only mirrored clamp-to-border is approximated. */
            CHECK((s.approximate & NVNGFX_SAMPLER_APPROX_WRAP) == (i == 4 ? NVNGFX_SAMPLER_APPROX_WRAP : 0));
        }
        CHECK(nvngfx_sampler_set_wrap(&s, 8, 0, 0) == -1);
    }

    /* NVNcompareMode / NVNcompareFunc: func 1..8 becomes 0..7. */
    {
        nvngfx_sampler_defaults(&s);
        CHECK(nvngfx_sampler_set_compare(&s, 0, 0) == 0);
        CHECK(s.compare == 0);
        for (unsigned f = 1; f <= 8; ++f) {
            CHECK(nvngfx_sampler_set_compare(&s, 1, f) == 0);
            CHECK(s.compare == f - 1);
            CHECK(s.nvn_compare_mode == 1 && s.nvn_compare_func == f);
        }
        CHECK(nvngfx_sampler_set_compare(&s, 1, 0) == -1);
        CHECK(nvngfx_sampler_set_compare(&s, 1, 9) == -1);
        CHECK(nvngfx_sampler_set_compare(&s, 2, 1) == -1);
    }

    /* NVNsamplerReduction 0..2 is Metal's 0..2. */
    CHECK(nvngfx_sampler_set_reduction(&s, 0) == 0 && s.reduction == NVNGFX_REDUCTION_AVERAGE);
    CHECK(s.nvn_reduction == 0);
    CHECK(nvngfx_sampler_set_reduction(&s, 1) == 0 && s.reduction == NVNGFX_REDUCTION_MIN);
    CHECK(s.nvn_reduction == 1);
    CHECK(nvngfx_sampler_set_reduction(&s, 2) == 0 && s.reduction == NVNGFX_REDUCTION_MAX);
    CHECK(s.nvn_reduction == 2);
    CHECK(nvngfx_sampler_set_reduction(&s, 3) == -1);

    /* LOD / anisotropy clamping is reported, not silent. */
    {
        nvngfx_sampler_defaults(&s);
        /* Getters must return what the guest set; clamping belongs to the
         * Metal descriptor, not to the state the guest can read back. */
        nvngfx_sampler_set_lod_clamp(&s, 2.0f, 1.0f);
        CHECK(s.lod_min == 2.0f && s.lod_max == 1.0f);
        CHECK(s.approximate & NVNGFX_SAMPLER_APPROX_LOD);
        nvngfx_sampler_set_lod_clamp(&s, -1000.0f, 1000.0f);
        CHECK(s.lod_min == -1000.0f && s.lod_max == 1000.0f);
        CHECK(s.approximate & NVNGFX_SAMPLER_APPROX_LOD);
        nvngfx_sampler_set_anisotropy(&s, 32.0f);
        CHECK(s.max_aniso == 32.0f);
        CHECK(s.approximate & NVNGFX_SAMPLER_APPROX_ANISO);
        nvngfx_sampler_set_lod_bias(&s, 0.5f);
        CHECK(s.approximate & NVNGFX_SAMPLER_APPROX_BIAS);
    }

    /* Border colour: the three Metal constants pass, anything else is flagged. */
    {
        const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const float odd[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        nvngfx_sampler_defaults(&s);
        nvngfx_sampler_set_border(&s, transparent);
        CHECK((s.approximate & NVNGFX_SAMPLER_APPROX_BORDER) == 0);
        nvngfx_sampler_set_border(&s, white);
        CHECK((s.approximate & NVNGFX_SAMPLER_APPROX_BORDER) == 0);
        nvngfx_sampler_set_border(&s, odd);
        CHECK(s.approximate & NVNGFX_SAMPLER_APPROX_BORDER);
        CHECK(s.border[0] == 0.25f && s.border[3] == 1.0f);
    }

    /* A rejected setter must not leave half a sampler behind. */
    {
        nvngfx_sampler before;
        nvngfx_sampler_defaults(&s);
        CHECK(nvngfx_sampler_set_minmag(&s, 1, 1) == 0);
        before = s;
        CHECK(nvngfx_sampler_set_minmag(&s, 5, 2) == -1);
        CHECK(nvngfx_sampler_equals(&s, &before));
        CHECK(nvngfx_sampler_set_compare(&s, 2, 2) == -1);
        CHECK(nvngfx_sampler_equals(&s, &before));
    }

    /* Compare mode NONE still preserves the guest's function value. */
    {
        nvngfx_sampler_defaults(&s);
        CHECK(nvngfx_sampler_set_compare(&s, 0, 5) == 0);
        CHECK(s.compare == 0);
        CHECK(s.nvn_compare_mode == 0 && s.nvn_compare_func == 5);
    }

    /* Equality is on raw NVN state.  CLAMP and CLAMP_TO_EDGE share a Metal
     * mode but are different guest samplers. */
    {
        nvngfx_sampler a, b;
        nvngfx_sampler_defaults(&a);
        nvngfx_sampler_defaults(&b);
        CHECK(nvngfx_sampler_equals(&a, &b));
        CHECK(nvngfx_sampler_set_wrap(&a, 0, 0, 0) == 0);
        CHECK(nvngfx_sampler_set_wrap(&b, 7, 7, 7) == 0);
        CHECK(a.wrap_s == b.wrap_s);
        CHECK(!nvngfx_sampler_equals(&a, &b));
        nvngfx_sampler_defaults(&b);
        nvngfx_sampler_set_lod_bias(&b, 0.25f);
        CHECK(!nvngfx_sampler_equals(&a, &b));

        /* Border comparison is byte-exact, like nxnative's memcmp. */
        const float positive_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float negative_zero[4] = {-0.0f, 0.0f, 0.0f, 0.0f};
        nvngfx_sampler_set_border(&a, positive_zero);
        nvngfx_sampler_set_border(&b, negative_zero);
        CHECK(!nvngfx_sampler_equals(&a, &b));
    }

    if (g_fails) {
        std::printf("nvn_sampler_test: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("nvn_sampler_test: ok (filters, wrap, compare, reduction, clamps)\n");
    return 0;
}

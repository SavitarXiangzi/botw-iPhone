/*
 * Portable test for the descriptor pool table and handle packing.
 *
 * The expected values are nxnative's (hle_nvn.c): pool bases in creation
 * order rounded to 256 entries, nvnDeviceGetTextureHandle = tex | samp << 20,
 * nvnDeviceGetSeparateSamplerHandle = id << 20.  None of that needs a device.
 */
#include "nvn_pool.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

int main() {
    nvngfx_pool_map m;
    nvngfx_pool_map_init(&m);
    CHECK(m.count == 0 && m.next_base == 0);

    int base = -1;
    CHECK(nvngfx_pool_map_add(&m, 0x1000, 4, &base) == 1 && base == 0);
    /* Rounded up to 256, so the UI pool's ID 0 cannot collide. */
    CHECK(nvngfx_pool_map_add(&m, 0x2000, 300, &base) == 1 && base == 256);
    CHECK(nvngfx_pool_map_add(&m, 0x3000, 0, &base) == 1 && base == 768);

    /* Initializing the same pool again keeps its numbering. */
    CHECK(nvngfx_pool_map_add(&m, 0x1000, 999, &base) == 1 && base == 0);
    CHECK(m.count == 3);

    CHECK(nvngfx_pool_map_base(&m, 0x2000) == 256);
    CHECK(nvngfx_pool_map_base(&m, 0x9999) == -1);

    CHECK(nvngfx_pool_slot(&m, 0x1000, 0) == 0);
    CHECK(nvngfx_pool_slot(&m, 0x1000, 3) == 3);
    CHECK(nvngfx_pool_slot(&m, 0x1000, 4) == -1);   /* outside the pool */
    CHECK(nvngfx_pool_slot(&m, 0x2000, 7) == 263);
    CHECK(nvngfx_pool_slot(&m, 0x9999, 0) == -1);

    CHECK(nvngfx_texture_handle(5, 9) == (5ull | (9ull << 20)));
    CHECK(nvngfx_texture_handle_tex(nvngfx_texture_handle(5, 9)) == 5);
    CHECK(nvngfx_texture_handle_samp(nvngfx_texture_handle(5, 9)) == 9);
    /* The mask is 20 bits wide; a larger index is not allowed to spill into
     * the sampler field. */
    CHECK(nvngfx_texture_handle(0x1FFFFF, 0) == 0xFFFFFull);

    CHECK(nvngfx_separate_sampler_handle(3) == (3ull << 20));
    CHECK(nvngfx_separate_sampler_index(nvngfx_separate_sampler_handle(3)) == 3);
    CHECK(nvngfx_texture_handle_tex(nvngfx_separate_sampler_handle(3)) == 0);

    /* A 17th pool is refused instead of being aliased onto pool 0. */
    for (int i = m.count; i < NVNGFX_POOL_MAX; ++i)
        CHECK(nvngfx_pool_map_add(&m, 0x4000 + (unsigned)i, 1, &base) == 1);
    CHECK(m.count == NVNGFX_POOL_MAX);
    base = 123;
    CHECK(nvngfx_pool_map_add(&m, 0x5000, 1, &base) == 0 && base == 0);
    CHECK(nvngfx_pool_map_base(&m, 0x5000) == -1);

    if (g_failures == 0) std::printf("nvn_pool_test: ok (bases, slots, handles)\n");
    return g_failures ? 1 : 0;
}
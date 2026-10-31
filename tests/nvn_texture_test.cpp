/*
 * Portable test for the NVN format table and the texture builder state.
 *
 * Neither side needs Apple frameworks, so this runs on Windows/Linux and is
 * the only place the format mapping gets executed at all without a device.
 * The expectations are hand-checked against Apple's public MTLPixelFormat
 * values, not copied from the mapping function.
 */
#include "nvn_texture.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

static void expect(int nvn, unsigned mtl, int lossy, const char* what) {
    int l = -1;
    const unsigned got = nvngfx_format_to_mtl(nvn, &l);
    if (got != mtl || (mtl != 0 && l != lossy)) {
        std::printf("FAIL %s: format 0x%02X -> %u (lossy %d), expected %u (lossy %d)\n",
                    what, nvn, got, l, mtl, lossy);
        ++g_failures;
    }
}

int main() {
    /* RGBA8_UNORM / RGBA32_FLOAT / BGRA8_UNORM: the three an iOS render target
     * will use first. */
    expect(0x25, 70, 0, "RGBA8_UNORM");
    expect(0x2E, 125, 0, "RGBA32_FLOAT");
    expect(0x99, 80, 0, "BGRA8_UNORM (no alpha)");
    expect(0x9A, 80, 0, "BGRA8_UNORM");
    expect(0x9C, 81, 0, "BGRA8_UNORM_sRGB");

    /* Substitutes must say so. */
    expect(0x33, 252, 1, "DEPTH24 -> Depth32Float");
    expect(0x3A, 43, 1, "packed 16-bit, bit order unverified");

    /* Block compressed. */
    expect(0x41, 130, 0, "BC1");
    expect(0x42, 130, 0, "BC1 (alpha)");
    expect(0x49, 140, 0, "BC4 unsigned");
    expect(0x4D, 152, 0, "BC7");

    /* ASTC: 14 steps per colour space, Metal skips a value between 6x6 and 8x5. */
    expect(0x79, 204, 0, "ASTC 4x4 LDR");
    expect(0x7E, 210, 0, "ASTC 8x5 LDR");
    expect(0x87, 186, 0, "ASTC 4x4 sRGB");

    /* No Metal counterpart. */
    expect(0x00, 0, 0, "format 0");
    expect(0x19, 0, 0, "RGB8 has no 3-component Metal format");
    expect(0x9D, 0, 0, "out of range");

    CHECK(nvngfx_format_table_size() == 0x9D);

    /* Colour component classes: the clear pipeline emits float4 / uint4 /
     * int4 to match.  RGBA8_UNORM is float; RGBA8_UINT/SINT are the integer
     * classes; a depth/stencil or absent format is not a colour target. */
    CHECK(nvngfx_format_component_class(0x25) == 0);   /* RGBA8_UNORM */
    CHECK(nvngfx_format_component_class(0x2E) == 0);   /* RGBA32_FLOAT */
    CHECK(nvngfx_format_component_class(0x27) == 1);   /* RGBA8_UINT */
    CHECK(nvngfx_format_component_class(0x28) == 2);   /* RGBA8_SINT */
    CHECK(nvngfx_format_component_class(0x0B) == 1);   /* R32_UINT */
    CHECK(nvngfx_format_component_class(0x0C) == 2);   /* R32_SINT */
    CHECK(nvngfx_format_component_class(0x35) == -1);  /* DEPTH24_STENCIL8 */
    CHECK(nvngfx_format_component_class(0x00) == -1);  /* no format */

    /* Builder state: the setters are host code, so this is just arithmetic,
     * but it is the state nvnTextureInitialize will build from. */
    nvngfx_texdesc d;
    nvngfx_texdesc_defaults(&d);
    CHECK(d.rank == 2);
    CHECK(d.width == 1 && d.height == 1 && d.depth == 1);
    CHECK(d.levels == 1);
    CHECK(d.swz[0] == 2 && d.swz[1] == 3 && d.swz[2] == 4 && d.swz[3] == 5);

    d.rank = 2;
    d.width = 1280;
    d.height = 720;
    d.format = 0x25;
    int lossy = 1;
    CHECK(nvngfx_format_to_mtl(d.format, &lossy) == 70u);
    CHECK(lossy == 0);

    if (g_failures) {
        std::printf("nvn_texture_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvn_texture_test: ok (%d formats in table)\n", nvngfx_format_table_size());
    return 0;
}
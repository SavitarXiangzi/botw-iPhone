/* nvn_bcn: BC1/BC2/BC3/BC4/BC5 block decoding.
 *
 * Run by scripts/build_abi_test.ps1 and by CI's portable job.  The expected
 * values are the documented S3TC/RGTC arithmetic computed by hand, not a
 * snapshot of this implementation.  No game data, no Metal.
 */
#include "nvn_bcn.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static void put16(unsigned char* p, unsigned v) {
    p[0] = static_cast<unsigned char>(v & 0xFFu);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
}

static void put32(unsigned char* p, unsigned v) {
    p[0] = static_cast<unsigned char>(v & 0xFFu);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
    p[2] = static_cast<unsigned char>((v >> 16) & 0xFFu);
    p[3] = static_cast<unsigned char>((v >> 24) & 0xFFu);
}

/* Every texel of a BC1 colour block carrying the same 2-bit index. */
static unsigned spread2(unsigned index) {
    unsigned v = 0;
    for (int i = 0; i < 16; ++i) v |= index << (2 * i);
    return v;
}

/* Every texel of a BC4 alpha block carrying the same 3-bit index: 48 bits,
 * written as a 32-bit word plus a 16-bit tail. */
static void put_indices3(unsigned char* p, unsigned index) {
    unsigned long long bits = 0;
    for (int i = 0; i < 16; ++i)
        bits |= static_cast<unsigned long long>(index) << (3 * i);
    put32(p, static_cast<unsigned>(bits & 0xFFFFFFFFull));
    put16(p + 4, static_cast<unsigned>((bits >> 32) & 0xFFFFull));
}

static bool texel_is(const unsigned char* rgba, int i, int r, int g, int b,
                     int a) {
    const unsigned char* p = rgba + i * 4;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static void test_supported() {
    CHECK(!nvngfx_bcn_supported(0x25));       /* RGBA8 is not BCn */
    CHECK(nvngfx_bcn_supported(0x41));        /* RGB_DXT1 */
    CHECK(nvngfx_bcn_supported(0x44));        /* RGBA_DXT5 */
    CHECK(nvngfx_bcn_supported(0x49));        /* BC4U */
    CHECK(nvngfx_bcn_supported(0x4B));        /* BC5U */
    CHECK(!nvngfx_bcn_supported(0x4A));       /* BC4S: no signed decoder */
    CHECK(!nvngfx_bcn_supported(0x4D));       /* BC7: no BPTC decoder */
    CHECK(!nvngfx_bcn_supported(0x4F));       /* BC6H */

    CHECK(nvngfx_bcn_block_bytes(0x41) == 8);
    CHECK(nvngfx_bcn_block_bytes(0x44) == 16);
    CHECK(nvngfx_bcn_block_bytes(0x49) == 8);
    CHECK(nvngfx_bcn_block_bytes(0x4B) == 16);
    CHECK(nvngfx_bcn_block_bytes(0x25) == 0);

    CHECK(nvngfx_bcn_is_srgb(0x45));
    CHECK(nvngfx_bcn_is_srgb(0x48));
    CHECK(!nvngfx_bcn_is_srgb(0x44));
}

static void test_bc1_four_colour() {
    unsigned char block[8] = {};
    unsigned char rgba[64];
    /* c0 = red (0xF800), c1 = blue (0x001F), c0 > c1 -> four-colour mode. */
    put16(block, 0xF800);
    put16(block + 2, 0x001F);

    put32(block + 4, spread2(0));
    CHECK(nvngfx_bcn_decode(0x42, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 255, 0, 0, 255));
    CHECK(texel_is(rgba, 15, 255, 0, 0, 255));

    put32(block + 4, spread2(1));
    CHECK(nvngfx_bcn_decode(0x42, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 0, 0, 255, 255));

    /* c2 = (2*c0 + c1) / 3, c3 = (c0 + 2*c1) / 3. */
    put32(block + 4, spread2(2));
    CHECK(nvngfx_bcn_decode(0x42, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 170, 0, 85, 255));

    put32(block + 4, spread2(3));
    CHECK(nvngfx_bcn_decode(0x42, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 85, 0, 170, 255));
}

static void test_bc1_three_colour() {
    unsigned char block[8] = {};
    unsigned char rgba[64];
    /* c0 = black, c1 = white, c0 <= c1 -> three colours plus transparent. */
    put16(block, 0x0000);
    put16(block + 2, 0xFFFF);

    put32(block + 4, spread2(2));
    CHECK(nvngfx_bcn_decode(0x42, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 127, 127, 127, 255));

    put32(block + 4, spread2(3));
    CHECK(nvngfx_bcn_decode(0x42, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 0, 0, 0, 0));

    /* RGB_DXT1 (0x41) has no alpha channel: the same block must be opaque. */
    CHECK(nvngfx_bcn_decode(0x41, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 0, 0, 0, 255));
}

static void test_bc2_alpha() {
    unsigned char block[16] = {};
    unsigned char rgba[64];
    put16(block, 0x0000);        /* black */
    put16(block + 2, 0xFFFF);    /* white */
    put32(block + 4, 0);         /* every texel index 0 */
    /* DXT3 alpha: one nibble per texel, 16 nibbles in 8 bytes.  Texel 0 is
     * 0xF, texel 4 is 0x8, everything else 0x0. */
    put16(block + 8, 0x000F);
    put16(block + 10, 0x0008);
    CHECK(nvngfx_bcn_decode(0x43, block, rgba) == 0);
    CHECK(rgba[3] == 255);
    CHECK(rgba[4 * 4 + 3] == 136);
    CHECK(rgba[2 * 4 + 3] == 0);
    CHECK(texel_is(rgba, 0, 0, 0, 0, 255));
}

static void test_bc3_alpha() {
    unsigned char block[16] = {};
    unsigned char rgba[64];
    put16(block, 0xF800);        /* red */
    put16(block + 2, 0xF800);    /* red again: four-colour mode either way */
    put32(block + 4, 0);         /* every texel index 0 */
    block[8] = 255;              /* a0 */
    block[9] = 0;                /* a1: a0 > a1 -> eight-value mode */
    /* Eight-value mode: index 5 -> (3*255 + 4*0) / 7 = 109, index 7 ->
     * (1*255 + 6*0) / 7 = 36.  Texel 15 is the one whose three bits sit
     * above the first 32-bit word. */
    put_indices3(block + 10, 5);
    CHECK(nvngfx_bcn_decode(0x44, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 255, 0, 0, 109));
    CHECK(texel_is(rgba, 15, 255, 0, 0, 109));

    put_indices3(block + 10, 7);
    CHECK(nvngfx_bcn_decode(0x44, block, rgba) == 0);
    CHECK(texel_is(rgba, 15, 255, 0, 0, 36));
}

static void test_bc4_bc5() {
    unsigned char block[16] = {};
    unsigned char rgba[64];
    /* a0 = 0, a1 = 255 -> six-value mode: index 2 = 255/5 = 51,
     * index 6 = 0, index 7 = 255. */
    block[0] = 0;
    block[1] = 255;
    put_indices3(block + 2, 2);
    CHECK(nvngfx_bcn_decode(0x49, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 51, 0, 0, 255));
    CHECK(texel_is(rgba, 15, 51, 0, 0, 255));

    put_indices3(block + 2, 6);
    CHECK(nvngfx_bcn_decode(0x49, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 0, 0, 0, 255));

    put_indices3(block + 2, 7);
    CHECK(nvngfx_bcn_decode(0x49, block, rgba) == 0);
    CHECK(texel_is(rgba, 0, 255, 0, 0, 255));

    /* BC5: red from the first block, green from the second. */
    {
        unsigned char bc5[16] = {};
        unsigned char out2[64];
        std::memcpy(bc5, block, 8);
        bc5[8] = 255;
        bc5[9] = 0;
        put_indices3(bc5 + 10, 0);   /* all texels read a0 = 255 */
        put_indices3(bc5 + 2, 2);    /* red stays 51 */
        CHECK(nvngfx_bcn_decode(0x4B, bc5, out2) == 0);
        CHECK(texel_is(out2, 0, 51, 255, 0, 255));
        CHECK(texel_is(out2, 15, 51, 255, 0, 255));
    }
}

static void test_image_clipping() {
    /* Two BC1 blocks in one block row: red then blue.  The image is 6x3, so
     * each edge block supplies pixels outside the image; the decoder must
     * clip them.  Both the compressed and RGBA rows carry padding, and the
     * padding must remain untouched. */
    unsigned char src[32] = {};
    unsigned char dst[32 * 3];
    put16(src, 0xF800);              /* red */
    put16(src + 2, 0x001F);          /* blue */
    put32(src + 4, spread2(0));      /* every texel selects red */
    put16(src + 8, 0xF800);
    put16(src + 10, 0x001F);
    put32(src + 12, spread2(1));     /* every texel selects blue */
    std::memset(dst, 0xCD, sizeof dst);

    CHECK(nvngfx_bcn_decode_image(0x42, src, 32, 6, 3, dst, 32) == 0);
    for (unsigned y = 0; y < 3; ++y) {
        for (unsigned x = 0; x < 6; ++x) {
            const unsigned char* p = dst + y * 32 + x * 4;
            if (x < 4)
                CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255);
            else
                CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255);
        }
        for (unsigned x = 6 * 4; x < 32; ++x) CHECK(dst[y * 32 + x] == 0xCD);
    }

    CHECK(nvngfx_bcn_decode_image(0x42, src, 15, 6, 3, dst, 32) == -1);
    CHECK(nvngfx_bcn_decode_image(0x42, src, 32, 6, 3, dst, 23) == -1);
    CHECK(nvngfx_bcn_decode_image(0x42, src, 32, 0, 3, dst, 32) == -1);
    CHECK(nvngfx_bcn_decode_image(0x42, nullptr, 32, 6, 3, dst, 32) == -1);
    CHECK(nvngfx_bcn_decode_image(0x42, src, 32, 6, 3, nullptr, 32) == -1);
    CHECK(nvngfx_bcn_decode_image(0x4D, src, 32, 6, 3, dst, 32) == -1);
}

static void test_refusals() {
    unsigned char block[16] = {};
    unsigned char rgba[64];
    CHECK(nvngfx_bcn_decode(0x4A, block, rgba) == -1);   /* BC4S */
    CHECK(nvngfx_bcn_decode(0x4D, block, rgba) == -1);   /* BC7 */
    CHECK(nvngfx_bcn_decode(0x25, block, rgba) == -1);   /* RGBA8 */
    CHECK(nvngfx_bcn_decode(0x41, nullptr, rgba) == -1);
    CHECK(nvngfx_bcn_decode(0x41, block, nullptr) == -1);
}

int main() {
    test_supported();
    test_bc1_four_colour();
    test_bc1_three_colour();
    test_bc2_alpha();
    test_bc3_alpha();
    test_bc4_bc5();
    test_image_clipping();
    test_refusals();
    if (g_failures) {
        std::printf("nvn_bcn_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvn_bcn_test: ok (BC1 4/3-colour, BC2 alpha, BC3 alpha, "
                "BC4/BC5, image clipping, refusals)\n");
    return 0;
}

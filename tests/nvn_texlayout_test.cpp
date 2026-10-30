/* nvn_texlayout: NVN's block-linear storage arithmetic and the copy in and
 * out of it.
 *
 * Run by scripts/build_abi_test.ps1 and by CI's portable job.  The sizes are
 * checked against hand calculation from the rules (one GOB = 64 bytes x 8
 * rows, blocks 2^tile_h GOBs high), and the copy is checked by a round trip:
 * a synthetic image is written through nvngfx_tex_bl_offset into a storage
 * buffer, then read back with nvngfx_tex_deswizzle.  No game data, no Metal.
 */
#include "nvn_texlayout.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static nvngfx_texdesc make_desc(int format, int width, int height, int levels) {
    nvngfx_texdesc d;
    nvngfx_texdesc_defaults(&d);
    d.format = format;
    d.width = width;
    d.height = height;
    d.levels = levels;
    return d;
}

/* NVNformat values used here: 0x25 RGBA8_UNORM, 0x31 R8, 0x42 BC1, 0x7E
 * ASTC 8x5.  They are the same integers tests/nvn_texture_test.cpp maps. */
static void test_block_shapes() {
    unsigned bpb = 0, bw = 0, bh = 0;
    CHECK(nvngfx_tex_format_block(0x25, &bpb, &bw, &bh));
    CHECK(bpb == 4 && bw == 1 && bh == 1);
    CHECK(nvngfx_tex_format_block(0x31, &bpb, &bw, &bh));
    CHECK(bpb == 1 && bw == 1 && bh == 1);
    CHECK(nvngfx_tex_format_block(0x42, &bpb, &bw, &bh));
    CHECK(bpb == 8 && bw == 4 && bh == 4);
    CHECK(nvngfx_tex_format_block(0x79, &bpb, &bw, &bh));
    CHECK(bpb == 16 && bw == 4 && bh == 4);
    CHECK(nvngfx_tex_format_block(0x7E, &bpb, &bw, &bh));
    CHECK(bpb == 16 && bw == 8 && bh == 5);
    /* 0x00 is the "none" hole: no layout can be computed for it. */
    CHECK(!nvngfx_tex_format_block(0x00, &bpb, &bw, &bh));
}

static void test_sizes() {
    nvngfx_texlayout L;
    {
        /* 64x64 RGBA8: 256-byte rows -> 4 GOB columns, 8 GOB rows, and
         * 8 x 3 / 16 = 1 picks tile_h 3, i.e. one block of 8 GOB rows. */
        const nvngfx_texdesc d = make_desc(0x25, 64, 64, 1);
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        CHECK(L.bytes_per_block == 4);
        CHECK(L.tile_h == 3);
        CHECK(L.tile_d == 0);
        CHECK(L.layers == 1);
        CHECK(L.layer_size == 16384);
        CHECK(L.size == 16384);
        CHECK(L.alignment == 512);
    }
    {
        /* Same texture with two levels: level 1 (32x32) starts at 16384 and
         * is 2 GOB columns x 4 GOB rows = 4096 bytes. */
        const nvngfx_texdesc d = make_desc(0x25, 64, 64, 2);
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        CHECK(L.size == 20480);
        CHECK(nvngfx_tex_level_offset(&d, 0) == 0);
        CHECK(nvngfx_tex_level_offset(&d, 1) == 16384);
    }
    {
        /* BC1 64x64: 16x16 blocks of 8 bytes = 128-byte rows, 16 block
         * rows; 16 x 3 / 16 = 3 picks tile_h 1, so 2 GOB columns x 2 GOB
         * rows of 2 -> 2048 bytes, exactly 64x64 at half a byte per texel. */
        const nvngfx_texdesc d = make_desc(0x42, 64, 64, 1);
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        CHECK(L.bytes_per_block == 8);
        CHECK(L.tile_h == 1);
        CHECK(L.size == 2048);
    }
    {
        /* ASTC 8x5: 8 x 13 blocks of 16 bytes = 128-byte rows and 13 block
         * rows, which pad up to 2 GOB columns x 2 GOB rows. */
        const nvngfx_texdesc d = make_desc(0x7E, 64, 64, 1);
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        CHECK(L.size == 2048);
    }
    {
        /* LINEAR flag: the driver's stride wins, aligned to 32 bytes. */
        const nvngfx_texdesc d = make_desc(0x25, 1, 1, 1);
        nvngfx_texdesc lin = d;
        lin.flags = 0x10;
        CHECK(nvngfx_tex_layout(&lin, &L) == 0);
        CHECK(L.size == 32);
        CHECK(L.alignment == 32);
    }
    {
        /* A 2D array: every layer starts on a block boundary. */
        nvngfx_texdesc d = make_desc(0x25, 64, 64, 1);
        d.target = 4;
        d.depth = 4;
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        CHECK(L.layers == 4);
        CHECK(L.layer_size == 16384);
        CHECK(L.size == 4 * 16384);
    }
    {
        /* 1D array: NVN stores the layer count in height, and the storage is
         * one 1-row (64 texel) slice per layer, block aligned. */
        nvngfx_texdesc d = make_desc(0x25, 64, 1, 1);
        d.target = 3;
        d.height = 4;
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        CHECK(L.layers == 4);
        CHECK(L.layer_size == 2048);
        CHECK(L.size == 4 * 2048);
    }
    {
        /* Cubemap and cubemap array are six faces per cube.  The old shape()
         * treated a cube array as depth layers, which made every upload stop
         * after the first cube. */
        nvngfx_texdesc cube = make_desc(0x25, 64, 64, 1);
        cube.target = 8;
        CHECK(nvngfx_tex_layout(&cube, &L) == 0);
        CHECK(L.layers == 6);
        CHECK(L.layer_size == 16384);
        CHECK(L.size == 6 * 16384);

        nvngfx_texdesc array = cube;
        array.target = 9;
        array.depth = 2;
        CHECK(nvngfx_tex_layout(&array, &L) == 0);
        CHECK(L.layers == 12);
        CHECK(L.layer_size == 16384);
        CHECK(L.size == 12 * 16384);
    }
    {
        /* What the cube self-tests have to reserve in a guest pool.  A layer
         * starts block aligned, so a 2x2 RGBA8 face still costs a full 512
         * bytes; the pool budget check in nvn_metal.mm uses this arithmetic
         * and the simulator caught it when it was off (3072 > 1024). */
        nvngfx_texdesc tiny = make_desc(0x25, 2, 2, 1);
        tiny.target = 8;
        CHECK(nvngfx_tex_layout(&tiny, &L) == 0);
        CHECK(L.layers == 6);
        CHECK(L.layer_size == 512);
        CHECK(L.size == 3072);

        tiny.target = 9;
        tiny.depth = 2;
        CHECK(nvngfx_tex_layout(&tiny, &L) == 0);
        CHECK(L.layers == 12);
        CHECK(L.layer_size == 512);
        CHECK(L.size == 6144);
    }
}

static void test_bl_offset() {
    /* 64-byte GOB: 32-byte halves, then 16-byte quarters, then row pairs. */
    CHECK(nvngfx_tex_bl_offset(0, 0, 0, 256, 64, 3, 0) == 0);
    CHECK(nvngfx_tex_bl_offset(16, 0, 0, 256, 64, 3, 0) == 32);
    CHECK(nvngfx_tex_bl_offset(32, 0, 0, 256, 64, 3, 0) == 256);
    CHECK(nvngfx_tex_bl_offset(48, 0, 0, 256, 64, 3, 0) == 288);
    CHECK(nvngfx_tex_bl_offset(0, 1, 0, 256, 64, 3, 0) == 16);
    CHECK(nvngfx_tex_bl_offset(0, 2, 0, 256, 64, 3, 0) == 64);
    CHECK(nvngfx_tex_bl_offset(0, 8, 0, 256, 64, 3, 0) == 512);
    /* The next block column starts one block (8 GOBs) further on. */
    CHECK(nvngfx_tex_bl_offset(64, 0, 0, 256, 64, 3, 0) == 4096);
    CHECK(nvngfx_tex_bl_offset(0, 64, 0, 256, 64, 3, 0) == 4 * 4096);
}

/* Writes `img` (rows x row_bytes) into `storage` through bl_offset, then reads
 * it back with nvngfx_tex_deswizzle and compares. */
static void round_trip(const char* what, const nvngfx_texdesc& d, unsigned level,
                       unsigned row_bytes, unsigned rows, unsigned layer = 0) {
    nvngfx_texlayout L;
    CHECK(nvngfx_tex_layout(&d, &L) == 0);
    std::vector<unsigned char> storage(static_cast<size_t>(L.size), 0xCC);
    const size_t img = static_cast<size_t>(row_bytes) * rows;
    std::vector<unsigned char> src(img), dst(img, 0);
    for (size_t i = 0; i < img; ++i)
        src[i] = static_cast<unsigned char>((i * 37u + 11u) & 0xFFu);

    unsigned th = 0, td = 0;
    unsigned long long base = 0;
    CHECK(nvngfx_tex_level_geometry(&d, level, layer, nullptr, nullptr, nullptr,
                                    &base, &th, &td) == 0);
    const unsigned gobs = nvngfx_tex_level_gobs(&d, level);
    const unsigned laid = gobs * 64u;
    CHECK(base + static_cast<unsigned long long>(laid) * rows <= L.size);
    for (unsigned y = 0; y < rows; ++y) {
        for (unsigned x = 0; x < row_bytes; x += 16) {
            const unsigned n = row_bytes - x < 16 ? row_bytes - x : 16;
            std::memcpy(storage.data() + base +
                            nvngfx_tex_bl_offset(x, y, 0, laid, rows, th, td),
                        src.data() + static_cast<size_t>(y) * row_bytes + x, n);
        }
    }
    CHECK(nvngfx_tex_deswizzle(&d, level, layer, storage.data(), dst.data(),
                               row_bytes) == 0);
    if (std::memcmp(src.data(), dst.data(), img) != 0)
        std::printf("FAIL %s: deswizzle differs\n", what);

    /* And once more in the other direction: swizzle a fresh storage from the
     * same linear bytes and compare it with the hand-built one. */
    std::vector<unsigned char> again(storage.size(), 0xCC);
    CHECK(nvngfx_tex_swizzle(&d, level, layer, src.data(), row_bytes,
                             again.data()) == 0);
    if (std::memcmp(storage.data(), again.data(), storage.size()) != 0)
        std::printf("FAIL %s: swizzle differs\n", what);
}

static void test_round_trip() {
    /* 64x64 RGBA8, then a level whose row is not a multiple of 16 bytes so
     * the copy's tail path runs. */
    round_trip("rgba8 64x64", make_desc(0x25, 64, 64, 1), 0, 256, 64);
    round_trip("r8 40x40 (tail)", make_desc(0x31, 40, 40, 1), 0, 40, 40);
    /* A block-compressed level: one "pixel" is a 4x4 block, so the same
     * routine copies block rows. */
    round_trip("bc1 64x64", make_desc(0x42, 64, 64, 1), 0, 128, 16);
    round_trip("rgba8 mip 1", make_desc(0x25, 64, 64, 2), 1, 128, 32);

    /* A non-zero layer must read from its own block-aligned storage, not
     * from layer 0.  This is the arithmetic the Metal upload loop relies on
     * for arrays and cube faces. */
    nvngfx_texdesc array = make_desc(0x25, 64, 64, 1);
    array.target = 4;
    array.depth = 2;
    round_trip("rgba8 2D array layer 1", array, 0, 256, 64, 1);

    /* Cube faces and cube-array faces use the same per-layer arithmetic; the
     * last face is the one an off-by-one layer count would omit. */
    nvngfx_texdesc cube = make_desc(0x25, 64, 64, 1);
    cube.target = 8;
    round_trip("rgba8 cube face 5", cube, 0, 256, 64, 5);
    nvngfx_texdesc cubeArray = make_desc(0x25, 64, 64, 1);
    cubeArray.target = 9;
    cubeArray.depth = 2;
    round_trip("rgba8 cube array face 11", cubeArray, 0, 256, 64, 11);

    /* A 1D array's height is its layer count, but each layer has one row. */
    nvngfx_texdesc oneD = make_desc(0x25, 64, 1, 1);
    oneD.target = 3;
    oneD.height = 4;
    round_trip("rgba8 1D array layer 3", oneD, 0, 256, 1, 3);
}

/* Linear storage: rows at a byte pitch, a mip chain after them, and every
 * layer repeating the chain.  The old code computed stride * height, which
 * under-sized a mip chain and every array. */
static void test_linear() {
    nvngfx_texlayout L;
    {
        nvngfx_texdesc d = make_desc(0x25, 4, 4, 1);
        d.flags = 0x10;                  /* LINEAR */
        d.stride = 32;                   /* 16 real row bytes + 16 padding */
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        CHECK(L.alignment == 32);
        CHECK(L.layer_size == 128);
        CHECK(L.size == 128);
        CHECK(nvngfx_tex_level_geometry(&d, 0, 0, nullptr, nullptr, nullptr,
                                        nullptr, nullptr, nullptr) == 0);

        d.levels = 3;
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        CHECK(L.layer_size == 224);      /* 128 + 64 + 32, each 32-aligned */
        CHECK(L.size == 224);
        CHECK(nvngfx_tex_level_offset(&d, 0) == 0);
        CHECK(nvngfx_tex_level_offset(&d, 1) == 128);
        CHECK(nvngfx_tex_level_offset(&d, 2) == 192);

        d.target = 4;                    /* 2D array */
        d.depth = 2;
        d.levels = 1;
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        CHECK(L.layers == 2);
        CHECK(L.layer_size == 128);
        CHECK(L.size == 256);            /* the old code said 128 */

        d.target = 0;
        d.depth = 1;
        d.stride = 16;                   /* exactly one row */
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        CHECK(L.layer_size == 64);
        d.stride = 8;                    /* narrower than a row */
        CHECK(nvngfx_tex_layout(&d, &L) != 0);
    }
    {
        /* Copy in and out: the padding must be skipped, not sampled. */
        nvngfx_texdesc d = make_desc(0x25, 4, 4, 1);
        d.flags = 0x10;
        d.stride = 32;
        std::vector<unsigned char> storage(128, 0xEE);
        std::vector<unsigned char> packed(64, 0);
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x) {
                unsigned char* const p = storage.data() + y * 32 + x * 4;
                p[0] = static_cast<unsigned char>(y * 16 + x * 4);
                p[1] = 7;
                p[2] = 9;
                p[3] = 255;
            }
        CHECK(nvngfx_tex_deswizzle(&d, 0, 0, storage.data(), packed.data(),
                                   16) == 0);
        {
            bool ok = true;
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 4; ++x) {
                    const unsigned char* const p =
                        packed.data() + y * 16 + x * 4;
                    ok = ok && p[0] == static_cast<unsigned char>(y * 16 + x * 4) &&
                         p[1] == 7 && p[2] == 9 && p[3] == 255;
                }
            CHECK(ok);
        }
        {
            std::vector<unsigned char> back(128, 0);
            CHECK(nvngfx_tex_swizzle(&d, 0, 0, packed.data(), 16,
                                     back.data()) == 0);
            bool same = true;
            for (unsigned y = 0; y < 4; ++y)
                same = same &&
                       std::memcmp(back.data() + y * 32,
                                   storage.data() + y * 32, 16) == 0;
            CHECK(same);
            CHECK(back[16] == 0 && back[31] == 0);   /* padding untouched */
        }
    }
}

static void test_refusals() {
    nvngfx_texdesc d = make_desc(0x25, 64, 64, 1);
    std::vector<unsigned char> a(16384), b(16384);
    d.samples = 4;
    CHECK(nvngfx_tex_deswizzle(&d, 0, 0, a.data(), b.data(), 256) == -1);
    /* Linear is supported (see test_linear); a stride narrower than one row
     * is the refusal, and the copy must not guess a pitch. */
    d = make_desc(0x25, 64, 64, 1);
    d.flags = 0x10;
    d.stride = 128;
    CHECK(nvngfx_tex_deswizzle(&d, 0, 0, a.data(), b.data(), 256) == -1);
    d = make_desc(0x25, 64, 64, 1);
    d.target = 10;   /* BUFFER */
    CHECK(nvngfx_tex_deswizzle(&d, 0, 0, a.data(), b.data(), 256) == -1);
    /* A pitch smaller than the level's row is not a buffer to write into. */
    d = make_desc(0x25, 64, 64, 1);
    CHECK(nvngfx_tex_deswizzle(&d, 0, 0, a.data(), b.data(), 128) == -1);
    /* A layer past the descriptor's own count is not layer 0 by accident. */
    d.target = 4;
    d.depth = 2;
    CHECK(nvngfx_tex_deswizzle(&d, 0, 2, a.data(), b.data(), 256) == -1);
    /* An unknown format has no layout at all. */
    d = make_desc(0x00, 64, 64, 1);
    nvngfx_texlayout L;
    CHECK(nvngfx_tex_layout(&d, &L) != 0);
}

int main() {
    test_block_shapes();
    test_sizes();
    test_bl_offset();
    test_round_trip();
    test_linear();
    test_refusals();
    if (g_failures) {
        std::printf("nvn_texlayout_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvn_texlayout_test: ok (block shapes, sizes, offsets, "
                "round trip, linear, refusals)\n");
    return 0;
}
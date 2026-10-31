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

/* Writes a sub-rectangle through the same storage the Metal upload path
 * reads.  This is the CPU-side half of nvnTextureWriteTexels: the guest may
 * rewrite any mip/layer/slice after the first sample, and the bytes outside
 * the region must survive. */
static void test_write_region() {
    unsigned char patch[16];
    for (int i = 0; i < 4; ++i) {
        patch[i * 4 + 0] = 0;
        patch[i * 4 + 1] = 0;
        patch[i * 4 + 2] = 255;
        patch[i * 4 + 3] = 255;
    }

    {
        /* 4x4 RGBA8 block-linear: overwrite the inner 2x2 and leave the
         * border exactly as swizzled. */
        const nvngfx_texdesc d = make_desc(0x25, 4, 4, 1);
        nvngfx_texlayout L;
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        std::vector<unsigned char> storage(static_cast<size_t>(L.size), 0);
        std::vector<unsigned char> src(64), out(64, 0);
        for (size_t i = 0; i < src.size(); ++i)
            src[i] = static_cast<unsigned char>(i * 3u + 1u);
        CHECK(nvngfx_tex_swizzle(&d, 0, 0, src.data(), 16, storage.data()) == 0);
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 1, 1, 0, 2, 2, patch, 8, 0,
                                      storage.data()) == 0);
        CHECK(nvngfx_tex_deswizzle(&d, 0, 0, storage.data(), out.data(), 16) == 0);
        bool ok = true;
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x) {
                const bool inside = x >= 1 && x <= 2 && y >= 1 && y <= 2;
                const unsigned char* const p = out.data() + (y * 4 + x) * 4;
                if (inside)
                    ok = ok && p[0] == 0 && p[1] == 0 && p[2] == 255 &&
                         p[3] == 255;
                else
                    ok = ok && std::memcmp(p, src.data() + (y * 4 + x) * 4, 4) == 0;
            }
        CHECK(ok);

        /* The same patch written with a 16-byte row stride and a zero image
         * stride must land identically: one image is hblocks rows at the
         * caller's stride, which is how NVN resolves the strided variant. */
        const std::vector<unsigned char> compact = out;
        unsigned char padded[32];
        std::memset(padded, 0xEE, sizeof padded);
        std::memcpy(padded, patch, 8);
        std::memcpy(padded + 16, patch + 8, 8);
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 1, 1, 0, 2, 2, padded, 16, 0,
                                      storage.data()) == 0);
        CHECK(nvngfx_tex_deswizzle(&d, 0, 0, storage.data(), out.data(),
                                   16) == 0);
        CHECK(std::memcmp(out.data(), compact.data(), out.size()) == 0);

        /* A 2D level has no z slice 1. */
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 0, 0, 1, 4, 4, patch, 8, 0,
                                      storage.data()) == -1);
    }

    {
        /* LINEAR rows at stride 32: the 2x2 patch must not disturb the
         * padding after either row. */
        nvngfx_texdesc d = make_desc(0x25, 4, 4, 1);
        d.flags = 0x10;
        d.stride = 32;
        nvngfx_texlayout L;
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        std::vector<unsigned char> storage(static_cast<size_t>(L.size), 0xEE);
        std::vector<unsigned char> packed(64, 0), out(64, 0);
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x) {
                unsigned char* const p = storage.data() + y * 32 + x * 4;
                p[0] = static_cast<unsigned char>(y * 16 + x * 4);
                p[1] = 7;
                p[2] = 9;
                p[3] = 255;
            }
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 1, 1, 0, 2, 2, patch, 8, 0,
                                      storage.data()) == 0);
        CHECK(nvngfx_tex_deswizzle(&d, 0, 0, storage.data(), out.data(), 16) == 0);
        bool ok = true;
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x) {
                const bool inside = x >= 1 && x <= 2 && y >= 1 && y <= 2;
                const unsigned char* const p = out.data() + (y * 4 + x) * 4;
                if (inside)
                    ok = ok && p[0] == 0 && p[1] == 0 && p[2] == 255;
                else
                    ok = ok && p[0] == static_cast<unsigned char>(y * 16 + x * 4) &&
                         p[1] == 7 && p[2] == 9;
            }
        CHECK(ok);
        CHECK(storage[16] == 0xEE && storage[31] == 0xEE);
        CHECK(storage[48] == 0xEE && storage[63] == 0xEE);
        /* A stride that cannot hold one tight row is refused. */
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 0, 0, 0, 2, 2, patch, 4, 0,
                                      storage.data()) == -1);
    }

    {
        /* BC1 8x8: two block rows of two 8-byte blocks.  Rewrite only the
         * top-right block; the other three blocks must be untouched. */
        const nvngfx_texdesc d = make_desc(0x42, 8, 8, 1);
        nvngfx_texlayout L;
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        std::vector<unsigned char> storage(static_cast<size_t>(L.size), 0);
        unsigned char blocks[32];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j)
                blocks[i * 8 + j] = static_cast<unsigned char>(0x10 * i + j);
        std::vector<unsigned char> out(32, 0);
        CHECK(nvngfx_tex_swizzle(&d, 0, 0, blocks, 16, storage.data()) == 0);
        const unsigned char newBlock[8] = {0xAA, 0xBB, 0xCC, 0xDD,
                                           0x11, 0x22, 0x33, 0x44};
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 4, 0, 0, 4, 4, newBlock, 8, 0,
                                      storage.data()) == 0);
        CHECK(nvngfx_tex_deswizzle(&d, 0, 0, storage.data(), out.data(), 16) == 0);
        CHECK(std::memcmp(out.data() + 8, newBlock, 8) == 0);
        CHECK(std::memcmp(out.data() + 0, blocks, 8) == 0);
        CHECK(std::memcmp(out.data() + 16, blocks + 16, 16) == 0);
        /* A compressed region cannot start or end inside a block. */
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 1, 0, 0, 4, 4, newBlock, 8, 0,
                                      storage.data()) == -1);
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 8, 0, 0, 4, 4, newBlock, 8, 0,
                                      storage.data()) == -1);
    }

    {
        /* 3D: z=1 addresses the second slice, and slice 0 must not move. */
        nvngfx_texdesc d = make_desc(0x25, 4, 4, 1);
        d.target = 2;
        d.rank = 3;
        d.depth = 2;
        nvngfx_texlayout L;
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        std::vector<unsigned char> storage(static_cast<size_t>(L.size), 0);
        std::vector<unsigned char> src(128), out(128, 0);
        for (size_t i = 0; i < src.size(); ++i)
            src[i] = static_cast<unsigned char>(i + 1);
        CHECK(nvngfx_tex_swizzle(&d, 0, 0, src.data(), 16, storage.data()) == 0);
        unsigned char green[64];
        for (int i = 0; i < 16; ++i) {
            green[i * 4 + 0] = 0;
            green[i * 4 + 1] = 255;
            green[i * 4 + 2] = 0;
            green[i * 4 + 3] = 255;
        }
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 0, 0, 1, 4, 4, green, 16, 0,
                                      storage.data()) == 0);
        CHECK(nvngfx_tex_deswizzle(&d, 0, 0, storage.data(), out.data(), 16) == 0);
        CHECK(std::memcmp(out.data(), src.data(), 64) == 0);
        CHECK(std::memcmp(out.data() + 64, green, 64) == 0);
        /* 3D has one layer; layer 1 is not a thing. */
        CHECK(nvngfx_tex_write_region(&d, 0, 1, 0, 0, 0, 4, 4, green, 16, 0,
                                      storage.data()) == -1);
    }

    {
        const nvngfx_texdesc d = make_desc(0x25, 4, 4, 1);
        std::vector<unsigned char> storage(512, 0);
        CHECK(nvngfx_tex_write_region(nullptr, 0, 0, 0, 0, 0, 4, 4, patch, 16,
                                      0, storage.data()) == -1);
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 0, 0, 0, 4, 4, nullptr, 16, 0,
                                      storage.data()) == -1);
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 0, 0, 0, 4, 4, patch, 16, 0,
                                      nullptr) == -1);
        CHECK(nvngfx_tex_write_region(&d, 1, 0, 0, 0, 0, 4, 4, patch, 16, 0,
                                      storage.data()) == -1);
        CHECK(nvngfx_tex_write_region(&d, 0, 0, 0, 0, 0, 4, 4, patch, 0, 12,
                                      storage.data()) == -1);
    }
}

/* The CPU-side half of nvnTextureReadTexels: read a sub-rectangle out of the
 * guest's block-linear (or LINEAR) storage.  Every case is the same image the
 * write test builds, so a wrong byte offset shows up as a wrong texel. */
static void test_read_region() {
    {
        /* 4x4 RGBA8: read the inner 2x2 into a tightly packed buffer and
         * into one with a 16-byte row stride. */
        const nvngfx_texdesc d = make_desc(0x25, 4, 4, 1);
        nvngfx_texlayout L;
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        std::vector<unsigned char> storage(static_cast<size_t>(L.size), 0);
        std::vector<unsigned char> src(64);
        for (size_t i = 0; i < src.size(); ++i)
            src[i] = static_cast<unsigned char>(i * 5u + 3u);
        CHECK(nvngfx_tex_swizzle(&d, 0, 0, src.data(), 16, storage.data()) == 0);

        unsigned char tight[16] = {0};
        CHECK(nvngfx_tex_read_region(&d, 0, 0, 1, 1, 0, 2, 2, tight, 8, 0,
                                     storage.data()) == 0);
        bool ok = true;
        for (unsigned y = 0; y < 2; ++y)
            for (unsigned x = 0; x < 2; ++x)
                ok = ok && std::memcmp(tight + (y * 2 + x) * 4,
                                       src.data() + ((y + 1) * 4 + x + 1) * 4,
                                       4) == 0;
        CHECK(ok);

        unsigned char strided[32];
        std::memset(strided, 0xEE, sizeof strided);
        CHECK(nvngfx_tex_read_region(&d, 0, 0, 1, 1, 0, 2, 2, strided, 16, 0,
                                     storage.data()) == 0);
        CHECK(std::memcmp(strided, src.data() + (1 * 4 + 1) * 4, 8) == 0);
        CHECK(std::memcmp(strided + 16, src.data() + (2 * 4 + 1) * 4, 8) == 0);
        CHECK(strided[8] == 0xEE && strided[15] == 0xEE);
        CHECK(strided[24] == 0xEE && strided[31] == 0xEE);

        /* A whole level reads back exactly as it was swizzled. */
        std::vector<unsigned char> whole(64, 0);
        CHECK(nvngfx_tex_read_region(&d, 0, 0, 0, 0, 0, 4, 4, whole.data(),
                                     16, 0, storage.data()) == 0);
        CHECK(std::memcmp(whole.data(), src.data(), 64) == 0);

        /* Refusals: no destination, a stride narrower than one row, a 2D
         * level asked for slice 1, and a level past the descriptor. */
        CHECK(nvngfx_tex_read_region(&d, 0, 0, 0, 0, 0, 4, 4, nullptr, 16, 0,
                                     storage.data()) == -1);
        CHECK(nvngfx_tex_read_region(&d, 0, 0, 0, 0, 0, 2, 2, strided, 4, 0,
                                     storage.data()) == -1);
        CHECK(nvngfx_tex_read_region(&d, 0, 0, 0, 0, 1, 4, 4, whole.data(), 16,
                                     0, storage.data()) == -1);
        CHECK(nvngfx_tex_read_region(&d, 1, 0, 0, 0, 0, 4, 4, whole.data(), 16,
                                     0, storage.data()) == -1);
        /* A BC1 block cannot be read half-way: 2x2 texels are not a block. */
        const nvngfx_texdesc bc = make_desc(0x42, 8, 8, 1);
        std::vector<unsigned char> bcStorage(static_cast<size_t>(L.size), 0);
        unsigned char block[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        CHECK(nvngfx_tex_read_region(&bc, 0, 0, 2, 0, 0, 2, 4, block, 8, 0,
                                     bcStorage.data()) == -1);
    }

    {
        /* LINEAR rows at stride 32: only the region's own bytes move. */
        nvngfx_texdesc d = make_desc(0x25, 4, 4, 1);
        d.flags = 0x10;
        d.stride = 32;
        nvngfx_texlayout L;
        CHECK(nvngfx_tex_layout(&d, &L) == 0);
        std::vector<unsigned char> storage(static_cast<size_t>(L.size), 0xEE);
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x) {
                unsigned char* const p = storage.data() + y * 32 + x * 4;
                p[0] = static_cast<unsigned char>(y * 16 + x * 4);
                p[1] = 7;
                p[2] = 9;
                p[3] = 255;
            }
        unsigned char row[8] = {0};
        CHECK(nvngfx_tex_read_region(&d, 0, 0, 0, 2, 0, 2, 1, row, 8, 0,
                                     storage.data()) == 0);
        CHECK(row[0] == 32 && row[1] == 7 && row[2] == 9 && row[3] == 255);
        CHECK(row[4] == 36 && row[5] == 7 && row[6] == 9 && row[7] == 255);
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

/* The sparse-tile query: a 64 KiB tile is gw GOB columns wide and 8 << sh
 * block rows high, and only levels that can hold a whole tile are counted. */
static void test_sparse_tiles() {
    unsigned tw = 77, th = 77, td = 77;

    /* Not sparse: no tiles, and the outputs are cleared rather than stale. */
    nvngfx_texdesc d = make_desc(0x25, 4, 4, 1);
    CHECK(nvngfx_tex_sparse_tiles(&d, &tw, &th, &td) == 0);
    CHECK(tw == 0 && th == 0 && td == 0);

    /* Sparse 4x4 RGBA8: one tile is 128x128x1, but the level is too small. */
    d.flags = 0x20;
    CHECK(nvngfx_tex_sparse_tiles(&d, &tw, &th, &td) == 0);
    CHECK(tw == 128 && th == 128 && td == 1);

    /* 1024x1024 RGBA8, 5 levels: levels 0..3 hold a whole tile. */
    d = make_desc(0x25, 1024, 1024, 5);
    d.flags = 0x20;
    CHECK(nvngfx_tex_sparse_tiles(&d, &tw, &th, &td) == 4);
    CHECK(tw == 128 && th == 128 && td == 1);

    /* BC1 counts blocks: a tile is 512x256 texels, so only 2 levels fit. */
    d = make_desc(0x42, 1024, 1024, 5);
    d.flags = 0x20;
    CHECK(nvngfx_tex_sparse_tiles(&d, &tw, &th, &td) == 2);
    CHECK(tw == 512 && th == 256 && td == 1);

    /* 3D: the tile gains a depth of 1 << tile_d texels. */
    d = make_desc(0x25, 256, 256, 3);
    d.flags = 0x20;
    d.rank = 3;              /* what nvnTextureBuilderSetSize3D records */
    d.depth = 64;
    CHECK(nvngfx_tex_sparse_tiles(&d, &tw, &th, &td) == 2);
    CHECK(tw == 128 && th == 8 && td == 16);

    /* LINEAR overrides sparse: nothing is tiled. */
    d = make_desc(0x25, 1024, 1024, 5);
    d.flags = 0x30;
    CHECK(nvngfx_tex_sparse_tiles(&d, &tw, &th, &td) == 0);
    CHECK(tw == 0 && th == 0 && td == 0);

    /* Unknown format: no tile shape to report. */
    d = make_desc(0, 1024, 1024, 5);
    d.flags = 0x20;
    CHECK(nvngfx_tex_sparse_tiles(&d, &tw, &th, &td) == 0);

    /* A null descriptor is a caller bug, not an empty answer. */
    CHECK(nvngfx_tex_sparse_tiles(nullptr, &tw, &th, &td) == -1);
}

int main() {
    test_block_shapes();
    test_sizes();
    test_bl_offset();
    test_round_trip();
    test_linear();
    test_write_region();
    test_read_region();
    test_sparse_tiles();
    test_refusals();
    if (g_failures) {
        std::printf("nvn_texlayout_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvn_texlayout_test: ok (block shapes, sizes, offsets, "
                "round trip, linear, write region, read region, sparse tiles, "
                "refusals)\n");
    return 0;
}
/* NVN texture layout and the block-linear copy.  See nvn_texlayout.h for
 * provenance: the rules come from nxnative (MIT), whose nvn_tex.c ports the
 * deko3d (zlib) storage-size algorithm, and from deko3d's / Ryujinx's public
 * description of the Tegra X1 block-linear layout.
 *
 * One GOB is 64 bytes x 8 rows = 512 bytes.  A level is a grid of GOB columns
 * and GOB rows, grouped into "blocks" of 1 GOB wide x 2^tile_h GOBs high x
 * 2^tile_d GOBs deep; within a level the order is block column, then block
 * row, then block layer.
 */
#include "nvn_texlayout.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned bpb, bw, bh;
} fmt_t;

/* Bytes per block and block dimensions.  Same table nxnative measured; the
 * ranges are NVNformat's own grouping (DXT/RGTC/BPTC 4x4, then ASTC). */
static fmt_t fmt_of(int f) {
    static const unsigned char astc[14][2] = {
        {4, 4},  {5, 4},  {5, 5},  {6, 5},  {6, 6},  {8, 5},  {8, 6},
        {8, 8},  {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    static const struct {
        unsigned char lo, hi, bpb;
    } r[] = {
        {0x01, 0x04, 1},  {0x05, 0x09, 2},  {0x0A, 0x0C, 4},  {0x0D, 0x10, 2},
        {0x11, 0x15, 4},  {0x16, 0x18, 8},  {0x19, 0x1C, 3},  {0x1D, 0x21, 6},
        {0x22, 0x24, 12}, {0x25, 0x28, 4},  {0x29, 0x2D, 8},  {0x2E, 0x30, 16},
        {0x31, 0x31, 1},  {0x32, 0x32, 2},  {0x33, 0x35, 4},  {0x36, 0x36, 8},
        {0x37, 0x38, 4},  {0x39, 0x3C, 2},  {0x3D, 0x40, 4},  {0x51, 0x52, 1},
        {0x53, 0x54, 2},  {0x55, 0x56, 4},  {0x57, 0x58, 2},  {0x59, 0x5A, 4},
        {0x5B, 0x5C, 8},  {0x5D, 0x5E, 3},  {0x5F, 0x60, 6},  {0x61, 0x62, 12},
        {0x63, 0x64, 4},  {0x65, 0x66, 8},  {0x67, 0x68, 16}, {0x69, 0x6C, 4},
        {0x6D, 0x70, 4},  {0x71, 0x75, 8},  {0x76, 0x78, 16}, {0x95, 0x98, 2},
        {0x99, 0x9C, 4},
    };
    fmt_t t = {0, 1, 1};
    unsigned i;
    if (f >= 0x79 && f <= 0x94) {
        const int k = (f - 0x79) % 14;
        t.bpb = 16;
        t.bw = astc[k][0];
        t.bh = astc[k][1];
        return t;
    }
    if (f >= 0x41 && f <= 0x50) {
        const int b8 = (f <= 0x42) || (f == 0x45 || f == 0x46) ||
                       (f == 0x49 || f == 0x4A);
        t.bpb = b8 ? 8u : 16u;
        t.bw = 4;
        t.bh = 4;
        return t;
    }
    for (i = 0; i < sizeof r / sizeof *r; ++i)
        if (f >= r[i].lo && f <= r[i].hi) {
            t.bpb = r[i].bpb;
            return t;
        }
    return t;   /* bpb == 0: unknown */
}

int nvngfx_tex_format_block(int format, unsigned* bytes_per_block,
                            unsigned* block_width, unsigned* block_height) {
    const fmt_t t = fmt_of(format);
    if (bytes_per_block) *bytes_per_block = t.bpb;
    if (block_width) *block_width = t.bw;
    if (block_height) *block_height = t.bh;
    return t.bpb != 0;
}

static unsigned mip(unsigned s, unsigned l) {
    s >>= l;
    return s ? s : 1u;
}
static unsigned blk(unsigned s, unsigned b) { return (s + b - 1u) / b; }

/* Block height (log2) from the number of GOB rows: 1, 2, 4, 8, 16 GOBs. */
static unsigned pick_tile(unsigned gobs) {
    return gobs >= 16 ? 4u : gobs >= 8 ? 3u : gobs >= 4 ? 2u : gobs >= 2 ? 1u : 0u;
}

/* Shrinks a candidate block height until it fits the level.  NVN truncates
 * "block rows x 1.5 / 8" before rounding to a power of two; rounding up gave
 * one step too many for some real textures (nxnative's calibration notes). */
static unsigned adj_tile(unsigned shift, unsigned unit, unsigned dim) {
    if (!shift) return 0;
    {
        unsigned x = unit << (shift - 1);
        if (x >= dim)
            while (--shift) {
                x >>= 1;
                if (x < dim) break;
            }
    }
    return shift;
}

/* Byte pitch of a linear (non-tiled) texture.  NVN carries it in
 * NVNtextureBuilderSetStride; a zero stride means the guest let the driver
 * pick, which pads the row to the layout alignment (32 bytes, or 128 for the
 * linear render-target flag). */
static unsigned linear_pitch(const nvngfx_texdesc* d) {
    const fmt_t t = fmt_of(d->format);
    const unsigned align = (d->flags & 0x40) ? 128u : 32u;
    const unsigned row = blk((unsigned)d->width, t.bw) * t.bpb;
    if (d->stride > 0) return (unsigned)d->stride;
    return (row + align - 1u) / align * align;
}

static void samples_xy(int s, unsigned* sx, unsigned* sy) {
    *sx = *sy = 1;
    if (s == 2) *sx = 2;
    else if (s == 4) *sx = *sy = 2;
    else if (s == 8) { *sx = 4; *sy = 2; }
}

/* Dimensions and layering.  `rank` is authoritative because this backend
 * owns the SetSize1D/2D/3D setters; the target only adds the array / cube
 * shapes that rank alone cannot express.  (nvnative switches on the target
 * alone, but there the builder defaults always set it.) */
static void shape(const nvngfx_texdesc* d, unsigned* dpl, int* layered,
                  unsigned* w, unsigned* h, unsigned* z) {
    *w = (unsigned)d->width;
    *h = (unsigned)d->height;
    *z = (unsigned)d->depth;
    *layered = 0;
    const int t = d->target;
    if (t == 2 || d->rank == 3) {
        *dpl = 3;                       /* 3D */
        if (*z == 0) *z = 1;
        return;
    }
    if (t == 3) {                       /* 1D_ARRAY: layer count is height */
        *dpl = 1;
        *z = *h;
        *h = 1;
        *layered = 1;
        if (*z == 0) *z = 1;
        return;
    }
    if (t == 4 || t == 6) {             /* 2D_ARRAY, MS_ARRAY */
        *dpl = 2;
        *layered = 1;
        if (*z == 0) *z = 1;
        return;
    }
    if (t == 8) {                       /* CUBEMAP: six faces */
        *dpl = 2;
        *z = 6;
        *layered = 1;
        return;
    }
    if (t == 9) {                       /* CUBEMAP_ARRAY: depth cubes x 6 */
        *dpl = 2;
        *z = (*z ? *z : 1u) * 6u;
        *layered = 1;
        return;
    }
    if (d->rank == 1) {                 /* 1D */
        *dpl = 1;
        *h = 1;
        *z = 1;
        return;
    }
    *dpl = 2;                           /* 2D */
    *z = 1;
}

static int sparse_of(const nvngfx_texdesc* d) {
    return (d->flags & 0x20) && !(d->flags & 0x50) && d->target != 10;
}

/* Block height of a sparse texture's 64 KiB tiles, by bytes per block. */
static int sparse_tile_h(unsigned bpb) {
    return bpb == 1 ? 5 : bpb == 2 || bpb == 4 ? 4 : bpb == 8 || bpb == 16 ? 3 : -1;
}

static unsigned tile_shape_h(const nvngfx_texdesc* d, unsigned dpl, fmt_t t,
                             unsigned tile_h) {
    return dpl == 2 && !d->pkg_block_h && sparse_tile_h(t.bpb) >= 0
               ? (unsigned)sparse_tile_h(t.bpb)
               : tile_h;
}

/* Offset of level `level` from the start of its layer, plus the GOB columns
 * the level is laid out with. */
static unsigned long long level_walk(const nvngfx_texdesc* d, unsigned level,
                                     unsigned dpl, unsigned w, unsigned h,
                                     unsigned z, unsigned tile_h,
                                     unsigned tile_d, fmt_t t,
                                     unsigned* gobs) {
    unsigned sx, sy;
    const unsigned sh = tile_shape_h(d, dpl, t, tile_h);
    unsigned tw = 128u >> (sh + tile_d);
    int tiled = sparse_of(d);
    unsigned long long off = 0;
    unsigned i;
    samples_xy(d->samples, &sx, &sy);
    if (!tw) tw = 1;
    for (i = 0;; ++i) {
        const unsigned lw = blk(mip(w * sx, i), t.bw);
        const unsigned lh = dpl >= 2 ? blk(mip(h * sy, i), t.bh) : 1u;
        const unsigned ld = dpl >= 3 ? mip(z, i) : 1u;
        const unsigned lwb = lw * t.bpb;
        const unsigned th = adj_tile(tile_h, 8, lh);
        const unsigned td = adj_tile(tile_d, 1, ld);
        unsigned wg = (lwb + 63u) / 64u;
        const unsigned hg = (lh + 7u) / 8u;
        tiled = tiled && lwb >= tw * 64u && lh >= (8u << sh) && ld >= (1u << tile_d);
        if (tiled) wg = (wg + tw - 1u) / tw * tw;
        if (i == level) {
            if (gobs) *gobs = wg;
            return off;
        }
        {
            const unsigned ht = (hg + (1u << th) - 1u) >> th;
            const unsigned dt = (ld + (1u << td) - 1u) >> td;
            off += (unsigned long long)wg * ht * dt << (9 + th + td);
        }
    }
}

static unsigned long long level_offset(const nvngfx_texdesc* d, unsigned level,
                                       unsigned dpl, unsigned w, unsigned h,
                                       unsigned z, unsigned tile_h,
                                       unsigned tile_d, fmt_t t) {
    return level_walk(d, level, dpl, w, h, z, tile_h, tile_d, t, NULL);
}

int nvngfx_tex_layout(const nvngfx_texdesc* d, nvngfx_texlayout* o) {
    fmt_t t;
    unsigned dpl, w, h, z, tile_h = 0, tile_d = 0, levels, sx = 1, sy = 1;
    int layered;
    unsigned long long ls;
    if (!d || !o) return -1;
    memset(o, 0, sizeof *o);
    t = fmt_of(d->format);
    shape(d, &dpl, &layered, &w, &h, &z);
    samples_xy(d->samples, &sx, &sy);
    o->bytes_per_block = t.bpb;
    o->block_width = t.bw;
    o->block_height = t.bh;
    o->layers = layered ? z : 1u;
    if (!t.bpb) {
        o->alignment = 512;
        return -1;
    }
    if (d->target == 10) {                      /* BUFFER */
        o->layer_size = o->size = (unsigned long long)t.bpb * (unsigned)d->width;
        o->alignment = t.bpb == 12 ? 4u : t.bpb;
        return 0;
    }
    if (d->flags & 0x50) {                     /* LINEAR / LINEAR_RENDER_TARGET */
        /* Linear storage is row-major at a byte pitch: no GOBs, no tiles.
         * One pitch serves the whole chain, mip levels follow one another
         * (each starting on the layout alignment) and every layer repeats
         * the chain.  The old code returned stride * height, which is one
         * layer of one level -- too small for a mip chain or an array. */
        const unsigned pitch = linear_pitch(d);
        const unsigned nl = d->levels ? (unsigned)d->levels : 1u;
        unsigned long long total = 0;
        unsigned l;
        o->alignment = (d->flags & 0x40) ? 128u : 32u;
        o->tile_h = 0;
        o->tile_d = 0;
        if ((unsigned long long)pitch <
            (unsigned long long)blk(w, t.bw) * t.bpb)
            return -1;                          /* stride narrower than a row */
        for (l = 0; l < nl; ++l) {
            const unsigned rows_l = dpl >= 2 ? mip(h, l) : 1u;
            total = (total + o->alignment - 1ull) / o->alignment * o->alignment;
            total += (unsigned long long)pitch * rows_l;
        }
        o->layer_size = total;
        o->size = layered ? total * z : total;
        return 0;
    }
    if (d->pkg_block_h) {
        /* Packaged layout byte: low nibble block height, high nibble depth. */
        const unsigned pl = ((unsigned)d->pkg_block_h - 1u) & 0xffu;
        if (dpl >= 2) tile_h = pl & 0xfu;
        if (dpl == 3) tile_d = pl >> 4;
    } else if (dpl == 2 && sparse_of(d) && sparse_tile_h(t.bpb) >= 0 &&
               blk(w * sx, t.bw) * t.bpb >= (128u >> sparse_tile_h(t.bpb)) * 64u &&
               blk(h * sy, t.bh) >= (8u << sparse_tile_h(t.bpb))) {
        tile_h = (unsigned)sparse_tile_h(t.bpb);
    } else if (dpl == 2) {
        const unsigned ah = blk(h * sy, t.bh);
        tile_h = pick_tile(ah * 3u / 16u);
    } else if (dpl == 3) {
        tile_d = pick_tile(z + z / 2u);
    }
    o->tile_h = tile_h;
    o->tile_d = tile_d;
    levels = d->levels ? (unsigned)d->levels : 1u;
    ls = level_offset(d, levels, dpl, w, h, z, tile_h, tile_d, t);
    if (sparse_of(d)) ls = (ls + 0xFFFFull) & ~0xFFFFull;
    if (layered) {
        /* A layer starts on a block boundary. */
        const unsigned hh = blk(h, t.bh);
        unsigned th = tile_h, td = tile_d, sh;
        unsigned long long g;
        while (th && hh <= (8u << (th - 1u))) --th;
        while (td && z <= (1u << (td - 1u))) --td;
        sh = th + td + 9u;
        g = ls >> sh;
        if (ls != g << sh) ls = (g + 1) << sh;
    }
    o->layer_size = ls;
    o->size = layered ? ls * z : ls;
    o->alignment = sparse_of(d) ? 0x10000u : 512u;
    return 0;
}

unsigned long long nvngfx_tex_level_offset(const nvngfx_texdesc* d,
                                           unsigned level) {
    fmt_t t;
    unsigned dpl, w, h, z;
    int layered;
    nvngfx_texlayout L;
    if (!d) return 0;
    t = fmt_of(d->format);
    shape(d, &dpl, &layered, &w, &h, &z);
    if (!t.bpb || d->target == 10) return 0;
    if (nvngfx_tex_layout(d, &L) != 0) return 0;
    if (d->flags & 0x50) {
        const unsigned pitch = linear_pitch(d);
        unsigned long long off = 0;
        unsigned l;
        for (l = 0; l < level; ++l) {
            const unsigned rows_l = dpl >= 2 ? mip(h, l) : 1u;
            off = (off + L.alignment - 1ull) / L.alignment * L.alignment;
            off += (unsigned long long)pitch * rows_l;
        }
        return (off + L.alignment - 1ull) / L.alignment * L.alignment;
    }
    return level_offset(d, level, dpl, w, h, z, L.tile_h, L.tile_d, t);
}

unsigned nvngfx_tex_level_gobs(const nvngfx_texdesc* d, unsigned level) {
    fmt_t t;
    unsigned dpl, w, h, z, g = 0;
    int layered;
    nvngfx_texlayout L;
    if (!d) return 0;
    t = fmt_of(d->format);
    shape(d, &dpl, &layered, &w, &h, &z);
    if (!t.bpb || (d->flags & 0x50) || d->target == 10) return 0;
    if (nvngfx_tex_layout(d, &L) != 0) return 0;
    level_walk(d, level, dpl, w, h, z, L.tile_h, L.tile_d, t, &g);
    return g;
}

/* Per-level block height and depth: a level smaller than the texture shrinks
 * the block, never the storage. */
static void level_tiles(unsigned tile_h, unsigned tile_d, const nvngfx_texdesc* d,
                        unsigned level, unsigned dpl, unsigned h, unsigned z,
                        fmt_t t, unsigned* th, unsigned* td) {
    const unsigned lh = dpl >= 2 ? blk(mip(h, level), t.bh) : 1u;
    const unsigned ld = dpl >= 3 ? mip(z, level) : 1u;
    *th = adj_tile(tile_h, 8, lh);
    *td = adj_tile(tile_d, 1, ld);
    (void)d;
}

int nvngfx_tex_level_geometry(const nvngfx_texdesc* d, unsigned level,
                              unsigned layer, unsigned* row_bytes,
                              unsigned* rows, unsigned* slices,
                              unsigned long long* base, unsigned* tile_h,
                              unsigned* tile_d) {
    fmt_t t;
    unsigned dpl, w, h, z;
    int layered;
    nvngfx_texlayout L;
    if (!d) return -1;
    if (d->target == 5 || d->target == 6 || d->target == 10 ||
        d->samples > 1)
        return -1;
    t = fmt_of(d->format);
    if (!t.bpb || level >= (d->levels ? (unsigned)d->levels : 1u)) return -1;
    if (nvngfx_tex_layout(d, &L) != 0) return -1;
    if (layer >= L.layers) return -1;
    shape(d, &dpl, &layered, &w, &h, &z);
    {
        const unsigned lw = mip(w, level);
        const unsigned lh = dpl >= 2 ? mip(h, level) : 1u;
        if (row_bytes) *row_bytes = blk(lw, t.bw) * t.bpb;
        if (rows) *rows = dpl >= 2 ? blk(lh, t.bh) : 1u;
        if (slices) *slices = dpl == 3 ? mip(z, level) : 1u;
    }
    if (base) *base = (unsigned long long)layer * L.layer_size +
                      nvngfx_tex_level_offset(d, level);
    if (tile_h || tile_d) {
        unsigned th = 0, td = 0;
        if (!(d->flags & 0x50))
            level_tiles(L.tile_h, L.tile_d, d, level, dpl, h, z, t, &th, &td);
        if (tile_h) *tile_h = th;
        if (tile_d) *tile_d = td;
    }
    return 0;
}

/* Offset of (x, y, z) inside one GOB block, from the public description of
 * the Tegra X1 layout: a GOB is 64 bytes x 8 rows, and within it the order is
 * 32-byte halves, then 16-byte quarters, then row pairs. */
static unsigned gob_off(unsigned x, unsigned y) {
    return (x % 64u / 32u) * 256u + (y % 8u / 2u) * 64u +
           (x % 32u / 16u) * 32u + (y % 2u) * 16u + x % 16u;
}

unsigned long long nvngfx_tex_bl_offset(unsigned x, unsigned y, unsigned z,
                                        unsigned row_bytes, unsigned rows,
                                        unsigned tile_h, unsigned tile_d) {
    const unsigned bh = 1u << tile_h, bd = 1u << tile_d;
    const unsigned long long gobs_x = (row_bytes + 63u) / 64u;
    const unsigned long long blocks_y = (rows + 8u * bh - 1u) / (8u * bh);
    const unsigned long long block = 512ull * bh * bd;
    return (z / bd) * gobs_x * blocks_y * block +
           (y / (8u * bh)) * gobs_x * block + (x / 64u) * block +
           ((z % bd) * bh + (y % (8u * bh)) / 8u) * 512ull + gob_off(x, y);
}

static int move(const nvngfx_texdesc* d, unsigned level, unsigned layer,
                unsigned char* bl, unsigned char* lin, size_t pitch,
                int to_linear) {
    unsigned row_bytes = 0, rows = 0, slices = 0, th = 0, td = 0;
    unsigned long long base = 0;
    if (nvngfx_tex_level_geometry(d, level, layer, &row_bytes, &rows, &slices,
                                  &base, &th, &td) != 0)
        return -1;
    if (pitch < row_bytes) return -1;
    bl += base;
    if (d->flags & 0x50) {
        /* Already row-major on the guest side: copy row by row, honouring
         * both pitches.  `storage_pitch` comes from the descriptor, `pitch`
         * from the caller's buffer. */
        const size_t storage_pitch = linear_pitch(d);
        unsigned z, y;
        if (storage_pitch < row_bytes) return -1;
        for (z = 0; z < slices; ++z) {
            for (y = 0; y < rows; ++y) {
                unsigned char* const lin_row =
                    lin + ((size_t)z * rows + y) * pitch;
                unsigned char* const bl_row =
                    bl + ((size_t)z * rows + y) * storage_pitch;
                if (to_linear) memcpy(lin_row, bl_row, row_bytes);
                else memcpy(bl_row, lin_row, row_bytes);
            }
        }
        return 0;
    }
    {
        unsigned long long lb =
            (unsigned long long)nvngfx_tex_level_gobs(d, level) * 64ull;
        const unsigned bh = 1u << th, bd = 1u << td;
        const unsigned long long block = 512ull * bh * bd;
        const unsigned nfull = row_bytes >> 4, tail = row_bytes & 15u;
        unsigned long long gobs_x, blocks_y;
        unsigned z;
        static const unsigned short kox[4] = {0, 32, 256, 288};
        static const unsigned short koy[8] = {0, 16, 64, 80, 128, 144, 192, 208};
        if (lb < row_bytes) lb = row_bytes;
        gobs_x = (lb + 63ull) >> 6;
        blocks_y = ((unsigned long long)rows + 8ull * bh - 1ull) >> (3 + th);
        for (z = 0; z < slices; ++z) {
            const unsigned long long zb =
                (unsigned long long)(z >> td) * gobs_x * blocks_y * block +
                (unsigned long long)(z & (bd - 1u)) * bh * 512ull;
            unsigned y;
            for (y = 0; y < rows; ++y) {
                const unsigned long long p =
                    (unsigned long long)(y >> (3 + th)) * gobs_x * block +
                    (unsigned long long)((y & (8u * bh - 1u)) >> 3) * 512ull +
                    koy[y & 7u];
                unsigned char* const row =
                    lin + ((size_t)z * rows + y) * pitch;
                const unsigned long long yb = zb + p;
                unsigned j;
                if (to_linear) {
                    for (j = 0; j < nfull; ++j)
                        memcpy(row + (j << 4),
                               bl + yb + (unsigned long long)(j >> 2) * block +
                                   kox[j & 3u],
                               16);
                    if (tail)
                        memcpy(row + (nfull << 4),
                               bl + yb + (unsigned long long)(nfull >> 2) * block +
                                   kox[nfull & 3u],
                               tail);
                } else {
                    for (j = 0; j < nfull; ++j)
                        memcpy(bl + yb + (unsigned long long)(j >> 2) * block +
                                   kox[j & 3u],
                               row + (j << 4), 16);
                    if (tail)
                        memcpy(bl + yb + (unsigned long long)(nfull >> 2) * block +
                                   kox[nfull & 3u],
                               row + (nfull << 4), tail);
                }
            }
        }
    }
    return 0;
}

int nvngfx_tex_deswizzle(const nvngfx_texdesc* d, unsigned level,
                         unsigned layer, const void* storage, void* linear,
                         size_t pitch) {
    if (!storage || !linear) return -1;
    return move(d, level, layer, (unsigned char*)storage, (unsigned char*)linear,
                pitch, 1);
}

int nvngfx_tex_swizzle(const nvngfx_texdesc* d, unsigned level, unsigned layer,
                       const void* linear, size_t pitch, void* storage) {
    if (!storage || !linear) return -1;
    return move(d, level, layer, (unsigned char*)storage, (unsigned char*)linear,
                pitch, 0);
}

int nvngfx_tex_write_region(const nvngfx_texdesc* d, unsigned level,
                            unsigned layer, int x, int y, int z,
                            int width, int height, const void* src,
                            size_t src_row, size_t src_image, void* storage) {
    fmt_t t;
    unsigned row_bytes = 0, rows = 0, slices = 0;
    unsigned xb, yb, wb, hb;
    size_t tight_row, row, img, total;
    unsigned char* lin;
    unsigned char* dst;
    unsigned i;

    if (!d || !src || !storage) return -1;
    if (x < 0 || y < 0 || z < 0 || width <= 0 || height <= 0) return -1;
    t = fmt_of(d->format);
    if (!t.bpb) return -1;
    if (nvngfx_tex_level_geometry(d, level, layer, &row_bytes, &rows, &slices,
                                  NULL, NULL, NULL) != 0)
        return -1;
    if ((unsigned)z >= slices) return -1;
    if ((unsigned)x % t.bw || (unsigned)y % t.bh ||
        (unsigned)width % t.bw || (unsigned)height % t.bh)
        return -1;

    xb = (unsigned)x / t.bw;
    yb = (unsigned)y / t.bh;
    wb = (unsigned)width / t.bw;
    hb = (unsigned)height / t.bh;
    if (xb > row_bytes / t.bpb || wb > row_bytes / t.bpb - xb) return -1;
    if (yb > rows || hb > rows - yb) return -1;

    tight_row = (size_t)wb * t.bpb;
    row = src_row ? src_row : tight_row;
    if (row < tight_row) return -1;
    if (hb && row > SIZE_MAX / hb) return -1;
    img = src_image ? src_image : row * hb;
    if (img < row * hb) return -1;

    if (slices && rows > SIZE_MAX / row_bytes) return -1;
    if (slices > SIZE_MAX / ((size_t)row_bytes * rows)) return -1;
    total = (size_t)row_bytes * rows * slices;
    lin = (unsigned char*)malloc(total ? total : 1u);
    if (!lin) return -1;
    if (nvngfx_tex_deswizzle(d, level, layer, storage, lin, row_bytes) != 0) {
        free(lin);
        return -1;
    }

    dst = lin + (size_t)z * rows * row_bytes + (size_t)yb * row_bytes +
          (size_t)xb * t.bpb;
    for (i = 0; i < hb; ++i)
        memcpy(dst + (size_t)i * row_bytes,
               (const unsigned char*)src + (size_t)i * row, tight_row);

    if (nvngfx_tex_swizzle(d, level, layer, lin, row_bytes, storage) != 0) {
        free(lin);
        return -1;
    }
    free(lin);
    return 0;
}

/* NVN texture storage layout: block-linear (Tegra X1) sizes and offsets, and
 * the block-linear <-> linear copy.
 *
 * Why this is a separate C file: it is arithmetic over the guest's own bytes.
 * The backend needs it to know how large a texture really is and where each
 * mip level starts before it can copy a guest texture into an MTLTexture.
 * Keeping it free of Metal lets the same code be unit tested on Windows.
 *
 * Provenance
 * ----------
 * The layout rules are the ones nxnative measured against the real driver
 * (https://github.com/seedds/nxnative, MIT License, Copyright (c) 2026
 * seedds), dm/gfx/nvn/nvn_tex.c and dm/gfx/tex/nvn_deswizzle.c.  nvn_tex.c in
 * turn ports the storage-size algorithm from deko3d's source/dk_image.cpp
 * (zlib license, Copyright (C) 2018-2020 fincs).  The comments that say
 * "measured" below refer to those projects' notes, not to a measurement made
 * here; this project has no Switch hardware.
 *
 * What the format says
 * --------------------
 * A compressed format's "pixel" is a block: bytes_per_block bytes covering
 * block_width x block_height texels (4x4 for BCn, 4x4..12x12 for ASTC).  An
 * uncompressed format is 1x1.  All the arithmetic below counts block rows,
 * and a linear image is "row = one row of blocks", so the same copy routine
 * serves both.
 */
#ifndef NVN_TEXLAYOUT_H
#define NVN_TEXLAYOUT_H

#include <stddef.h>
#include "nvn_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned bytes_per_block;
    unsigned block_width, block_height;
    unsigned tile_h, tile_d;             /* log2 of the block height / depth */
    unsigned layers;                     /* 1 unless the target is layered */
    unsigned long long layer_size;       /* bytes per layer, block aligned */
    unsigned long long size;             /* whole texture */
    unsigned alignment;                  /* 512, or 64 KiB for sparse */
} nvngfx_texlayout;

/* Block shape of an NVNformat: returns 0 for a format this table does not
 * know (then no layout can be computed), 1 otherwise. */
int nvngfx_tex_format_block(int format, unsigned* bytes_per_block,
                            unsigned* block_width, unsigned* block_height);

/* Storage size, alignment and block height of a whole texture. */
int nvngfx_tex_layout(const nvngfx_texdesc* d, nvngfx_texlayout* out);

/* Byte offset of a mip level from the start of its layer. */
unsigned long long nvngfx_tex_level_offset(const nvngfx_texdesc* d,
                                           unsigned level);

/* GOB columns this level is laid out with (64 bytes each).  A sparse
 * texture's fully tiled levels are padded to whole 64 KiB tiles, so this can
 * exceed the level's own byte count. */
unsigned nvngfx_tex_level_gobs(const nvngfx_texdesc* d, unsigned level);

/* Geometry of one level/layer, the way nvngfx_tex_deswizzle addresses it.
 * rows counts block rows; slices is 1 for anything but 3D. */
int nvngfx_tex_level_geometry(const nvngfx_texdesc* d, unsigned level,
                              unsigned layer, unsigned* row_bytes,
                              unsigned* rows, unsigned* slices,
                              unsigned long long* base, unsigned* tile_h,
                              unsigned* tile_d);

/* Byte offset of linear (x byte-in-row, y block row, z slice) inside one
 * level/layer.  Exposed so a test can build a block-linear image by writing
 * through it and read it back with the copy below. */
unsigned long long nvngfx_tex_bl_offset(unsigned x, unsigned y, unsigned z,
                                        unsigned row_bytes, unsigned rows,
                                        unsigned tile_h, unsigned tile_d);

/* Copies one level/layer from whole-texture storage into a linear image.
 * `pitch` is bytes per row and must be at least the level's row_bytes.
 * Block-linear storage goes through the GOB arithmetic; a texture with the
 * LINEAR / LINEAR_RENDER_TARGET flag is copied row by row at the descriptor's
 * own stride.  Returns 0, or -1 for a texture this path does not handle:
 * multisampled, BUFFER, or a stride narrower than one row. */
int nvngfx_tex_deswizzle(const nvngfx_texdesc* d, unsigned level,
                         unsigned layer, const void* storage, void* linear,
                         size_t pitch);

/* The inverse, used by the round-trip test. */
int nvngfx_tex_swizzle(const nvngfx_texdesc* d, unsigned level, unsigned layer,
                       const void* linear, size_t pitch, void* storage);

/* Writes one rectangular region of one level/layer/slice into the guest's
 * block-linear (or LINEAR) storage.  `x`/`y`/`width`/`height` are texels;
 * for a compressed format they must be multiples of the format's block
 * dimensions, because a BCn block cannot be partially updated without
 * re-encoding it.  `z` selects a slice of a 3D level and must be zero for a
 * 2D/array layer.  `src_row` and `src_image` are byte strides: a zero row
 * stride is the tightly packed row, and a zero image stride is the resolved
 * row stride times the region's block rows.  The usual read-modify-write is
 * used: the level is
 * deswizzled, the region copied in, and the level swizzled back, so the
 * bytes outside the region (including LINEAR row padding) are preserved.
 * Returns 0, or -1 for a region/format/stride this path refuses. */
int nvngfx_tex_write_region(const nvngfx_texdesc* d, unsigned level,
                            unsigned layer, int x, int y, int z,
                            int width, int height, const void* src,
                            size_t src_row, size_t src_image, void* storage);

/* The inverse of nvngfx_tex_write_region: reads one rectangular region of one
 * level/layer/slice out of the guest's block-linear (or LINEAR) storage into a
 * linear image.  `dst_row` and `dst_image` are byte strides: a zero row stride
 * is the tightly packed row, and a zero image stride is the resolved row
 * stride times the region's block rows.  A stride below that minimum is
 * refused rather than letting rows or slices overlap.  Same region, format
 * and stride rules as the write path, including block alignment for
 * compressed formats.  Returns 0, or -1 for a region/format/stride this path
 * refuses. */
int nvngfx_tex_read_region(const nvngfx_texdesc* d, unsigned level,
                           unsigned layer, int x, int y, int z,
                           int width, int height, void* dst,
                           size_t dst_row, size_t dst_image,
                           const void* storage);

#ifdef __cplusplus
}
#endif

#endif /* NVN_TEXLAYOUT_H */
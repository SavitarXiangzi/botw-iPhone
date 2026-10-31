/* Portable NVN format table and texture description.
 *
 * Why this file exists separately from nvn_metal.mm: it is pure data plus
 * arithmetic, so it compiles and gets tested on Windows/Linux.  Only the
 * MTLTexture creation needs Apple frameworks.
 *
 * NVNformat -> MTLPixelFormat table
 * --------------------------------
 * The numeric values below are derived from the public nxnative project
 * (https://github.com/seedds/nxnative, MIT License, Copyright (c) 2026 seedds),
 * dm/gfx/tex/nvn_fmt_mtl.c, which is the only public, annotated mapping this
 * project could find.  Apple's MTLPixelFormat constants are public API.
 *
 * Two honest caveats are inherited:
 *   - some entries are SUBSTITUTES (kind == 2), e.g. DEPTH24 -> Depth32Float;
 *     the caller is told, so a lossy mapping is never silent;
 *   - the 16-bit packed formats were matched by name rather than by observed
 *     bit order, so they are marked substitutes as well.
 *
 * Nothing here needs Nintendo's nvn.h: the NVN side is just the integer the
 * guest passes to nvnTextureBuilderSetFormat.
 */
#ifndef NVN_TEXTURE_H
#define NVN_TEXTURE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the builder setters collected, in host terms.  The guest's
 * NVNtextureBuilder layout is not used at all -- we own the setters, so the
 * state is whatever the guest told us. */
typedef struct {
    int rank;      /* 1, 2 or 3: which of SetSize1D/2D/3D was used */
    int target;    /* raw NVNtextureTarget, kept for logging only */
    int format;    /* raw NVNformat */
    int flags;
    int samples;   /* 0 or 1 == single sampled */
    int width, height, depth, levels;
    unsigned long long pool;   /* guest NVNmemoryPool address, 0 = none */
    long long offset;
    long long stride;
    /* Builder state that does not describe the storage: the device the
     * builder was created against (preserved across SetDefaults), the
     * GL texture name (opaque, no GL interop exists), and the plane a
     * sample reads (0 depth, 1 stencil). */
    unsigned long long device;
    unsigned gl_name;
    int dsmode;
    unsigned char swz[4];      /* channel swizzle, R/G/B/A order */
    /* SetPackagedTextureLayout's first byte (0 when the guest never called
     * it): low nibble = log2 block height, high nibble = log2 block depth.
     * The setter is not wired yet, so this stays 0 and the automatic block
     * height rule applies -- the same default nxnative uses when a title does
     * not package a layout. */
    unsigned char pkg_block_h;
} nvngfx_texdesc;

/* Zeroes the struct and applies NVN's builder defaults. */
void nvngfx_texdesc_defaults(nvngfx_texdesc* d);

/* Returns the MTLPixelFormat value, or 0 when Metal has no counterpart.
 * `lossy` (may be NULL) is set to 1 when the mapping is a substitute rather
 * than an exact match. */
unsigned nvngfx_format_to_mtl(int nvn_format, int* lossy);

/* Number of slots in the table: 0x00..0x9C inclusive.  Entry 0 is the
 * "no format" hole, so the highest addressable NVNformat is 0x9C. */
int nvngfx_format_table_size(void);

/* Component class of a colour format: 0 = floating point (including unorm,
 * snorm and sRGB), 1 = unsigned integer, 2 = signed integer.  Returns -1 for
 * a format Metal cannot use as a colour attachment.  A fragment shader must
 * declare float4 / uint4 / int4 to match, so the clear pipeline needs this. */
int nvngfx_format_component_class(int nvn_format);

#ifdef __cplusplus
}
#endif
#endif /* NVN_TEXTURE_H */
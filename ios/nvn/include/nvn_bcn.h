/* BCn (DXT/RGTC) block decoding for the iOS texture path.
 *
 * Apple GPUs do not implement the BCn families on iOS/tvOS: a
 * MTLPixelFormatBC* descriptor is invalid there, so an NVN title that stores
 * DXT1/DXT5 textures cannot be sampled as-is.  This file decodes the block
 * to RGBA8 on the CPU while the texture is uploaded, which costs memory and
 * a memcpy but needs no ASTC encoder.
 *
 * Scope: the unsigned families this backend supports -- BC1
 * (0x41/0x42/0x45/0x46), BC2/DXT3 (0x43/0x47), BC3/DXT5 (0x44/0x48),
 * BC4U (0x49) and BC5U (0x4B).  Signed BC4/BC5 and the BPTC families
 * (BC6H/BC7) have no decoder here and are reported as unsupported rather
 * than decoded wrongly.
 *
 * Provenance: the block layouts are the documented S3TC/RGTC ones (four
 * 16-bit RGB565 endpoints or two 8-bit alpha endpoints plus packed indices).
 * No game data and no Metal in this file, so it unit tests on Windows.
 */
#ifndef NVN_BCN_H
#define NVN_BCN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when `format` is an NVNformat this decoder handles. */
int nvngfx_bcn_supported(int format);

/* Bytes of one 4x4 block: 8 for BC1/BC4, 16 for BC2/BC3/BC5, 0 otherwise. */
unsigned nvngfx_bcn_block_bytes(int format);

/* True when the NVNformat stores sRGB-encoded colours (the *_SRGB DXT
 * variants).  The caller then wants an sRGB MTLPixelFormat. */
int nvngfx_bcn_is_srgb(int format);

/* Decodes one 4x4 block into 16 RGBA8 texels, row-major, 64 bytes.
 * Returns 0, or -1 for a format this decoder does not handle. */
int nvngfx_bcn_decode(int format, const unsigned char* src, unsigned char* rgba);

/* Decodes a whole level (or one Z slice of a 3D level) into RGBA8.
 *
 * `src_pitch` is the compressed row pitch, i.e. blocks_x * block_bytes;
 * `rgba_pitch` must be at least width * 4.  Both pitches may have padding.
 * A partial edge block is decoded and clipped to width/height, exactly as
 * the GPU would ignore texels outside the image.  Returns 0, or -1 for a
 * null pointer, an unsupported format, zero dimensions, or a pitch narrower
 * than one row. */
int nvngfx_bcn_decode_image(int format, const unsigned char* src,
                            size_t src_pitch, unsigned width, unsigned height,
                            unsigned char* rgba, size_t rgba_pitch);

#ifdef __cplusplus
}
#endif

#endif /* NVN_BCN_H */

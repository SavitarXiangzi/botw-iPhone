/* BCn block decoding -- see nvn_bcn.h for scope and provenance. */
#include "nvn_bcn.h"

#include <string.h>

typedef struct {
    unsigned char r, g, b, a;
} rgba8;

static unsigned le16(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned le32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

/* RGB565 -> RGBA8 with bit replication, the way the hardware expands it. */
static void rgb565(unsigned v, rgba8* c) {
    const unsigned r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
    c->r = (unsigned char)((r << 3) | (r >> 2));
    c->g = (unsigned char)((g << 2) | (g >> 4));
    c->b = (unsigned char)((b << 3) | (b >> 2));
    c->a = 255;
}

static int format_kind(int format) {
    switch (format) {
    case 0x41: case 0x45: return 1;   /* RGB_DXT1, RGB_DXT1_SRGB: no alpha */
    case 0x42: case 0x46: return 2;   /* RGBA_DXT1 (+SRGB) */
    case 0x43: case 0x47: return 3;   /* RGBA_DXT3 (+SRGB) */
    case 0x44: case 0x48: return 4;   /* RGBA_DXT5 (+SRGB) */
    case 0x49: return 5;              /* BC4U */
    case 0x4B: return 6;              /* BC5U */
    default: return 0;
    }
}

int nvngfx_bcn_supported(int format) { return format_kind(format) != 0; }

unsigned nvngfx_bcn_block_bytes(int format) {
    switch (format_kind(format)) {
    case 1: case 2: case 5: return 8;
    case 3: case 4: case 6: return 16;
    default: return 0;
    }
}

int nvngfx_bcn_is_srgb(int format) {
    return format == 0x45 || format == 0x46 || format == 0x47 || format == 0x48;
}

/* The 4-texel palette of a BC1 colour block.  DXT3/DXT5 always use the
 * four-colour mode, DXT1 picks it only when c0 > c1. */
static void bc1_palette(const unsigned char* src, int four_colour_only,
                        rgba8 pal[4]) {
    const unsigned c0 = le16(src), c1 = le16(src + 2);
    int four = four_colour_only || c0 > c1;
    rgb565(c0, &pal[0]);
    rgb565(c1, &pal[1]);
    if (four) {
        pal[2].r = (unsigned char)((2u * pal[0].r + pal[1].r) / 3u);
        pal[2].g = (unsigned char)((2u * pal[0].g + pal[1].g) / 3u);
        pal[2].b = (unsigned char)((2u * pal[0].b + pal[1].b) / 3u);
        pal[2].a = 255;
        pal[3].r = (unsigned char)((pal[0].r + 2u * pal[1].r) / 3u);
        pal[3].g = (unsigned char)((pal[0].g + 2u * pal[1].g) / 3u);
        pal[3].b = (unsigned char)((pal[0].b + 2u * pal[1].b) / 3u);
        pal[3].a = 255;
    } else {
        pal[2].r = (unsigned char)((pal[0].r + pal[1].r) / 2u);
        pal[2].g = (unsigned char)((pal[0].g + pal[1].g) / 2u);
        pal[2].b = (unsigned char)((pal[0].b + pal[1].b) / 2u);
        pal[2].a = 255;
        pal[3].r = pal[3].g = pal[3].b = 0;
        pal[3].a = 0;
    }
}

/* Decodes a BC4/DXT5-alpha block (2 endpoints + 16 three-bit indices) into
 * 16 unsigned values. */
static void bc4_values(const unsigned char* src, unsigned char out[16]) {
    const unsigned a0 = src[0], a1 = src[1];
    unsigned char pal[8];
    unsigned long long bits;
    unsigned i;
    pal[0] = (unsigned char)a0;
    pal[1] = (unsigned char)a1;
    if (a0 > a1) {
        for (i = 1; i < 7; ++i)
            pal[i + 1] = (unsigned char)(((7u - i) * a0 + i * a1) / 7u);
    } else {
        for (i = 1; i < 5; ++i)
            pal[i + 1] = (unsigned char)(((5u - i) * a0 + i * a1) / 5u);
        pal[6] = 0;
        pal[7] = 255;
    }
    /* 48 index bits: 16 texels x 3 bits, so texel 10 starts at bit 30 and
     * texels 11..15 live above the first 32-bit word.  Reading only one
     * word made the last six texels read palette entry 0. */
    bits = (unsigned long long)le32(src + 2) |
           ((unsigned long long)le16(src + 6) << 32);
    for (i = 0; i < 16; ++i)
        out[i] = pal[(unsigned)((bits >> (3 * i)) & 7ull)];
}

int nvngfx_bcn_decode(int format, const unsigned char* src, unsigned char* rgba) {
    const int kind = format_kind(format);
    rgba8 out[16];
    unsigned i;
    if (!src || !rgba || !kind) return -1;

    if (kind == 1 || kind == 2 || kind == 3 || kind == 4) {
        rgba8 pal[4];
        const int colour_only = (kind == 3 || kind == 4);
        bc1_palette(src, colour_only, pal);
        {
            const unsigned bits = le32(src + 4);
            for (i = 0; i < 16; ++i) out[i] = pal[(bits >> (2 * i)) & 3u];
        }
        if (kind == 1) {
            /* RGB_DXT1 has no alpha channel: index 3's transparent black
             * would be a hole if it survived, so force opaque. */
            for (i = 0; i < 16; ++i) out[i].a = 255;
        }
        if (kind == 3) {
            /* DXT3: one explicit 4-bit alpha per texel. */
            for (i = 0; i < 16; ++i) {
                const unsigned nib = (le16(src + 8 + (i / 4) * 2) >> (4 * (i % 4))) & 15u;
                out[i].a = (unsigned char)(nib * 17u);
            }
        } else if (kind == 4) {
            unsigned char a[16];
            bc4_values(src + 8, a);
            for (i = 0; i < 16; ++i) out[i].a = a[i];
        }
    } else if (kind == 5) {
        unsigned char r[16];
        bc4_values(src, r);
        for (i = 0; i < 16; ++i) {
            out[i].r = r[i];
            out[i].g = 0;
            out[i].b = 0;
            out[i].a = 255;
        }
    } else if (kind == 6) {
        unsigned char r[16], g[16];
        bc4_values(src, r);
        bc4_values(src + 8, g);
        for (i = 0; i < 16; ++i) {
            out[i].r = r[i];
            out[i].g = g[i];
            out[i].b = 0;
            out[i].a = 255;
        }
    } else {
        return -1;
    }

    for (i = 0; i < 16; ++i) {
        rgba[i * 4 + 0] = out[i].r;
        rgba[i * 4 + 1] = out[i].g;
        rgba[i * 4 + 2] = out[i].b;
        rgba[i * 4 + 3] = out[i].a;
    }
    return 0;
}

int nvngfx_bcn_decode_image(int format, const unsigned char* src,
                            size_t src_pitch, unsigned width, unsigned height,
                            unsigned char* rgba, size_t rgba_pitch) {
    const int kind = format_kind(format);
    const unsigned block_bytes = nvngfx_bcn_block_bytes(format);
    unsigned by, bx;
    if (!kind || !src || !rgba || width == 0 || height == 0) return -1;
    if (src_pitch < (size_t)((width + 3u) / 4u) * block_bytes) return -1;
    if (rgba_pitch < (size_t)width * 4u) return -1;

    for (by = 0; by < height; by += 4u) {
        const unsigned copy_h = height - by < 4u ? height - by : 4u;
        for (bx = 0; bx < width; bx += 4u) {
            const unsigned copy_w = width - bx < 4u ? width - bx : 4u;
            unsigned char block[64];
            unsigned y;
            if (nvngfx_bcn_decode(format, src + (size_t)(by / 4u) * src_pitch +
                                              (size_t)(bx / 4u) * block_bytes,
                                  block) != 0)
                return -1;
            for (y = 0; y < copy_h; ++y)
                memcpy(rgba + (size_t)(by + y) * rgba_pitch + (size_t)bx * 4u,
                       block + y * 16u, (size_t)copy_w * 4u);
        }
    }
    return 0;
}

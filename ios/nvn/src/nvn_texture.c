/* NVNformat -> MTLPixelFormat, ported from nxnative (MIT) as described in
 * nvn_texture.h.  Entry order follows the NVN enum so the table is diffable
 * against the source; unlisted values (0, > 0x9C, the 3-component families
 * Metal has no format for) map to 0. */
#include "nvn_texture.h"

#include <string.h>

enum {   /* MTLPixelFormat, as declared by Apple */
    R8Unorm = 10, R8Snorm = 12, R8Uint = 13, R8Sint = 14,
    R16Unorm = 20, R16Snorm = 22, R16Uint = 23, R16Sint = 24, R16Float = 25,
    RG8Unorm = 30, RG8Snorm = 32, RG8Uint = 33, RG8Sint = 34,
    B5G6R5Unorm = 40, A1BGR5Unorm = 41, ABGR4Unorm = 42, BGR5A1Unorm = 43,
    R32Uint = 53, R32Sint = 54, R32Float = 55,
    RG16Unorm = 60, RG16Snorm = 62, RG16Uint = 63, RG16Sint = 64, RG16Float = 65,
    RGBA8Unorm = 70, RGBA8Unorm_sRGB = 71, RGBA8Snorm = 72, RGBA8Uint = 73, RGBA8Sint = 74,
    BGRA8Unorm = 80, BGRA8Unorm_sRGB = 81,
    RGB10A2Unorm = 90, RGB10A2Uint = 91, RG11B10Float = 92, RGB9E5Float = 93,
    RG32Uint = 103, RG32Sint = 104, RG32Float = 105,
    RGBA16Unorm = 110, RGBA16Snorm = 112, RGBA16Uint = 113, RGBA16Sint = 114, RGBA16Float = 115,
    RGBA32Uint = 123, RGBA32Sint = 124, RGBA32Float = 125,
    BC1 = 130, BC1_sRGB = 131, BC2 = 132, BC2_sRGB = 133, BC3 = 134, BC3_sRGB = 135,
    BC4_U = 140, BC4_S = 141, BC5_U = 142, BC5_S = 143,
    BC6H_S = 150, BC6H_U = 151, BC7 = 152, BC7_sRGB = 153,
    ASTC_SRGB0 = 186, ASTC_LDR0 = 204,
    Depth16Unorm = 250, Depth32Float = 252, Stencil8 = 253, Depth32Float_Stencil8 = 260,
};

typedef struct {
    unsigned short mtl;
    unsigned char kind;    /* 0 none, 1 exact, 2 substitute */
    unsigned char noalpha; /* alpha reads as 1 */
    unsigned char rb;      /* red and blue are exchanged */
} ent;

#define E(m)  {m, 1, 0, 0}
#define X(m)  {m, 1, 1, 0}
#define S(m)  {m, 2, 0, 0}
#define SX(m) {m, 2, 1, 0}
#define N     {0, 0, 0, 0}
#define SR(m) {m, 2, 0, 1}
#define SXR(m){m, 2, 1, 1}

static const ent k_table[0x9D] = {
    [0x01] = E(R8Unorm), E(R8Snorm), E(R8Uint), E(R8Sint),
    [0x05] = E(R16Float), E(R16Unorm), E(R16Snorm), E(R16Uint), E(R16Sint),
    [0x0A] = E(R32Float), E(R32Uint), E(R32Sint),
    [0x0D] = E(RG8Unorm), E(RG8Snorm), E(RG8Uint), E(RG8Sint),
    [0x11] = E(RG16Float), E(RG16Unorm), E(RG16Snorm), E(RG16Uint), E(RG16Sint),
    [0x16] = E(RG32Float), E(RG32Uint), E(RG32Sint),
    /* 0x19-0x24: RGB8/16/32 have no 3-component Metal format -> none. */
    [0x19] = N, N, N, N, N, N, N, N, N, N, N, N,
    [0x25] = E(RGBA8Unorm), E(RGBA8Snorm), E(RGBA8Uint), E(RGBA8Sint),
    [0x29] = E(RGBA16Float), E(RGBA16Unorm), E(RGBA16Snorm), E(RGBA16Uint), E(RGBA16Sint),
    [0x2E] = E(RGBA32Float), E(RGBA32Uint), E(RGBA32Sint),
    [0x31] = E(Stencil8), E(Depth16Unorm),
    /* DEPTH24 -> Depth32Float: a precision upgrade, equivalent as a target. */
    [0x33] = S(Depth32Float), E(Depth32Float),
    /* DEPTH24_STENCIL8 -> Depth32Float_Stencil8, likewise a substitute. */
    [0x35] = S(Depth32Float_Stencil8), E(Depth32Float_Stencil8),
    [0x37] = X(RGBA8Unorm_sRGB), E(RGBA8Unorm_sRGB),
    /* 16-bit packed: matched by name, bit order unverified -> substitutes. */
    [0x39] = S(ABGR4Unorm), SXR(BGR5A1Unorm), SR(BGR5A1Unorm), SR(B5G6R5Unorm),
    [0x3D] = E(RGB10A2Unorm), E(RGB10A2Uint), E(RG11B10Float), E(RGB9E5Float),
    /* DXT / RGTC / BPTC.  RGB_DXT1 has no alpha. */
    [0x41] = X(BC1), E(BC1), E(BC2), E(BC3), X(BC1_sRGB), E(BC1_sRGB), E(BC2_sRGB), E(BC3_sRGB),
    [0x49] = E(BC4_U), E(BC4_S), E(BC5_U), E(BC5_S), E(BC7), E(BC7_sRGB), E(BC6H_S), E(BC6H_U),
    /* *_UI2F / *_I2F: integer storage read as float; Metal has no such read
     * mode, so the integer format is substituted and the shader converts. */
    [0x51] = S(R8Uint), S(R8Sint), S(R16Uint), S(R16Sint), S(R32Uint), S(R32Sint),
    [0x57] = S(RG8Uint), S(RG8Sint), S(RG16Uint), S(RG16Sint), S(RG32Uint), S(RG32Sint),
    [0x5D] = N, N, N, N, N, N,
    [0x63] = S(RGBA8Uint), S(RGBA8Sint), S(RGBA16Uint), S(RGBA16Sint), S(RGBA32Uint), S(RGBA32Sint),
    [0x69] = N, N, S(RGB10A2Uint), N,
    [0x6D] = X(RGBA8Unorm), X(RGBA8Snorm), X(RGBA8Uint), X(RGBA8Sint),
    [0x71] = X(RGBA16Float), X(RGBA16Unorm), X(RGBA16Snorm), X(RGBA16Uint), X(RGBA16Sint),
    [0x76] = X(RGBA32Float), X(RGBA32Uint), X(RGBA32Sint),
    /* 0x79-0x94 ASTC, computed below (LDR 0x79-0x86, sRGB 0x87-0x94). */
    [0x95] = S(B5G6R5Unorm), SX(BGR5A1Unorm), S(BGR5A1Unorm), S(A1BGR5Unorm),
    [0x99] = X(BGRA8Unorm), E(BGRA8Unorm), X(BGRA8Unorm_sRGB), E(BGRA8Unorm_sRGB),
};

/* NVN ASTC order is 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10
 * 12x10 12x12; Metal skips one value between 6x6 and 8x5. */
static const unsigned char k_astc_off[14] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14};

unsigned nvngfx_format_to_mtl(int f, int* lossy) {
    ent e;
    memset(&e, 0, sizeof e);
    if (f >= 0x79 && f <= 0x94) {
        const int k = (f - 0x79) % 14;
        e.mtl = (unsigned short)((f < 0x87 ? ASTC_LDR0 : ASTC_SRGB0) + k_astc_off[k]);
        e.kind = 1;
    } else if (f > 0 && f < (int)(sizeof k_table / sizeof *k_table)) {
        e = k_table[f];
    }
    if (lossy) *lossy = e.kind == 2;
    return e.kind ? e.mtl : 0u;
}

int nvngfx_format_table_size(void) { return (int)(sizeof k_table / sizeof *k_table); }

int nvngfx_format_component_class(int f) {
    int lossy = 0;
    const unsigned mtl = nvngfx_format_to_mtl(f, &lossy);
    switch (mtl) {
    case R8Uint: case R16Uint: case RG8Uint: case RG16Uint:
    case R32Uint: case RG32Uint: case RGBA8Uint: case RGB10A2Uint:
    case RGBA16Uint: case RGBA32Uint:
        return 1;
    case R8Sint: case R16Sint: case RG8Sint: case RG16Sint:
    case R32Sint: case RG32Sint: case RGBA8Sint: case RGBA16Sint:
    case RGBA32Sint:
        return 2;
    default:
        break;
    }
    if (!mtl) return -1;
    if (mtl >= Depth16Unorm) return -1;   /* depth / stencil, not colour */
    return 0;
}

void nvngfx_texdesc_defaults(nvngfx_texdesc* d) {
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->rank = 2;
    d->width = 1;
    d->height = 1;
    d->depth = 1;
    d->levels = 1;
    d->swz[0] = 2;  /* R */
    d->swz[1] = 3;  /* G */
    d->swz[2] = 4;  /* B */
    d->swz[3] = 5;  /* A */
}
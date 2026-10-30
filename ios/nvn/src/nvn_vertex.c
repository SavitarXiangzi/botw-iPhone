/* NVN vertex input: the guest structs and the NVNformat classification.
 * Pure C, no Apple headers -- see nvn_vertex.h for why. */
#include "nvn_vertex.h"

/* format -> (kind, components, bytes), for the 12 scalar formats that repeat
 * at every component count.  Order is NVN's own, which is also bit depth
 * order: 8-bit types, then 16, then 32. */
typedef struct {
    unsigned char kind;
    unsigned char bytes;
} scalar;

static const scalar kScalar[12] = {
    {NVNGFX_VF_U8N, 1}, {NVNGFX_VF_I8N, 1}, {NVNGFX_VF_U8, 1}, {NVNGFX_VF_I8, 1},
    {NVNGFX_VF_F16, 2}, {NVNGFX_VF_U16N, 2}, {NVNGFX_VF_I16N, 2},
    {NVNGFX_VF_U16, 2}, {NVNGFX_VF_I16, 2},
    {NVNGFX_VF_F32, 4}, {NVNGFX_VF_U32, 4}, {NVNGFX_VF_I32, 4},
};

typedef struct {
    unsigned char kind;     /* nvngfx_vfmt */
    unsigned char comps;
    unsigned char bytes;
} vin;

static vin mk(unsigned kind, unsigned comps, unsigned bytes) {
    vin v;
    v.kind = (unsigned char)kind;
    v.comps = (unsigned char)comps;
    v.bytes = (unsigned char)bytes;
    return v;
}

#define VNONE       mk(NVNGFX_VF_NONE, 0, 0)
#define VUNSUP      mk(NVNGFX_VF_UNSUPPORTED, 0, 0)

static vin scalar_at(unsigned idx, unsigned comps) {
    const scalar* s = &kScalar[idx % 12u];
    return mk(s->kind, comps, (unsigned)s->bytes * comps);
}

static vin classify(unsigned f) {
    /* 0x01..0x0C one component, 0x0D..0x18 two, 0x25..0x30 four: the same 12
     * formats, three times. */
    if (f - 0x01u < 0x0Cu) return scalar_at(f - 0x01u, 1);
    if (f - 0x0Du < 0x0Cu) return scalar_at(f - 0x0Du, 2);
    if (f - 0x25u < 0x0Cu) return scalar_at(f - 0x25u, 4);
    if (f - 0x6Du < 0x0Cu) return scalar_at(f - 0x6Du, 4);   /* RGBX = RGBA storage */

    /* Three components: 8- and 16-bit are packed (3 and 6 bytes) while
     * Metal's UChar3 / UShort3 take 4 and 8, so only the 32-bit ones map. */
    if (f - 0x19u < 0x04u) return VUNSUP;                     /* RGB8 */
    if (f - 0x1Du < 0x05u) return VUNSUP;                     /* RGB16 */
    if (f - 0x22u < 0x03u) return scalar_at(9u + (f - 0x22u), 3);   /* RGB32 */

    switch (f) {
    case 0x00: return VNONE;                                  /* NVN_FORMAT_NONE */
    case 0x3D: return mk(NVNGFX_VF_U10_10_10_2N, 4, 4);      /* RGB10A2 */
    case 0x69: return mk(NVNGFX_VF_I10_10_10_2N, 4, 4);      /* RGB10A2SN */
    case 0x3F: return mk(NVNGFX_VF_F11_11_10, 3, 4);         /* R11G11B10F */
    case 0x40: return mk(NVNGFX_VF_F9E5, 3, 4);              /* RGB9E5F */
    case 0x9A: return mk(NVNGFX_VF_BGRA8N, 4, 4);            /* BGRA8 */
    default: break;
    }
    /* Everything else that has a value is a texture-only format (depth,
     * stencil, sRGB, packed 16-bit colour, compressed, integer-read-as-float
     * _UI2F/_I2F, ...).  There is no Metal vertex format for them here. */
    return VUNSUP;
}

unsigned nvngfx_vattr_pack(nvngfx_vattr a) {
    return (a.format & 0xFFu) | ((a.offset & 0x7FFFFu) << 8) |
           ((a.stream & 0x1Fu) << 27);
}

nvngfx_vattr nvngfx_vattr_unpack(unsigned bits) {
    nvngfx_vattr a;
    a.format = bits & 0xFFu;
    a.offset = (bits >> 8) & 0x7FFFFu;
    a.stream = (bits >> 27) & 0x1Fu;
    return a;
}

void nvngfx_vstream_defaults(nvngfx_vstream* s) {
    if (!s) return;
    s->stride = 0;
    s->divisor = 0;
}

nvngfx_vfmt nvngfx_vertex_type(unsigned nvn_format) {
    return (nvngfx_vfmt)classify(nvn_format).kind;
}

int nvngfx_vertex_components(unsigned nvn_format) {
    return (int)classify(nvn_format).comps;
}

int nvngfx_vertex_bytes(unsigned nvn_format) {
    return (int)classify(nvn_format).bytes;
}

unsigned long long nvngfx_vertex_layout_id(const nvngfx_vattr* attrs, unsigned num_attrs,
                                           const nvngfx_vstream* streams, unsigned num_streams) {
    /* FNV-1a over the packed guest words: one byte order, no padding, and a
     * layout that differs anywhere gives a different id. */
    unsigned long long h = 1469598103934665603ull;
    unsigned i;
    for (i = 0; i < num_attrs; ++i) {
        const unsigned w = nvngfx_vattr_pack(attrs[i]);
        int b;
        for (b = 0; b < 4; ++b) {
            h ^= (unsigned long long)((w >> (8 * b)) & 0xFFu);
            h *= 1099511628211ull;
        }
    }
    for (i = 0; i < num_streams; ++i) {
        h ^= (unsigned long long)streams[i].stride;
        h *= 1099511628211ull;
        h ^= (unsigned long long)streams[i].divisor;
        h *= 1099511628211ull;
    }
    h ^= ((unsigned long long)num_attrs << 8) | (unsigned long long)num_streams;
    h *= 1099511628211ull;
    return h;
}
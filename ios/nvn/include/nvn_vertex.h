/* Vertex input state: NVN's attrib / stream structs and the format mapping.
 *
 * Why this is separate from nvn_metal.mm: the decision "which NVNformat is
 * which kind of attribute" is pure data, so it is compiled and tested on
 * Windows/Linux.  Only turning that kind into an MTLVertexFormat needs Metal.
 *
 * The guest structs
 * -----------------
 * Both are small enough that the guest usually keeps them on its stack and
 * memcpys them around, so this layer reads and writes the guest's own bytes
 * rather than keeping a side table keyed by address (a stale key would be a
 * silent misread).  The layouts are the ones nxnative asserts at compile time
 * (dm/gfx/vn/nvn_obj.h, FITS(va_st, NVNvertexAttribState)):
 *
 *   NVNvertexAttribState: uint32 { format:8, offset:19, stream:5 }   4 bytes
 *   NVNvertexStreamState: uint32 stride, uint32 divisor              8 bytes
 *
 * The 19-bit offset means an attribute offset of at most 0x7FFFF, which is
 * what the hardware supports; nxnative logs an error past that.
 *
 * The format table
 * ----------------
 * NVNformat is one enum for textures and vertex attributes.  The values below
 * are the ones in nxnative's reconstructed nvn.h (NVN_FORMAT_R8 = 0x01 ...
 * NVN_FORMAT_BGRA8 = 0x9A), which is also what ios/nvn/src/nvn_texture.c
 * indexes with.  Only the formats with a real Metal vertex counterpart are
 * accepted: a 3-component 8- or 16-bit format is *not*, because Metal's
 * UChar3 / UShort3 occupy four elements' worth of space (4 and 8 bytes) while
 * NVN's RGB8 / RGB16 are packed into 3 and 6.  Reading those through Metal
 * would silently shift every vertex.
 */
#ifndef NVN_VERTEX_H
#define NVN_VERTEX_H

#ifdef __cplusplus
extern "C" {
#endif

#define NVNGFX_MAX_VERTEX_ATTRIBS 16
#define NVNGFX_MAX_VERTEX_STREAMS 16

/* What an NVNformat means as a vertex attribute, without naming Metal. */
typedef enum {
    NVNGFX_VF_NONE = 0,      /* NVN_FORMAT_NONE: the attribute is not used */
    NVNGFX_VF_UNSUPPORTED,   /* a real NVNformat Metal cannot fetch */
    NVNGFX_VF_U8,  NVNGFX_VF_U8N,  NVNGFX_VF_I8,  NVNGFX_VF_I8N,
    NVNGFX_VF_U16, NVNGFX_VF_U16N, NVNGFX_VF_I16, NVNGFX_VF_I16N,
    NVNGFX_VF_F16,
    NVNGFX_VF_U32, NVNGFX_VF_I32,  NVNGFX_VF_F32,
    NVNGFX_VF_U10_10_10_2N,  /* RGB10A2 */
    NVNGFX_VF_I10_10_10_2N,  /* RGB10A2SN */
    NVNGFX_VF_F11_11_10,     /* R11G11B10F */
    NVNGFX_VF_F9E5,          /* RGB9E5F */
    NVNGFX_VF_BGRA8N         /* BGRA8: channels swapped, not just 4x u8 */
} nvngfx_vfmt;

/* One attribute, decoded out of its 4 guest bytes. */
typedef struct {
    unsigned format;   /* raw NVNformat */
    unsigned offset;   /* byte offset inside the element */
    unsigned stream;   /* which vertex stream supplies it */
} nvngfx_vattr;

typedef struct {
    unsigned stride;
    unsigned divisor;
} nvngfx_vstream;

/* Packs/unpacks the guest's 4-byte NVNvertexAttribState.  Out-of-range
 * fields are masked, not clamped: a stream index of 40 is a bug in the
 * caller, but it must not corrupt the offset. */
unsigned nvngfx_vattr_pack(nvngfx_vattr a);
nvngfx_vattr nvngfx_vattr_unpack(unsigned bits);

/* nvnVertexStreamStateSetDefaults: stride 0, divisor 0. */
void nvngfx_vstream_defaults(nvngfx_vstream* s);

/* Classification of a raw NVNformat.  `components` is 1..4 (0 when the
 * format is not a vertex format), `bytes` is the storage size of one element
 * (0 when unknown). */
nvngfx_vfmt nvngfx_vertex_type(unsigned nvn_format);
int nvngfx_vertex_components(unsigned nvn_format);
int nvngfx_vertex_bytes(unsigned nvn_format);

/* A stable id over the bound attribute/stream state, used to cache the
 * Metal pipeline (a vertex descriptor is baked into it).  Same layout in,
 * same id out; any change that affects fetching changes the id. */
unsigned long long nvngfx_vertex_layout_id(const nvngfx_vattr* attrs, unsigned num_attrs,
                                           const nvngfx_vstream* streams, unsigned num_streams);

#ifdef __cplusplus
}
#endif
#endif /* NVN_VERTEX_H */
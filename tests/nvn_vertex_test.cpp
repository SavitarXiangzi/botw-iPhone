/* nvn_vertex: the guest's attrib/stream bytes and the NVNformat table.
 *
 * Run by scripts/build_abi_test.ps1 and by CI's portable job.  No Metal and
 * no game data; everything here is a property of the packing rules. */
#include "nvn_vertex.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static void test_packing() {
    /* Round trip at the extremes the 8/19/5 split allows. */
    const nvngfx_vattr a = {0xFFu, 0x7FFFFu, 31u};
    const unsigned bits = nvngfx_vattr_pack(a);
    const nvngfx_vattr b = nvngfx_vattr_unpack(bits);
    CHECK(b.format == a.format);
    CHECK(b.offset == a.offset);
    CHECK(b.stream == a.stream);

    /* Out-of-range fields are masked: an offset of 0x80000 must not spill
     * into the stream index. */
    const nvngfx_vattr wide = {0x2Eu, 0x80001u, 2u};
    const nvngfx_vattr w = nvngfx_vattr_unpack(nvngfx_vattr_pack(wide));
    CHECK(w.format == 0x2Eu);
    CHECK(w.offset == 1u);
    CHECK(w.stream == 2u);

    /* The layout nxnative asserts: format occupies the low 8 bits. */
    const nvngfx_vattr one = {0x2Eu, 0, 0};
    CHECK(nvngfx_vattr_pack(one) == 0x2Eu);
    const nvngfx_vattr stream1 = {0, 16u, 1u};
    CHECK(nvngfx_vattr_pack(stream1) == ((16u << 8) | (1u << 27)));

    nvngfx_vstream s;
    s.stride = 32; s.divisor = 0;
    nvngfx_vstream_defaults(&s);
    CHECK(s.stride == 0 && s.divisor == 0);
    nvngfx_vstream_defaults(nullptr);   /* must not crash */
}

static int test_formats() {
    const struct {
        unsigned fmt;
        nvngfx_vfmt kind;
        int comps, bytes;
    } cases[] = {
        {0x00, NVNGFX_VF_NONE, 0, 0},              /* NVN_FORMAT_NONE */
        {0x01, NVNGFX_VF_U8N, 1, 1},               /* R8 */
        {0x02, NVNGFX_VF_I8N, 1, 1},               /* R8SN */
        {0x03, NVNGFX_VF_U8, 1, 1},                /* R8UI */
        {0x05, NVNGFX_VF_F16, 1, 2},               /* R16F */
        {0x06, NVNGFX_VF_U16N, 1, 2},              /* R16 */
        {0x0A, NVNGFX_VF_F32, 1, 4},               /* R32F */
        {0x0D, NVNGFX_VF_U8N, 2, 2},               /* RG8 */
        {0x11, NVNGFX_VF_F16, 2, 4},               /* RG16F */
        {0x16, NVNGFX_VF_F32, 2, 8},               /* RG32F */
        {0x19, NVNGFX_VF_UNSUPPORTED, 0, 0},       /* RGB8: 3 bytes, Metal UChar3 is 4 */
        {0x22, NVNGFX_VF_F32, 3, 12},              /* RGB32F: exactly Float3 */
        {0x25, NVNGFX_VF_U8N, 4, 4},               /* RGBA8 */
        {0x29, NVNGFX_VF_F16, 4, 8},               /* RGBA16F */
        {0x2E, NVNGFX_VF_F32, 4, 16},              /* RGBA32F */
        {0x31, NVNGFX_VF_UNSUPPORTED, 0, 0},       /* STENCIL8: not a vertex format */
        {0x3D, NVNGFX_VF_U10_10_10_2N, 4, 4},      /* RGB10A2 */
        {0x3F, NVNGFX_VF_F11_11_10, 3, 4},         /* R11G11B10F */
        {0x40, NVNGFX_VF_F9E5, 3, 4},              /* RGB9E5F */
        {0x51, NVNGFX_VF_UNSUPPORTED, 0, 0},       /* R8_UI2F: integer read as float */
        {0x69, NVNGFX_VF_I10_10_10_2N, 4, 4},      /* RGB10A2SN */
        {0x6D, NVNGFX_VF_U8N, 4, 4},               /* RGBX8 shares RGBA storage */
        {0x76, NVNGFX_VF_F32, 4, 16},              /* RGBX32F */
        {0x9A, NVNGFX_VF_BGRA8N, 4, 4},            /* BGRA8 */
        {0x9C, NVNGFX_VF_UNSUPPORTED, 0, 0},       /* BGRA8_SRGB */
        {0x100, NVNGFX_VF_UNSUPPORTED, 0, 0},      /* past the enum */
    };
    for (const auto& c : cases) {
        const nvngfx_vfmt k = nvngfx_vertex_type(c.fmt);
        const int comps = nvngfx_vertex_components(c.fmt);
        const int bytes = nvngfx_vertex_bytes(c.fmt);
        if (k != c.kind || comps != c.comps || bytes != c.bytes) {
            std::printf("FAIL format %#x: kind %d comps %d bytes %d, want %d/%d/%d\n",
                        c.fmt, (int)k, comps, bytes, (int)c.kind, c.comps, c.bytes);
            ++g_failures;
        }
    }
    return (int)(sizeof cases / sizeof cases[0]);
}

static void test_layout_id() {
    nvngfx_vattr attrs[2] = {{0x2Eu, 0, 0}, {0x25u, 12, 0}};
    nvngfx_vstream streams[2] = {{0, 0}, {16, 0}};
    const unsigned long long a = nvngfx_vertex_layout_id(attrs, 2, streams, 1);
    CHECK(nvngfx_vertex_layout_id(attrs, 2, streams, 1) == a);

    /* Any change that affects fetching must change the id... */
    streams[0].stride = 16;
    const unsigned long long b = nvngfx_vertex_layout_id(attrs, 2, streams, 1);
    CHECK(b != a);

    /* ...and unused stream slots must not. */
    nvngfx_vstream wide[2] = {{16, 0}, {99, 7}};
    CHECK(nvngfx_vertex_layout_id(attrs, 2, wide, 1) == b);

    /* An attribute used by the shader but not bound is a different layout. */
    CHECK(nvngfx_vertex_layout_id(attrs, 1, streams, 1) != b);

    attrs[1].offset = 8;
    CHECK(nvngfx_vertex_layout_id(attrs, 2, streams, 1) != b);

    /* A stream that becomes instanced must rebuild the pipeline. */
    nvngfx_vstream inst[2] = {{16, 1}, {0, 0}};
    CHECK(nvngfx_vertex_layout_id(attrs, 2, inst, 1) != nvngfx_vertex_layout_id(attrs, 2, streams, 1));
}

int main() {
    test_packing();
    const int formats = test_formats();
    test_layout_id();
    if (g_failures) {
        std::printf("nvn_vertex_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvn_vertex_test: ok (packing, %d formats, layout id)\n", formats);
    return 0;
}
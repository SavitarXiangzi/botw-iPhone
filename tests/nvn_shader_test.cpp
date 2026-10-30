/* Portable test for the shader seam.
 *
 * It cannot test a translator that does not exist.  What it can do is pin the
 * behaviour everything else relies on: the seam refuses guest SASS with a
 * reason, passthrough is opt-in, a non-terminated buffer is refused, and the
 * built-in program defines exactly the names the Metal backend looks up,
 * and that the offline MSL blob table can be installed and queried safely. */
#include "nvn_shader.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

int main() {
    const char* why = nullptr;

    /* Off by default, and the default answer is "no translator". */
    CHECK(nvngfx_shader_msl_passthrough() == 0);
    CHECK(nvngfx_shader_to_msl("abcd", 5, NVNGFX_STAGE_VERTEX, &why) == nullptr);
    CHECK(why != nullptr && std::strstr(why, "no translator") != nullptr);

    /* A null code address is its own reason, not "no translator". */
    why = nullptr;
    CHECK(nvngfx_shader_to_msl(nullptr, 0, NVNGFX_STAGE_VERTEX, &why) == nullptr);
    CHECK(why != nullptr && std::strstr(why, "null") != nullptr);

    static const char src[] = "// not really metal";
    nvngfx_shader_set_msl_passthrough(1);
    CHECK(nvngfx_shader_msl_passthrough() == 1);

    /* Passthrough returns the source pointer unchanged and says so. */
    why = nullptr;
    CHECK(nvngfx_shader_to_msl(src, sizeof src, NVNGFX_STAGE_FRAGMENT, &why) == src);
    CHECK(why != nullptr && std::strcmp(why, "passthrough") == 0);

    /* No NUL inside the declared size: refused, because Metal needs a C string. */
    why = nullptr;
    CHECK(nvngfx_shader_to_msl("abcd", 4, NVNGFX_STAGE_VERTEX, &why) == nullptr);
    CHECK(why != nullptr && std::strstr(why, "NUL") != nullptr);

    /* size == 0 means "the caller guarantees a C string". */
    CHECK(nvngfx_shader_to_msl(src, 0, NVNGFX_STAGE_VERTEX, &why) == src);

    nvngfx_shader_set_msl_passthrough(0);
    CHECK(nvngfx_shader_to_msl(src, 0, NVNGFX_STAGE_VERTEX, &why) == nullptr);

    /* The built-in program must define the two entry points the backend looks
     * up by name; a rename here would only show up as a nil function on a
     * device, which is exactly the failure this test exists to prevent. */
    const char* vs = nvngfx_shader_selftest_msl(NVNGFX_STAGE_VERTEX);
    const char* fs = nvngfx_shader_selftest_msl(NVNGFX_STAGE_FRAGMENT);
    CHECK(vs != nullptr && std::strstr(vs, "nvngfx_main_vs") != nullptr);
    CHECK(vs != nullptr && std::strstr(vs, "[[vertex_id]]") != nullptr);
    CHECK(fs != nullptr && std::strstr(fs, "nvngfx_main_fs") != nullptr);
    CHECK(nvngfx_shader_selftest_msl(NVNGFX_STAGE_COMPUTE) == nullptr);

    /* The layered-texture self-test must declare a 2D array, not a plain
     * texture2d: a plain declaration would still compile and bind, but it
     * cannot prove that destinationSlice:1 reached the GPU. */
    const char* fs_array = nvngfx_shader_selftest_msl_variant(
        NVNGFX_STAGE_FRAGMENT, 9);
    CHECK(fs_array != nullptr &&
          std::strstr(fs_array, "texture2d_array<float>") != nullptr);
    CHECK(fs_array != nullptr && std::strstr(fs_array, "in.uv, 0") != nullptr);
    CHECK(fs_array != nullptr && std::strstr(fs_array, "in.uv, 1") != nullptr);
    CHECK(nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 9) ==
          nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_VERTEX, 4));
    CHECK(nvngfx_shader_selftest_msl_variant(NVNGFX_STAGE_COMPUTE, 9) ==
          nullptr);


    /* Offline MSL blobs: what the Windows-side chain produces is installed by
     * key, looked up case-insensitively, and a bad table is rejected whole
     * without disturbing the one already installed. */
    CHECK(nvngfx_shader_blob_count() == 0);
    why = nullptr;
    CHECK(nvngfx_shader_lookup("0123456789abcdef", NVNGFX_STAGE_VERTEX, &why) == nullptr);
    CHECK(why != nullptr && std::strstr(why, "no offline MSL blob table") != nullptr);

    static const nvngfx_shader_blob blobs[] = {
        {"0123456789ABCDEF", NVNGFX_STAGE_VERTEX, "vertex void nvngfx_main_vs() {}", nullptr, 0, nullptr, 0},
        {"fedcba9876543210", NVNGFX_STAGE_FRAGMENT,
         "fragment void nvngfx_main_fs() {}", nullptr, 0, nullptr, 0},
    };
    CHECK(nvngfx_shader_set_blobs(blobs, 2) == 0);
    CHECK(nvngfx_shader_blob_count() == 2);
    why = nullptr;
    CHECK(nvngfx_shader_lookup("0123456789abcdef", NVNGFX_STAGE_VERTEX, &why) == blobs[0].msl);
    CHECK(why != nullptr && std::strcmp(why, "offline MSL blob") == 0);

    /* The stage is part of the identity: the same key at another stage misses. */
    why = nullptr;
    CHECK(nvngfx_shader_lookup("0123456789abcdef", NVNGFX_STAGE_FRAGMENT, &why) == nullptr);
    CHECK(why != nullptr && std::strstr(why, "no offline MSL blob for this key") != nullptr);

    why = nullptr;
    CHECK(nvngfx_shader_lookup("0123", NVNGFX_STAGE_VERTEX, &why) == nullptr);
    CHECK(why != nullptr && std::strstr(why, "16 hex") != nullptr);

    /* A duplicate key+stage is rejected, and the installed table survives. */
    static const nvngfx_shader_blob dup[] = {
        {"0123456789abcdef", NVNGFX_STAGE_VERTEX, "x", nullptr, 0, nullptr, 0},
        {"0123456789ABCDEF", NVNGFX_STAGE_VERTEX, "y", nullptr, 0, nullptr, 0},
    };
    CHECK(nvngfx_shader_set_blobs(dup, 2) == -2);
    CHECK(nvngfx_shader_blob_count() == 2);
    why = nullptr;
    CHECK(nvngfx_shader_lookup("fedcba9876543210", NVNGFX_STAGE_FRAGMENT, &why) == blobs[1].msl);

    /* Compute blobs are refused rather than silently never matched. */
    static const nvngfx_shader_blob cs[] = {
        {"0123456789abcdef", NVNGFX_STAGE_COMPUTE, "z", nullptr, 0, nullptr, 0},
    };
    CHECK(nvngfx_shader_set_blobs(cs, 1) == -1);
    CHECK(nvngfx_shader_set_blobs(nullptr, 1) == -1);

    /* The argument layout sidecar.  A real fragment shader names 42 entries,
     * so the interesting cases are holes in the id range, a packed slot that
     * differs from the id, bindless handles in a UBO, and a table that has to
     * be refused whole rather than half-used.  C1 is the embedded constant
     * segment: it has no NVN binding slot, but its bytes must be present (an
     * explicit empty segment is valid). */
    {
        static const unsigned char c1[] = {
            0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x40,
            0x00, 0x00, 0x40, 0x3f, 0x00, 0x00, 0x80, 0x3f,
        };
        static const nvngfx_shader_arg layout[] = {
            {5, NVNGFX_ARG_UBO, 0, 0, 0},
            {9, NVNGFX_ARG_TEXTURE, 0, 1, 0},
            {12, NVNGFX_ARG_SAMPLER_OF_TEXTURE, 0, 2, 0},
            {13, NVNGFX_ARG_C1, -1, 3, 0},
            {14, NVNGFX_ARG_UNSUPPORTED, -1, 4, 0},
        };
        static const nvngfx_shader_blob with_args[] = {
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", layout, 5,
             c1, sizeof c1},
        };
        CHECK(nvngfx_shader_set_blobs(with_args, 1) == 0);
        CHECK(nvngfx_shader_blob_count() == 1);

        const nvngfx_shader_blob* b =
            nvngfx_shader_lookup_blob("0011223344556677", NVNGFX_STAGE_FRAGMENT, &why);
        CHECK(b == &with_args[0]);
        CHECK(b != nullptr && b->num_args == 5);
        CHECK(b != nullptr && b->args[2].kind == NVNGFX_ARG_SAMPLER_OF_TEXTURE);
        CHECK(b != nullptr && b->c1 == c1 && b->c1_size == sizeof c1);
        CHECK(nvngfx_shader_lookup("0011223344556677", NVNGFX_STAGE_FRAGMENT, &why) ==
              with_args[0].msl);

        /* A blob's layout is copied verbatim, holes and packed positions
         * included.  The backend writes byte 8*packed, not 8*id. */
        nvngfx_shader_arg copied[NVNGFX_MAX_ARGS] = {};
        CHECK(nvngfx_shader_copy_args(b, copied, NVNGFX_MAX_ARGS) == 5);
        CHECK(copied[0].id == 5 && copied[0].slot == 0 && copied[0].packed == 0);
        CHECK(copied[1].id == 9 && copied[1].kind == NVNGFX_ARG_TEXTURE &&
              copied[1].packed == 1);
        CHECK(copied[2].id == 12 && copied[2].packed == 2);
        CHECK(copied[3].kind == NVNGFX_ARG_C1 && copied[3].slot == -1 &&
              copied[3].packed == 3);
        CHECK(copied[4].kind == NVNGFX_ARG_UNSUPPORTED && copied[4].slot == -1 &&
              copied[4].packed == 4);
        /* A destination that cannot hold the whole layout gets nothing. */
        CHECK(nvngfx_shader_copy_args(b, copied, 4) == 0);
        CHECK(nvngfx_shader_copy_args(nullptr, copied, NVNGFX_MAX_ARGS) == 0);

        /* Bindless: one UBO handle at byte 0xA8, with both resource halves,
         * plus a type-1 sampler word in another UBO slot.  The generator's
         * decoder is tested in test_gen_blobs.py; this pins the ABI fields. */
        static const nvngfx_shader_arg bindless_layout[] = {
            {3, NVNGFX_ARG_TEXTURE_IN_UBO, 4, 0, 0xA8},
            {4, NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE, 4, 1, 0xA8},
            {5, NVNGFX_ARG_SAMPLER_IN_UBO, 5, 2, 0x20},
        };
        static const nvngfx_shader_blob bindless_blob[] = {
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", bindless_layout, 3,
             nullptr, 0},
        };
        CHECK(nvngfx_shader_set_blobs(bindless_blob, 1) == 0);
        b = nvngfx_shader_lookup_blob("0011223344556677",
                                      NVNGFX_STAGE_FRAGMENT, &why);
        CHECK(b != nullptr && b->args[0].packed == 0 &&
              b->args[0].offset == 0xA8 &&
              b->args[1].kind == NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE);
        CHECK(b != nullptr && b->args[2].kind == NVNGFX_ARG_SAMPLER_IN_UBO &&
              b->args[2].slot == 5 && b->args[2].offset == 0x20);
        CHECK(nvngfx_shader_set_blobs(with_args, 1) == 0);

        /* packed is a permutation independent of id order: sorted-by-id rows
         * may carry reversed packed values, and are still valid. */
        static const nvngfx_shader_arg reordered[] = {
            {5, NVNGFX_ARG_UBO, 0, 1, 0},
            {9, NVNGFX_ARG_TEXTURE, 0, 0, 0},
        };
        static const nvngfx_shader_blob reordered_blob[] = {
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", reordered, 2,
             nullptr, 0},
        };
        CHECK(nvngfx_shader_set_blobs(reordered_blob, 1) == 0);
        b = nvngfx_shader_lookup_blob("0011223344556677",
                                      NVNGFX_STAGE_FRAGMENT, &why);
        CHECK(b != nullptr && b->args[0].id == 5 && b->args[0].packed == 1);
        CHECK(b != nullptr && b->args[1].id == 9 && b->args[1].packed == 0);
        CHECK(nvngfx_shader_set_blobs(with_args, 1) == 0);

        /* A zero-length c1 is explicit data, not a missing sidecar. */
        static const nvngfx_shader_blob empty_c1[] = {
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", layout, 5,
             c1, 0},
        };
        CHECK(nvngfx_shader_set_blobs(empty_c1, 1) == 0);
        b = nvngfx_shader_lookup_blob("0011223344556677", NVNGFX_STAGE_FRAGMENT, &why);
        CHECK(b != nullptr && b->c1 == c1 && b->c1_size == 0);
        CHECK(nvngfx_shader_set_blobs(with_args, 1) == 0);

        /* Every way a layout can be wrong; the installed table must survive. */
        static const nvngfx_shader_arg descending[] = {
            {9, NVNGFX_ARG_TEXTURE, 0, 0, 0},
            {5, NVNGFX_ARG_UBO, 0, 1, 0},
        };
        static const nvngfx_shader_arg duplicate_id[] = {
            {5, NVNGFX_ARG_UBO, 0, 0, 0},
            {5, NVNGFX_ARG_TEXTURE, 0, 1, 0},
        };
        static const nvngfx_shader_arg duplicate_packed[] = {
            {5, NVNGFX_ARG_UBO, 0, 0, 0},
            {6, NVNGFX_ARG_TEXTURE, 0, 0, 0},
        };
        static const nvngfx_shader_arg packed_out_of_range[] = {
            {5, NVNGFX_ARG_UBO, 0, 1, 0},
        };
        static const nvngfx_shader_arg texture_slot_too_high[] = {
            {7, NVNGFX_ARG_TEXTURE, NVNGFX_MAX_TEXTURE_SLOTS, 0, 0},
        };
        static const nvngfx_shader_arg id_too_high[] = {
            {NVNGFX_MAX_ARG_ID + 1, NVNGFX_ARG_UBO, 0, 0, 0},
        };
        static const nvngfx_shader_arg bad_kind[] = {
            {1, 0, 0, 0, 0},
        };
        static const nvngfx_shader_arg unsupported_with_slot[] = {
            {1, NVNGFX_ARG_UNSUPPORTED, 0, 0, 0},
        };
        static const nvngfx_shader_arg unsupported_with_offset[] = {
            {1, NVNGFX_ARG_UNSUPPORTED, -1, 0, 4},
        };
        static const nvngfx_shader_arg c1_with_slot[] = {
            {1, NVNGFX_ARG_C1, 0, 0, 0},
        };
        static const nvngfx_shader_arg c1_with_offset[] = {
            {1, NVNGFX_ARG_C1, -1, 0, 4},
        };
        static const nvngfx_shader_arg c1_without_bytes[] = {
            {1, NVNGFX_ARG_C1, -1, 0, 0},
        };
        static const nvngfx_shader_arg missing_slot[] = {
            {1, NVNGFX_ARG_UBO, -1, 0, 0},
        };
        static const nvngfx_shader_arg ubo_with_offset[] = {
            {1, NVNGFX_ARG_UBO, 0, 0, 4},
        };
        static const nvngfx_shader_arg bindless_slot_too_high[] = {
            {1, NVNGFX_ARG_TEXTURE_IN_UBO, NVNGFX_MAX_UBO_SLOTS, 0, 0},
        };
        static const nvngfx_shader_arg bindless_unaligned[] = {
            {1, NVNGFX_ARG_SAMPLER_OF_UBO_TEXTURE, 0, 0, 2},
        };
        static const nvngfx_shader_arg too_many[NVNGFX_MAX_ARGS + 1] = {};
        static const nvngfx_shader_blob bad[] = {
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", descending, 2, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", duplicate_id, 2, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", duplicate_packed, 2, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", packed_out_of_range, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", texture_slot_too_high, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", id_too_high, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", bad_kind, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", unsupported_with_slot, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", unsupported_with_offset, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", c1_with_slot, 1, c1, sizeof c1},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", c1_with_offset, 1, c1, sizeof c1},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", c1_without_bytes, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", c1_without_bytes, 1, c1,
             NVNGFX_MAX_C1_BYTES + 1},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", c1_without_bytes, 1, nullptr, 1},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", missing_slot, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", ubo_with_offset, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", bindless_slot_too_high, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", bindless_unaligned, 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", too_many,
             NVNGFX_MAX_ARGS + 1, nullptr, 0},
            {"0011223344556677", NVNGFX_STAGE_FRAGMENT, "x", layout, 0, nullptr, 0},
        };
        for (std::size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i)
            CHECK(nvngfx_shader_set_blobs(&bad[i], 1) == -1);
        CHECK(nvngfx_shader_blob_count() == 1);
        CHECK(nvngfx_shader_set_blobs(nullptr, 0) == 0);
        CHECK(nvngfx_shader_blob_count() == 0);
    }

    nvngfx_shader_clear_blobs();
    CHECK(nvngfx_shader_blob_count() == 0);


    /* SHA-256 and the shader key ---------------------------------------- */
    {
        unsigned char d[32];
        char hex[65];
        static const char kEmpty[] =
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        static const char kAbc[] =
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        nvngfx_sha256("", 0, d);
        for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
        CHECK(std::strcmp(hex, kEmpty) == 0);
        nvngfx_sha256("abc", 3, d);
        for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
        CHECK(std::strcmp(hex, kAbc) == 0);
    }

    /* The key rule, against values computed independently in Python:
     *   key = sha256(control[0:nctl] || code[0:ncode])[0..8]
     *   nctl = 0x760 <= u32@0x20 < 0x880 ? u32@0x20 : 0x880
     * ncode = u32@0x704.  A change here silently orphans every offline blob. */
    {
        static unsigned char ctl[0x880];
        static unsigned char code[0x40];
        char key[17];
        for (unsigned i = 0; i < sizeof ctl; ++i) ctl[i] = static_cast<unsigned char>(i & 0xFF);
        for (unsigned i = 0; i < sizeof code; ++i) code[i] = static_cast<unsigned char>(0xA0 + i);
        auto put32 = [](unsigned char* p, unsigned at, unsigned v) {
            p[at + 0] = static_cast<unsigned char>(v);
            p[at + 1] = static_cast<unsigned char>(v >> 8);
            p[at + 2] = static_cast<unsigned char>(v >> 16);
            p[at + 3] = static_cast<unsigned char>(v >> 24);
        };
        put32(ctl, 0x20, 0x880);
        put32(ctl, 0x704, 0x40);
        CHECK(nvngfx_shader_key_from_guest(ctl, code, key) == 0);
        CHECK(std::strcmp(key, "2c8239fb360e36c1") == 0);

        /* A control section shorter than 0x880 hashes only its own length. */
        put32(ctl, 0x20, 0x800);
        CHECK(nvngfx_shader_key_from_guest(ctl, code, key) == 0);
        CHECK(std::strcmp(key, "a4a67a417698a5e1") == 0);

        /* No code length in the control section: no key, and no guessing. */
        put32(ctl, 0x704, 0);
        CHECK(nvngfx_shader_key_from_guest(ctl, code, key) == -1);
        CHECK(nvngfx_shader_key_from_guest(nullptr, code, key) == -1);
        CHECK(nvngfx_shader_key_from_guest(ctl, nullptr, key) == -1);
    }

    if (g_failures) {
        std::printf("nvn_shader_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvn_shader_test: ok (seam, blob table, packed slots, "
                "bindless, c1, SHA-256, shader key, layered fixture)\n");
    return 0;
}

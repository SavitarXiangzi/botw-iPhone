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


    /* Offline MSL blobs: what the Windows-side chain produces is installed by
     * key, looked up case-insensitively, and a bad table is rejected whole
     * without disturbing the one already installed. */
    CHECK(nvngfx_shader_blob_count() == 0);
    why = nullptr;
    CHECK(nvngfx_shader_lookup("0123456789abcdef", NVNGFX_STAGE_VERTEX, &why) == nullptr);
    CHECK(why != nullptr && std::strstr(why, "no offline MSL blob table") != nullptr);

    static const nvngfx_shader_blob blobs[] = {
        {"0123456789ABCDEF", NVNGFX_STAGE_VERTEX, "vertex void nvngfx_main_vs() {}"},
        {"fedcba9876543210", NVNGFX_STAGE_FRAGMENT, "fragment void nvngfx_main_fs() {}"},
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
        {"0123456789abcdef", NVNGFX_STAGE_VERTEX, "x"},
        {"0123456789ABCDEF", NVNGFX_STAGE_VERTEX, "y"},
    };
    CHECK(nvngfx_shader_set_blobs(dup, 2) == -2);
    CHECK(nvngfx_shader_blob_count() == 2);
    why = nullptr;
    CHECK(nvngfx_shader_lookup("fedcba9876543210", NVNGFX_STAGE_FRAGMENT, &why) == blobs[1].msl);

    /* Compute blobs are refused rather than silently never matched. */
    static const nvngfx_shader_blob cs[] = {
        {"0123456789abcdef", NVNGFX_STAGE_COMPUTE, "z"},
    };
    CHECK(nvngfx_shader_set_blobs(cs, 1) == -1);
    CHECK(nvngfx_shader_set_blobs(nullptr, 1) == -1);

    nvngfx_shader_clear_blobs();
    CHECK(nvngfx_shader_blob_count() == 0);

    if (g_failures) {
        std::printf("nvn_shader_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvn_shader_test: ok (seam refuses SASS, passthrough opt-in, blobs by key)\n");
    return 0;
}
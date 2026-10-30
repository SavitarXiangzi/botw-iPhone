/* Portable test for the shader seam.
 *
 * It cannot test a translator that does not exist.  What it can do is pin the
 * behaviour everything else relies on: the seam refuses guest SASS with a
 * reason, passthrough is opt-in, a non-terminated buffer is refused, and the
 * built-in program defines exactly the names the Metal backend looks up. */
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

    if (g_failures) {
        std::printf("nvn_shader_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvn_shader_test: ok (seam refuses SASS, passthrough opt-in)\n");
    return 0;
}
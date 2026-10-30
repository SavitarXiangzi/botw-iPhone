/*
 * Smoke test for the C ABI the iOS shell actually calls.
 *
 * It runs on the null backend, so it verifies the *boundary*, not the GPU:
 * that create/destroy are balanced, that the NVN registry is installed and
 * counted, and that the frame pump advances.  On device the same calls go
 * through the Metal backend instead; if this test passes and the device HUD
 * shows a non-zero implementation count, the wiring is right.
 */
#include "botw_core.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

int main() {
    botw_core_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.width = 1280;
    desc.height = 720;

    botw_core* core = botw_core_create(&desc);
    CHECK(core != nullptr);
    if (!core) {
        std::printf("core_smoke_test: create failed\n");
        return 1;
    }

    botw_core_nvn_stats s;
    std::memset(&s, 0, sizeof s);
    botw_core_nvn_stats_get(core, &s);
    CHECK(s.total > 400);
    CHECK(s.implemented == 0);          /* null backend implements nothing */
    CHECK(s.backend != nullptr && std::strcmp(s.backend, "null") == 0);
    /* The null backend has no swapchain, so nothing may ever be presented. */
    CHECK(s.presents == 0);

    CHECK(botw_core_nvn_has(core, "nvnCommandBufferDrawArrays") == 0);
    CHECK(botw_core_nvn_has(core, "nvnDefinitelyNotARealProc") == 0);

    CHECK(botw_core_frame_index(core) == 0);
    for (int i = 0; i < 5; ++i) CHECK(botw_core_frame(core, 1.0 / 60.0) == 0);
    CHECK(botw_core_frame_index(core) == 5);

    const char* err = botw_core_last_error(core);
    CHECK(err != nullptr && err[0] != '\0');

    botw_core_destroy(core);
    CHECK(botw_core_last_error(nullptr) != nullptr);
    CHECK(botw_core_frame(nullptr, 0.0) == -1);

    if (g_failures) {
        std::printf("core_smoke_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("core_smoke_test: ok (nvn %u/%u on %s)\n", s.implemented, s.total, s.backend);
    return 0;
}

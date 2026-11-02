#include "botw_core.h"

#include "guest_run.h"

#include <cstdint>
#include <cstdio>

#if defined(__aarch64__)
extern "C" void botw_app_guest_probe(void*);
#endif

/* This is deliberately an app-facing debug hook, not part of the game path.
 * It proves that the iOS binary can enter native AArch64 guest code through
 * botw_guest_run, use the mapped guest stack, and return through the sentinel.
 * The actual title image and HLE bootstrap are separate milestones. */
extern "C" int botw_core_guest_selftest(void) {
#if defined(__aarch64__)
    botw_guest_run run{};
    uint64_t words[4] = {};
    const uint64_t magic = 0x5457424Full; /* "BOTW" little-endian */

    if (botw_guest_run_init(&run, nullptr, 0, 0x8000) != 0) {
        std::printf("botw_guest_selftest: init failed: %s\n", run.error);
        return 2;
    }

    run.args[0] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(words));
    const int entered = botw_guest_run_enter_at(
        &run, reinterpret_cast<const void*>(&botw_app_guest_probe));

    const bool ok = entered == 0 &&
                    run.reason == BOTW_GUEST_EXIT_RETURNED &&
                    words[0] == magic &&
                    words[1] == magic + 0x101u &&
                    words[2] == 0x11Fu &&
                    words[3] == run.stack_top - 16u &&
                    run.exit_x0 == magic;

    std::printf(
        "botw_guest_selftest: %s (reason=%d words=%llx/%llx/%llx sp=%llx exit_x0=%llx)\n",
        ok ? "ok" : "FAIL", run.reason,
        static_cast<unsigned long long>(words[0]),
        static_cast<unsigned long long>(words[1]),
        static_cast<unsigned long long>(words[2]),
        static_cast<unsigned long long>(words[3]),
        static_cast<unsigned long long>(run.exit_x0));

    botw_guest_run_close(&run);
    return ok ? 0 : 1;
#else
    std::printf("botw_guest_selftest: skipped (needs AArch64)\n");
    return -1;
#endif
}

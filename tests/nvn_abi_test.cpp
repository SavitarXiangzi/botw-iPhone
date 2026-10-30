/*
 * Portable ABI test for the NVN host-HLE boundary.
 *
 * Runs anywhere a C++ compiler exists -- including Windows, where the iOS
 * toolchain does not.  It is the gate that keeps scripts/gen_nvn.py honest:
 * if the registry, the generated header or the dispatch table drift apart,
 * this fails.
 */
#include "nvn_api.h"
#include "nvn_backend.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
static int g_missing = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static void count_missing(const char* name) {
    (void)name;
    ++g_missing;
}

static void probe(nvngfx_ctx*, nvngfx_args*) {}

static nvngfx_proc fake_resolve(void* user, const char* name) {
    const char* const* owned = static_cast<const char* const*>(user);
    for (int i = 0; owned[i]; ++i) {
        if (std::strcmp(owned[i], name) == 0) return probe;
    }
    return 0;
}

/* Resolve every registry entry; returns how many came back unresolved. */
static int resolve_all() {
    int unresolved = 0;
    for (int i = 0; i < nvngfx_count(); ++i) {
        const char* name = nvngfx_proc_name(static_cast<nvn_proc_index>(i));
        if (!nvngfx_lookup(name)) {
            ++unresolved;
            std::printf("  unresolved: %s\n", name);
        }
    }
    return unresolved;
}

int main() {
    nvngfx_set_missing_reporter(count_missing);
    nvngfx_install(nvngfx_null_backend());

    const int n = nvngfx_count();
    CHECK(n == NVN_PROC_COUNT);
    CHECK(n > 400); /* the real NVN surface is ~500 entry points */

    /* The argument block carries both integer (x0..x7) and FP (s0..s7 /
     * d0..d7) registers.  nvnSamplerBuilderSetLodBias is the first bound
     * entry point that needs the second half. */
    static_assert(sizeof(nvngfx_args) == 16 * sizeof(uint64_t));
    static_assert(offsetof(nvngfx_args, x) == 0);
    static_assert(offsetof(nvngfx_args, f) == 8 * sizeof(uint64_t));
    {
        nvngfx_args a{};
        nvngfx_arg_set_f32(&a, 3, 0.5f);
        CHECK(nvngfx_arg_f32(&a, 3) == 0.5f);
        CHECK(a.f[3] == 0x3f000000u);
        CHECK(nvngfx_arg_f32(&a, 0) == 0.0f);

        nvngfx_arg_set_f64(&a, 5, 1.5);
        CHECK(nvngfx_arg_f64(&a, 5) == 1.5);
        CHECK(a.f[5] == 0x3ff8000000000000ull);
        CHECK(a.f[3] == 0x3f000000u);   /* f64 must not clobber another slot */
    }

    /* Binary search requires strict ordering; verify it end to end. */
    for (int i = 1; i < n; ++i) {
        const char* a = nvngfx_proc_name(static_cast<nvn_proc_index>(i - 1));
        const char* b = nvngfx_proc_name(static_cast<nvn_proc_index>(i));
        CHECK(std::strcmp(a, b) < 0);
    }

    /* Every registry entry resolves (to the stub at worst); with no backend
     * every single one of them is reported missing, exactly once. */
    CHECK(resolve_all() == 0);
    CHECK(g_missing == n);
    CHECK(resolve_all() == 0);
    CHECK(g_missing == n); /* "once per name" holds on repeated resolve */

    /* The null backend must never claim an implementation. */
    for (int i = 0; i < n; ++i) {
        const char* name = nvngfx_proc_name(static_cast<nvn_proc_index>(i));
        if (nvngfx_is_implemented(name)) {
            std::printf("  null backend claims: %s\n", name);
            ++g_failures;
        }
    }

    /* The frame path the M3 exit criterion depends on must be in the registry. */
    CHECK(nvngfx_lookup("nvnCommandBufferDrawArrays") != 0);
    CHECK(nvngfx_lookup("nvnCommandBufferDrawElements") != 0);
    CHECK(nvngfx_lookup("nvnCommandBufferBeginRecording") != 0);
    CHECK(nvngfx_lookup("nvnQueuePresentTexture") != 0);
    CHECK(nvngfx_lookup("nvnWindowAcquireTexture") != 0);
    CHECK(nvngfx_lookup("nvnMemoryPoolBuilderSetStorage") != 0);
    CHECK(nvngfx_lookup("nvnTextureBuilderSetStorage") != 0);

    /* A name the title never asks for is rejected, not stubbed.
     * (This is the one intentional stderr line in the whole test.) */
    CHECK(nvngfx_lookup("nvnDefinitelyNotARealProc") == 0);
    CHECK(nvngfx_lookup(nullptr) == 0);

    /* A backend owning a subset flips is_implemented for exactly that subset,
     * and install() resets the "already reported" bookkeeping. */
    static const char* const owned[] = {"nvnCommandBufferDrawArrays",
                                        "nvnQueuePresentTexture", 0};
    nvngfx_backend fake = {"fake", const_cast<char**>(owned), fake_resolve};
    nvngfx_install(&fake);
    g_missing = 0;
    CHECK(resolve_all() == 0);
    CHECK(g_missing == n - 2);
    CHECK(nvngfx_is_implemented("nvnCommandBufferDrawArrays") == 1);
    CHECK(nvngfx_is_implemented("nvnQueuePresentTexture") == 1);
    CHECK(nvngfx_is_implemented("nvnCommandBufferDrawElements") == 0);

    if (g_failures) {
        std::printf("nvn_abi_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvn_abi_test: ok (%d procs)\n", n);
    return 0;
}

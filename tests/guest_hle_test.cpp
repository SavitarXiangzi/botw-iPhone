/* HLE registry: implementations, shared stubs, replacement, capacity.
 *
 * The registry is host data only; the one function this test calls is the
 * zero-return stub itself, which is also what an unimplemented guest import
 * would land on.  On AArch64 that call proves the stub returns 0 in x0.
 */
#include "guest_hle.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_hle_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

static uint64_t impl_a(void) { return 0xaaaa; }
static uint64_t impl_b(void) { return 0xbbbb; }

static botw_hle_table g_hle;
static botw_hle_table g_full;

int main() {
    botw_hle_init(&g_hle);
    CHECK(g_hle.error[0] == '\0');
    const size_t libc = botw_hle_implementation_count(&g_hle);
    CHECK(libc >= 30);
    CHECK(botw_hle_stub_count(&g_hle) == 0);

    /* Passthroughs are present; variadic calls are deliberately absent. */
    CHECK(botw_hle_lookup(&g_hle, "memcpy") != nullptr);
    CHECK(botw_hle_lookup(&g_hle, "strlen") != nullptr);
    CHECK(botw_hle_lookup(&g_hle, "sqrtf") != nullptr);
    CHECK(botw_hle_lookup(&g_hle, "printf") == nullptr);
    CHECK(botw_hle_lookup(&g_hle, "vsnprintf") == nullptr);
    CHECK(botw_hle_lookup(&g_hle, "no_such_symbol") == nullptr);

    /* Register, replace, and look up. */
    CHECK(botw_hle_register(&g_hle, "test_impl", (void*)(uintptr_t)&impl_a) == 0);
    CHECK(botw_hle_lookup(&g_hle, "test_impl") == (void*)(uintptr_t)&impl_a);
    CHECK(botw_hle_register(&g_hle, "test_impl", (void*)(uintptr_t)&impl_b) == 0);
    CHECK(botw_hle_lookup(&g_hle, "test_impl") == (void*)(uintptr_t)&impl_b);
    CHECK(botw_hle_implementation_count(&g_hle) == libc + 1);

    /* An unknown name gets the shared stub, and the same pointer again. */
    void* stub = botw_hle_resolve(&g_hle, "no_such_symbol");
    CHECK(stub == (void*)(uintptr_t)&botw_hle_stub);
    CHECK(botw_hle_stub_count(&g_hle) == 1);
    CHECK(botw_hle_resolve(&g_hle, "no_such_symbol") == stub);
    CHECK(botw_hle_stub_count(&g_hle) == 1);

    /* A registered implementation wins over the stub path. */
    CHECK(botw_hle_resolve(&g_hle, "test_impl") == (void*)(uintptr_t)&impl_b);
    CHECK(botw_hle_stub_count(&g_hle) == 1);

    /* Replacing a stub with an implementation moves the counters. */
    CHECK(botw_hle_register(&g_hle, "no_such_symbol", (void*)(uintptr_t)&impl_a) == 0);
    CHECK(botw_hle_resolve(&g_hle, "no_such_symbol") == (void*)(uintptr_t)&impl_a);
    CHECK(botw_hle_stub_count(&g_hle) == 0);
    CHECK(botw_hle_implementation_count(&g_hle) == libc + 2);

    /* The stub really returns zero when called. */
    typedef uint64_t (*fn)(void);
    CHECK(((fn)stub)() == 0);

    /* Fill a fresh table to its last slot.  The next new name must fail
     * loudly instead of spinning in the open-addressing probe. */
    botw_hle_init(&g_full);
    const size_t base = botw_hle_implementation_count(&g_full);
    const size_t room = (size_t)BOTW_HLE_SLOTS - base;
    std::vector<std::string> names;
    names.reserve(room);
    for (size_t i = 0; i < room; i++) {
        names.push_back("guest_symbol_" + std::to_string(i));
    }
    for (const std::string& name : names) {
        CHECK(botw_hle_resolve(&g_full, name.c_str()) != nullptr);
    }
    CHECK(g_full.used == BOTW_HLE_SLOTS);
    CHECK(botw_hle_stub_count(&g_full) == room);
    CHECK(botw_hle_resolve(&g_full, "one_too_many") == nullptr);
    CHECK(g_full.error[0] != '\0');

    /* A full table can still replace an existing name. */
    CHECK(botw_hle_register(&g_full, names[0].c_str(), (void*)(uintptr_t)&impl_a) == 0);
    CHECK(botw_hle_lookup(&g_full, names[0].c_str()) == (void*)(uintptr_t)&impl_a);

    if (g_fail) {
        std::printf("guest_hle_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_hle_test: ok (%zu libc implementations, stubs shared, full table refused)\n",
                libc);
    return 0;
}

/* nn::os HLE: the thread table, TLS, virtual clock and memory blocks.
 *
 * The registry test runs everywhere.  The run-dependent part maps real guest
 * worker stacks on POSIX hosts but never executes guest code; the actual
 * cooperative switch is covered by guest_thread_test on the arm64 runner.
 *
 * What this test proves:
 *   - all 33 names are claimed by name, not silently left on the stub;
 *   - ThreadType handles map to run slots, start/ready/reap state changes in
 *     the run, and IDs are stable and distinct;
 *   - YieldThread/SleepThread set the run's switch request;
 *   - WaitThread only returns early for an already-DONE target;
 *   - TLS values are per current slot, and time/memory answers round-trip.
 */
#include "guest_hle.h"
#include "guest_hle_os.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_hle_os_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

static const char* const kOsNames[BOTW_HLE_OS_COUNT] = {
    "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE",
    "_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE",
    "_ZN2nn2os11GetHostArgcEv",
    "_ZN2nn2os11GetHostArgvEv",
    "_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE",
    "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE",
    "_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm",
    "_ZN2nn2os11SleepThreadENS_8TimeSpanE",
    "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE",
    "_ZN2nn2os11YieldThreadEv",
    "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi",
    "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii",
    "_ZN2nn2os13ConvertToTickENS_8TimeSpanE",
    "_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE",
    "_ZN2nn2os13GetSystemTickEv",
    "_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc",
    "_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE",
    "_ZN2nn2os15FreeMemoryBlockEmm",
    "_ZN2nn2os15GetCurrentFiberEv",
    "_ZN2nn2os16GetCurrentThreadEv",
    "_ZN2nn2os17ConvertToTimeSpanENS0_4TickE",
    "_ZN2nn2os17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE",
    "_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE",
    "_ZN2nn2os17SetMemoryHeapSizeEm",
    "_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim",
    "_ZN2nn2os19AllocateMemoryBlockEPmm",
    "_ZN2nn2os19GetCurrentStackInfoEPmS1_",
    "_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi",
    "_ZN2nn2os20GetCurrentCoreNumberEv",
    "_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE",
    "_ZN2nn2os20SetThreadNamePointerEPNS0_10ThreadTypeEPKc",
    "_ZN2nn2os22GetSystemTickFrequencyEv",
    "_ZN2nn2os26GetThreadAvailableCoreMaskEv",
};

static botw_hle_table g_hle;
static botw_hle_os_state g_os;

static void dummy_entry(void*) {
}

/* A one-shot timer is used only to prove the slot is reusable. */
static void dummy_tls_dtor(void*) {
}

template <typename Fn>
static Fn lookup(const char* name) {
    return reinterpret_cast<Fn>(botw_hle_lookup(&g_hle, name));
}

int main() {
    botw_hle_init(&g_hle);
    CHECK(g_hle.error[0] == '\0');

    for (unsigned i = 0; i < BOTW_HLE_OS_COUNT; i++) {
        CHECK(botw_hle_lookup(&g_hle, kOsNames[i]) != nullptr);
    }
    CHECK(botw_hle_stub_count(&g_hle) == 0);

    botw_hle_os_reset(&g_os);
    CHECK(botw_hle_os_register(&g_hle, &g_os) == 0);
    CHECK(g_os.threads[0].used);
    CHECK(g_os.threads[0].id == BOTW_HLE_OS_MAIN_ID);
    CHECK(g_os.next_thread_id == BOTW_HLE_OS_MAIN_ID + 1u);

    void* main_thread = lookup<void* (*)()>("_ZN2nn2os16GetCurrentThreadEv")();
    CHECK(main_thread == (void*)g_os.main_type);
    CHECK(lookup<uint64_t (*)(const void*)>("_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE")(main_thread) ==
          BOTW_HLE_OS_MAIN_ID);
    CHECK(lookup<void* (*)()>("_ZN2nn2os15GetCurrentFiberEv")() == main_thread);
    CHECK(lookup<int (*)()>("_ZN2nn2os11GetHostArgcEv")() == 1);
    CHECK(lookup<char** (*)()>("_ZN2nn2os11GetHostArgvEv")()[0] != nullptr);
    CHECK(lookup<uint64_t (*)()>("_ZN2nn2os26GetThreadAvailableCoreMaskEv")() == 7);
    CHECK(lookup<int (*)()>("_ZN2nn2os20GetCurrentCoreNumberEv")() == 0);

    CHECK(lookup<int (*)(const void*)>("_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE")(main_thread) ==
          0x2C);
    lookup<void (*)(void*, int)>("_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi")(main_thread, 10);
    CHECK(lookup<int (*)(const void*)>("_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE")(main_thread) ==
          10);
    int core_id = -1;
    uint64_t core_mask = 0;
    CHECK(lookup<uint32_t (*)(int*, uint64_t*, const void*)>(
              "_ZN2nn2os17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE")(
              &core_id, &core_mask, main_thread) == 0);
    CHECK(core_id == 0);
    CHECK(core_mask == 7);

    lookup<void (*)(void*, const char*)>("_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc")(
        main_thread, "main-renamed");
    CHECK(std::strcmp(lookup<const char* (*)(const void*)>(
                          "_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE")(main_thread),
                      "main-renamed") == 0);
    const char* pointer_name = "pointer-name";
    lookup<void (*)(void*, const char*)>("_ZN2nn2os20SetThreadNamePointerEPNS0_10ThreadTypeEPKc")(
        main_thread, pointer_name);
    CHECK(lookup<const char* (*)(const void*)>(
              "_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE")(main_thread) == pointer_name);

    /* Time is deterministic and advances on every tick query. */
    uint64_t tick0 = lookup<uint64_t (*)()>("_ZN2nn2os13GetSystemTickEv")();
    uint64_t tick1 = lookup<uint64_t (*)()>("_ZN2nn2os13GetSystemTickEv")();
    CHECK(tick1 == tick0 + 1);
    CHECK(lookup<uint64_t (*)()>("_ZN2nn2os22GetSystemTickFrequencyEv")() ==
          BOTW_HLE_OS_TICK_FREQ);
    CHECK(lookup<uint64_t (*)(int64_t)>("_ZN2nn2os13ConvertToTickENS_8TimeSpanE")(1000000000ll) ==
          BOTW_HLE_OS_TICK_FREQ);
    CHECK(lookup<int64_t (*)(uint64_t)>("_ZN2nn2os17ConvertToTimeSpanENS0_4TickE")(
              BOTW_HLE_OS_TICK_FREQ) == 1000000000ll);
    CHECK(lookup<uint64_t (*)(int64_t)>("_ZN2nn2os13ConvertToTickENS_8TimeSpanE")(-1) == 0);
    botw_hle_os_advance(&g_os, 100);
    CHECK(lookup<uint64_t (*)()>("_ZN2nn2os13GetSystemTickEv")() == tick1 + 101);

    /* TLS slots belong to the current slot (main when no run is attached). */
    uint32_t tls = 0xffffffffu;
    CHECK(lookup<uint32_t (*)(uint32_t*, void (*)(void*))>(
              "_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE")(&tls, dummy_tls_dtor) == 0);
    CHECK(tls == 0);
    CHECK(lookup<uint64_t (*)(uint32_t)>("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE")(tls) == 0);
    lookup<void (*)(uint32_t, uint64_t)>("_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm")(tls, 0xdeadbeefull);
    CHECK(lookup<uint64_t (*)(uint32_t)>("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE")(tls) ==
          0xdeadbeefull);
    lookup<void (*)(uint32_t)>("_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE")(tls);
    CHECK(lookup<uint64_t (*)(uint32_t)>("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE")(tls) == 0);
    uint32_t tls2 = 0xffffffffu;
    CHECK(lookup<uint32_t (*)(uint32_t*, void (*)(void*))>(
              "_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE")(&tls2, nullptr) == 0);
    CHECK(tls2 == 0);

    /* Memory blocks are page-aligned and free through their own header. */
    size_t block = 0;
    CHECK(lookup<uint32_t (*)(size_t*, size_t)>(
              "_ZN2nn2os19AllocateMemoryBlockEPmm")(&block, 0x1234) == 0);
    CHECK(block != 0);
    CHECK(block % 0x1000u == 0);
    *reinterpret_cast<volatile unsigned char*>(block) = 0x5a;
    lookup<void (*)(size_t)>("_ZN2nn2os17SetMemoryHeapSizeEm")(0x4000000);
    CHECK(g_os.memory_heap_size == 0x4000000);
    lookup<void (*)(size_t, size_t)>("_ZN2nn2os15FreeMemoryBlockEmm")(block, 0x1234);
    CHECK(g_os.block_allocs == 1);
    CHECK(g_os.block_frees == 1);

    /* Without a run, CreateThread must say no instead of inventing a slot. */
    uint64_t fake_type[8] = {0};
    CHECK(lookup<uint32_t (*)(void*, void (*)(void*), void*, void*, size_t, int)>(
              "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi")(
              fake_type, dummy_entry, nullptr, nullptr, 0x8000, 0x20) != 0);
    CHECK(g_os.thread_creates == 0);

#if defined(_WIN32)
    std::printf("guest_hle_os_test: skipped run section (needs POSIX mmap)\n");
#else
    botw_guest_run run;
    uint64_t type_a[64] = {0};
    uint64_t type_b[64] = {0};
    CHECK(botw_guest_run_init(&run, nullptr, 0, 0x8000) == 0);
    botw_hle_os_attach_run(&g_os, &run);
    CHECK(g_os.attached);
    CHECK(g_os.run == &run);

    CHECK(lookup<uint32_t (*)(void*, void (*)(void*), void*, void*, size_t, int)>(
              "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi")(
              type_a, dummy_entry, (void*)0x1234, nullptr, 0x8000, 0x20) == 0);
    CHECK(lookup<uint32_t (*)(void*, void (*)(void*), void*, void*, size_t, int, int)>(
              "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii")(
              type_b, dummy_entry, (void*)0x5678, nullptr, 0x4000, 0x18, 1) == 0);
    CHECK(g_os.thread_creates == 2);
    CHECK(g_os.threads[1].used);
    CHECK(g_os.threads[2].used);
    CHECK(g_os.threads[1].id != g_os.threads[2].id);
    CHECK(run.threads[1].state == BOTW_GUEST_THREAD_CREATED);
    CHECK(run.threads[2].state == BOTW_GUEST_THREAD_CREATED);

    lookup<void (*)(void*)>("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE")(type_a);
    CHECK(run.threads[1].state == BOTW_GUEST_THREAD_READY);
    CHECK(g_os.thread_starts == 1);
    CHECK(lookup<uint64_t (*)(const void*)>(
              "_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE")(type_a) == g_os.threads[1].id);
    CHECK(lookup<int (*)(const void*)>(
              "_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE")(type_a) == 0x20);

    run.current = 1;
    run.threads[1].state = BOTW_GUEST_THREAD_RUNNING;
    CHECK(lookup<void* (*)()>("_ZN2nn2os16GetCurrentThreadEv")() == (void*)type_a);
    size_t stack_base = 0;
    size_t stack_size = 0;
    lookup<void (*)(size_t*, size_t*)>("_ZN2nn2os19GetCurrentStackInfoEPmS1_")(
        &stack_base, &stack_size);
    CHECK(stack_base == (size_t)(uintptr_t)(run.threads[1].stack + run.threads[1].guard_bytes));
    CHECK(stack_size == run.threads[1].stack_bytes - run.threads[1].guard_bytes);

    run.active = 1;
    run.switch_requested = 0;
    unsigned long yields = g_os.yields;
    lookup<void (*)()>("_ZN2nn2os11YieldThreadEv")();
    CHECK(g_os.yields == yields + 1);
    CHECK(run.switch_requested == 1);

    run.switch_requested = 0;
    lookup<void (*)(int64_t)>("_ZN2nn2os11SleepThreadENS_8TimeSpanE")(5000000);
    CHECK(g_os.sleeps == 1);
    CHECK(g_os.last_sleep_ns == 5000000);
    CHECK(g_os.sleep_deadline > g_os.tick);
    CHECK(run.switch_requested == 1);

    /* A live target only requests a switch; a DONE target joins immediately. */
    run.switch_requested = 0;
    unsigned long waits = g_os.thread_waits;
    lookup<void (*)(void*)>("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE")(type_b);
    CHECK(g_os.thread_waits == waits + 1);
    CHECK(run.switch_requested == 1);
    run.threads[2].state = BOTW_GUEST_THREAD_DONE;
    run.switch_requested = 0;
    lookup<void (*)(void*)>("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE")(type_b);
    CHECK(g_os.thread_waits == waits + 1);
    CHECK(run.switch_requested == 0);

    lookup<void (*)(void*)>("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE")(type_b);
    CHECK(run.threads[2].state == BOTW_GUEST_THREAD_FREE);
    CHECK(run.threads[2].stack == nullptr);
    CHECK(!g_os.threads[2].used);
    CHECK(g_os.thread_destroys == 1);

    /* A READY thread cannot be reaped; a DONE one can. */
    lookup<void (*)(void*)>("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE")(type_a);
    CHECK(g_os.threads[1].used);
    run.threads[1].state = BOTW_GUEST_THREAD_DONE;
    lookup<void (*)(void*)>("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE")(type_a);
    CHECK(!g_os.threads[1].used);
    CHECK(run.threads[1].stack == nullptr);

    run.current = -1;
    run.active = 0;
    botw_hle_os_attach_run(&g_os, nullptr);
    CHECK(!g_os.attached);
    botw_guest_run_close(&run);
#endif

    if (g_fail) {
        std::printf("guest_hle_os_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_hle_os_test: ok (%u nn::os names, thread table, TLS, time, memory blocks)\n",
                BOTW_HLE_OS_COUNT);
    return 0;
}

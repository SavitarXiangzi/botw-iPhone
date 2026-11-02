/* nn::os HLE: threads, TLS, virtual clock, sync objects, message queues and
 * memory blocks.
 *
 * The registry test runs everywhere.  The run-dependent part maps real guest
 * worker stacks on POSIX hosts but never executes guest code; the actual
 * cooperative switch is covered by guest_thread_test on the arm64 runner.
 *
 * What this test proves:
 *   - all 78 table entries are claimed by name; all 74 title imports are
 *     covered, plus four helper entries;
 *   - ThreadType handles map to run slots, start/ready/reap state changes in
 *     the run, and IDs are stable and distinct;
 *   - YieldThread/SleepThread set the run's switch request;
 *   - WaitThread only returns early for an already-DONE target;
 *   - TLS values are per current slot, and time/memory answers round-trip;
 *   - MessageQueue FIFO peek/receive and LIFO jam ordering;
 *   - SystemEvent clear modes, MultiWait link order and WaitAny selection.
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
    "_ZN2nn2os10ClearEventEPNS0_9EventTypeE",
    "_ZN2nn2os11SignalEventEPNS0_9EventTypeE",
    "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE",
    "_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE",
    "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE",
    "_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE",
    "_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE",
    "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE",
    "_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE",
    "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE",
    "_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi",
    "_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE",
    "_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE",
    "_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeEi",
    "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE",
    "_ZN2nn2os17FinalizeSemaphoreEPNS0_13SemaphoreTypeE",
    "_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE",
    "_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii",
    "_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE",
    "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE",
    "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE",
    "_ZN2nn2os9WaitEventEPNS0_9EventTypeE",
    "_ZN2nn2os20FinalizeMessageQueueEPNS0_16MessageQueueTypeE",
    "_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm",
    "_ZN2nn2os19TrySendMessageQueueEPNS0_16MessageQueueTypeEm",
    "_ZN2nn2os15JamMessageQueueEPNS0_16MessageQueueTypeEm",
    "_ZN2nn2os19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE",
    "_ZN2nn2os22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE",
    "_ZN2nn2os19TryPeekMessageQueueEPmPKNS0_16MessageQueueTypeE",
    "_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm",
    "_ZN2nn2os21InitializeSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeE",
    "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE",
    "_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE",
    "_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE",
    "_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE",
    "_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE",
    "_ZN2nn2os20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE",
    "_ZN2nn2os19InitializeMultiWaitEPNS0_13MultiWaitTypeE",
    "_ZN2nn2os17FinalizeMultiWaitEPNS0_13MultiWaitTypeE",
    "_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE",
    "_ZN2nn2os21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE",
    "_ZN2nn2os23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE",
    "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE",
    "_ZN2nn2os7WaitAnyEPNS0_13MultiWaitTypeE",
    "_ZN2nn2os23SetUserExceptionHandlerEPFvPNS0_17UserExceptionInfoEEPvmS2_",
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

    /* Mutexes are keyed by the guest object pointer.  Recursive locks count
     * depth; a non-recursive second lock fails without touching the object. */
    uint64_t mutex_obj[8] = {0};
    uint64_t recursive_obj[8] = {0};
    lookup<void (*)(void*, int, int)>("_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi")(
        mutex_obj, 0, 0x20);
    CHECK(g_os.mutexes[0].used);
    CHECK(g_os.mutexes[0].owner == -1);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE")(mutex_obj) == 1);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE")(mutex_obj) == 0);
    lookup<void (*)(void*)>("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE")(mutex_obj);
    CHECK(g_os.mutexes[0].owner == -1);
    lookup<void (*)(void*)>("_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE")(mutex_obj);
    CHECK(!g_os.mutexes[0].used);

    lookup<void (*)(void*, int, int)>("_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi")(
        recursive_obj, 1, 0x20);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE")(recursive_obj) == 1);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE")(recursive_obj) == 1);
    CHECK(g_os.mutexes[0].depth == 2);
    lookup<void (*)(void*)>("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE")(recursive_obj);
    CHECK(g_os.mutexes[0].depth == 1);
    lookup<void (*)(void*)>("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE")(recursive_obj);
    CHECK(g_os.mutexes[0].owner == -1);
    lookup<void (*)(void*)>("_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE")(recursive_obj);

    /* Manual-clear events stay signaled; auto-clear events consume one wait. */
    uint64_t event_obj[8] = {0};
    uint64_t auto_event_obj[8] = {0};
    lookup<void (*)(void*, int, int)>(
        "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE")(event_obj, 0, 0);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE")(event_obj) == 0);
    lookup<void (*)(void*)>("_ZN2nn2os11SignalEventEPNS0_9EventTypeE")(event_obj);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE")(event_obj) == 1);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE")(event_obj) == 1);
    lookup<void (*)(void*)>("_ZN2nn2os10ClearEventEPNS0_9EventTypeE")(event_obj);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE")(event_obj) == 0);
    lookup<void (*)(void*)>("_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE")(event_obj);
    CHECK(!g_os.events[0].used);

    lookup<void (*)(void*, int, int)>(
        "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE")(auto_event_obj, 1, 1);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE")(auto_event_obj) == 1);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE")(auto_event_obj) == 0);
    lookup<void (*)(void*)>("_ZN2nn2os11SignalEventEPNS0_9EventTypeE")(auto_event_obj);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE")(auto_event_obj) == 1);
    CHECK(lookup<int (*)(void*)>("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE")(auto_event_obj) == 0);
    lookup<void (*)(void*)>("_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE")(auto_event_obj);

    /* Light events use the same clear modes and a timed wait returns bool. */
    uint64_t light_obj[8] = {0};
    lookup<void (*)(void*, int, int)>(
        "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE")(
        light_obj, 1, 1);
    CHECK(lookup<int (*)(void*, int64_t)>(
              "_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE")(
              light_obj, 0) == 1);
    CHECK(lookup<int (*)(void*, int64_t)>(
              "_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE")(
              light_obj, 0) == 0);
    lookup<void (*)(void*)>("_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE")(light_obj);
    lookup<void (*)(void*)>("_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE")(light_obj);
    CHECK(lookup<int (*)(void*, int64_t)>(
              "_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE")(
              light_obj, 0) == 0);
    lookup<void (*)(void*)>("_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE")(light_obj);
    lookup<void (*)(void*)>("_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE")(light_obj);
    CHECK(!g_os.light_events[0].used);

    /* Semaphore counts obey the maximum and hand permits back on release. */
    uint64_t sem_obj[8] = {0};
    lookup<void (*)(void*, int, int)>(
        "_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii")(sem_obj, 1, 2);
    CHECK(g_os.semaphores[0].count == 1);
    lookup<void (*)(void*)>("_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE")(sem_obj);
    CHECK(g_os.semaphores[0].count == 0);
    lookup<void (*)(void*)>("_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE")(sem_obj);
    CHECK(g_os.semaphores[0].count == 1);
    lookup<void (*)(void*, int)>("_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeEi")(sem_obj, 1);
    CHECK(g_os.semaphores[0].count == 2);
    lookup<void (*)(void*)>("_ZN2nn2os17FinalizeSemaphoreEPNS0_13SemaphoreTypeE")(sem_obj);
    CHECK(!g_os.semaphores[0].used);
    CHECK(g_os.mutex_locks == 3);
    CHECK(g_os.event_signals == 2);
    CHECK(g_os.light_event_signals == 1);
    CHECK(g_os.semaphore_acquires == 1);
    CHECK(g_os.semaphore_releases == 2);

    /* Message queues use the caller's ring: FIFO send/receive, LIFO jam, and
     * TryPeek without consuming.  Blocking handoff is covered by
     * guest_message_queue_test on the AArch64 runner. */
    uint64_t mq_obj[16] = {0};
    size_t mq_buffer[4] = {0};
    auto mq_init = lookup<void (*)(void*, size_t*, size_t)>(
        "_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm");
    auto mq_fini = lookup<void (*)(void*)>(
        "_ZN2nn2os20FinalizeMessageQueueEPNS0_16MessageQueueTypeE");
    auto mq_try_send = lookup<int (*)(void*, size_t)>(
        "_ZN2nn2os19TrySendMessageQueueEPNS0_16MessageQueueTypeEm");
    auto mq_try_recv = lookup<int (*)(size_t*, void*)>(
        "_ZN2nn2os22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE");
    auto mq_try_peek = lookup<int (*)(size_t*, const void*)>(
        "_ZN2nn2os19TryPeekMessageQueueEPmPKNS0_16MessageQueueTypeE");
    CHECK(mq_init != nullptr);
    CHECK(mq_fini != nullptr);
    CHECK(mq_try_send != nullptr);
    CHECK(mq_try_recv != nullptr);
    CHECK(mq_try_peek != nullptr);

    mq_init(mq_obj, mq_buffer, 2);
    CHECK(g_os.message_queues[0].used);
    CHECK(g_os.message_queues[0].capacity == 2);
    CHECK(mq_try_send(mq_obj, 0x11) == 1);
    CHECK(mq_try_send(mq_obj, 0x22) == 1);
    CHECK(mq_try_send(mq_obj, 0x33) == 0);
    size_t mq_value = 0;
    CHECK(mq_try_peek(&mq_value, mq_obj) == 1);
    CHECK(mq_value == 0x11);
    CHECK(mq_try_recv(&mq_value, mq_obj) == 1);
    CHECK(mq_value == 0x11);
    CHECK(mq_try_recv(&mq_value, mq_obj) == 1);
    CHECK(mq_value == 0x22);
    CHECK(mq_try_recv(&mq_value, mq_obj) == 0);
    CHECK(mq_try_send(mq_obj, 0x33) == 1);

#if !defined(__aarch64__)
    /* JamMessageQueue is a blocking import on the real target, so this host
     * only call is valid in the portable build. */
    auto mq_jam = lookup<void (*)(void*, size_t)>(
        "_ZN2nn2os15JamMessageQueueEPNS0_16MessageQueueTypeEm");
    CHECK(mq_jam != nullptr);
    mq_jam(mq_obj, 0x44);
    CHECK(mq_try_peek(&mq_value, mq_obj) == 1);
    CHECK(mq_value == 0x44);
    CHECK(mq_try_recv(&mq_value, mq_obj) == 1);
    CHECK(mq_value == 0x44);
#endif

    CHECK(mq_try_recv(&mq_value, mq_obj) == 1);
    CHECK(mq_value == 0x33);
    CHECK(g_os.message_queues[0].count == 0);
    CHECK(g_os.message_queues[0].receiver_count == 0);
    CHECK(g_os.message_queues[0].sender_count == 0);
#if defined(__aarch64__)
    CHECK(g_os.message_peeks == 1);
#else
    CHECK(g_os.message_peeks == 2);   /* the portable-only Jam check peeks too */
#endif
    mq_fini(mq_obj);
    CHECK(!g_os.message_queues[0].used);

    /* SystemEvent: auto-clear consumption, timed immediate success, and the
     * helper clear path.  The blocking wait is covered on AArch64. */
    uint64_t sys_obj[16] = {0};
    uint8_t exc_stack[32] = {0};
    auto sys_init = lookup<void (*)(void*, int)>(
        "_ZN2nn2os21InitializeSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeE");
    auto sys_destroy = lookup<void (*)(void*)>(
        "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
    auto sys_signal = lookup<void (*)(void*)>(
        "_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    auto sys_clear = lookup<void (*)(void*)>(
        "_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE");
    auto sys_wait = lookup<void (*)(void*)>(
        "_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE");
    auto sys_try = lookup<int (*)(void*)>(
        "_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE");
    auto sys_timed = lookup<int (*)(void*, int64_t)>(
        "_ZN2nn2os20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE");
    CHECK(sys_init != nullptr);
    CHECK(sys_destroy != nullptr);
    CHECK(sys_signal != nullptr);
    CHECK(sys_clear != nullptr);
    CHECK(sys_wait != nullptr);
    CHECK(sys_try != nullptr);
    CHECK(sys_timed != nullptr);

    sys_init(sys_obj, 1);
    CHECK(g_os.system_events[0].used);
    CHECK(g_os.system_events[0].auto_clear == 1);
    CHECK(sys_try(sys_obj) == 0);
    sys_signal(sys_obj);
    CHECK(sys_try(sys_obj) == 1);
    CHECK(sys_try(sys_obj) == 0);
    sys_signal(sys_obj);
    CHECK(sys_timed(sys_obj, 0) == 1);
    sys_signal(sys_obj);
    sys_wait(sys_obj);
    CHECK(sys_try(sys_obj) == 0);
    sys_signal(sys_obj);
    sys_clear(sys_obj);
    CHECK(sys_try(sys_obj) == 0);
    CHECK(g_os.system_event_waits == 7);
    CHECK(g_os.system_event_signals == 4);
    sys_destroy(sys_obj);
    CHECK(!g_os.system_events[0].used);
    CHECK(g_os.system_event_destroys == 1);

    /* MultiWait observes Events without consuming them and picks the first
     * signaled holder in link order.  Relinking moves a holder to the tail. */
    uint64_t mw_obj[8] = {0};
    uint64_t mw_h1[8] = {0};
    uint64_t mw_h2[8] = {0};
    uint64_t mw_ev1[8] = {0};
    uint64_t mw_ev2[8] = {0};
    auto ev_init = lookup<void (*)(void*, int, int)>(
        "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE");
    auto ev_signal = lookup<void (*)(void*)>(
        "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    auto ev_try = lookup<int (*)(void*)>(
        "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE");
    auto ev_fini = lookup<void (*)(void*)>(
        "_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE");
    auto mw_init = lookup<void (*)(void*)>(
        "_ZN2nn2os19InitializeMultiWaitEPNS0_13MultiWaitTypeE");
    auto mw_fini = lookup<void (*)(void*)>(
        "_ZN2nn2os17FinalizeMultiWaitEPNS0_13MultiWaitTypeE");
    auto mw_holder_init = lookup<void (*)(void*, void*)>(
        "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE");
    auto mw_holder_fini = lookup<void (*)(void*)>(
        "_ZN2nn2os23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE");
    auto mw_link = lookup<void (*)(void*, void*)>(
        "_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE");
    auto mw_unlink = lookup<void (*)(void*)>(
        "_ZN2nn2os21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE");
    auto mw_wait_any = lookup<void* (*)(void*)>(
        "_ZN2nn2os7WaitAnyEPNS0_13MultiWaitTypeE");
    CHECK(ev_init && ev_signal && ev_try && ev_fini);
    CHECK(mw_init && mw_fini && mw_holder_init && mw_holder_fini);
    CHECK(mw_link && mw_unlink && mw_wait_any);

    ev_init(mw_ev1, 0, 1);
    ev_init(mw_ev2, 0, 1);
    mw_init(mw_obj);
    CHECK(g_os.multi_waits[0].used);
    CHECK(g_os.multi_waits[0].waiter == -1);
    mw_holder_init(mw_h1, mw_ev1);
    mw_holder_init(mw_h2, mw_ev2);
    mw_link(mw_obj, mw_h1);
    mw_link(mw_obj, mw_h2);
    CHECK(g_os.multi_waits[0].holder_count == 2);
    CHECK(g_os.multi_wait_links == 2);

    ev_signal(mw_ev2);
    CHECK(mw_wait_any(mw_obj) == mw_h2);
    CHECK(ev_try(mw_ev2) == 1);           /* WaitAny did not consume it */
    CHECK(ev_try(mw_ev1) == 0);
    ev_signal(mw_ev1);
    ev_signal(mw_ev2);
    CHECK(mw_wait_any(mw_obj) == mw_h1);  /* link order wins */
    CHECK(ev_try(mw_ev1) == 1);
    CHECK(ev_try(mw_ev2) == 1);

    mw_unlink(mw_h1);
    mw_link(mw_obj, mw_h1);               /* h2 is now first */
    ev_signal(mw_ev1);
    ev_signal(mw_ev2);
    CHECK(mw_wait_any(mw_obj) == mw_h2);
    CHECK(ev_try(mw_ev2) == 1);
    CHECK(ev_try(mw_ev1) == 1);
    CHECK(g_os.multi_wait_waits == 3);
    CHECK(g_os.multi_wait_links == 3);

    mw_unlink(mw_h2);
    mw_unlink(mw_h1);
    mw_holder_fini(mw_h2);
    mw_holder_fini(mw_h1);
    mw_fini(mw_obj);
    ev_fini(mw_ev1);
    ev_fini(mw_ev2);
    CHECK(!g_os.multi_waits[0].used);
    CHECK(!g_os.multi_wait_holders[0].used);
    CHECK(!g_os.events[0].used);

    auto set_user_exception = lookup<void (*)(void (*)(void*), void*, size_t, void*)>(
        "_ZN2nn2os23SetUserExceptionHandlerEPFvPNS0_17UserExceptionInfoEEPvmS2_");
    CHECK(set_user_exception != nullptr);
    set_user_exception(&dummy_entry, &g_os, sizeof exc_stack, exc_stack);
    CHECK(g_os.user_exception_handler == &dummy_entry);
    CHECK(g_os.user_exception_arg == &g_os);
    CHECK(g_os.user_exception_stack_size == sizeof exc_stack);
    CHECK(g_os.user_exception_stack == exc_stack);
    CHECK(g_os.user_exception_sets == 1);

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
    std::printf("guest_hle_os_test: ok (%u nn::os names, thread table, TLS, time, "
                "memory blocks, sync objects, message queues)\n",
                BOTW_HLE_OS_COUNT);
    return 0;
}

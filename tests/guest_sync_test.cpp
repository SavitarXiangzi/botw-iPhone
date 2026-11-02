/* Synchronization HLE: real mutex/event/light-event/semaphore blocking.
 *
 * Four guest workers run on their own stacks.  Main holds a mutex while
 * worker-a blocks on it, times out on a light event, unlocks, and joins the
 * worker.  Later phases block main on an event while worker-b signals it,
 * block worker-c on a semaphore after it signals main, and block main on an
 * untimed light event while worker-d signals it.  Every wait resumes at the
 * instruction after its import, so the counter values are also a frame-restore
 * test.
 *
 * On x86 the test skips; guest_hle_os_test still exercises the nonblocking
 * paths and object tables on the portable host.
 */
#include "guest_hle.h"
#include "guest_hle_os.h"
#include "guest_run.h"

#include <cstdint>
#include <cstdio>

#if defined(_WIN32) || !defined(__aarch64__)

int main() {
    std::printf("guest_sync_test: skipped (needs AArch64 + mmap)\n");
    return 0;
}

#else

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_sync_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

extern "C" void botw_guest_sync_main_entry(void* args);
extern "C" void botw_guest_sync_worker_a_entry(void* args);
extern "C" void botw_guest_sync_worker_b_entry(void* args);
extern "C" void botw_guest_sync_worker_c_entry(void* args);
extern "C" void botw_guest_sync_worker_d_entry(void* args);

struct sync_probe_args {
    uint64_t* counter;
    void* lock_mutex;
    void* unlock_mutex;
    void* wait_event;
    void* signal_event;
    void* wait_light;
    void* signal_light;
    void* timed_wait_light;
    void* acquire_semaphore;
    void* release_semaphore;
    void* wait_thread;
    void* start_thread;
    void* mutex;
    void* event;
    void* light;
    void* semaphore;
    void* worker_a;
    void* worker_b;
    void* worker_c;
    void* worker_d;
    int64_t timeout_ns;
};

typedef uint32_t (*os_create7_fn)(void*, void (*)(void*), void*, void*,
                                  size_t, int, int);
typedef void (*os_start_fn)(void*);
typedef void (*os_init_mutex_fn)(void*, int, int);
typedef void (*os_init_event_fn)(void*, int, int);
typedef void (*os_init_light_event_fn)(void*, int, int);
typedef void (*os_init_semaphore_fn)(void*, int, int);

int main() {
    botw_hle_table hle;
    botw_hle_os_state os;
    botw_guest_run run;
    uint64_t worker_a_type[64] = {0};
    uint64_t worker_b_type[64] = {0};
    uint64_t worker_c_type[64] = {0};
    uint64_t worker_d_type[64] = {0};
    uint64_t mutex[16] = {0};
    uint64_t event[16] = {0};
    uint64_t light[16] = {0};
    uint64_t semaphore[16] = {0};
    uint64_t counter[16] = {0};
    sync_probe_args args;
    os_create7_fn create;
    os_start_fn start;
    os_init_mutex_fn init_mutex;
    os_init_event_fn init_event;
    os_init_light_event_fn init_light;
    os_init_semaphore_fn init_semaphore;

    botw_hle_init(&hle);
    CHECK(hle.error[0] == '\0');
    botw_hle_os_reset(&os);
    CHECK(botw_hle_os_register(&hle, &os) == 0);
    CHECK(botw_guest_run_init(&run, nullptr, 0, 0x8000) == 0);
    botw_hle_os_attach_run(&os, &run);

    create = (os_create7_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii");
    start = (os_start_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    init_mutex = (os_init_mutex_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi");
    init_event = (os_init_event_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE");
    init_light = (os_init_light_event_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE");
    init_semaphore = (os_init_semaphore_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii");
    CHECK(create != nullptr);
    CHECK(start != nullptr);
    CHECK(init_mutex != nullptr);
    CHECK(init_event != nullptr);
    CHECK(init_light != nullptr);
    CHECK(init_semaphore != nullptr);

    /* Attach clears sync objects, so initialize them afterwards.  The event
     * and light event are auto-clear; the semaphore starts empty. */
    init_mutex(mutex, 0, 0x20);
    init_event(event, 0, 1);
    init_light(light, 0, 1);
    init_semaphore(semaphore, 0, 1);
    CHECK(os.mutexes[0].used);
    CHECK(os.events[0].used);
    CHECK(os.light_events[0].used);
    CHECK(os.semaphores[0].used);

    CHECK(create(worker_a_type, &botw_guest_sync_worker_a_entry, &args,
                 nullptr, 0x8000, 0x20, 0) == 0);
    CHECK(create(worker_b_type, &botw_guest_sync_worker_b_entry, &args,
                 nullptr, 0x8000, 0x20, 0) == 0);
    CHECK(create(worker_c_type, &botw_guest_sync_worker_c_entry, &args,
                 nullptr, 0x8000, 0x20, 0) == 0);
    CHECK(create(worker_d_type, &botw_guest_sync_worker_d_entry, &args,
                 nullptr, 0x8000, 0x20, 0) == 0);
    CHECK(run.threads[1].state == BOTW_GUEST_THREAD_CREATED);
    CHECK(run.threads[2].state == BOTW_GUEST_THREAD_CREATED);
    CHECK(run.threads[3].state == BOTW_GUEST_THREAD_CREATED);
    CHECK(run.threads[4].state == BOTW_GUEST_THREAD_CREATED);

    args.counter = counter;
    args.lock_mutex = botw_hle_lookup(&hle, "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE");
    args.unlock_mutex = botw_hle_lookup(&hle, "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE");
    args.wait_event = botw_hle_lookup(&hle, "_ZN2nn2os9WaitEventEPNS0_9EventTypeE");
    args.signal_event = botw_hle_lookup(&hle, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    args.wait_light = botw_hle_lookup(&hle, "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE");
    args.signal_light = botw_hle_lookup(&hle, "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE");
    args.timed_wait_light = botw_hle_lookup(
        &hle, "_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE");
    args.acquire_semaphore = botw_hle_lookup(
        &hle, "_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE");
    args.release_semaphore = botw_hle_lookup(
        &hle, "_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE");
    args.wait_thread = botw_hle_lookup(&hle, "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE");
    args.start_thread = (void*)start;
    args.mutex = mutex;
    args.event = event;
    args.light = light;
    args.semaphore = semaphore;
    args.worker_a = worker_a_type;
    args.worker_b = worker_b_type;
    args.worker_c = worker_c_type;
    args.worker_d = worker_d_type;
    args.timeout_ns = 2000000;
    CHECK(args.lock_mutex != nullptr);
    CHECK(args.unlock_mutex != nullptr);
    CHECK(args.wait_event != nullptr);
    CHECK(args.signal_event != nullptr);
    CHECK(args.wait_light != nullptr);
    CHECK(args.signal_light != nullptr);
    CHECK(args.timed_wait_light != nullptr);
    CHECK(args.acquire_semaphore != nullptr);
    CHECK(args.release_semaphore != nullptr);
    CHECK(args.wait_thread != nullptr);

    run.args[0] = (uint64_t)(uintptr_t)&args;
    CHECK(botw_guest_run_enter_at(&run, (const void*)&botw_guest_sync_main_entry) == 0);
    CHECK(run.error[0] == '\0');
    CHECK(run.reason == BOTW_GUEST_EXIT_RETURNED);

    CHECK(counter[0] == 1);             /* worker-a acquired */
    CHECK(counter[1] == 1);             /* worker-a unlocked */
    CHECK(counter[2] == 0);             /* phase-1 timed wait expired */
    CHECK(counter[3] == 1);             /* main joined worker-a */
    CHECK(counter[4] == 1);             /* worker-b signalled */
    CHECK(counter[5] == 1);             /* main WaitEvent returned true */
    CHECK(counter[6] == 1);             /* worker-c acquired after release */
    CHECK(counter[7] == 1);             /* main WaitEvent returned true */
    CHECK(counter[8] == 1);             /* WaitLightEvent returned */
    CHECK(counter[9] == 0);             /* phase-5 timed wait expired */
    CHECK(counter[10] == 1);            /* worker-d signalled */
    CHECK(counter[11] == 1);            /* main reached the end */
    CHECK(counter[12] == 1);            /* worker-d returned */
    CHECK(counter[13] == 1);            /* worker-c signalled */

    CHECK(run.blocks == 9);
    CHECK(run.wakes == 9);
    CHECK(run.switches == 12);
    CHECK(run.thread_exits == 4);
    CHECK(os.thread_creates == 4);
    CHECK(os.thread_starts == 4);
    CHECK(os.mutex_blocks == 1);
    CHECK(os.mutex_locks == 2);
    CHECK(os.mutex_unlocks == 2);
    CHECK(os.event_blocks == 2);
    CHECK(os.event_waits == 2);
    CHECK(os.event_signals == 2);
    CHECK(os.light_event_blocks == 3);
    CHECK(os.light_event_waits == 3);
    CHECK(os.light_event_signals == 1);
    CHECK(os.light_event_timeouts == 2);
    CHECK(os.semaphore_blocks == 1);
    CHECK(os.semaphore_acquires == 1);
    CHECK(os.semaphore_releases == 1);
    CHECK(os.mutexes[0].owner == -1);
    CHECK(os.mutexes[0].depth == 0);
    CHECK(os.events[0].signaled == 0);
    CHECK(os.light_events[0].signaled == 0);
    CHECK(os.semaphores[0].count == 0);
    CHECK(os.events[0].waiter_count == 0);
    CHECK(os.light_events[0].waiter_count == 0);
    CHECK(os.semaphores[0].waiter_count == 0);

    if (g_fail) {
        std::printf("guest_sync_test: %d failure(s)\n", g_fail);
        std::printf("  run: blocks=%lu wakes=%lu switches=%lu exits=%lu clock=%llu\n",
                    run.blocks, run.wakes, run.switches, run.thread_exits,
                    (unsigned long long)run.clock);
        std::printf("  os: mutex=%lu/%lu/%lu event=%lu/%lu/%lu light=%lu/%lu/%lu/%lu sem=%lu/%lu/%lu\n",
                    os.mutex_blocks, os.mutex_locks, os.mutex_unlocks,
                    os.event_blocks, os.event_waits, os.event_signals,
                    os.light_event_blocks, os.light_event_waits,
                    os.light_event_signals, os.light_event_timeouts,
                    os.semaphore_blocks, os.semaphore_acquires,
                    os.semaphore_releases);
        std::printf("  counter:");
        for (int i = 0; i < 14; i++) std::printf(" %llu", (unsigned long long)counter[i]);
        std::printf("\n  states: mutex.owner=%d depth=%d event=%d light=%d sem=%d error='%s'\n",
                    os.mutexes[0].owner, os.mutexes[0].depth,
                    os.events[0].signaled, os.light_events[0].signaled,
                    os.semaphores[0].count, run.error);
    }

    unsigned long blocks = run.blocks;
    unsigned long wakes = run.wakes;
    botw_hle_os_attach_run(&os, nullptr);
    botw_guest_run_close(&run);

    if (g_fail) return 1;
    std::printf("guest_sync_test: ok (%lu blocks, %lu wakes, mutex/event/light/semaphore)\n",
                blocks, wakes);
    return 0;
}

#endif

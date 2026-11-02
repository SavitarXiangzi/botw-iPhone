/* SystemEvent and MultiWait HLE: blocking wait, timed wait and WaitAny.
 *
 * Two guest workers are created before the run starts.  Main blocks on an
 * auto-clear SystemEvent while worker-sys signals it, then has no runnable
 * peer for a timed SystemEvent wait, so the virtual clock must advance to the
 * deadline.  It then links two Events into a MultiWait, blocks in WaitAny
 * while worker-mw signals Event 1, and finally observes an already-signaled
 * Event 2 without blocking.  WaitAny must return the holder pointer without
 * consuming an auto-clear Event; the guest clears it with TryWaitEvent.
 *
 * All object memory lives in host arrays so the test can inspect the HLE
 * tables after the guest returns.  On x86 the test skips; guest_hle_os_test
 * covers the portable nonblocking paths.
 */
#include "guest_hle.h"
#include "guest_hle_os.h"
#include "guest_run.h"

#include <cstdint>
#include <cstdio>

#if defined(_WIN32) || !defined(__aarch64__)

int main() {
    std::printf("guest_system_event_test: skipped (needs AArch64 + mmap)\n");
    return 0;
}

#else

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_system_event_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

extern "C" void botw_guest_se_mw_main_entry(void* args);
extern "C" void botw_guest_se_mw_worker_sys_entry(void* args);
extern "C" void botw_guest_se_mw_worker_mw_entry(void* args);

struct se_mw_probe_args {
    uint64_t* counter;
    void* start_thread;
    void* wait_thread;
    void* init_system_event;
    void* signal_system_event;
    void* wait_system_event;
    void* timed_wait_system_event;
    void* try_wait_system_event;
    void* init_event;
    void* try_wait_event;
    void* signal_event;
    void* init_multi_wait;
    void* init_multi_wait_holder;
    void* link_multi_wait_holder;
    void* wait_any;
    void* system_event_a;
    void* system_event_b;
    void* event_1;
    void* event_2;
    void* multi_wait;
    void* holder_1;
    void* holder_2;
    void* worker_sys;
    void* worker_mw;
    int64_t timeout_ns;
};

typedef uint32_t (*os_create7_fn)(void*, void (*)(void*), void*, void*,
                                  size_t, int, int);

int main() {
    botw_hle_table hle;
    botw_hle_os_state os;
    botw_guest_run run;
    uint64_t worker_sys_type[64] = {0};
    uint64_t worker_mw_type[64] = {0};
    uint64_t system_event_a[16] = {0};
    uint64_t system_event_b[16] = {0};
    uint64_t event_1[16] = {0};
    uint64_t event_2[16] = {0};
    uint64_t multi_wait[16] = {0};
    uint64_t holder_1[16] = {0};
    uint64_t holder_2[16] = {0};
    uint64_t counter[16] = {0};
    se_mw_probe_args args;
    os_create7_fn create;

    botw_hle_init(&hle);
    CHECK(hle.error[0] == '\0');
    botw_hle_os_reset(&os);
    CHECK(botw_hle_os_register(&hle, &os) == 0);
    CHECK(botw_guest_run_init(&run, nullptr, 0, 0x8000) == 0);
    botw_hle_os_attach_run(&os, &run);

    create = (os_create7_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii");
    CHECK(create != nullptr);
    CHECK(create(worker_sys_type, &botw_guest_se_mw_worker_sys_entry, &args,
                 nullptr, 0x8000, 0x20, 0) == 0);
    CHECK(create(worker_mw_type, &botw_guest_se_mw_worker_mw_entry, &args,
                 nullptr, 0x8000, 0x20, 0) == 0);
    CHECK(run.threads[1].state == BOTW_GUEST_THREAD_CREATED);
    CHECK(run.threads[2].state == BOTW_GUEST_THREAD_CREATED);

    args.counter = counter;
    args.start_thread = botw_hle_lookup(
        &hle, "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    args.wait_thread = botw_hle_lookup(
        &hle, "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE");
    args.init_system_event = botw_hle_lookup(
        &hle, "_ZN2nn2os21InitializeSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeE");
    args.signal_system_event = botw_hle_lookup(
        &hle, "_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    args.wait_system_event = botw_hle_lookup(
        &hle, "_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE");
    args.timed_wait_system_event = botw_hle_lookup(
        &hle, "_ZN2nn2os20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE");
    args.try_wait_system_event = botw_hle_lookup(
        &hle, "_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE");
    args.init_event = botw_hle_lookup(
        &hle, "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE");
    args.try_wait_event = botw_hle_lookup(
        &hle, "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE");
    args.signal_event = botw_hle_lookup(
        &hle, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    args.init_multi_wait = botw_hle_lookup(
        &hle, "_ZN2nn2os19InitializeMultiWaitEPNS0_13MultiWaitTypeE");
    args.init_multi_wait_holder = botw_hle_lookup(
        &hle, "_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE");
    args.link_multi_wait_holder = botw_hle_lookup(
        &hle, "_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE");
    args.wait_any = botw_hle_lookup(
        &hle, "_ZN2nn2os7WaitAnyEPNS0_13MultiWaitTypeE");
    args.system_event_a = system_event_a;
    args.system_event_b = system_event_b;
    args.event_1 = event_1;
    args.event_2 = event_2;
    args.multi_wait = multi_wait;
    args.holder_1 = holder_1;
    args.holder_2 = holder_2;
    args.worker_sys = worker_sys_type;
    args.worker_mw = worker_mw_type;
    args.timeout_ns = 2000000;

    CHECK(args.start_thread != nullptr);
    CHECK(args.wait_thread != nullptr);
    CHECK(args.init_system_event != nullptr);
    CHECK(args.signal_system_event != nullptr);
    CHECK(args.wait_system_event != nullptr);
    CHECK(args.timed_wait_system_event != nullptr);
    CHECK(args.try_wait_system_event != nullptr);
    CHECK(args.init_event != nullptr);
    CHECK(args.try_wait_event != nullptr);
    CHECK(args.signal_event != nullptr);
    CHECK(args.init_multi_wait != nullptr);
    CHECK(args.init_multi_wait_holder != nullptr);
    CHECK(args.link_multi_wait_holder != nullptr);
    CHECK(args.wait_any != nullptr);

    run.args[0] = (uint64_t)(uintptr_t)&args;
    CHECK(botw_guest_run_enter_at(
              &run, (const void*)&botw_guest_se_mw_main_entry) == 0);
    CHECK(run.error[0] == '\0');
    CHECK(run.reason == BOTW_GUEST_EXIT_RETURNED);

    CHECK(counter[0] == 1);              /* worker-sys entered */
    CHECK(counter[1] == 1);              /* worker-sys signalled */
    CHECK(counter[2] == 1);              /* WaitSystemEvent returned */
    CHECK(counter[3] == 0);              /* timed wait expired */
    CHECK(counter[4] == 1);              /* WaitAny selected holder 1 */
    CHECK(counter[5] == 1);              /* TryWaitEvent(Event 1) */
    CHECK(counter[6] == 1);              /* worker-mw entered */
    CHECK(counter[7] == 1);              /* worker-mw signalled Event 1 */
    CHECK(counter[8] == 1);              /* main joined worker-sys */
    CHECK(counter[9] == 1);              /* main joined worker-mw */
    CHECK(counter[10] == 1);             /* main reached the end */
    CHECK(counter[11] == 1);             /* immediate WaitAny holder 2 */
    CHECK(counter[12] == 1);             /* TryWaitEvent(Event 2) */

    CHECK(os.system_event_waits == 2);
    CHECK(os.system_event_blocks == 2);
    CHECK(os.system_event_signals == 1);
    CHECK(os.system_event_timeouts == 1);
    CHECK(os.multi_wait_waits == 2);
    CHECK(os.multi_wait_blocks == 1);
    CHECK(os.multi_wait_wakes == 1);
    CHECK(os.multi_wait_links == 2);
    CHECK(os.event_waits == 2);
    CHECK(os.event_signals == 2);

    CHECK(os.system_events[0].used);
    CHECK(os.system_events[0].signaled == 0);
    CHECK(os.system_events[1].used);
    CHECK(os.system_events[1].signaled == 0);
    CHECK(os.events[0].signaled == 0);
    CHECK(os.events[1].signaled == 0);
    CHECK(os.multi_waits[0].holder_count == 2);
    CHECK(os.multi_waits[0].waiter == -1);
    CHECK(os.multi_wait_holders[0].owner == 0);
    CHECK(os.multi_wait_holders[1].owner == 0);

    CHECK(run.blocks == 3);
    CHECK(run.wakes == 3);
    CHECK(run.switches == 4);
    CHECK(run.thread_exits == 2);
    CHECK(os.thread_creates == 2);
    CHECK(os.thread_starts == 2);

    if (g_fail) {
        std::printf("guest_system_event_test: %d failure(s)\n", g_fail);
        std::printf("  run: blocks=%lu wakes=%lu switches=%lu exits=%lu clock=%llu error='%s'\n",
                    run.blocks, run.wakes, run.switches, run.thread_exits,
                    (unsigned long long)run.clock, run.error);
        std::printf("  se: waits=%lu blocks=%lu signals=%lu timeouts=%lu\n",
                    os.system_event_waits, os.system_event_blocks,
                    os.system_event_signals, os.system_event_timeouts);
        std::printf("  mw: waits=%lu blocks=%lu wakes=%lu links=%lu\n",
                    os.multi_wait_waits, os.multi_wait_blocks,
                    os.multi_wait_wakes, os.multi_wait_links);
        std::printf("  counter:");
        for (int i = 0; i < 13; i++) {
            std::printf(" %llu", (unsigned long long)counter[i]);
        }
        std::printf("\n");
    }

    botw_hle_os_attach_run(&os, nullptr);
    botw_guest_run_close(&run);

    if (g_fail) return 1;
    std::printf("guest_system_event_test: ok (SystemEvent wait/timeout, WaitAny holder handoff)\n");
    return 0;
}

#endif

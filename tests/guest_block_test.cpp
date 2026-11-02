/* Blocking nn::os imports: BLOCKED, wake, and virtual-clock sleep.
 *
 * This runs real AArch64 guest code.  Main starts a worker and immediately
 * calls WaitThread before the worker has run; the blocking import wrapper
 * saves main's frame, switches to the worker, and the worker-exit path wakes
 * main.  Main then calls SleepThread with no runnable peer, forcing the
 * scheduler to advance the virtual clock to the deadline.  The counters are
 * written only after each resume, so a wrapper that returned early would
 * leave them zero and a lost frame would fault.
 *
 * On x86 the test skips; the portable HLE test still covers state/counters.
 */
#include "guest_hle.h"
#include "guest_hle_os.h"
#include "guest_run.h"

#include <cstdint>
#include <cstdio>

#if defined(_WIN32) || !defined(__aarch64__)

int main() {
    std::printf("guest_block_test: skipped (needs AArch64 + mmap)\n");
    return 0;
}

#else

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_block_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

extern "C" void botw_guest_block_main_entry(void* args);
extern "C" void botw_guest_block_worker_entry(void* args);

struct block_probe_args {
    uint64_t* counter;
    void*     wait_thread;
    void*     sleep_thread;
    void*     get_tick;
    void*     worker_type;
    int64_t   sleep_ns;
};

typedef uint32_t (*os_create7_fn)(void*, void (*)(void*), void*, void*,
                                  size_t, int, int);
typedef void (*os_start_fn)(void*);

int main() {
    botw_hle_table hle;
    botw_hle_os_state os;
    botw_guest_run run;
    uint64_t worker_type[64] = {0};
    uint64_t counter[5] = {0, 0, 0, 0, 0};
    const int64_t sleep_ns = 5000000;
    block_probe_args args;
    os_create7_fn create;
    os_start_fn start;
    void* wait_fn;
    void* sleep_fn;
    void* tick_fn;
    int worker;

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
    wait_fn = botw_hle_lookup(&hle, "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE");
    sleep_fn = botw_hle_lookup(&hle, "_ZN2nn2os11SleepThreadENS_8TimeSpanE");
    tick_fn = botw_hle_lookup(&hle, "_ZN2nn2os13GetSystemTickEv");
    CHECK(create != nullptr);
    CHECK(start != nullptr);
    CHECK(wait_fn != nullptr);
    CHECK(sleep_fn != nullptr);
    CHECK(tick_fn != nullptr);

    CHECK(create(worker_type, &botw_guest_block_worker_entry, counter, nullptr,
                 0x8000, 0x20, 0) == 0);
    worker = os.threads[1].slot;
    CHECK(worker == 1);
    CHECK(run.threads[worker].state == BOTW_GUEST_THREAD_CREATED);
    start(worker_type);
    CHECK(run.threads[worker].state == BOTW_GUEST_THREAD_READY);

    args.counter = counter;
    args.wait_thread = wait_fn;
    args.sleep_thread = sleep_fn;
    args.get_tick = tick_fn;
    args.worker_type = worker_type;
    args.sleep_ns = sleep_ns;
    run.args[0] = (uint64_t)(uintptr_t)&args;

    CHECK(botw_guest_run_enter_at(&run, (const void*)&botw_guest_block_main_entry) == 0);
    CHECK(run.error[0] == '\0');
    CHECK(run.reason == BOTW_GUEST_EXIT_RETURNED);
    CHECK(run.blocks == 2);
    CHECK(run.wakes >= 2);
    CHECK(run.switches == 2);          /* main -> worker, worker -> main */
    CHECK(run.thread_exits == 1);
    CHECK(os.thread_waits == 1);
    CHECK(os.wait_blocks == 1);
    CHECK(os.sleeps == 1);
    CHECK(os.sleep_blocks == 1);
    CHECK(counter[0] == 1);            /* worker ran while main blocked */
    CHECK(counter[1] == 1);            /* main resumed after WaitThread */
    CHECK(counter[2] == 1);            /* main resumed after SleepThread */
    CHECK(counter[3] != 0);
    CHECK(counter[4] >= counter[3] + (uint64_t)(BOTW_HLE_OS_TICK_FREQ / 200u));
    CHECK(run.clock >= counter[4]);

    CHECK(run.current == -1);
    CHECK(run.threads[0].state == BOTW_GUEST_THREAD_DONE);
    CHECK(run.threads[worker].state == BOTW_GUEST_THREAD_DONE);
    CHECK(run.threads[0].wait_kind == BOTW_GUEST_WAIT_NONE);
    CHECK(run.threads[worker].wait_kind == BOTW_GUEST_WAIT_NONE);

    unsigned long blocks = run.blocks;
    unsigned long wakes = run.wakes;
    botw_hle_os_attach_run(&os, nullptr);
    botw_guest_run_close(&run);

    if (g_fail) {
        std::printf("guest_block_test: %d failure(s)\n", g_fail);
        std::printf("  run: blocks=%lu wakes=%lu switches=%lu exits=%lu clock=%llu\n",
                    run.blocks, run.wakes, run.switches, run.thread_exits,
                    (unsigned long long)run.clock);
        std::printf("  os: calls=%lu creates=%lu starts=%lu waits=%lu "
                    "wait_blocks=%lu sleeps=%lu sleep_blocks=%lu\n",
                    os.calls, os.thread_creates, os.thread_starts,
                    os.thread_waits, os.wait_blocks, os.sleeps,
                    os.sleep_blocks);
        std::printf("  counter=%llu,%llu,%llu,%llu,%llu states=%d,%d error='%s'\n",
                    (unsigned long long)counter[0], (unsigned long long)counter[1],
                    (unsigned long long)counter[2], (unsigned long long)counter[3],
                    (unsigned long long)counter[4], run.threads[0].state,
                    run.threads[worker].state, run.error);
        return 1;
    }
    std::printf("guest_block_test: ok (wait join, timed wake, %lu blocks, %lu wakes)\n",
                blocks, wakes);
    return 0;
}

#endif

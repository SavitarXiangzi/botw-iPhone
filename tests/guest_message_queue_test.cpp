/* MessageQueue HLE: FIFO handoff, pending sender admission and LIFO jam.
 *
 * The queue objects and ring buffers live in host memory in this test, but
 * every operation enters through the registered import.  The AArch64
 * blocking wrappers save the caller's frame, so the received value must be
 * written into counter memory before the receiver is made READY.
 *
 * On x86 the test skips; guest_hle_os_test exercises the nonblocking path and
 * both queue orderings there.
 */
#include "guest_hle.h"
#include "guest_hle_os.h"
#include "guest_run.h"

#include <cstdint>
#include <cstdio>

#if defined(_WIN32) || !defined(__aarch64__)

int main() {
    std::printf("guest_message_queue_test: skipped (needs AArch64 + mmap)\n");
    return 0;
}

#else

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_message_queue_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

extern "C" void botw_guest_mq_main_entry(void* args);
extern "C" void botw_guest_mq_worker_recv_entry(void* args);
extern "C" void botw_guest_mq_worker_send_entry(void* args);
extern "C" void botw_guest_mq_worker_jam_entry(void* args);

struct mq_probe_args {
    uint64_t* counter;
    void* sleep_thread;
    void* send_message;
    void* jam_message;
    void* receive_message;
    void* try_send_message;
    void* try_receive_message;
    void* try_peek_message;
    void* wait_thread;
    void* start_thread;
    void* receive_queue;
    void* send_queue;
    void* lifo_queue;
    void* worker_recv;
    void* worker_send;
    void* worker_jam;
    int64_t sleep_ns;
};

typedef uint32_t (*os_create7_fn)(void*, void (*)(void*), void*, void*,
                                  size_t, int, int);
typedef void (*os_start_fn)(void*);
typedef void (*mq_init_fn)(void*, size_t*, size_t);
typedef void (*mq_fini_fn)(void*);

int main() {
    botw_hle_table hle;
    botw_hle_os_state os;
    botw_guest_run run;
    uint64_t worker_recv_type[64] = {0};
    uint64_t worker_send_type[64] = {0};
    uint64_t worker_jam_type[64] = {0};
    uint64_t receive_queue[16] = {0};
    uint64_t send_queue[16] = {0};
    uint64_t lifo_queue[16] = {0};
    size_t receive_buffer[1] = {0};
    size_t send_buffer[1] = {0};
    size_t lifo_buffer[3] = {0};
    uint64_t counter[24] = {0};
    mq_probe_args args;
    os_create7_fn create;
    os_start_fn start;
    mq_init_fn mq_init;
    mq_fini_fn mq_fini;

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
    mq_init = (mq_init_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm");
    mq_fini = (mq_fini_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os20FinalizeMessageQueueEPNS0_16MessageQueueTypeE");
    CHECK(create != nullptr);
    CHECK(start != nullptr);
    CHECK(mq_init != nullptr);
    CHECK(mq_fini != nullptr);

    mq_init(receive_queue, receive_buffer, 1);
    mq_init(send_queue, send_buffer, 1);
    mq_init(lifo_queue, lifo_buffer, 3);
    CHECK(os.message_queues[0].used);
    CHECK(os.message_queues[1].used);
    CHECK(os.message_queues[2].used);

    CHECK(create(worker_recv_type, &botw_guest_mq_worker_recv_entry, &args,
                 nullptr, 0x8000, 0x20, 0) == 0);
    CHECK(create(worker_send_type, &botw_guest_mq_worker_send_entry, &args,
                 nullptr, 0x8000, 0x20, 0) == 0);
    CHECK(create(worker_jam_type, &botw_guest_mq_worker_jam_entry, &args,
                 nullptr, 0x8000, 0x20, 0) == 0);

    args.counter = counter;
    args.sleep_thread = botw_hle_lookup(&hle, "_ZN2nn2os11SleepThreadENS_8TimeSpanE");
    args.send_message = botw_hle_lookup(
        &hle, "_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm");
    args.jam_message = botw_hle_lookup(
        &hle, "_ZN2nn2os15JamMessageQueueEPNS0_16MessageQueueTypeEm");
    args.receive_message = botw_hle_lookup(
        &hle, "_ZN2nn2os19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE");
    args.try_send_message = botw_hle_lookup(
        &hle, "_ZN2nn2os19TrySendMessageQueueEPNS0_16MessageQueueTypeEm");
    args.try_receive_message = botw_hle_lookup(
        &hle, "_ZN2nn2os22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE");
    args.try_peek_message = botw_hle_lookup(
        &hle, "_ZN2nn2os19TryPeekMessageQueueEPmPKNS0_16MessageQueueTypeE");
    args.wait_thread = botw_hle_lookup(&hle, "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE");
    args.start_thread = (void*)start;
    args.receive_queue = receive_queue;
    args.send_queue = send_queue;
    args.lifo_queue = lifo_queue;
    args.worker_recv = worker_recv_type;
    args.worker_send = worker_send_type;
    args.worker_jam = worker_jam_type;
    args.sleep_ns = 1000000;

    CHECK(args.sleep_thread != nullptr);
    CHECK(args.send_message != nullptr);
    CHECK(args.jam_message != nullptr);
    CHECK(args.receive_message != nullptr);
    CHECK(args.try_send_message != nullptr);
    CHECK(args.try_receive_message != nullptr);
    CHECK(args.try_peek_message != nullptr);
    CHECK(args.wait_thread != nullptr);

    run.args[0] = (uint64_t)(uintptr_t)&args;
    CHECK(botw_guest_run_enter_at(&run, (const void*)&botw_guest_mq_main_entry) == 0);
    CHECK(run.error[0] == '\0');
    CHECK(run.reason == BOTW_GUEST_EXIT_RETURNED);

    CHECK(counter[0] == 1);                 /* receiver entered */
    CHECK(counter[1] == 1);                 /* receiver resumed */
    CHECK(counter[2] == 1);                 /* main sent */
    CHECK(counter[3] == 1);                 /* sender entered */
    CHECK(counter[4] == 1);                 /* sender resumed */
    CHECK(counter[5] == 1);                 /* jammer entered */
    CHECK(counter[6] == 1);                 /* jammer resumed */
    CHECK(counter[7] == 1);                 /* main reached the end */
    CHECK(counter[8] == 0xAAAA);            /* direct receiver handoff */
    CHECK(counter[9] == 0x1111);            /* first queued message */
    CHECK(counter[10] == 0x2222);           /* admitted blocked sender */
    CHECK(counter[11] == 1);                /* TryPeek succeeded */
    CHECK(counter[12] == 3);                /* Jam puts the newest first */
    CHECK(counter[13] == 3);
    CHECK(counter[14] == 2);
    CHECK(counter[15] == 1);
    CHECK(counter[16] == 0);                /* empty TryReceive */
    CHECK(counter[17] == 0x10);
    CHECK(counter[18] == 0x20);
    CHECK(counter[19] == 0);                /* full TrySend */

    CHECK(os.message_queues[0].count == 0);
    CHECK(os.message_queues[1].count == 0);
    CHECK(os.message_queues[2].count == 0);
    CHECK(os.message_queues[0].receiver_count == 0);
    CHECK(os.message_queues[1].sender_count == 0);
    CHECK(os.message_queues[2].receiver_count == 0);

    CHECK(os.message_sends == 4);
    CHECK(os.message_send_blocks == 1);
    CHECK(os.message_jams == 4);
    CHECK(os.message_jam_blocks == 1);
    CHECK(os.message_receives == 8);
    CHECK(os.message_receive_blocks == 1);
    CHECK(os.message_try_sends == 1);
    CHECK(os.message_try_receives == 1);
    CHECK(os.message_peeks == 1);

    CHECK(run.blocks == 9);
    CHECK(run.wakes == 9);
    CHECK(run.thread_exits == 3);
    CHECK(os.thread_creates == 3);
    CHECK(os.thread_starts == 3);

    if (g_fail) {
        std::printf("guest_message_queue_test: %d failure(s)\n", g_fail);
        std::printf("  run: blocks=%lu wakes=%lu switches=%lu exits=%lu clock=%llu\n",
                    run.blocks, run.wakes, run.switches, run.thread_exits,
                    (unsigned long long)run.clock);
        std::printf("  mq: sends=%lu/%lu jams=%lu/%lu recv=%lu/%lu try=%lu/%lu peek=%lu\n",
                    os.message_sends, os.message_send_blocks,
                    os.message_jams, os.message_jam_blocks,
                    os.message_receives, os.message_receive_blocks,
                    os.message_try_sends, os.message_try_receives,
                    os.message_peeks);
        std::printf("  counter:");
        for (int i = 0; i < 20; i++) std::printf(" %llu", (unsigned long long)counter[i]);
        std::printf("\n  queue counts=%zu/%zu/%zu waiters=%d/%d error='%s'\n",
                    os.message_queues[0].count, os.message_queues[1].count,
                    os.message_queues[2].count,
                    os.message_queues[1].sender_count,
                    os.message_queues[0].receiver_count, run.error);
    }

    mq_fini(receive_queue);
    mq_fini(send_queue);
    mq_fini(lifo_queue);
    CHECK(!os.message_queues[0].used);

    botw_hle_os_attach_run(&os, nullptr);
    botw_guest_run_close(&run);

    if (g_fail) return 1;
    std::printf("guest_message_queue_test: ok (%lu blocks, %lu wakes, FIFO send/LIFO jam)\n",
                (unsigned long)9, (unsigned long)9);
    return 0;
}

#endif

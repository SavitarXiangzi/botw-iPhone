/*
 * guest_message_queue_probe_arm64.S -- blocking MessageQueue handoff.
 *
 * void botw_guest_mq_main_entry(struct mq_probe_args* args);
 * void botw_guest_mq_worker_recv_entry(struct mq_probe_args* args);
 * void botw_guest_mq_worker_send_entry(struct mq_probe_args* args);
 * void botw_guest_mq_worker_jam_entry(struct mq_probe_args* args);
 *
 * mq_probe_args (all fields are 8 bytes):
 *   +0    uint64_t* counter
 *   +8    SleepThread
 *   +16   SendMessageQueue
 *   +24   JamMessageQueue
 *   +32   ReceiveMessageQueue
 *   +40   TrySendMessageQueue
 *   +48   TryReceiveMessageQueue
 *   +56   TryPeekMessageQueue
 *   +64   WaitThread
 *   +72   StartThread
 *   +80   receive queue (capacity 1)
 *   +88   send/jam queue (capacity 1)
 *   +96   LIFO queue (capacity 3)
 *   +104  worker-recv ThreadType*
 *   +112  worker-send ThreadType*
 *   +120  worker-jam ThreadType*
 *   +128  sleep nanoseconds
 *
 * Counter slots:
 *   0 worker-recv entered Receive        1 worker-recv resumed
 *   2 main sent to the blocked receiver  3 worker-send entered Send
 *   4 worker-send resumed               5 worker-jam entered Jam
 *   6 worker-jam resumed                7 main reached the end
 *   8 message written by main to worker 9 first receive of 0x1111
 *  10 pending sender message 0x2222     11 TryPeek returned true
 *  12 TryPeek value 3                   13/14/15 LIFO receives 3/2/1
 *  16 TryReceive on empty returned false
 *  17/18 blocked-jam receives 0x10/0x20 19 TrySend on full returned false
 *
 * SleepThread is used as the scheduling boundary: the worker is READY, main
 * parks with a deadline, the worker blocks, and the virtual clock then wakes
 * main.  No real time passes.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define MQ_SYM(name) _##name
#else
#  define MQ_SYM(name) name
#endif

    .text
    .p2align 2
    .globl MQ_SYM(botw_guest_mq_main_entry)
#if defined(__ELF__)
    .type MQ_SYM(botw_guest_mq_main_entry), %function
#endif
MQ_SYM(botw_guest_mq_main_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]

    /* Phase 1: worker-recv blocks on an empty queue; main sends directly. */
    ldr  x0, [x19, #104]
    ldr  x9, [x19, #72]
    blr  x9                         /* start worker-recv */
    ldr  x0, [x19, #128]
    ldr  x9, [x19, #8]
    blr  x9                         /* sleep -> worker-recv blocks */
    ldr  x0, [x19, #80]
    movz x1, #0xAAAA
    ldr  x9, [x19, #16]
    blr  x9                         /* direct handoff to blocked receiver */
    mov  x9, #1
    str  x9, [x20, #16]
    ldr  x0, [x19, #104]
    ldr  x9, [x19, #64]
    blr  x9                         /* join worker-recv */

    /* Phase 2: queue full, worker-send blocks; main receives twice. */
    ldr  x0, [x19, #88]
    movz x1, #0x1111
    ldr  x9, [x19, #16]
    blr  x9
    ldr  x0, [x19, #112]
    ldr  x9, [x19, #72]
    blr  x9                         /* start worker-send */
    ldr  x0, [x19, #128]
    ldr  x9, [x19, #8]
    blr  x9                         /* sleep -> worker-send blocks */
    add  x0, x20, #72               /* counter + 9 */
    ldr  x1, [x19, #88]
    ldr  x9, [x19, #32]
    blr  x9                         /* receive 0x1111, admit sender */
    add  x0, x20, #80               /* counter + 10 */
    ldr  x1, [x19, #88]
    ldr  x9, [x19, #32]
    blr  x9                         /* receive the pending 0x2222 */
    ldr  x0, [x19, #112]
    ldr  x9, [x19, #64]
    blr  x9                         /* join worker-send */

    /* Phase 3: Jam is LIFO; TryPeek does not consume; TrySend sees full. */
    ldr  x0, [x19, #96]
    mov  x1, #1
    ldr  x9, [x19, #24]
    blr  x9
    ldr  x0, [x19, #96]
    mov  x1, #2
    ldr  x9, [x19, #24]
    blr  x9
    ldr  x0, [x19, #96]
    mov  x1, #3
    ldr  x9, [x19, #24]
    blr  x9
    ldr  x0, [x19, #96]
    mov  x1, #4
    ldr  x9, [x19, #40]
    blr  x9                         /* TrySend on full -> false */
    str  x0, [x20, #152]            /* counter + 19 */
    add  x0, x20, #96               /* counter + 12 */
    ldr  x1, [x19, #96]
    ldr  x9, [x19, #56]
    blr  x9                         /* TryPeek -> true, value 3 */
    str  x0, [x20, #88]             /* counter + 11 */
    add  x0, x20, #104              /* counter + 13 */
    ldr  x1, [x19, #96]
    ldr  x9, [x19, #32]
    blr  x9
    add  x0, x20, #112              /* counter + 14 */
    ldr  x1, [x19, #96]
    ldr  x9, [x19, #32]
    blr  x9
    add  x0, x20, #120              /* counter + 15 */
    ldr  x1, [x19, #96]
    ldr  x9, [x19, #32]
    blr  x9
    add  x0, x20, #128              /* counter + 16 */
    ldr  x1, [x19, #96]
    ldr  x9, [x19, #48]
    blr  x9                         /* TryReceive on empty -> false */
    str  x0, [x20, #128]

    /* Phase 4: full queue makes Jam block; receive admits it at the front. */
    ldr  x0, [x19, #88]
    mov  x1, #0x10
    ldr  x9, [x19, #16]
    blr  x9
    ldr  x0, [x19, #120]
    ldr  x9, [x19, #72]
    blr  x9                         /* start worker-jam */
    ldr  x0, [x19, #128]
    ldr  x9, [x19, #8]
    blr  x9                         /* sleep -> worker-jam blocks */
    add  x0, x20, #136              /* counter + 17 */
    ldr  x1, [x19, #88]
    ldr  x9, [x19, #32]
    blr  x9                         /* receive 0x10, admit the jam */
    add  x0, x20, #144              /* counter + 18 */
    ldr  x1, [x19, #88]
    ldr  x9, [x19, #32]
    blr  x9                         /* receive 0x20 */
    ldr  x0, [x19, #120]
    ldr  x9, [x19, #64]
    blr  x9                         /* join worker-jam */

    mov  x9, #1
    str  x9, [x20, #56]             /* counter + 7 */
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size MQ_SYM(botw_guest_mq_main_entry), . - MQ_SYM(botw_guest_mq_main_entry)
#endif

    .p2align 2
    .globl MQ_SYM(botw_guest_mq_worker_recv_entry)
#if defined(__ELF__)
    .type MQ_SYM(botw_guest_mq_worker_recv_entry), %function
#endif
MQ_SYM(botw_guest_mq_worker_recv_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]
    mov  x9, #1
    str  x9, [x20, #0]              /* worker-recv entered */
    add  x0, x20, #64               /* counter + 8: receive output */
    ldr  x1, [x19, #80]
    ldr  x9, [x19, #32]
    blr  x9                         /* blocks until main sends */
    mov  x9, #1
    str  x9, [x20, #8]              /* worker-recv resumed */
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size MQ_SYM(botw_guest_mq_worker_recv_entry), . - MQ_SYM(botw_guest_mq_worker_recv_entry)
#endif

    .p2align 2
    .globl MQ_SYM(botw_guest_mq_worker_send_entry)
#if defined(__ELF__)
    .type MQ_SYM(botw_guest_mq_worker_send_entry), %function
#endif
MQ_SYM(botw_guest_mq_worker_send_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]
    mov  x9, #1
    str  x9, [x20, #24]             /* worker-send entered */
    ldr  x0, [x19, #88]
    movz x1, #0x2222
    ldr  x9, [x19, #16]
    blr  x9                         /* blocks while the queue is full */
    mov  x9, #1
    str  x9, [x20, #32]             /* worker-send resumed */
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size MQ_SYM(botw_guest_mq_worker_send_entry), . - MQ_SYM(botw_guest_mq_worker_send_entry)
#endif

    .p2align 2
    .globl MQ_SYM(botw_guest_mq_worker_jam_entry)
#if defined(__ELF__)
    .type MQ_SYM(botw_guest_mq_worker_jam_entry), %function
#endif
MQ_SYM(botw_guest_mq_worker_jam_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]
    mov  x9, #1
    str  x9, [x20, #40]             /* worker-jam entered */
    ldr  x0, [x19, #88]
    mov  x1, #0x20
    ldr  x9, [x19, #24]
    blr  x9                         /* blocks while the queue is full */
    mov  x9, #1
    str  x9, [x20, #48]             /* worker-jam resumed */
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size MQ_SYM(botw_guest_mq_worker_jam_entry), . - MQ_SYM(botw_guest_mq_worker_jam_entry)
#endif

#endif /* __aarch64__ */

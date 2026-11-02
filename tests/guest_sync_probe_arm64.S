/*
 * guest_sync_probe_arm64.S -- block on mutex/event/light-event/semaphore.
 *
 * void botw_guest_sync_main_entry(struct sync_probe_args* args);
 * void botw_guest_sync_worker_a_entry(struct sync_probe_args* args);
 * void botw_guest_sync_worker_b_entry(struct sync_probe_args* args);
 * void botw_guest_sync_worker_c_entry(struct sync_probe_args* args);
 * void botw_guest_sync_worker_d_entry(struct sync_probe_args* args);
 *
 * sync_probe_args (all fields are 8 bytes):
 *   +0   uint64_t* counter
 *   +8   LockMutex
 *   +16  UnlockMutex
 *   +24  WaitEvent
 *   +32  SignalEvent
 *   +40  WaitLightEvent
 *   +48  SignalLightEvent
 *   +56  TimedWaitLightEvent
 *   +64  AcquireSemaphore
 *   +72  ReleaseSemaphore
 *   +80  WaitThread
 *   +88  StartThread
 *   +96  MutexType*
 *   +104 EventType*
 *   +112 LightEventType*
 *   +120 SemaphoreType*
 *   +128 worker-a ThreadType*
 *   +136 worker-b ThreadType*
 *   +144 worker-c ThreadType*
 *   +152 worker-d ThreadType*
 *   +160 timed-wait nanoseconds
 *
 * Counter slots:
 *   0 worker-a acquired the mutex      1 worker-a unlocked it
 *   2 main timed-light result (0)       3 main joined worker-a
 *   4 worker-b signalled the event      5 main WaitEvent result (1)
 *   6 worker-c acquired the semaphore   7 main WaitEvent result (1)
 *   8 main WaitLightEvent returned      9 main timed-light result (0)
 *  10 worker-d signalled the light     11 main reached the end
 *  12 worker-d returned                13 worker-c signalled the event
 *
 * Phase 1 gives the mutex to main, starts worker-a, then parks main on a
 * timed light event.  Worker-a blocks on the mutex, which leaves only main's
 * deadline runnable; the scheduler must advance the virtual clock, main must
 * time out, unlock, and hand ownership to worker-a.  Later phases exercise
 * event signal/wake, semaphore handoff, and an untimed light-event wait.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define SYNC_SYM(name) _##name
#else
#  define SYNC_SYM(name) name
#endif

    .text
    .p2align 2
    .globl SYNC_SYM(botw_guest_sync_main_entry)
#if defined(__ELF__)
    .type SYNC_SYM(botw_guest_sync_main_entry), %function
#endif
SYNC_SYM(botw_guest_sync_main_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]

    /* Phase 1: main owns the mutex; worker-a will block on it. */
    ldr  x0, [x19, #96]
    ldr  x9, [x19, #8]
    blr  x9
    ldr  x0, [x19, #128]
    ldr  x9, [x19, #88]
    blr  x9                         /* start worker-a */
    ldr  x0, [x19, #112]
    ldr  x1, [x19, #160]
    ldr  x9, [x19, #56]
    blr  x9                         /* timed light wait -> timeout */
    str  x0, [x20, #16]
    ldr  x0, [x19, #96]
    ldr  x9, [x19, #16]
    blr  x9                         /* unlock; ownership moves to worker-a */
    ldr  x0, [x19, #128]
    ldr  x9, [x19, #80]
    blr  x9                         /* join worker-a */
    mov  x9, #1
    str  x9, [x20, #24]

    /* Phase 2: wait on an auto-clear event, worker-b signals it. */
    ldr  x0, [x19, #136]
    ldr  x9, [x19, #88]
    blr  x9
    ldr  x0, [x19, #104]
    ldr  x9, [x19, #24]
    blr  x9
    str  x0, [x20, #40]
    ldr  x0, [x19, #136]
    ldr  x9, [x19, #80]
    blr  x9

    /* Phase 3: worker-c signals the event, then blocks on the semaphore. */
    ldr  x0, [x19, #144]
    ldr  x9, [x19, #88]
    blr  x9
    ldr  x0, [x19, #104]
    ldr  x9, [x19, #24]
    blr  x9
    str  x0, [x20, #56]
    ldr  x0, [x19, #120]
    ldr  x9, [x19, #72]
    blr  x9                         /* release one permit -> worker-c */
    ldr  x0, [x19, #144]
    ldr  x9, [x19, #80]
    blr  x9

    /* Phase 4: an untimed light-event wait, signalled by worker-d. */
    ldr  x0, [x19, #152]
    ldr  x9, [x19, #88]
    blr  x9
    ldr  x0, [x19, #112]
    ldr  x9, [x19, #40]
    blr  x9
    mov  x9, #1
    str  x9, [x20, #64]
    ldr  x0, [x19, #152]
    ldr  x9, [x19, #80]
    blr  x9

    /* Phase 5: no runnable peer, so the deadline alone must wake main. */
    ldr  x0, [x19, #112]
    ldr  x1, [x19, #160]
    ldr  x9, [x19, #56]
    blr  x9
    str  x0, [x20, #72]
    mov  x9, #1
    str  x9, [x20, #88]

    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size SYNC_SYM(botw_guest_sync_main_entry), . - SYNC_SYM(botw_guest_sync_main_entry)
#endif

    .p2align 2
    .globl SYNC_SYM(botw_guest_sync_worker_a_entry)
#if defined(__ELF__)
    .type SYNC_SYM(botw_guest_sync_worker_a_entry), %function
#endif
SYNC_SYM(botw_guest_sync_worker_a_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]
    ldr  x0, [x19, #96]
    ldr  x9, [x19, #8]
    blr  x9                         /* blocks until main unlocks */
    mov  x9, #1
    str  x9, [x20, #0]
    ldr  x0, [x19, #96]
    ldr  x9, [x19, #16]
    blr  x9
    mov  x9, #1
    str  x9, [x20, #8]
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size SYNC_SYM(botw_guest_sync_worker_a_entry), . - SYNC_SYM(botw_guest_sync_worker_a_entry)
#endif

    .p2align 2
    .globl SYNC_SYM(botw_guest_sync_worker_b_entry)
#if defined(__ELF__)
    .type SYNC_SYM(botw_guest_sync_worker_b_entry), %function
#endif
SYNC_SYM(botw_guest_sync_worker_b_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]
    ldr  x0, [x19, #104]
    ldr  x9, [x19, #32]
    blr  x9
    mov  x9, #1
    str  x9, [x20, #32]
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size SYNC_SYM(botw_guest_sync_worker_b_entry), . - SYNC_SYM(botw_guest_sync_worker_b_entry)
#endif

    .p2align 2
    .globl SYNC_SYM(botw_guest_sync_worker_c_entry)
#if defined(__ELF__)
    .type SYNC_SYM(botw_guest_sync_worker_c_entry), %function
#endif
SYNC_SYM(botw_guest_sync_worker_c_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]
    ldr  x0, [x19, #104]
    ldr  x9, [x19, #32]
    blr  x9                         /* wake main */
    mov  x9, #1
    str  x9, [x20, #104]            /* counter[13] */
    ldr  x0, [x19, #120]
    ldr  x9, [x19, #64]
    blr  x9                         /* blocks until main releases */
    mov  x9, #1
    str  x9, [x20, #48]
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size SYNC_SYM(botw_guest_sync_worker_c_entry), . - SYNC_SYM(botw_guest_sync_worker_c_entry)
#endif

    .p2align 2
    .globl SYNC_SYM(botw_guest_sync_worker_d_entry)
#if defined(__ELF__)
    .type SYNC_SYM(botw_guest_sync_worker_d_entry), %function
#endif
SYNC_SYM(botw_guest_sync_worker_d_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]
    ldr  x0, [x19, #112]
    ldr  x9, [x19, #48]
    blr  x9
    mov  x9, #1
    str  x9, [x20, #80]             /* counter[10] */
    mov  x9, #1
    str  x9, [x20, #96]             /* counter[12] */
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size SYNC_SYM(botw_guest_sync_worker_d_entry), . - SYNC_SYM(botw_guest_sync_worker_d_entry)
#endif

#endif /* __aarch64__ */

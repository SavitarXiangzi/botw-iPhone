/*
 * guest_system_event_probe_arm64.S -- SystemEvent and MultiWait handoff.
 *
 * void botw_guest_se_mw_main_entry(struct se_mw_probe_args* args);
 * void botw_guest_se_mw_worker_sys_entry(struct se_mw_probe_args* args);
 * void botw_guest_se_mw_worker_mw_entry(struct se_mw_probe_args* args);
 *
 * se_mw_probe_args (all fields are 8 bytes):
 *   +0    uint64_t* counter
 *   +8    StartThread
 *   +16   WaitThread
 *   +24   InitializeSystemEvent
 *   +32   SignalSystemEvent
 *   +40   WaitSystemEvent
 *   +48   TimedWaitSystemEvent
 *   +56   TryWaitSystemEvent
 *   +64   InitializeEvent
 *   +72   TryWaitEvent
 *   +80   SignalEvent
 *   +88   InitializeMultiWait
 *   +96   InitializeMultiWaitHolder
 *   +104  LinkMultiWaitHolder
 *   +112  WaitAny
 *   +120  system event A (auto-clear)
 *   +128  system event B (auto-clear, timed-out)
 *   +136  Event 1
 *   +144  Event 2
 *   +152  MultiWaitType
 *   +160  holder 1 -> Event 1
 *   +168  holder 2 -> Event 2
 *   +176  worker-sys ThreadType*
 *   +184  worker-mw ThreadType*
 *   +192  timeout nanoseconds
 *
 * Counter slots:
 *   0 worker-sys entered                 1 worker-sys signalled
 *   2 main WaitSystemEvent returned      3 main TimedWaitSystemEvent result (0)
 *   4 WaitAny selected holder 1          5 TryWaitEvent(Event 1) result (1)
 *   6 worker-mw entered                  7 worker-mw signalled Event 1
 *   8 main joined worker-sys             9 main joined worker-mw
 *  10 main reached the end              11 immediate WaitAny selected holder 2
 *  12 TryWaitEvent(Event 2) result (1)
 *
 * The timed wait has no runnable peer, so the virtual clock must advance to
 * its deadline.  WaitAny must observe an auto-clear Event without consuming
 * it; the guest clears it with TryWaitEvent after the holder is returned.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define SE_SYM(name) _##name
#else
#  define SE_SYM(name) name
#endif

    .text
    .p2align 2
    .globl SE_SYM(botw_guest_se_mw_main_entry)
#if defined(__ELF__)
    .type SE_SYM(botw_guest_se_mw_main_entry), %function
#endif
SE_SYM(botw_guest_se_mw_main_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]

    /* Phase 1: initialize an auto-clear SystemEvent and block on it. */
    ldr  x0, [x19, #120]
    mov  x1, #1
    ldr  x9, [x19, #24]
    blr  x9
    ldr  x0, [x19, #176]
    ldr  x9, [x19, #8]
    blr  x9                         /* start worker-sys */
    ldr  x0, [x19, #120]
    ldr  x9, [x19, #40]
    blr  x9                         /* blocks until worker-sys signals */
    mov  x9, #1
    str  x9, [x20, #16]
    ldr  x0, [x19, #176]
    ldr  x9, [x19, #16]
    blr  x9                         /* join worker-sys */
    mov  x9, #1
    str  x9, [x20, #64]

    /* Phase 2: no runnable peer, so the timed SystemEvent wait must expire. */
    ldr  x0, [x19, #128]
    mov  x1, #1
    ldr  x9, [x19, #24]
    blr  x9
    ldr  x0, [x19, #128]
    ldr  x1, [x19, #192]
    ldr  x9, [x19, #48]
    blr  x9
    str  x0, [x20, #24]

    /* Phase 3: Event 1 is linked first, so WaitAny must return holder 1. */
    ldr  x0, [x19, #136]
    mov  x1, #0
    mov  x2, #1
    ldr  x9, [x19, #64]
    blr  x9
    ldr  x0, [x19, #144]
    mov  x1, #0
    mov  x2, #1
    ldr  x9, [x19, #64]
    blr  x9
    ldr  x0, [x19, #152]
    ldr  x9, [x19, #88]
    blr  x9
    ldr  x0, [x19, #160]
    ldr  x1, [x19, #136]
    ldr  x9, [x19, #96]
    blr  x9
    ldr  x0, [x19, #168]
    ldr  x1, [x19, #144]
    ldr  x9, [x19, #96]
    blr  x9
    ldr  x0, [x19, #152]
    ldr  x1, [x19, #160]
    ldr  x9, [x19, #104]
    blr  x9
    ldr  x0, [x19, #152]
    ldr  x1, [x19, #168]
    ldr  x9, [x19, #104]
    blr  x9
    ldr  x0, [x19, #184]
    ldr  x9, [x19, #8]
    blr  x9                         /* start worker-mw */
    ldr  x0, [x19, #152]
    ldr  x9, [x19, #112]
    blr  x9                         /* WaitAny blocks until Event 1 */
    ldr  x9, [x19, #160]
    cmp  x0, x9
    cset w10, eq
    str  x10, [x20, #32]
    ldr  x0, [x19, #136]
    ldr  x9, [x19, #72]
    blr  x9                         /* consume Event 1 */
    str  x0, [x20, #40]

    /* Phase 4: Event 2 is already signaled, so WaitAny returns holder 2
     * without blocking. */
    ldr  x0, [x19, #144]
    ldr  x9, [x19, #80]
    blr  x9
    ldr  x0, [x19, #152]
    ldr  x9, [x19, #112]
    blr  x9
    ldr  x9, [x19, #168]
    cmp  x0, x9
    cset w10, eq
    str  x10, [x20, #88]
    ldr  x0, [x19, #144]
    ldr  x9, [x19, #72]
    blr  x9
    str  x0, [x20, #96]

    ldr  x0, [x19, #184]
    ldr  x9, [x19, #16]
    blr  x9                         /* join worker-mw */
    mov  x9, #1
    str  x9, [x20, #72]
    mov  x9, #1
    str  x9, [x20, #80]             /* main reached the end */

    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size SE_SYM(botw_guest_se_mw_main_entry), . - SE_SYM(botw_guest_se_mw_main_entry)
#endif

    .p2align 2
    .globl SE_SYM(botw_guest_se_mw_worker_sys_entry)
#if defined(__ELF__)
    .type SE_SYM(botw_guest_se_mw_worker_sys_entry), %function
#endif
SE_SYM(botw_guest_se_mw_worker_sys_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]
    mov  x9, #1
    str  x9, [x20, #0]              /* worker-sys entered */
    ldr  x0, [x19, #120]
    ldr  x9, [x19, #32]
    blr  x9                         /* wake main */
    mov  x9, #1
    str  x9, [x20, #8]
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size SE_SYM(botw_guest_se_mw_worker_sys_entry), . - SE_SYM(botw_guest_se_mw_worker_sys_entry)
#endif

    .p2align 2
    .globl SE_SYM(botw_guest_se_mw_worker_mw_entry)
#if defined(__ELF__)
    .type SE_SYM(botw_guest_se_mw_worker_mw_entry), %function
#endif
SE_SYM(botw_guest_se_mw_worker_mw_entry):
    stp  x29, x30, [sp, #-32]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    mov  x19, x0
    ldr  x20, [x19, #0]
    mov  x9, #1
    str  x9, [x20, #48]             /* worker-mw entered */
    ldr  x0, [x19, #136]
    ldr  x9, [x19, #80]
    blr  x9                         /* wake main through WaitAny */
    mov  x9, #1
    str  x9, [x20, #56]
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #32
    ret
#if defined(__ELF__)
    .size SE_SYM(botw_guest_se_mw_worker_mw_entry), . - SE_SYM(botw_guest_se_mw_worker_mw_entry)
#endif

#endif /* __aarch64__ */

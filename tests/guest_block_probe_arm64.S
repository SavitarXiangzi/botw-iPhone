/*
 * guest_block_probe_arm64.S -- prove that blocking imports really switch.
 *
 * void botw_guest_block_main_entry(struct block_probe_args* args);
 * void botw_guest_block_worker_entry(uint64_t* counter);
 *
 * block_probe_args:
 *   +0   uint64_t* counter   [0] worker ran, [1] after wait, [2] after sleep,
 *                            [3] tick before sleep, [4] tick after sleep
 *   +8   void*     wait_thread (nn::os::WaitThread)
 *   +16  void*     sleep_thread (nn::os::SleepThread)
 *   +24  void*     get_tick (nn::os::GetSystemTick)
 *   +32  void*     worker ThreadType*
 *   +40  int64_t   sleep nanoseconds
 *
 * WaitThread blocks the main thread before the worker has ever run.  The
 * scheduler must restore the worker, let it return, wake the joiner from the
 * exit trampoline, and resume main after the import call.  SleepThread then
 * blocks with no runnable peer, so the scheduler must advance the virtual
 * clock to the deadline before returning to main.  Every value written here
 * therefore lives across two abandoned host C frames.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define BLOCK_SYM(name) _##name
#else
#  define BLOCK_SYM(name) name
#endif

    .text
    .p2align 2
    .globl BLOCK_SYM(botw_guest_block_main_entry)
#if defined(__ELF__)
    .type BLOCK_SYM(botw_guest_block_main_entry), %function
#endif
BLOCK_SYM(botw_guest_block_main_entry):
    stp  x29, x30, [sp, #-80]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    stp  x21, x22, [sp, #32]
    stp  x23, x24, [sp, #48]
    str  x25, [sp, #64]

    ldr  x19, [x0, #0]          /* counter */
    ldr  x20, [x0, #8]          /* WaitThread */
    ldr  x21, [x0, #16]         /* SleepThread */
    ldr  x22, [x0, #24]         /* GetSystemTick */
    ldr  x23, [x0, #32]         /* worker ThreadType* */
    ldr  x24, [x0, #40]         /* sleep nanoseconds */
    str  x23, [x19, #40]        /* counter[5]: the pointer the guest will pass */
    str  x24, [x19, #48]        /* counter[6]: the span the guest will pass */

    mov  x0, x23
    blr  x20                    /* BLOCKED until the worker returns */
    mov  x9, #1
    str  x9, [x19, #8]
    str  x24, [x19, #56]        /* counter[7]: x24 after WaitThread */

    blr  x22                    /* tick before sleep */
    str  x0, [x19, #24]
    str  x24, [x19, #64]        /* counter[8]: x24 after GetSystemTick */
    mov  x0, x24
    blr  x21                    /* BLOCKED until the virtual deadline */
    blr  x22                    /* tick after sleep */
    str  x0, [x19, #32]
    mov  x9, #1
    str  x9, [x19, #16]

    ldr  x25, [sp, #64]
    ldp  x23, x24, [sp, #48]
    ldp  x21, x22, [sp, #32]
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #80
    ret
#if defined(__ELF__)
    .size BLOCK_SYM(botw_guest_block_main_entry), . - BLOCK_SYM(botw_guest_block_main_entry)
#endif

    .p2align 2
    .globl BLOCK_SYM(botw_guest_block_worker_entry)
#if defined(__ELF__)
    .type BLOCK_SYM(botw_guest_block_worker_entry), %function
#endif
BLOCK_SYM(botw_guest_block_worker_entry):
    mov  x9, #1
    str  x9, [x0]
    ret
#if defined(__ELF__)
    .size BLOCK_SYM(botw_guest_block_worker_entry), . - BLOCK_SYM(botw_guest_block_worker_entry)
#endif

#endif /* __aarch64__ */

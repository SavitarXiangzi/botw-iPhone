/* tests/guest_run_probe_arm64.S -- a guest program that is really entered.
 *
 * void botw_guest_run_probe_entry(uint64_t* counter, void* block, uint64_t value);
 *
 * It builds a frame on the guest stack it was given, counts to three through
 * memory, calls the rewritten gate block with `value`, stores the answer the
 * dispatcher wrote into x0 at counter[1], and returns to the sentinel.
 *
 * That is the smallest program that exercises every piece at once: the guest
 * stack (stp/ldp), the key-page indirection inside the block, the trampoline,
 * the dispatcher, and the exit path back into the host.
 *
 * Only assembles for AArch64.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define RUN_PROBE_SYM(name) _##name
#else
#  define RUN_PROBE_SYM(name) name
#endif

    .text
    .p2align 2
    .globl RUN_PROBE_SYM(botw_guest_run_probe_entry)
#if defined(__ELF__)
    .type RUN_PROBE_SYM(botw_guest_run_probe_entry), %function
#endif
RUN_PROBE_SYM(botw_guest_run_probe_entry):
    stp  x29, x30, [sp, #-48]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    str  x21, [sp, #32]

    mov  x19, x0                    /* counter */
    mov  x20, x1                    /* gate block */
    mov  x21, x2                    /* value */

    mov  x9,  #3
1:
    ldr  x10, [x19]
    add  x10, x10, #1
    str  x10, [x19]
    subs x9,  x9, #1
    b.ne 1b

    mov  x0, x21
    blr  x20                        /* through the gate block and back */
    str  x0, [x19, #8]              /* the dispatcher's answer */

    ldr  x21, [sp, #32]
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #48
    ret
#if defined(__ELF__)
    .size RUN_PROBE_SYM(botw_guest_run_probe_entry), . - RUN_PROBE_SYM(botw_guest_run_probe_entry)
#endif

#endif /* __aarch64__ */

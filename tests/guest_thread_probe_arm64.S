/*
 * guest_thread_probe_arm64.S -- a guest thread body for guest_thread_test.
 *
 *   void botw_guest_thread_probe_entry(struct probe_args* args);
 *
 * probe_args:
 *   +0   uint64_t* counter   counter[0] = iterations, [1] = id, [2] = sp
 *   +8   uint64_t  id
 *   +16  uint64_t  iterations
 *   +24  void*     gate block
 *   +32  void*     yield function (nn::os::YieldThread, may be NULL)
 *
 * Each iteration calls the rewritten gate block, which dispatches to the
 * host.  The host gate callback yields, so the scheduler saves this thread's
 * register frame and resumes another one.  The loop counter lives in x21
 * (callee-saved), so it survives a switch.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define PROBE_SYM(name) _##name
#else
#  define PROBE_SYM(name) name
#endif

    .text
    .p2align 2
    .globl PROBE_SYM(botw_guest_thread_probe_entry)
#if defined(__ELF__)
    .type PROBE_SYM(botw_guest_thread_probe_entry), %function
#endif
PROBE_SYM(botw_guest_thread_probe_entry):
    stp  x29, x30, [sp, #-64]!
    mov  x29, sp
    str  x19, [sp, #16]
    str  x20, [sp, #24]
    str  x21, [sp, #32]
    str  x22, [sp, #40]
    str  x23, [sp, #48]

    ldr  x19, [x0, #0]          /* counter */
    ldr  x20, [x0, #8]          /* id */
    ldr  x21, [x0, #16]         /* iterations */
    ldr  x22, [x0, #24]         /* gate block */
    ldr  x23, [x0, #32]         /* nn::os::YieldThread */

    str  x20, [x19, #8]         /* counter[1] = id */
    mov  x9,  sp
    str  x9,  [x19, #16]        /* counter[2] = sp */

1:
    cbz  x23, 2f
    blr  x23                    /* guest-visible YieldThread import */
2:
    ldr  x9, [x19]
    add  x9, x9, #1
    str  x9, [x19]
    mov  x0, x20
    blr  x22
    subs x21, x21, #1
    b.ne 1b

    ldr  x19, [sp, #16]
    ldr  x20, [sp, #24]
    ldr  x21, [sp, #32]
    ldr  x22, [sp, #40]
    ldr  x23, [sp, #48]
    ldp  x29, x30, [sp], #64
    ret
#if defined(__ELF__)
    .size PROBE_SYM(botw_guest_thread_probe_entry), . - PROBE_SYM(botw_guest_thread_probe_entry)
#endif

#endif /* __aarch64__ */

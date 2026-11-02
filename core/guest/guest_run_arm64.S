/*
 * guest_run_arm64.S -- enter a guest, and get the host back when it returns.
 *
 *   void botw_guest_run_enter_asm(botw_guest_run_ctx* ctx);
 *
 * On entry x0 is the context.  The sequence is:
 *
 *   1. push the host's fp/lr, save x19..x28 and the host sp into the context;
 *   2. publish the context pointer in a global, because every register is
 *      about to belong to the guest;
 *   3. load the guest's x0..x30 from the context (x18 excepted: Apple
 *      reserves it), set sp to the guest stack, install the sentinel in x30
 *      and branch to the entry;
 *   4. when the guest returns, store its x0 and sp, restore the host and ret.
 *
 * x9/x10/x11 are used as scratch between step 3 and the branch.  That is
 * legal: they are intra-procedure-call temporaries, so a callee must not
 * expect them to carry anything on entry.
 *
 * The global is a single word and the harness is documented as one-run-at-a-
 * time; a nested run would overwrite the outer one's saved context.
 *
 * Only assembles for AArch64.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define RUN_SYM(name) _##name
#else
#  define RUN_SYM(name) name
#endif

/* botw_guest_regs.pc */
#define FRAME_PC 256

    .data
    .p2align 3
/* The context pointer the sentinel loads.  Nothing in the guest can reach it:
 * the guest has no pointer to this translation unit. */
RUN_SYM(botw_guest_run_globals):
    .quad 0

    .text
    .p2align 2
    .globl RUN_SYM(botw_guest_run_enter_asm)
#if defined(__ELF__)
    .type RUN_SYM(botw_guest_run_enter_asm), %function
#endif
RUN_SYM(botw_guest_run_enter_asm):
    stp  x29, x30, [sp, #-16]!
    mov  x29, sp
    mov  x2,  x0                       /* ctx */

    stp  x19, x20, [x2, #248]
    stp  x21, x22, [x2, #264]
    stp  x23, x24, [x2, #280]
    stp  x25, x26, [x2, #296]
    stp  x27, x28, [x2, #312]
    mov  x9,  sp
    str  x9,  [x2, #328]               /* host sp after the pushed pair */

    adrp x9,  RUN_SYM(botw_guest_run_globals)@PAGE
    add  x9,  x9, RUN_SYM(botw_guest_run_globals)@PAGEOFF
    str  x2,  [x9]

    /* Guest registers.  x2 is the base, so it is loaded last; x18 is skipped
     * because Apple reserves it; x30 is the sentinel, loaded below. */
    ldp  x0,  x1,  [x2, #0]
    ldr  x3,       [x2, #24]
    ldp  x4,  x5,  [x2, #32]
    ldp  x6,  x7,  [x2, #48]
    ldp  x8,  x9,  [x2, #64]
    ldp  x10, x11, [x2, #80]
    ldp  x12, x13, [x2, #96]
    ldp  x14, x15, [x2, #112]
    ldp  x16, x17, [x2, #128]
    ldp  x19, x20, [x2, #152]
    ldp  x21, x22, [x2, #168]
    ldp  x23, x24, [x2, #184]
    ldp  x25, x26, [x2, #200]
    ldp  x27, x28, [x2, #216]
    ldr  x29,      [x2, #232]

    ldr  x10, [x2, #360]               /* guest stack top */
    ldr  x11, [x2, #352]               /* entry */
    ldr  x2,  [x2, #16]                /* guest x2, last */
    mov  sp,  x10
    adr  x30, 1f
    br   x11

1:  /* The guest returned to the sentinel. */
    b    RUN_SYM(botw_guest_run_finish_asm)
#if defined(__ELF__)
    .size RUN_SYM(botw_guest_run_enter_asm), . - RUN_SYM(botw_guest_run_enter_asm)
#endif

/* Restore the host after either the sentinel or the last guest thread exits.
 * x0 is the guest's return value, sp is the guest's stack pointer. */
    .p2align 2
.globl RUN_SYM(botw_guest_run_finish_asm)
#if defined(__ELF__)
    .type RUN_SYM(botw_guest_run_finish_asm), %function
#endif
RUN_SYM(botw_guest_run_finish_asm):
    adrp x9,  RUN_SYM(botw_guest_run_globals)@PAGE
    add  x9,  x9, RUN_SYM(botw_guest_run_globals)@PAGEOFF
    ldr  x9,  [x9]
    str  x0,  [x9, #336]               /* exit_x0 */
    mov  x10, sp
    str  x10, [x9, #344]               /* exit_sp */

    ldp  x19, x20, [x9, #248]
    ldp  x21, x22, [x9, #264]
    ldp  x23, x24, [x9, #280]
    ldp  x25, x26, [x9, #296]
    ldp  x27, x28, [x9, #312]
    ldr  x10, [x9, #328]               /* host sp */
    mov  sp,  x10
    ldp  x29, x30, [sp], #16
    ret
#if defined(__ELF__)
    .size RUN_SYM(botw_guest_run_finish_asm), . - RUN_SYM(botw_guest_run_finish_asm)
#endif

/* A created thread starts here with x0=arg, x19=guest entry, x20=exit stub.
 * It has a normal guest stack and a synthetic frame whose pc pointed here.
 * Set a clean frame pointer and the real LR, then enter the guest. */
    .p2align 2
    .globl RUN_SYM(botw_guest_thread_start_asm)
#if defined(__ELF__)
    .type RUN_SYM(botw_guest_thread_start_asm), %function
#endif
RUN_SYM(botw_guest_thread_start_asm):
    mov  x29, xzr
    mov  x30, x20
    br   x19
#if defined(__ELF__)
    .size RUN_SYM(botw_guest_thread_start_asm), . - RUN_SYM(botw_guest_thread_start_asm)
#endif

/* A guest thread returned to the exit stub.  Ask the scheduler for the next
 * frame; NULL means the run is over and the host must be restored. */
    .p2align 2
    .globl RUN_SYM(botw_guest_thread_exit_asm)
#if defined(__ELF__)
    .type RUN_SYM(botw_guest_thread_exit_asm), %function
#endif
RUN_SYM(botw_guest_thread_exit_asm):
    bl   RUN_SYM(botw_guest_thread_exit_frame)
    cbz  x0, 2f
    mov  x9,  x0
    ldr  w10, [x9, #8]                 /* botw_guest_resume.kind */
    ldr  x0,  [x9, #0]                 /* botw_guest_resume.regs */
    cbnz w10, 3f
    b    RUN_SYM(botw_guest_resume_frame)
3:  b    RUN_SYM(botw_guest_resume_import_frame)
2:  mov  x0, xzr
    b    RUN_SYM(botw_guest_run_finish_asm)
#if defined(__ELF__)
    .size RUN_SYM(botw_guest_thread_exit_asm), . - RUN_SYM(botw_guest_thread_exit_asm)
#endif

/* Restore a saved gate frame and return into its block's ldp.  The frame
 * lives in a thread struct, not on the stack being abandoned, so sp may move
 * before the remaining loads.  x18 is never loaded: it is reserved by Apple. */
    .p2align 2
    .globl RUN_SYM(botw_guest_resume_frame)
#if defined(__ELF__)
    .type RUN_SYM(botw_guest_resume_frame), %function
#endif
RUN_SYM(botw_guest_resume_frame):
    mov  x9,  x0
    ldr  x16, [x9, #248]               /* frame->sp */
    sub  x16, x16, #16
    ldr  x11, [x9, #232]               /* frame->x[29] */
    ldr  x12, [x9, #240]               /* frame->x[30] */
    stp  x11, x12, [x16]               /* the block's ldp slot */
    mov  sp,  x16
    ldp  x0,  x1,  [x9, #0]
    ldp  x2,  x3,  [x9, #16]
    ldp  x4,  x5,  [x9, #32]
    ldp  x6,  x7,  [x9, #48]
    ldr  x8,       [x9, #64]
    ldp  x10, x11, [x9, #80]
    ldp  x12, x13, [x9, #96]
    ldp  x14, x15, [x9, #112]
    ldp  x16, x17, [x9, #128]
    ldp  x19, x20, [x9, #152]
    ldp  x21, x22, [x9, #168]
    ldp  x23, x24, [x9, #184]
    ldp  x25, x26, [x9, #200]
    ldp  x27, x28, [x9, #216]
    ldr  x30,      [x9, #256]
    ldr  x9,       [x9, #72]           /* x9 last: it was the base */
    ret
#if defined(__ELF__)
    .size RUN_SYM(botw_guest_resume_frame), . - RUN_SYM(botw_guest_resume_frame)
#endif

/* Restore a frame saved at a direct HLE import.  There is no gate block to
 * reload x29/x30, so restore every register here, put the incoming sp back
 * exactly where it was, and branch to the instruction after the import.
 * x17 temporarily carries the new sp while the guest's own x17 is restored;
 * x18 is not loaded (Apple reserves it). */
    .p2align 2
    .globl RUN_SYM(botw_guest_resume_import_frame)
#if defined(__ELF__)
    .type RUN_SYM(botw_guest_resume_import_frame), %function
#endif
RUN_SYM(botw_guest_resume_import_frame):
    mov  x9,  x0
    ldr  x17, [x9, #248]               /* frame->sp */
    ldp  x0,  x1,  [x9, #0]
    ldp  x2,  x3,  [x9, #16]
    ldp  x4,  x5,  [x9, #32]
    ldp  x6,  x7,  [x9, #48]
    ldr  x8,       [x9, #64]
    ldp  x10, x11, [x9, #80]
    ldp  x12, x13, [x9, #96]
    ldp  x14, x15, [x9, #112]
    ldr  x16,      [x9, #128]
    ldp  x19, x20, [x9, #152]
    ldp  x21, x22, [x9, #168]
    ldp  x23, x24, [x9, #184]
    ldp  x25, x26, [x9, #200]
    ldp  x27, x28, [x9, #216]
    ldr  x29,      [x9, #232]
    ldr  x30,      [x9, #FRAME_PC]
    mov  sp,  x17
    ldr  x17, [x9, #136]
    ldr  x9,  [x9, #72]
    br   x30
#if defined(__ELF__)
    .size RUN_SYM(botw_guest_resume_import_frame), . - RUN_SYM(botw_guest_resume_import_frame)
#endif

#endif /* __aarch64__ */

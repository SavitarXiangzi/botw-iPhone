/*
 * guest_hle_os_arm64.S -- blocking nn::os imports without unwinding C.
 *
 * A direct import is a normal AArch64 `blr` into host code.  Calling
 * botw_guest_run_yield() from inside that host function cannot switch yet:
 * the guest's return address is still in x30 and its live registers are in
 * the host C frame.  These wrappers save the complete guest frame first, ask
 * C whether the operation should block, and then either restore that frame or
 * jump to another thread's saved frame.  The host C frames are abandoned on
 * the guest stack; they are never unwound, and the thread resumes directly at
 * the instruction after its import call.
 *
 * C interface (guest_hle_os.h):
 *
 *   int botw_hle_os_blocking_call(botw_guest_regs* frame, unsigned op,
 *                                 const botw_guest_resume** next_resume);
 *
 * The C side returns BOTW_HLE_OS_RETURN, SWITCHED or STOPPED.  The local
 * 288-byte block is 272 bytes of botw_guest_regs plus an 8-byte next-frame
 * slot and 8 bytes of padding.  next_resume describes both the register frame
 * and whether it must return through a gate block or an import.  The guest's incoming sp is recorded before
 * this block is allocated, so resuming sees exactly the stack of the caller.
 *
 * Only assembles for AArch64.  The portable build registers plain C entry
 * points instead, because it cannot execute a guest thread.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define HLE_SYM(name) _##name
#else
#  define HLE_SYM(name) name
#endif

/* botw_guest_regs: x[0..30], sp, pc, nzcv; see guest_gate.h. */
#define FR_SP       248
#define FR_PC       256
#define FR_NZCV     264
#define FR_NEXT     272
#define FR_ALLOC    288

    .text
    .p2align 2

    .globl HLE_SYM(botw_hle_os_guest_sleep)
#if defined(__ELF__)
    .type HLE_SYM(botw_hle_os_guest_sleep), %function
#endif
HLE_SYM(botw_hle_os_guest_sleep):
    mov  x9, #1                       /* BOTW_HLE_OS_OP_SLEEP */
    b    .Lblocking_common
#if defined(__ELF__)
    .size HLE_SYM(botw_hle_os_guest_sleep), . - HLE_SYM(botw_hle_os_guest_sleep)
#endif

    .p2align 2
    .globl HLE_SYM(botw_hle_os_guest_wait_thread)
#if defined(__ELF__)
    .type HLE_SYM(botw_hle_os_guest_wait_thread), %function
#endif
HLE_SYM(botw_hle_os_guest_wait_thread):
    mov  x9, #2                       /* BOTW_HLE_OS_OP_WAIT_THREAD */
    b    .Lblocking_common
#if defined(__ELF__)
    .size HLE_SYM(botw_hle_os_guest_wait_thread), . - HLE_SYM(botw_hle_os_guest_wait_thread)
#endif

    .p2align 2
    .globl HLE_SYM(botw_hle_os_guest_lock_mutex)
#if defined(__ELF__)
    .type HLE_SYM(botw_hle_os_guest_lock_mutex), %function
#endif
HLE_SYM(botw_hle_os_guest_lock_mutex):
    mov  x9, #3                       /* BOTW_HLE_OS_OP_LOCK_MUTEX */
    b    .Lblocking_common
#if defined(__ELF__)
    .size HLE_SYM(botw_hle_os_guest_lock_mutex), . - HLE_SYM(botw_hle_os_guest_lock_mutex)
#endif

    .p2align 2
    .globl HLE_SYM(botw_hle_os_guest_wait_event)
#if defined(__ELF__)
    .type HLE_SYM(botw_hle_os_guest_wait_event), %function
#endif
HLE_SYM(botw_hle_os_guest_wait_event):
    mov  x9, #4                       /* BOTW_HLE_OS_OP_WAIT_EVENT */
    b    .Lblocking_common
#if defined(__ELF__)
    .size HLE_SYM(botw_hle_os_guest_wait_event), . - HLE_SYM(botw_hle_os_guest_wait_event)
#endif

    .p2align 2
    .globl HLE_SYM(botw_hle_os_guest_wait_light_event)
#if defined(__ELF__)
    .type HLE_SYM(botw_hle_os_guest_wait_light_event), %function
#endif
HLE_SYM(botw_hle_os_guest_wait_light_event):
    mov  x9, #5                       /* BOTW_HLE_OS_OP_WAIT_LIGHT_EVENT */
    b    .Lblocking_common
#if defined(__ELF__)
    .size HLE_SYM(botw_hle_os_guest_wait_light_event), . - HLE_SYM(botw_hle_os_guest_wait_light_event)
#endif

    .p2align 2
    .globl HLE_SYM(botw_hle_os_guest_timed_wait_light_event)
#if defined(__ELF__)
    .type HLE_SYM(botw_hle_os_guest_timed_wait_light_event), %function
#endif
HLE_SYM(botw_hle_os_guest_timed_wait_light_event):
    mov  x9, #6                       /* BOTW_HLE_OS_OP_TIMED_WAIT_LIGHT_EVENT */
    b    .Lblocking_common
#if defined(__ELF__)
    .size HLE_SYM(botw_hle_os_guest_timed_wait_light_event), . - HLE_SYM(botw_hle_os_guest_timed_wait_light_event)
#endif

    .p2align 2
    .globl HLE_SYM(botw_hle_os_guest_acquire_semaphore)
#if defined(__ELF__)
    .type HLE_SYM(botw_hle_os_guest_acquire_semaphore), %function
#endif
HLE_SYM(botw_hle_os_guest_acquire_semaphore):
    mov  x9, #7                       /* BOTW_HLE_OS_OP_ACQUIRE_SEMAPHORE */
    b    .Lblocking_common
#if defined(__ELF__)
    .size HLE_SYM(botw_hle_os_guest_acquire_semaphore), . - HLE_SYM(botw_hle_os_guest_acquire_semaphore)
#endif

    .p2align 2
.Lblocking_common:
    sub  sp,  sp, #FR_ALLOC
    stp  x0,  x1,  [sp, #0]
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]          /* x9 is the operation token */
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]
    str  x30, [sp, #240]              /* guest link register */
    add  x10, sp, #FR_ALLOC
    str  x10, [sp, #FR_SP]            /* incoming guest sp */
    str  x30, [sp, #FR_PC]            /* resume directly after the call */
    mrs  x10, nzcv
    str  x10, [sp, #FR_NZCV]
    str  xzr, [sp, #FR_NEXT]

    mov  x0, sp
    ldr  x1, [sp, #72]                /* operation token saved above */
    add  x2, sp, #FR_NEXT
    bl   HLE_SYM(botw_hle_os_blocking_call)

    cbz  w0, .Lresume_current
    cmp  w0, #1
    b.eq .Lresume_next

    /* STOPPED: no runnable frame and no timed sleeper can make progress. */
    mov  x0, xzr
    b    HLE_SYM(botw_guest_run_finish_asm)

.Lresume_current:
    mov  x0, sp
    b    HLE_SYM(botw_guest_resume_import_frame)

.Lresume_next:
    ldr  x9, [sp, #FR_NEXT]           /* const botw_guest_resume* */
    cbz  x9, .Lstopped
    ldr  w10, [x9, #8]                 /* botw_guest_resume.kind */
    ldr  x0,  [x9, #0]                 /* botw_guest_resume.regs */
    cbnz w10, 4f
    b    HLE_SYM(botw_guest_resume_frame)
4:  b    HLE_SYM(botw_guest_resume_import_frame)

.Lstopped:
    mov  x0, xzr
    b    HLE_SYM(botw_guest_run_finish_asm)

#endif /* __aarch64__ */

/*
 * guest_resume_arm64.S -- restore saved guest frames.
 *
 * Two frame shapes exist and they must not share a restore sequence:
 *
 *   - a gate frame was saved by guest_gate_arm64.S at a rewritten gate block.
 *     Restoring it returns into that block's `ldp x29,x30,[sp],#16`, so the
 *     block reloads x29/x30 from the 16 bytes it owns on the guest stack.
 *   - an import frame was saved by guest_hle_os_arm64.S at a direct HLE
 *     import.  There is no gate block; restore every register, put the incoming
 *     sp back exactly where it was, and branch to the instruction after the
 *     import.
 *
 * This file is standalone so the gate test and the HLE tests can link only the
 * restore path they need.  It references no C symbol.  x18 is never loaded:
 * Apple platforms reserve it.  Only assembles for AArch64.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define RESUME_SYM(name) _##name
#else
#  define RESUME_SYM(name) name
#endif

/* botw_guest_regs.pc */
#define FRAME_PC 256

    .text
    .p2align 2

/* Restore a saved gate frame and return into its block's ldp.  The frame
 * lives in a thread struct, not on the stack being abandoned, so sp may move
 * before the remaining loads.  x18 is never loaded: it is reserved by Apple. */
    .p2align 2
    .globl RESUME_SYM(botw_guest_resume_frame)
#if defined(__ELF__)
    .type RESUME_SYM(botw_guest_resume_frame), %function
#endif
RESUME_SYM(botw_guest_resume_frame):
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
    .size RESUME_SYM(botw_guest_resume_frame), . - RESUME_SYM(botw_guest_resume_frame)
#endif

/* Restore a frame saved at a direct HLE import.  There is no gate block to
 * reload x29/x30, so restore every register here, put the incoming sp back
 * exactly where it was, and branch to the instruction after the import.
 * x17 temporarily carries the new sp while the guest's own x17 is restored;
 * x18 is not loaded (Apple reserves it). */
    .p2align 2
    .globl RESUME_SYM(botw_guest_resume_import_frame)
#if defined(__ELF__)
    .type RESUME_SYM(botw_guest_resume_import_frame), %function
#endif
RESUME_SYM(botw_guest_resume_import_frame):
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
    .size RESUME_SYM(botw_guest_resume_import_frame), . - RESUME_SYM(botw_guest_resume_import_frame)
#endif


#endif /* __aarch64__ */

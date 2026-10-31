/*
 * guest_gate_arm64.S -- take over a rewritten gate block on AArch64.
 *
 * A gate block (see scripts/guest/third_party/nxnative/patch_nso.py) is
 *
 *     stp  x29, x30, [sp, #-16]!      ; the block owns 16 bytes of guest stack
 *     ...load x30 = slot[KGATE]...
 *     ldr  x29, [sp]
 *     blr  x30                        ; <- we land here
 *     ldp  x29, x30, [sp], #16        ; <- the address in x30 on entry
 *     b    <site+4>
 *
 * so on entry:
 *
 *     x30        = address of the block's `ldp`, i.e. where the gate returns to
 *     [sp]       = guest x29 (the block also left it in x29)
 *     [sp + 8]   = guest x30 (the guest's link register)
 *     x0..x28    = the guest's registers, untouched
 *     x18        = whatever the guest had; the rewrite never reads it
 *
 * This trampoline spills x0..x30, sp, pc and nzcv into a botw_guest_regs,
 * calls botw_guest_gate_dispatch(), restores everything and returns to the
 * block's `ldp`.  The block then reloads x29/x30 from [sp] and pops, which is
 * why we write the dispatcher's x29/x30 back into those two slots and leave sp
 * exactly where the block put it.
 *
 * Two details worth stating because they are easy to get wrong:
 *
 *  - pc is the block's `ldp`, not the guest's link register.  A dispatcher
 *    that wants to resume somewhere else can overwrite pc; x[30] stays the
 *    guest's x30 and is restored by the block's ldp.
 *  - the frame is restored from the bottom up with post-index loads and sp is
 *    moved only after the last load.  Nothing here reads memory below sp while
 *    sp is still the frame, so a signal delivered at any point cannot clobber
 *    a value that has not been consumed yet.
 *
 * On Apple platforms x18 is reserved for the OS.  It is saved and put back
 * unchanged, so the trampoline is transparent; a dispatcher must not write
 * x[18].
 *
 * Only assembles for AArch64.  On other hosts guest_gate.c supplies an empty
 * botw_guest_gate_entry and the gate test skips itself.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define GATE_SYM(name) _##name
#else
#  define GATE_SYM(name) name
#endif

/* botw_guest_regs: x[] at 0, then sp / pc / nzcv; see guest_gate.h. */
#define FR_SP       248
#define FR_PC       256
#define FR_NZCV     264

/* The block's own 16 bytes sit directly on top of the frame. */
#define FR_GUEST_LR 280         /* [sp + FR_SIZE + 8] at entry */
#define FR_GUEST_SP 288         /* [sp + FR_SIZE + 16] at entry */

    .text
    .p2align 2
    .globl GATE_SYM(botw_guest_gate_entry)
#if defined(__ELF__)
    .type GATE_SYM(botw_guest_gate_entry), %function
#endif
GATE_SYM(botw_guest_gate_entry):
    sub  sp,  sp, #272              /* sizeof(botw_guest_regs), 16-byte aligned */
    stp  x0,  x1,  [sp, #0]
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]       /* x29 already holds the guest x29 */
    ldr  x9,  [sp, #FR_GUEST_LR]
    str  x9,  [sp, #240]            /* x30 = the guest's link register */
    add  x9,  sp, #FR_GUEST_SP
    str  x9,  [sp, #FR_SP]          /* the guest's sp, above the block's pair */
    str  x30, [sp, #FR_PC]          /* x30 is the block's ldp: that is the pc */
    mrs  x9,  nzcv
    str  x9,  [sp, #FR_NZCV]

    mov  x0,  sp
    bl   GATE_SYM(botw_guest_gate_dispatch)

    ldr  x9,  [sp, #FR_NZCV]
    msr  nzcv, x9                   /* nothing below sets the flags */
    ldr  x29, [sp, #FR_SP]          /* x29 is free now: the block reloads it */
    sub  x29, x29, #16              /* where the block will ldp from */
    ldr  x11, [sp, #232]            /* the dispatcher's x29 ... */
    ldr  x12, [sp, #240]            /* ... and x30, for the block's ldp */
    stp  x11, x12, [x29]
    ldr  x30, [sp, #FR_PC]

    ldp  x0,  x1,  [sp], #16
    ldp  x2,  x3,  [sp], #16
    ldp  x4,  x5,  [sp], #16
    ldp  x6,  x7,  [sp], #16
    ldp  x8,  x9,  [sp], #16
    ldp  x10, x11, [sp], #16
    ldp  x12, x13, [sp], #16
    ldp  x14, x15, [sp], #16
    ldp  x16, x17, [sp], #16
    ldp  x18, x19, [sp], #16
    ldp  x20, x21, [sp], #16
    ldp  x22, x23, [sp], #16
    ldp  x24, x25, [sp], #16
    ldp  x26, x27, [sp], #16
    ldr  x28, [sp], #16             /* skips x29/x30: the block loads those */

    mov  sp,  x29
    ret                             /* x30 = the block's ldp */
#if defined(__ELF__)
    .size GATE_SYM(botw_guest_gate_entry), . - GATE_SYM(botw_guest_gate_entry)
#endif

#endif /* __aarch64__ */

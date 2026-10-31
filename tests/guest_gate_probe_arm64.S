/*
 * guest_gate_probe_arm64.S -- a synthetic gate block, for guest_gate_test.cpp.
 *
 * This is the exact shape the rewriter emits around an `svc` (see
 * patch_nso.py, site_svc), minus the key page:
 *
 *     stp  x29, x30, [sp, #-16]!
 *     ldr  x29, [sp]
 *     blr  x30                        ; here: bl to the real entry
 *  1: ldp  x29, x30, [sp], #16
 *
 * The registers are loaded with known values first so the test can tell what
 * the trampoline saw and what it put back.  The driver is assembly rather than
 * inline asm because Apple's ABI reserves x29 as FP: a clobber list cannot
 * name it, and the block has to overwrite it.
 *
 * void botw_guest_gate_probe(uint64_t* out);
 *
 *   out[0..7]   x0..x7  after the block's ldp
 *   out[8]      x8
 *   out[9]      x19
 *   out[10]     x20
 *   out[11]     x29
 *   out[12]     x30
 *   out[13]     sp after the block's ldp
 *   out[14]     the sp the frame must report (the block's sp + 16)
 *   out[15]     the pc the frame must report (the block's ldp)
 *   out[16]     nzcv after the block's ldp
 *   out[17]     unused
 *   out[18]     x9 (the probe's last value there is the nzcv pattern)
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define GATE_SYM(name) _##name
#else
#  define GATE_SYM(name) name
#endif

    .text
    .p2align 2
    .globl GATE_SYM(botw_guest_gate_probe)
#if defined(__ELF__)
    .type GATE_SYM(botw_guest_gate_probe), %function
#endif
GATE_SYM(botw_guest_gate_probe):
    stp  x19, x20, [sp, #-16]!
    stp  x21, x22, [sp, #-16]!
    stp  x29, x30, [sp, #-16]!      /* our own frame, and the return address */
    mov  x21, x0                    /* x0 becomes a guest value below */

    mov  x9,  sp
    str  x9,  [x21, #112]           /* out[14]: what frame->sp must be */
    adr  x9,  1f
    str  x9,  [x21, #120]           /* out[15]: what frame->pc must be */

    mov  x0,  #0x100
    mov  x1,  #0x101
    mov  x2,  #0x102
    mov  x3,  #0x103
    mov  x4,  #0x104
    mov  x5,  #0x105
    mov  x6,  #0x106
    mov  x7,  #0x107
    mov  x8,  #0x108
    mov  x19, #0x113
    mov  x20, #0x114
    mov  x29, #0x11d
    mov  x30, #0x11e
    mov  x9,  #0xa0000000
    msr  nzcv, x9

    stp  x29, x30, [sp, #-16]!      /* the block's frame */
    ldr  x29, [sp]
    bl   GATE_SYM(botw_guest_gate_entry)
1:
    ldp  x29, x30, [sp], #16

    stp  x0,  x1,  [x21, #0]
    stp  x2,  x3,  [x21, #16]
    stp  x4,  x5,  [x21, #32]
    stp  x6,  x7,  [x21, #48]
    str  x8,  [x21, #64]
    stp  x19, x20, [x21, #72]
    stp  x29, x30, [x21, #88]
    str  x9,  [x21, #144]           /* out[18]: the guest's x9 */
    mov  x9,  sp
    str  x9,  [x21, #104]           /* out[13]: sp after the ldp */
    mrs  x9,  nzcv
    str  x9,  [x21, #128]           /* out[16] */

    ldp  x29, x30, [sp], #16
    ldp  x21, x22, [sp], #16
    ldp  x19, x20, [sp], #16
    ret
#if defined(__ELF__)
    .size GATE_SYM(botw_guest_gate_probe), . - GATE_SYM(botw_guest_gate_probe)
#endif

#endif /* __aarch64__ */

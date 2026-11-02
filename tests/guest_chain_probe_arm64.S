/* tests/guest_chain_probe_arm64.S -- drive one real gate block.
 *
 * void botw_guest_chain_probe(void* block, uint64_t out[15]);
 *
 * The probe fills known values into x0..x8, x19, x20 and the guest x29,
 * plants 0xa0000000 in NZCV, then calls `block` with blr.  The block is the
 * exact instruction sequence the rewriter emits around an svc:
 *
 *     stp  x29, x30, [sp, #-16]!
 *     adrp x30, key_page ; ldr x30,[x30,#KGATE*8] ; ldr x30,[x30]
 *     ldr  x29, [sp]
 *     blr  x30
 *     ldp  x29, x30, [sp], #16
 *     ret                         ; the real block branches to site+4
 *
 * so a successful return means the key page indirection, the trampoline, the
 * dispatcher and the block's own ldp all agreed.  The probe records what came
 * back; the C test compares it with what the dispatcher saw.
 *
 * Only assembles for AArch64.  The test skips itself on other hosts.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define CHAIN_SYM(name) _##name
#else
#  define CHAIN_SYM(name) name
#endif

    .text
    .p2align 2
    .globl CHAIN_SYM(botw_guest_chain_probe)
#if defined(__ELF__)
    .type CHAIN_SYM(botw_guest_chain_probe), %function
#endif
CHAIN_SYM(botw_guest_chain_probe):
    stp  x29, x30, [sp, #-112]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    stp  x21, x22, [sp, #32]
    mov  x21, x1                    /* out */
    mov  x9,  x0                    /* block */

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
    mov  x10, #0xa0000000
    msr  nzcv, x10
    mov  x29, #0x11d                /* the guest x29 the block will save */
    blr  x9

    stp  x0,  x1,  [x21, #0]
    stp  x2,  x3,  [x21, #16]
    stp  x4,  x5,  [x21, #32]
    stp  x6,  x7,  [x21, #48]
    stp  x8,  x19, [x21, #64]
    stp  x20, x29, [x21, #80]
    str  x30, [x21, #96]
    mrs  x10, nzcv
    str  x10, [x21, #104]
    mov  x10, sp
    str  x10, [x21, #112]

    ldp  x19, x20, [sp, #16]
    ldp  x21, x22, [sp, #32]
    ldp  x29, x30, [sp], #112
    ret
#if defined(__ELF__)
    .size CHAIN_SYM(botw_guest_chain_probe), . - CHAIN_SYM(botw_guest_chain_probe)
#endif

#endif /* __aarch64__ */

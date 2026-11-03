/*
 * guest_app_probe_arm64.S -- the first guest program the iOS app executes.
 *
 * The probe is intentionally tiny: it touches the guest stack, writes three
 * values into the caller-supplied argument block, returns a magic value in x0,
 * and returns through the exit sentinel installed by botw_guest_run.
 *
 * It is built into the app bundle and entered with botw_guest_run_enter_at(),
 * so the path exercised here is the real one: private guest stack, guard page,
 * x30 sentinel, register setup and host return.  HLE imports are covered by
 * the separate guest-core tests; this probe only proves that the app can
 * start and stop native AArch64 guest code.
 */
#if defined(__aarch64__)

#if defined(__APPLE__)
#  define APP_SYM(name) _##name
#else
#  define APP_SYM(name) name
#endif

    .text
    .p2align 2
    .globl APP_SYM(botw_app_guest_probe)
#if defined(__ELF__)
    .type APP_SYM(botw_app_guest_probe), %function
#endif
APP_SYM(botw_app_guest_probe):
    stp  x29, x30, [sp, #-16]!
    mov  x29, sp

    /* "BOTW" in the low 32 bits, then a second value derived from it. */
    mov  x1, #0x424F
    movk x1, #0x5457, lsl #16
    str  x1, [x0, #0]
    add  x2, x1, #0x0101
    str  x2, [x0, #8]

    ldr  x3, [x0, #0]
    eor  x2, x2, x3
    str  x2, [x0, #16]

    /* The saved sp proves that the guest is on its own mapped stack. */
    mov  x3, sp
    str  x3, [x0, #24]

    mov  x0, x1
    ldp  x29, x30, [sp], #16
    ret
#if defined(__ELF__)
    .size APP_SYM(botw_app_guest_probe), . - APP_SYM(botw_app_guest_probe)
#endif


/*
 * botw_app_guest_map_probe -- run one rewritten gate block from a mapped
 * synthetic image.  This is the app-side counterpart of tests/guest_run_probe:
 * it uses the guest stack, calls the patch-area block, stores the dispatcher's
 * answer, and returns through botw_guest_run's exit sentinel.
 *
 *   void botw_app_guest_map_probe(uint64_t out[4], void* block, uint64_t value);
 */
    .p2align 2
    .globl APP_SYM(botw_app_guest_map_probe)
#if defined(__ELF__)
    .type APP_SYM(botw_app_guest_map_probe), %function
#endif
APP_SYM(botw_app_guest_map_probe):
    stp  x29, x30, [sp, #-64]!
    mov  x29, sp
    stp  x19, x20, [sp, #16]
    stp  x21, x22, [sp, #32]

    mov  x19, x0                    /* out */
    mov  x20, x1                    /* gate block in the mapped patch area */
    mov  x21, x2                    /* value the dispatcher must observe */
    add  x9, x21, #1
    str  x9, [x19, #0]
    str  x21, [x19, #24]

    mov  x0, x21
    blr  x20                        /* key page -> trampoline -> HLE gate */
    str  x0, [x19, #8]
    mov  x9, sp
    str  x9, [x19, #16]             /* probe frame sp, above the block's pair */

    ldp  x21, x22, [sp, #32]
    ldp  x19, x20, [sp, #16]
    ldp  x29, x30, [sp], #64
    ret
#if defined(__ELF__)
    .size APP_SYM(botw_app_guest_map_probe), . - APP_SYM(botw_app_guest_map_probe)
#endif

#endif /* __aarch64__ */

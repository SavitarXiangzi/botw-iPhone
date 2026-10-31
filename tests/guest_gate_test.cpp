/* AArch64-only test for the gate trampoline (core/guest/guest_gate_arm64.S).
 *
 * Synthetic: it runs the exact instruction sequence the rewriter puts around
 * an `svc` (stp / blr / ldp), with known values in the registers, and checks
 * what the dispatcher saw and what the guest got back.  No title data, and
 * nothing here executes guest code.
 *
 * What it pins down:
 *
 *   frame    x0..x8, x19, x20, the guest x29, the guest x30 (which the
 *            trampoline must take from the block's stp, not from the live
 *            link register), sp (above the block's 16 bytes) and pc (the
 *            block's ldp);
 *   return   x0 from the dispatcher, every other register restored, x29/x30
 *            taken from the frame by the block's ldp, sp back where it was,
 *            NZCV round-tripped;
 *   no handler   x0 reads 0 instead of a stale handle.
 *
 * On a non-AArch64 host it prints "skipped" and passes: the trampoline is
 * assembly, and guest_gate.c supplies an empty entry there.
 */
#include "guest_gate.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__aarch64__)

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_gate_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

/* tests/guest_gate_probe_arm64.S */
extern "C" void botw_guest_gate_probe(uint64_t* out);

static uint64_t g_out[32];
static botw_guest_regs g_seen;
static int g_mode;
static int g_calls;

extern "C" void gate_test_dispatch(botw_guest_regs* regs) {
    g_seen = *regs;
    g_calls++;
    regs->x[0] = 0xdead0000ull + (uint64_t)g_mode;
    if (g_mode == 1) {
        /* Exercise the write-back into the block's stp slot: whatever the
         * dispatcher leaves here is what the block's `ldp` restores. */
        regs->x[29] = 0x2299;
        regs->x[30] = 0x22aa;
        regs->nzcv = 0x50000000ull;   /* Z and V, not the 0xa0000000 it entered with */
    }
}

static void run(int mode) {
    g_mode = mode;
    g_calls = 0;
    std::memset(g_out, 0, sizeof g_out);
    std::memset(&g_seen, 0, sizeof g_seen);
    botw_guest_gate_probe(g_out);
}

#endif /* __aarch64__ */

int main() {
#if !defined(__aarch64__)
    std::printf("guest_gate_test: skipped (the trampoline is AArch64 assembly)\n");
    return 0;
#else
    botw_guest_gate_set_handler(gate_test_dispatch);
    CHECK(botw_guest_gate_get_handler() == gate_test_dispatch);

    /* 1. A dispatcher that edits x0, x29, x30 and NZCV. */
    run(1);
    CHECK(g_calls == 1);
    for (int i = 0; i <= 8; i++) CHECK(g_seen.x[i] == (uint64_t)(0x100 + i));
    CHECK(g_seen.x[19] == 0x113);
    CHECK(g_seen.x[20] == 0x114);
    CHECK(g_seen.x[29] == 0x11d);
    CHECK(g_seen.x[30] == 0x11e);   /* the guest's LR, not the bl's return address */
    CHECK(g_seen.sp == g_out[14]);  /* the guest's sp, above the block's 16 bytes */
    CHECK(g_seen.pc == g_out[15]);  /* the block's ldp */
    CHECK(g_seen.nzcv == 0xa0000000u);

    CHECK(g_out[0] == 0xdead0001u);
    for (int i = 1; i <= 8; i++) CHECK(g_out[i] == (uint64_t)(0x100 + i));
    CHECK(g_out[9] == 0x113);
    CHECK(g_out[10] == 0x114);
    CHECK(g_out[11] == 0x2299);     /* block's ldp took the dispatcher's x29 */
    CHECK(g_out[12] == 0x22aa);     /* ... and x30 */
    CHECK(g_out[13] == g_out[14]);  /* sp back where the block left it */
    CHECK(g_out[16] == 0x50000000u);
    CHECK(g_out[18] == 0xa0000000u);  /* x9 was the probe's nzcv pattern */

    /* 2. A dispatcher that only sets x0: the block restores what went in. */
    run(0);
    CHECK(g_calls == 1);
    CHECK(g_out[0] == 0xdead0000u);
    CHECK(g_out[11] == 0x11d);
    CHECK(g_out[12] == 0x11e);
    CHECK(g_out[13] == g_out[14]);
    CHECK(g_out[16] == 0xa0000000u);

    /* 3. No dispatcher at all: a zero return, not a stale x0. */
    botw_guest_gate_set_handler(nullptr);
    CHECK(botw_guest_gate_get_handler() == nullptr);
    run(0);
    CHECK(g_calls == 0);
    CHECK(g_out[0] == 0);
    CHECK(g_out[1] == 0x101);
    CHECK(g_out[8] == 0x108);
    CHECK(g_out[11] == 0x11d);
    CHECK(g_out[12] == 0x11e);
    CHECK(g_out[13] == g_out[14]);

    if (g_fail) {
        std::printf("guest_gate_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_gate_test: ok (frame, return, write-back, NZCV, no-handler)\n");
    return 0;
#endif
}

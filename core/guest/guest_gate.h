/* The register frame the rewritten gate blocks hand to the host.
 *
 * A gate block does `blr x30` with x0..x8 (and everything else) untouched, so
 * the host sees the guest registers directly.  The trampoline in
 * guest_gate_arm64.S spills the full set, calls the C dispatcher, restores it
 * and returns to the instruction after the `blr`.
 *
 * The offsets are hard-coded in the assembly; the static asserts in
 * guest_gate.c fail the build if this struct ever changes shape.
 */
#ifndef BOTW_GUEST_GATE_H
#define BOTW_GUEST_GATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t x[31];   /* offset 0   : x0..x30 */
    uint64_t sp;      /* offset 248 : stack pointer at the gate call */
    uint64_t pc;      /* offset 256 : return address into the gate block */
    uint64_t nzcv;    /* offset 264 : condition flags (NZCV) */
} botw_guest_regs;

/* Bit 63 of nzcv is not an architectural condition flag.  The cooperative
 * scheduler sets it when a gate dispatcher is handing control to a frame
 * saved at a direct HLE import instead of a gate block.  The trampoline then
 * takes the import restore path rather than the gate block's ldp path. */
#define BOTW_GUEST_NZCV_IMPORT_FRAME (1ull << 63)

typedef void (*botw_guest_gate_dispatch_fn)(botw_guest_regs* regs);

/* One global dispatcher: the guest core is cooperative and runs on a single
 * host thread, so there is no thread-local register to hang it off. */
void botw_guest_gate_set_handler(botw_guest_gate_dispatch_fn fn);
botw_guest_gate_dispatch_fn botw_guest_gate_get_handler(void);

/* Called by the trampoline, not by host code. */
void botw_guest_gate_dispatch(botw_guest_regs* regs);

/* Entry point installed at thread slot KGATE.  On entry x30 is the gate
 * block's return address and [sp]/[sp+8] are the guest's x29/x30, saved by
 * the gate block's `stp x29, x30, [sp, #-16]!`. */
void botw_guest_gate_entry(void);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_GATE_H */

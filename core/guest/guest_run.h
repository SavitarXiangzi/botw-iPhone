/* Hosting a guest that actually runs: entry, stack, exit.
 *
 * The gate trampoline makes one *call* transparent.  A running guest also
 * needs a stack of its own and a way to come back when its outermost function
 * returns.  This module supplies both:
 *
 *   - a private mapping for the guest stack, with a PROT_NONE guard page
 *     below it, so an overflow faults at the boundary instead of quietly
 *     eating host memory;
 *   - an exit trampoline installed in x30 before the first instruction, so
 *     the guest's final `ret` lands back in the host instead of on whatever
 *     the C compiler happened to leave there.
 *
 * The exit path records the guest's x0 and sp, restores the host's
 * callee-saved registers and returns, so botw_guest_run_enter() is an
 * ordinary C call from the caller's point of view.
 *
 * Constraints, all of them real:
 *
 *   - one run at a time.  The saved host registers live in a single global
 *     because the guest core is cooperative and single-threaded; a nested run
 *     would overwrite them.
 *   - no step budget and no signal handler.  A guest that loops forever, or
 *     branches to an unmapped address, takes the process with it.  When that
 *     matters, run the guest in a child process and treat a signal as the
 *     diagnosis.
 *   - x18 is never written.  It is reserved on Apple platforms, and the
 *     rewrite makes guest code independent of it.
 *   - x9, x10 and x11 are scratch at entry (AAPCS64 intra-procedure-call
 *     temporaries), so `regs.x[9..11]` are not honoured on entry.
 *
 * Only the AArch64 build can actually enter; other hosts get a clean error
 * from botw_guest_run_init() and the test skips itself.
 */
#ifndef BOTW_GUEST_RUN_H
#define BOTW_GUEST_RUN_H

#include <stddef.h>
#include <stdint.h>

#include "guest_gate.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOTW_GUEST_EXIT_NONE = 0,   /* never entered, or still inside */
    BOTW_GUEST_EXIT_RETURNED,   /* the guest executed ret to the sentinel */
    BOTW_GUEST_EXIT_REFUSED     /* enter() refused before executing anything */
} botw_guest_exit;

/* Called from the gate trampoline while the guest is inside.  `user` is the
 * pointer given to botw_guest_run_set_gate().  Returning normally resumes the
 * guest; writing regs->x[] is how a call is answered. */
typedef void (*botw_guest_run_gate_fn)(void* user, botw_guest_regs* regs);

/* Saved host state.  The offsets are hard-coded in guest_run_arm64.S and the
 * static asserts in guest_run.c fail the build if this struct changes. */
typedef struct {
    uint64_t x[31];      /*   0 : initial guest x0..x30 (x18 is not loaded) */
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;  /* 248 */
    uint64_t host_sp;    /* 328 : host sp after the pushed fp/lr pair */
    uint64_t exit_x0;    /* 336 : guest x0 at the sentinel */
    uint64_t exit_sp;    /* 344 : guest sp at the sentinel */
    uint64_t entry;      /* 352 : where to jump */
    uint64_t stack_top;  /* 360 : initial guest sp */
} botw_guest_run_ctx;

typedef struct {
    const uint8_t* image_base;   /* for the bounds check in enter() */
    size_t   image_size;
    uint8_t* stack;              /* the whole mapping, guard page included */
    size_t   stack_bytes;
    size_t   guard_bytes;
    uint64_t stack_top;          /* 16-byte aligned */
    uint64_t args[8];            /* x0..x7 at entry */
    unsigned long gates;         /* gate dispatches during the last run */
    int      reason;             /* botw_guest_exit */
    uint64_t exit_x0;
    uint64_t exit_sp;
    int      active;
    botw_guest_run_gate_fn gate;
    void*    gate_user;
    botw_guest_run_ctx ctx;
    char     error[160];
} botw_guest_run;

/* Maps the stack: `stack_bytes` usable plus one guard page below it.  0 means
 * 64 KiB.  `image_base` / `image_size` bound enter(); pass NULL / 0 only when
 * every entry will come through botw_guest_run_enter_at(). */
int botw_guest_run_init(botw_guest_run* run, const void* image_base,
                        size_t image_size, size_t stack_bytes);

/* Unmaps the stack.  Safe on a zeroed or already-closed run. */
void botw_guest_run_close(botw_guest_run* run);

void botw_guest_run_set_gate(botw_guest_run* run, botw_guest_run_gate_fn fn, void* user);

/* Runs <image_base + entry_off> until the guest returns.  Returns 0 when the
 * guest got as far as executing, -1 with `error` set when it refused. */
int botw_guest_run_enter(botw_guest_run* run, size_t entry_off);

/* Same, for code that is not part of the image (tests, a JIT arena). */
int botw_guest_run_enter_at(botw_guest_run* run, const void* entry);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_RUN_H */

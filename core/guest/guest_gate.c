#include "guest_gate.h"

#include <stddef.h>

/* Keep the hard-coded offsets in guest_gate_arm64.S honest.  A typedef instead
 * of _Static_assert: MSVC compiles .c as C89 unless told otherwise. */
#define BOTW_STATIC_ASSERT(cond, name) typedef char botw_static_assert_##name[(cond) ? 1 : -1]

BOTW_STATIC_ASSERT(offsetof(botw_guest_regs, x) == 0, frame_x_first);
BOTW_STATIC_ASSERT(offsetof(botw_guest_regs, sp) == 248, frame_sp_offset);
BOTW_STATIC_ASSERT(offsetof(botw_guest_regs, pc) == 256, frame_pc_offset);
BOTW_STATIC_ASSERT(offsetof(botw_guest_regs, nzcv) == 264, frame_nzcv_offset);
BOTW_STATIC_ASSERT(sizeof(botw_guest_regs) == 272, frame_size);
BOTW_STATIC_ASSERT(offsetof(botw_guest_regs, x) % 8 == 0, frame_x_aligned);

static botw_guest_gate_dispatch_fn g_handler;

void botw_guest_gate_set_handler(botw_guest_gate_dispatch_fn fn) {
    g_handler = fn;
}

botw_guest_gate_dispatch_fn botw_guest_gate_get_handler(void) {
    return g_handler;
}

void botw_guest_gate_dispatch(botw_guest_regs* regs) {
    if (!regs) return;
    if (g_handler) {
        g_handler(regs);
        return;
    }
    /* No handler: return 0 rather than whatever the caller happened to have in
     * x0.  An unimplemented HLE call should look like a zero return, not like
     * a random handle that fails somewhere else. */
    regs->x[0] = 0;
}

#if !defined(__aarch64__)
/* guest_gate_arm64.S only assembles for AArch64.  Host builds still compile
 * and link this translation unit; the gate test skips itself there. */
void botw_guest_gate_entry(void) {
}
#endif

#include "guest_run.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#  include <sys/mman.h>
#  include <unistd.h>
#  ifndef MAP_ANONYMOUS
#    define MAP_ANONYMOUS MAP_ANON
#  endif
#endif

/* Keep the hard-coded offsets in guest_run_arm64.S honest.  A typedef instead
 * of _Static_assert: MSVC compiles .c as C89 unless told otherwise. */
#define BOTW_STATIC_ASSERT(cond, name) typedef char botw_run_assert_##name[(cond) ? 1 : -1]

BOTW_STATIC_ASSERT(offsetof(botw_guest_run_ctx, x) == 0, ctx_x);
BOTW_STATIC_ASSERT(offsetof(botw_guest_run_ctx, x19) == 248, ctx_x19);
BOTW_STATIC_ASSERT(offsetof(botw_guest_run_ctx, x28) == 320, ctx_x28);
BOTW_STATIC_ASSERT(offsetof(botw_guest_run_ctx, host_sp) == 328, ctx_host_sp);
BOTW_STATIC_ASSERT(offsetof(botw_guest_run_ctx, exit_x0) == 336, ctx_exit_x0);
BOTW_STATIC_ASSERT(offsetof(botw_guest_run_ctx, exit_sp) == 344, ctx_exit_sp);
BOTW_STATIC_ASSERT(offsetof(botw_guest_run_ctx, entry) == 352, ctx_entry);
BOTW_STATIC_ASSERT(offsetof(botw_guest_run_ctx, stack_top) == 360, ctx_stack_top);
BOTW_STATIC_ASSERT(sizeof(botw_guest_run_ctx) == 368, ctx_size);

#if defined(__aarch64__)
extern void botw_guest_run_enter_asm(botw_guest_run_ctx* ctx);

static botw_guest_run* g_active;

/* The trampoline calls this through the single global handler slot, because
 * botw_guest_gate_dispatch_fn carries no user pointer. */
static void run_gate(botw_guest_regs* regs) {
    botw_guest_run* run = g_active;
    if (!run) return;
    run->gates++;
    if (run->gate) run->gate(run->gate_user, regs);
}
#else
/* guest_run_arm64.S emits nothing here.  enter_at() refuses before this could
 * be called, but the symbol still has to exist for the link.  It is a global
 * rather than a static so a host build does not warn about a function that is
 * only reachable through the AArch64 branch. */
void botw_guest_run_enter_asm(botw_guest_run_ctx* ctx) {
    (void)ctx;
}
#endif

static void run_error(botw_guest_run* run, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(run->error, sizeof run->error, fmt, ap);
    va_end(ap);
}

int botw_guest_run_init(botw_guest_run* run, const void* image_base,
                        size_t image_size, size_t stack_bytes) {
    if (!run) return -1;
    memset(run, 0, sizeof *run);
    run->reason = BOTW_GUEST_EXIT_NONE;
    run->image_base = (const uint8_t*)image_base;
    run->image_size = image_size;

#if defined(_WIN32)
    (void)stack_bytes;
    run_error(run, "guest execution needs a POSIX mmap; this host only loads images");
    return -1;
#else
    {
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        size_t usable;
        size_t total;
        uint8_t* mem;

        if (page < 0x1000) page = 0x1000;
        if (stack_bytes == 0) stack_bytes = 0x10000;
        usable = (stack_bytes + page - 1) & ~(page - 1);
        total = usable + page;
        mem = (uint8_t*)mmap(NULL, total, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            run_error(run, "cannot map %zu bytes for the guest stack", total);
            return -1;
        }
        if (mprotect(mem, page, PROT_NONE) != 0) {
            munmap(mem, total);
            run_error(run, "cannot protect the guard page");
            return -1;
        }
        run->stack = mem;
        run->stack_bytes = total;
        run->guard_bytes = page;
        run->stack_top = (uint64_t)(uintptr_t)(mem + total);
        return 0;
    }
#endif
}

void botw_guest_run_close(botw_guest_run* run) {
    if (!run) return;
#if !defined(_WIN32)
    if (run->stack && run->stack_bytes) munmap(run->stack, run->stack_bytes);
#endif
    memset(run, 0, sizeof *run);
}

void botw_guest_run_set_gate(botw_guest_run* run, botw_guest_run_gate_fn fn, void* user) {
    if (!run) return;
    run->gate = fn;
    run->gate_user = user;
}

int botw_guest_run_enter(botw_guest_run* run, size_t entry_off) {
    if (!run) return -1;
    if (!run->image_base) {
        run_error(run, "no image base: use botw_guest_run_enter_at()");
        run->reason = BOTW_GUEST_EXIT_REFUSED;
        return -1;
    }
    if (entry_off + 4 > run->image_size) {
        run_error(run, "entry %#zx is outside a %zu-byte image", entry_off, run->image_size);
        run->reason = BOTW_GUEST_EXIT_REFUSED;
        return -1;
    }
    return botw_guest_run_enter_at(run, run->image_base + entry_off);
}

int botw_guest_run_enter_at(botw_guest_run* run, const void* entry) {
    if (!run) return -1;
    if (!entry) {
        run_error(run, "null entry");
        run->reason = BOTW_GUEST_EXIT_REFUSED;
        return -1;
    }
    if (!run->stack) {
        run_error(run, "no guest stack: run_init() failed or the run was closed");
        run->reason = BOTW_GUEST_EXIT_REFUSED;
        return -1;
    }
#if !defined(__aarch64__)
    run_error(run, "guest execution needs AArch64; this host can only load images");
    run->reason = BOTW_GUEST_EXIT_REFUSED;
    return -1;
#else
    botw_guest_gate_dispatch_fn prev;

    if (run->active) {
        run_error(run, "a run is already active; nesting is not supported");
        run->reason = BOTW_GUEST_EXIT_REFUSED;
        return -1;
    }

    /* Every run starts from a clean register file: x0..x7 from args, the rest
     * zero.  x18 is never loaded by the trampoline. */
    memset(&run->ctx, 0, sizeof run->ctx);
    memcpy(run->ctx.x, run->args, sizeof run->args);
    run->ctx.entry = (uint64_t)(uintptr_t)entry;
    run->ctx.stack_top = run->stack_top;

    run->gates = 0;
    run->exit_x0 = 0;
    run->exit_sp = 0;
    run->reason = BOTW_GUEST_EXIT_NONE;
    run->error[0] = '\0';
    run->active = 1;

    prev = botw_guest_gate_get_handler();
    g_active = run;
    botw_guest_gate_set_handler(run_gate);

    botw_guest_run_enter_asm(&run->ctx);

    botw_guest_gate_set_handler(prev);
    g_active = NULL;
    run->active = 0;

    run->exit_x0 = run->ctx.exit_x0;
    run->exit_sp = run->ctx.exit_sp;
    run->reason = BOTW_GUEST_EXIT_RETURNED;
    return 0;
#endif
}

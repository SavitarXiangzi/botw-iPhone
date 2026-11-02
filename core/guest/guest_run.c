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
/* guest_run_arm64.S reads the resume descriptor as {regs, kind}. */
BOTW_STATIC_ASSERT(offsetof(botw_guest_resume, regs) == 0, resume_regs);
BOTW_STATIC_ASSERT(offsetof(botw_guest_resume, kind) == 8, resume_kind);
BOTW_STATIC_ASSERT(sizeof(botw_guest_resume) == 16, resume_size);

#if defined(__aarch64__)
extern void botw_guest_run_enter_asm(botw_guest_run_ctx* ctx);
extern void botw_guest_thread_start_asm(void);
extern void botw_guest_thread_exit_asm(void);
#else
/* guest_run_arm64.S emits nothing here.  enter_at() refuses before this could
 * be called, but the symbol still has to exist for the link.  It is a global
 * rather than a static so a host build does not warn about a function that is
 * only reachable through the AArch64 branch. */
void botw_guest_run_enter_asm(botw_guest_run_ctx* ctx) {
    (void)ctx;
}
#endif

static botw_guest_run* g_active;

/* Round-robin from `after`, returning the first READY thread.  `after` is a
 * valid index, or -1 when the caller has no current thread. */
static int pick_next(botw_guest_run* run, int after) {
    int i;
    for (i = 1; i <= BOTW_GUEST_MAX_THREADS; i++) {
        int idx = (after + i) % BOTW_GUEST_MAX_THREADS;
        if (run->threads[idx].state == BOTW_GUEST_THREAD_READY) return idx;
    }
    return -1;
}

/* The trampoline consumes this descriptor immediately, so one slot in the run
 * is enough and cannot be overwritten while an assembly restore is pending. */
static const botw_guest_resume* set_resume(botw_guest_run* run, int next) {
    run->resume.regs = &run->threads[next].frame;
    run->resume.kind = run->threads[next].frame_kind;
    return &run->resume;
}

/* A thread slot became DONE.  Any joiners waiting for it become runnable,
 * but keep their saved frames: the scheduler restores one when it resumes. */
static int wake_joiners(botw_guest_run* run, int target) {
    int i, woke = 0;
    for (i = 0; i < BOTW_GUEST_MAX_THREADS; i++) {
        botw_guest_thread* t = &run->threads[i];
        if (t->state == BOTW_GUEST_THREAD_BLOCKED &&
            t->wait_kind == BOTW_GUEST_WAIT_THREAD &&
            t->wait_arg == (uint64_t)(unsigned)target) {
            t->state = BOTW_GUEST_THREAD_READY;
            t->wait_kind = BOTW_GUEST_WAIT_NONE;
            t->wait_arg = 0;
            run->wakes++;
            woke++;
        }
    }
    return woke;
}

/* Wake every sleeper whose absolute deadline has arrived.  The clock is
 * virtual and monotonic; this never consults the host wall clock. */
static int wake_expired(botw_guest_run* run) {
    int i, woke = 0;
    for (i = 0; i < BOTW_GUEST_MAX_THREADS; i++) {
        botw_guest_thread* t = &run->threads[i];
        if (t->state == BOTW_GUEST_THREAD_BLOCKED &&
            t->wait_kind == BOTW_GUEST_WAIT_SLEEP &&
            t->wait_arg <= run->clock) {
            t->state = BOTW_GUEST_THREAD_READY;
            t->wait_kind = BOTW_GUEST_WAIT_NONE;
            t->wait_arg = 0;
            run->wakes++;
            woke++;
        }
    }
    return woke;
}

/* With nothing runnable, step the virtual clock to the earliest sleep
 * deadline.  That gives the scheduler a way out of "every thread is asleep"
 * without a host timer thread, while arbitrary event waits still deadlock
 * loudly instead of pretending to succeed. */
static int advance_to_earliest_sleeper(botw_guest_run* run) {
    uint64_t earliest = UINT64_MAX;
    int i;
    for (i = 0; i < BOTW_GUEST_MAX_THREADS; i++) {
        const botw_guest_thread* t = &run->threads[i];
        if (t->state == BOTW_GUEST_THREAD_BLOCKED &&
            t->wait_kind == BOTW_GUEST_WAIT_SLEEP &&
            t->wait_arg < earliest) {
            earliest = t->wait_arg;
        }
    }
    if (earliest == UINT64_MAX) return 0;
    if (earliest > run->clock) run->clock = earliest;
    (void)wake_expired(run);
    return 1;
}

#if defined(__aarch64__)
/* The trampoline calls this through the single global handler slot, because
 * botw_guest_gate_dispatch_fn carries no user pointer. */
static void run_gate(botw_guest_regs* regs) {
    botw_guest_run* run = g_active;
    int cur, next;

    if (!run) return;
    run->gates++;
    if (run->gate) run->gate(run->gate_user, regs);

    /* A gate is a scheduling boundary too.  This does not force a switch;
     * it only makes timed threads eligible for one. */
    (void)wake_expired(run);

    if (!run->switch_requested) return;
    run->switch_requested = 0;

    cur = run->current;
    if (cur < 0 || cur >= BOTW_GUEST_MAX_THREADS) return;
    if (run->threads[cur].state == BOTW_GUEST_THREAD_RUNNING) {
        run->threads[cur].frame = *regs;
        run->threads[cur].has_frame = 1;
        run->threads[cur].frame_kind = BOTW_GUEST_FRAME_GATE;
        run->threads[cur].state = BOTW_GUEST_THREAD_READY;
    }

    next = pick_next(run, cur);
    if (next < 0) {
        /* A yield must leave something runnable.  Keep the current thread
         * alive rather than restoring a frame that no longer describes it. */
        run->threads[cur].state = BOTW_GUEST_THREAD_RUNNING;
        return;
    }
    if (next == cur) {
        run->threads[cur].state = BOTW_GUEST_THREAD_RUNNING;
        return;
    }
    run->current = next;
    run->threads[next].state = BOTW_GUEST_THREAD_RUNNING;
    *regs = run->threads[next].frame;
    if (run->threads[next].frame_kind == BOTW_GUEST_FRAME_IMPORT) {
        regs->nzcv |= BOTW_GUEST_NZCV_IMPORT_FRAME;
    } else {
        regs->nzcv &= ~BOTW_GUEST_NZCV_IMPORT_FRAME;
    }
    run->switches++;
}
#endif

/* Called by the thread-exit trampoline.  It runs on the exiting thread's
 * guest stack; the returned frame is restored by that same trampoline. */
const botw_guest_resume* botw_guest_thread_exit_frame(void) {
    botw_guest_run* run = g_active;
    int cur, next;

    if (!run) return NULL;
    cur = run->current;
    if (cur >= 0 && cur < BOTW_GUEST_MAX_THREADS) {
        run->threads[cur].state = BOTW_GUEST_THREAD_DONE;
        run->threads[cur].has_frame = 0;
        run->threads[cur].wait_kind = BOTW_GUEST_WAIT_NONE;
        run->threads[cur].wait_arg = 0;
        run->thread_exits++;
        (void)wake_joiners(run, cur);
    }
    run->switch_requested = 0;
    next = pick_next(run, cur);
    if (next < 0) return NULL;
    run->current = next;
    run->threads[next].state = BOTW_GUEST_THREAD_RUNNING;
    run->switches++;
    return set_resume(run, next);
}

static void run_error(botw_guest_run* run, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(run->error, sizeof run->error, fmt, ap);
    va_end(ap);
}

/* Maps `stack_bytes` usable bytes plus one PROT_NONE guard page below them.
 * On a host without POSIX mmap this fails; guest execution is refused there
 * anyway, but thread_create() must report the same reason. */
static int run_map_stack(size_t stack_bytes, uint8_t** out_mem, size_t* out_total,
                         size_t* out_guard, uint64_t* out_top) {
#if defined(_WIN32)
    (void)stack_bytes;
    (void)out_mem;
    (void)out_total;
    (void)out_guard;
    (void)out_top;
    return -1;
#else
    long page_size = sysconf(_SC_PAGESIZE);
    size_t page;
    size_t usable;
    size_t total;
    uint8_t* mem;

    if (page_size < 0) return -1;
    page = (size_t)page_size;
    if (page < 0x1000) page = 0x1000;
    if (stack_bytes == 0) stack_bytes = 0x10000;
    usable = (stack_bytes + page - 1) & ~(page - 1);
    total = usable + page;
    mem = (uint8_t*)mmap(NULL, total, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return -1;
    if (mprotect(mem, page, PROT_NONE) != 0) {
        munmap(mem, total);
        return -1;
    }
    *out_mem = mem;
    *out_total = total;
    *out_guard = page;
    *out_top = (uint64_t)(uintptr_t)(mem + total);
    return 0;
#endif
}

int botw_guest_run_init(botw_guest_run* run, const void* image_base,
                        size_t image_size, size_t stack_bytes) {
    if (!run) return -1;
    memset(run, 0, sizeof *run);
    run->reason = BOTW_GUEST_EXIT_NONE;
    run->image_base = (const uint8_t*)image_base;
    run->image_size = image_size;

    {
        uint8_t* mem = NULL;
        size_t total = 0;
        size_t guard = 0;
        uint64_t top = 0;

        if (run_map_stack(stack_bytes, &mem, &total, &guard, &top) != 0) {
            run_error(run, "guest execution needs a POSIX mmap; this host only loads images");
            return -1;
        }
        run->stack = mem;
        run->stack_bytes = total;
        run->guard_bytes = guard;
        run->stack_top = top;

        /* Thread 0 is the entry thread.  It gets the stack mapped above and
         * becomes RUNNING only when enter() is called. */
        run->threads[0].state = BOTW_GUEST_THREAD_CREATED;
        run->threads[0].generation = run->generation;
        run->threads[0].stack = mem;
        run->threads[0].stack_bytes = total;
        run->threads[0].guard_bytes = guard;
        run->threads[0].stack_top = top;
        run->thread_count = 1;
        run->current = -1;
        return 0;
    }
}

void botw_guest_run_close(botw_guest_run* run) {
    int i;
    if (!run) return;
#if !defined(_WIN32)
    for (i = 1; i < BOTW_GUEST_MAX_THREADS; i++) {
        if (run->threads[i].stack &&
            run->threads[i].stack != run->stack &&
            run->threads[i].stack_bytes) {
            munmap(run->threads[i].stack, run->threads[i].stack_bytes);
        }
    }
    if (run->stack && run->stack_bytes) munmap(run->stack, run->stack_bytes);
#else
    (void)i;
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

int botw_guest_run_thread_create(botw_guest_run* run, const void* entry,
                                 uint64_t arg, size_t stack_bytes) {
    int i;
    uint8_t* mem = NULL;
    size_t total = 0;
    size_t guard = 0;
    uint64_t top = 0;
    botw_guest_thread* t;

    if (!run || !entry) {
        if (run) run_error(run, "null thread entry");
        return -1;
    }
    if (!run->stack) {
        run_error(run, "run_init() has not mapped a guest stack");
        return -1;
    }
    if (stack_bytes > BOTW_GUEST_THREAD_STACK_MAX) {
        run_error(run, "thread stack of %zu bytes exceeds the %u-byte limit",
                  stack_bytes, (unsigned)BOTW_GUEST_THREAD_STACK_MAX);
        return -1;
    }
    for (i = 1; i < BOTW_GUEST_MAX_THREADS; i++) {
        if (run->threads[i].state == BOTW_GUEST_THREAD_FREE) break;
    }
    if (i == BOTW_GUEST_MAX_THREADS) {
        run_error(run, "thread table is full (%u slots)", (unsigned)BOTW_GUEST_MAX_THREADS);
        return -1;
    }
    if (run_map_stack(stack_bytes, &mem, &total, &guard, &top) != 0) {
        run_error(run, "cannot map a guest thread stack");
        return -1;
    }
    t = &run->threads[i];
    memset(t, 0, sizeof *t);
    t->state = BOTW_GUEST_THREAD_CREATED;
    t->generation = run->generation;
    t->entry = (uint64_t)(uintptr_t)entry;
    t->arg = arg;
    t->stack = mem;
    t->stack_bytes = total;
    t->guard_bytes = guard;
    t->stack_top = top;

    /* A created thread does not resume a gate block; it starts through two
     * tiny host stubs.  start_asm takes x0=arg and x19=entry, sets x30 to the
     * exit stub and branches to the entry.  The synthetic frame leaves the
     * usual 16 bytes above sp for the trampoline's x29/x30 store. */
    memset(&t->frame, 0, sizeof t->frame);
    t->frame_kind = BOTW_GUEST_FRAME_IMPORT;
    t->frame.x[0] = arg;
    t->frame.x[19] = t->entry;
#if defined(__aarch64__)
    t->frame.x[20] = (uint64_t)(uintptr_t)&botw_guest_thread_exit_asm;
    t->frame.pc = (uint64_t)(uintptr_t)&botw_guest_thread_start_asm;
#else
    t->frame.x[20] = 0;
    t->frame.pc = 0;
#endif
    t->frame.sp = top;

    run->thread_count = i + 1;
    return i;
}

int botw_guest_run_thread_start(botw_guest_run* run, int id) {
    if (!run || id <= 0 || id >= BOTW_GUEST_MAX_THREADS) {
        if (run) run_error(run, "thread id %d is out of range", id);
        return -1;
    }
    if (run->threads[id].state != BOTW_GUEST_THREAD_CREATED) {
        run_error(run, "thread %d is not in CREATED state", id);
        return -1;
    }
    run->threads[id].state = BOTW_GUEST_THREAD_READY;
    return 0;
}

int botw_guest_run_thread_reap(botw_guest_run* run, int id) {
    botw_guest_thread* t;

    if (!run || id <= 0 || id >= BOTW_GUEST_MAX_THREADS) {
        if (run) run_error(run, "thread id %d cannot be reaped", id);
        return -1;
    }
    t = &run->threads[id];
    if (t->state != BOTW_GUEST_THREAD_DONE &&
        t->state != BOTW_GUEST_THREAD_CREATED &&
        t->state != BOTW_GUEST_THREAD_FREE) {
        run_error(run, "thread %d is still live (state %d)", id, t->state);
        return -1;
    }
#if !defined(_WIN32)
    if (t->stack && t->stack != run->stack && t->stack_bytes) {
        munmap(t->stack, t->stack_bytes);
    }
#endif
    memset(t, 0, sizeof *t);
    return 0;
}

void botw_guest_run_yield(botw_guest_run* run) {
    if (!run || !run->active || run->current < 0) return;
    run->switch_requested = 1;
    run->threads[run->current].yields++;
}

int botw_guest_run_is_executing(const botw_guest_run* run) {
    return run != NULL && g_active == run && run->active;
}

uint64_t botw_guest_run_clock(const botw_guest_run* run) {
    return run ? run->clock : 0;
}

uint64_t botw_guest_run_advance_clock(botw_guest_run* run, uint64_t ticks) {
    if (!run) return 0;
    if (UINT64_MAX - run->clock < ticks) run->clock = UINT64_MAX;
    else run->clock += ticks;
    (void)wake_expired(run);
    return run->clock;
}

const botw_guest_resume* botw_guest_run_block_and_switch(
    botw_guest_run* run, const botw_guest_regs* frame,
    int wait_kind, uint64_t wait_arg) {
    botw_guest_thread* t;
    int cur, next;

    if (!run || !frame) return NULL;
    if (!botw_guest_run_is_executing(run)) {
        run_error(run, "block requested outside an executing guest run");
        return NULL;
    }
    cur = run->current;
    if (cur < 0 || cur >= BOTW_GUEST_MAX_THREADS) {
        run_error(run, "block requested with no current thread");
        return NULL;
    }
    if (wait_kind != BOTW_GUEST_WAIT_THREAD && wait_kind != BOTW_GUEST_WAIT_SLEEP) {
        run_error(run, "block requested with unknown wait kind %d", wait_kind);
        return NULL;
    }
    t = &run->threads[cur];
    if (t->state != BOTW_GUEST_THREAD_RUNNING) {
        run_error(run, "thread %d blocked from state %d", cur, t->state);
        return NULL;
    }
    if (frame->pc == 0) {
        run_error(run, "thread %d blocked without a return address", cur);
        return NULL;
    }

    t->frame = *frame;
    t->has_frame = 1;
    t->frame_kind = BOTW_GUEST_FRAME_IMPORT;
    t->state = BOTW_GUEST_THREAD_BLOCKED;
    t->wait_kind = wait_kind;
    t->wait_arg = wait_arg;
    run->blocks++;
    run->switch_requested = 0;

    /* Sleep(0) and a deadline already reached must not strand the thread. */
    (void)wake_expired(run);
    next = pick_next(run, cur);
    while (next < 0) {
        if (!advance_to_earliest_sleeper(run)) break;
        next = pick_next(run, cur);
    }
    if (next < 0) {
        run_error(run, "guest deadlock: all threads are blocked and no sleep deadline can wake one");
        return NULL;
    }
    run->current = next;
    run->threads[next].state = BOTW_GUEST_THREAD_RUNNING;
    if (next != cur) run->switches++;
    return set_resume(run, next);
}

int botw_guest_run_current(const botw_guest_run* run) {
    return run ? run->current : -1;
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
    int i;

    if (run->active) {
        run_error(run, "a run is already active; nesting is not supported");
        run->reason = BOTW_GUEST_EXIT_REFUSED;
        return -1;
    }

    /* A live worker from a previous run has a frame pointing into code and a
     * stack that are not being reset; refuse instead of resuming a stale
     * slice.  DONE threads are harmless and remain in the table. */
    for (i = 1; i < BOTW_GUEST_MAX_THREADS; i++) {
        if ((run->threads[i].state == BOTW_GUEST_THREAD_READY ||
             run->threads[i].state == BOTW_GUEST_THREAD_RUNNING ||
             run->threads[i].state == BOTW_GUEST_THREAD_BLOCKED) &&
            run->threads[i].generation != run->generation) {
            run_error(run, "thread %d is still live from a previous run", i);
            run->reason = BOTW_GUEST_EXIT_REFUSED;
            return -1;
        }
    }

    /* Every run starts from a clean register file: x0..x7 from args, the rest
     * zero.  x18 is never loaded by the trampoline. */
    memset(&run->ctx, 0, sizeof run->ctx);
    memcpy(run->ctx.x, run->args, sizeof run->args);
    run->ctx.entry = (uint64_t)(uintptr_t)entry;
    run->ctx.stack_top = run->stack_top;

    run->threads[0].state = BOTW_GUEST_THREAD_RUNNING;
    run->threads[0].has_frame = 0;
    run->threads[0].entry = (uint64_t)(uintptr_t)entry;
    run->threads[0].arg = run->args[0];
    memset(&run->threads[0].frame, 0, sizeof run->threads[0].frame);
    run->current = 0;
    run->switch_requested = 0;
    run->switches = 0;
    run->blocks = 0;
    run->wakes = 0;
    run->thread_exits = 0;

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

    run->threads[0].state = BOTW_GUEST_THREAD_DONE;
    run->threads[0].wait_kind = BOTW_GUEST_WAIT_NONE;
    run->threads[0].wait_arg = 0;
    run->current = -1;
    run->generation++;
    run->exit_x0 = run->ctx.exit_x0;
    run->exit_sp = run->ctx.exit_sp;
    run->reason = run->error[0] ? BOTW_GUEST_EXIT_STOPPED : BOTW_GUEST_EXIT_RETURNED;
    return 0;
#endif
}

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
    BOTW_GUEST_EXIT_REFUSED,    /* enter() refused before executing anything */
    BOTW_GUEST_EXIT_STOPPED     /* the scheduler had to stop a deadlocked run */
} botw_guest_exit;

/* Called from the gate trampoline while the guest is inside.  `user` is the
 * pointer given to botw_guest_run_set_gate().  Returning normally resumes the
 * guest; writing regs->x[] is how a call is answered. */
typedef void (*botw_guest_run_gate_fn)(void* user, botw_guest_regs* regs);

/* Called by the scheduler immediately before a timed waiter is made READY.
 * The HLE uses it to drop the thread from its sync object's waiter list and,
 * for TimedWaitLightEvent, to write the false result into the saved frame.
 * It runs while the thread is still BLOCKED, and must not switch threads. */
typedef void (*botw_guest_run_timeout_fn)(void* user, int thread_slot,
                                          int wait_kind, uint64_t wait_arg);

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

/* Cooperatively scheduled guest threads.
 *
 * A thread owns a guest stack and one saved register frame.  The frame is
 * captured when the thread yields at a gate and restored when the scheduler
 * comes back to it.  Thread 0 is the entry thread created by run_init()/enter;
 * created threads start in CREATED and only become eligible when start() is
 * called.  The table is fixed-size: no allocation happens while the guest is
 * running.
 */
#define BOTW_GUEST_MAX_THREADS 32
#define BOTW_GUEST_THREAD_STACK_MAX (8u * 1024u * 1024u)

typedef enum {
    BOTW_GUEST_THREAD_FREE = 0,
    BOTW_GUEST_THREAD_CREATED,  /* stack and frame exist, not started */
    BOTW_GUEST_THREAD_READY,    /* runnable, waiting for a time slice */
    BOTW_GUEST_THREAD_RUNNING,
    BOTW_GUEST_THREAD_BLOCKED,  /* has a resumable frame, waiting for a wake */
    BOTW_GUEST_THREAD_DONE
} botw_guest_thread_state;

/* Why a BLOCKED thread is not runnable.  Thread exit wakes THREAD waiters;
 * the scheduler's virtual clock wakes timed waiters; a sync object's signal
 * operation wakes MUTEX/EVENT/LIGHT_EVENT/SEMAPHORE waiters.  wait_arg is the
 * object index for the sync kinds and the target slot for THREAD. */
typedef enum {
    BOTW_GUEST_WAIT_NONE = 0,
    BOTW_GUEST_WAIT_THREAD,     /* wait_arg is the target thread slot */
    BOTW_GUEST_WAIT_SLEEP,      /* deadline is an absolute run-clock tick */
    BOTW_GUEST_WAIT_MUTEX,      /* wait_arg is a host-side mutex index */
    BOTW_GUEST_WAIT_EVENT,      /* wait_arg is a host-side event index */
    BOTW_GUEST_WAIT_LIGHT_EVENT,/* wait_arg is a host-side light-event index */
    BOTW_GUEST_WAIT_SEMAPHORE   /* wait_arg is a host-side semaphore index */
} botw_guest_wait_kind;

/* No deadline.  Timed waits store an absolute run-clock tick instead. */
#define BOTW_GUEST_NO_DEADLINE UINT64_MAX

/* The two saved-frame shapes need different restore sequences.  A gate frame
 * returns through the rewritten block's `ldp x29,x30,[sp],#16`; an import
 * frame returns directly to the instruction after `blr`. */
typedef enum {
    BOTW_GUEST_FRAME_GATE = 0,
    BOTW_GUEST_FRAME_IMPORT = 1
} botw_guest_frame_kind;

typedef struct {
    botw_guest_regs* regs;
    int              kind;      /* botw_guest_frame_kind */
} botw_guest_resume;

typedef struct {
    int      state;             /* botw_guest_thread_state */
    int      has_frame;         /* frame holds a resumable gate snapshot */
    uint64_t entry;             /* guest entry address */
    uint64_t arg;               /* passed in x0 */
    uint8_t* stack;             /* whole mapping, guard page included */
    size_t   stack_bytes;
    size_t   guard_bytes;
    uint64_t stack_top;
    botw_guest_regs frame;      /* valid when has_frame is set */
    int      frame_kind;        /* botw_guest_frame_kind for frame */
    int      wait_kind;         /* botw_guest_wait_kind while BLOCKED */
    uint64_t wait_arg;          /* target slot or sync-object index */
    uint64_t wait_deadline;     /* absolute tick, or BOTW_GUEST_NO_DEADLINE */
    unsigned long yields;       /* explicit yields observed from this thread */
    unsigned long generation;   /* run generation that created this thread */
} botw_guest_thread;

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
    botw_guest_run_timeout_fn timeout_fn;
    void*    timeout_user;
    botw_guest_run_ctx ctx;
    botw_guest_thread threads[BOTW_GUEST_MAX_THREADS];
    int      thread_count;       /* high-water mark, informational */
    int      current;            /* running thread, -1 when idle */
    int      switch_requested;   /* set by botw_guest_run_yield() */
    botw_guest_resume resume;    /* consumed immediately after a switch */
    unsigned long switches;      /* context switches, self-yields excluded */
    unsigned long blocks;        /* entries through block_and_switch() */
    unsigned long wakes;         /* blocked threads made READY again */
    unsigned long thread_exits;  /* created threads that returned */
    uint64_t clock;              /* virtual ticking clock shared with nn::os */
    unsigned long generation;    /* bumped after each completed run */
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

/* Installs the timed-wait cleanup hook, or clears it with fn == NULL. */
void botw_guest_run_set_timeout(botw_guest_run* run, botw_guest_run_timeout_fn fn,
                                void* user);

/* Runs <image_base + entry_off> until the guest returns.  Returns 0 when the
 * guest got as far as executing, -1 with `error` set when it refused. */
int botw_guest_run_enter(botw_guest_run* run, size_t entry_off);

/* Same, for code that is not part of the image (tests, a JIT arena). */
int botw_guest_run_enter_at(botw_guest_run* run, const void* entry);

/* Allocates a guest stack (with a guard page) and a CREATED thread.  `entry`
 * is a guest code address; `arg` is placed in x0.  Returns the thread index
 * (>= 1), or -1 with error set.  Thread 0 is the entry thread and is not
 * created through this API. */
int botw_guest_run_thread_create(botw_guest_run* run, const void* entry,
                                 uint64_t arg, size_t stack_bytes);

/* Makes a CREATED thread runnable.  Returns 0, or -1 with error set. */
int botw_guest_run_thread_start(botw_guest_run* run, int id);


/* Frees a DONE or never-started CREATED worker's stack and returns its slot
 * to FREE.  Thread 0 and live threads are refused.  DestroyThread HLE uses
 * this, so a reaped slot can be reused without waiting for run_close(). */
int botw_guest_run_thread_reap(botw_guest_run* run, int id);

/* Asks the scheduler to switch at the end of the current gate callback.  It
 * is a no-op outside an active run.  HLE primitives call this to implement a
 * yield; a guest cannot observe it until the gate returns. */
void botw_guest_run_yield(botw_guest_run* run);

/* True only between enter_asm() and its return, not merely run->active.  The
 * blocking HLE wrappers use this to refuse to abandon a synthetic test state
 * that no assembly frame would resume. */
int botw_guest_run_is_executing(const botw_guest_run* run);

/* Runs the virtual clock forward and wakes every SLEEP waiter whose deadline
 * has arrived.  Returns the new clock value.  Safe on a closed run. */
uint64_t botw_guest_run_advance_clock(botw_guest_run* run, uint64_t ticks);

/* The current thread's virtual-clock value. */
uint64_t botw_guest_run_clock(const botw_guest_run* run);

/* Parks the current RUNNING thread and returns the next runnable frame, or
 * NULL when every thread is blocked and no future sleep deadline can make
 * progress (a deadlock; run->error says so).  `frame` must be the complete
 * register snapshot at the import boundary, including the return address.
 *
 * This is the context switch used by a blocking HLE import.  Unlike yield(),
 * it happens immediately: the caller must restore the returned frame rather
 * than return from the host call. */
const botw_guest_resume* botw_guest_run_block_and_switch(
    botw_guest_run* run, const botw_guest_regs* frame,
    int wait_kind, uint64_t wait_arg, uint64_t wait_deadline);

/* Makes a BLOCKED thread runnable.  Returns 1 when it changed state, 0 when
 * the slot was not blocked, -1 on a bad slot.  The caller has already updated
 * the object it was waiting for (mutex owner, semaphore count, ...). */
int botw_guest_run_wake(botw_guest_run* run, int thread_slot);

/* Index of the running thread, or -1. */
int botw_guest_run_current(const botw_guest_run* run);

/* Called by the thread-exit trampoline, not by normal host code.  Marks the
 * current thread DONE and returns the next frame (with its restore kind), or
 * NULL when the run has no runnable thread left. */
const botw_guest_resume* botw_guest_thread_exit_frame(void);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_RUN_H */

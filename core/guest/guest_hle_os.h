/* nn::os HLE: threads, TLS, the virtual clock and the memory-block heap.
 *
 * The 1.6.0 main image imports 78 nn::os names.  They are not a service: they
 * are the SDK's own operating-system layer, so a zero-return stub is not a
 * neutral answer.  This layer claims the 33 names the scheduler can answer
 * today:
 *
 *   - the thread table (CreateThread / StartThread / WaitThread / DestroyThread,
 *     current-thread queries, names, priority and core masks);
 *   - per-thread TLS slots;
 *   - GetSystemTick / frequency / conversions;
 *   - AllocateMemoryBlock / FreeMemoryBlock / SetMemoryHeapSize.
 *
 * Synchronization objects (Mutex, Event, LightEvent, Semaphore, MessageQueue,
 * SystemEvent, MultiWait) stay on the shared stub for now.  Implementing them
 * before the scheduler can block and wake would produce a lock that reports
 * success without excluding anyone.
 *
 * Scheduling contract, stated plainly because it is not the real kernel:
 *
 *   - CreateThread reserves a worker in the attached botw_guest_run and gives
 *     it a private mmap'd guest stack with a guard page.  The `stack` pointer
 *     the guest passes is recorded for diagnostics but is not used: the host
 *     needs a guard page and ownership it can prove.
 *   - StartThread moves CREATED -> READY.  The first switch happens at the
 *     next host gate, not inside the import call.
 *   - YieldThread and SleepThread call botw_guest_run_yield().  The guest keeps
 *     executing until the next gate, where the trampoline restores another
 *     thread's frame.  SleepThread does not suspend the caller yet; it records
 *     the request and yields.
 *   - WaitThread returns immediately when the target is already DONE.  For a
 *     live target it records a wait and requests a yield; this is not a join
 *     until the scheduler can put the caller in BLOCKED and wake it on exit.
 *   - DestroyThread reaps a DONE (or never-started CREATED) worker and returns
 *     its slot and stack to the run.
 *
 * ThreadType pointers are opaque guest handles.  The game's own ThreadType
 * objects live in guest memory; the HLE never writes into them.  The main
 * thread has no such object, so GetCurrentThread() returns a state-owned
 * 0x200-byte block that is only used as an identity token.
 *
 * All state is plain host data.  Nothing here executes guest code, and nothing
 * here creates a host thread: the scheduler remains cooperative and
 * single-threaded.
 */
#ifndef BOTW_GUEST_HLE_OS_H
#define BOTW_GUEST_HLE_OS_H

#include <stdint.h>

#include "guest_hle.h"
#include "guest_run.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOTW_HLE_OS_COUNT 33u
#define BOTW_HLE_OS_TLS_SLOTS 16u
#define BOTW_HLE_OS_NAME_MAX 64u
#define BOTW_HLE_OS_MAIN_ID 0x100u
#define BOTW_HLE_OS_MAIN_TYPE_BYTES 0x200u
#define BOTW_HLE_OS_TICK_FREQ 0x124F800ull

/* A failure that is recognizably ours: bit31 set, module 0x4F, description 1.
 * Callers only test "zero or not"; this is not a Nintendo result code. */
#define BOTW_HLE_OS_ERROR 0x804F0001u

typedef struct {
    unsigned used;
    void*    type;              /* guest ThreadType* (identity token) */
    int      slot;              /* botw_guest_run thread index */
    int      priority;
    int      core_id;
    uint64_t core_mask;
    uint64_t id;                /* GetThreadId value, stable for the run */
    uint64_t guest_stack;       /* CreateThread argument, diagnostics only */
    uint64_t guest_stack_size;  /* CreateThread argument, diagnostics only */
    const char* name_ptr;       /* SetThreadNamePointer value, if any */
    char     name[BOTW_HLE_OS_NAME_MAX];
} botw_hle_os_thread;

typedef struct {
    botw_guest_run* run;
    int attached;
    botw_hle_os_thread threads[BOTW_GUEST_MAX_THREADS];
    uint8_t  main_type[BOTW_HLE_OS_MAIN_TYPE_BYTES];
    uint64_t next_thread_id;
    uint64_t tick;
    uint64_t memory_heap_size;
    int64_t  last_sleep_ns;
    uint64_t sleep_deadline;
    uint64_t tls[BOTW_GUEST_MAX_THREADS][BOTW_HLE_OS_TLS_SLOTS];
    uint8_t  tls_used[BOTW_HLE_OS_TLS_SLOTS];
    void*    tls_dtor[BOTW_HLE_OS_TLS_SLOTS];
    unsigned long calls;
    unsigned long thread_creates;
    unsigned long thread_starts;
    unsigned long thread_waits;
    unsigned long thread_destroys;
    unsigned long yields;
    unsigned long sleeps;
    unsigned long block_allocs;
    unsigned long block_frees;
    char     error[160];
} botw_hle_os_state;

/* Clears every mapping, TLS value and counter.  Does not free worker stacks;
 * the attached run owns those, and botw_guest_run_close() remains the final
 * owner. */
void botw_hle_os_reset(botw_hle_os_state* state);

/* The singleton used when register() is given NULL. */
botw_hle_os_state* botw_hle_os_default(void);

/* Registers the 33 nn::os implementations.  `state` may be NULL.  Returns 0,
 * or -1 with hle->error set. */
int botw_hle_os_register(botw_hle_table* hle, botw_hle_os_state* state);

/* Binds the state to the run whose thread table StartThread/GetCurrentThread
 * use.  Passing NULL detaches.  Worker mappings are cleared; the run itself is
 * never modified here.  Call after botw_guest_run_init() and before the guest
 * can reach an nn::os import. */
void botw_hle_os_attach_run(botw_hle_os_state* state, botw_guest_run* run);

/* Adds to the deterministic virtual clock.  GetSystemTick() also advances one
 * tick per call so a polling loop makes progress without a host clock. */
void botw_hle_os_advance(botw_hle_os_state* state, uint64_t ticks);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_HLE_OS_H */

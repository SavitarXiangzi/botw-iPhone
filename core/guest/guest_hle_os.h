/* nn::os HLE: threads, TLS, the virtual clock, synchronization and the
 * memory-block heap.
 *
 * The 1.6.0 main image imports 74 nn::os names.  They are not a service: they
 * are the SDK's own operating-system layer, so a zero-return stub is not a
 * neutral answer.  This table has 63 entries: 62 imported names the scheduler
 * can answer, plus FreeMemoryBlock for allocator symmetry.  Twelve imported
 * names remain on the shared stub:
 *
 *   - the thread table (CreateThread / StartThread / WaitThread / DestroyThread,
 *     current-thread queries, names, priority and core masks);
 *   - per-thread TLS slots;
 *   - GetSystemTick / frequency / conversions;
 *   - Mutex / Event / LightEvent / Semaphore, with real blocking and wake;
 *   - MessageQueue: FIFO send, LIFO jam, receive and peek, with blocked
 *     senders and receivers handed their message before they are resumed;
 *   - AllocateMemoryBlock / FreeMemoryBlock / SetMemoryHeapSize.
 *
 * The twelve are SystemEvent (4), MultiWait (7), and SetUserExceptionHandler
 * (1).  A lock that reports success without excluding anyone would be worse
 * than an explicit stub, so these stay explicit until their ordering rules
 * are implemented.
 *
 * Scheduling contract, stated plainly because it is not the real kernel:
 *
 *   - CreateThread reserves a worker in the attached botw_guest_run and gives
 *     it a private mmap'd guest stack with a guard page.  The `stack` pointer
 *     the guest passes is recorded for diagnostics but is not used: the host
 *     needs a guard page and ownership it can prove.
 *   - StartThread moves CREATED -> READY.  The first switch happens at the
 *     next host gate, not inside the import call.
 *   - YieldThread requests a switch at the next gate.  It cannot switch inside
 *     a host call because the guest's return point would be lost.
 *   - SleepThread, WaitThread, LockMutex, WaitEvent, WaitLightEvent,
 *     TimedWaitLightEvent, AcquireSemaphore, SendMessageQueue,
 *     JamMessageQueue and ReceiveMessageQueue use AArch64 import wrappers
 *     that save the complete register frame, park the caller in BLOCKED, and
 *     restore another runnable frame.  Sleep and timed waits are woken by the
 *     virtual run clock; a live WaitThread target wakes its joiners from the
 *     thread-exit trampoline; a signal/release/message handoff transfers the
 *     object exactly once before making the chosen waiter READY.
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

#define BOTW_HLE_OS_COUNT 78u
#define BOTW_HLE_OS_MAX_MUTEX 128u
#define BOTW_HLE_OS_MAX_EVENT 128u
#define BOTW_HLE_OS_MAX_LIGHT_EVENT 128u
#define BOTW_HLE_OS_MAX_SEMAPHORE 64u
#define BOTW_HLE_OS_MAX_MESSAGE_QUEUE 64u
#define BOTW_HLE_OS_MAX_SYSTEM_EVENT 128u
#define BOTW_HLE_OS_MAX_MULTI_WAIT 64u
#define BOTW_HLE_OS_MAX_MULTI_WAIT_HOLDER 128u
#define BOTW_HLE_OS_TLS_SLOTS 16u
#define BOTW_HLE_OS_NAME_MAX 64u
#define BOTW_HLE_OS_MAIN_ID 0x100u
#define BOTW_HLE_OS_MAIN_TYPE_BYTES 0x200u
#define BOTW_HLE_OS_TICK_FREQ 0x124F800ull

/* A failure that is recognizably ours: bit31 set, module 0x4F, description 1.
 * Callers only test "zero or not"; this is not a Nintendo result code. */
#define BOTW_HLE_OS_ERROR 0x804F0001u

/* Blocking import entry points exported by guest_hle_os_arm64.S.  The table
 * registers these instead of the plain C implementations on AArch64. */
typedef enum {
    BOTW_HLE_OS_OP_SLEEP = 1,
    BOTW_HLE_OS_OP_WAIT_THREAD = 2,
    BOTW_HLE_OS_OP_LOCK_MUTEX = 3,
    BOTW_HLE_OS_OP_WAIT_EVENT = 4,
    BOTW_HLE_OS_OP_WAIT_LIGHT_EVENT = 5,
    BOTW_HLE_OS_OP_TIMED_WAIT_LIGHT_EVENT = 6,
    BOTW_HLE_OS_OP_ACQUIRE_SEMAPHORE = 7,
    BOTW_HLE_OS_OP_SEND_MESSAGE_QUEUE = 8,
    BOTW_HLE_OS_OP_RECEIVE_MESSAGE_QUEUE = 9,
    BOTW_HLE_OS_OP_JAM_MESSAGE_QUEUE = 10,
    BOTW_HLE_OS_OP_WAIT_SYSTEM_EVENT = 11,
    BOTW_HLE_OS_OP_TIMED_WAIT_SYSTEM_EVENT = 12,
    BOTW_HLE_OS_OP_WAIT_ANY = 13
} botw_hle_os_block_op;

enum {
    BOTW_HLE_OS_RETURN = 0,     /* resume the frame that made the call */
    BOTW_HLE_OS_SWITCHED = 1,   /* next_frame holds another thread's frame */
    BOTW_HLE_OS_STOPPED = 2     /* deadlock; finish the guest run */
};

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

/* Sync objects are keyed by the guest's own object pointer.  The HLE never
 * writes into that memory: the pointer is only an identity token, exactly as
 * ThreadType* is.  A fixed table with explicit "table full" errors is safer
 * than silently aliasing objects after a wrap. */
typedef struct {
    int      used;
    void*    type;
    int      owner;             /* thread slot, -1 when unlocked */
    int      depth;             /* recursion count for a recursive mutex */
    int      recursive;
    int      waiters[BOTW_GUEST_MAX_THREADS];
    int      waiter_count;
} botw_hle_os_mutex;

typedef struct {
    int      used;
    void*    type;
    int      signaled;
    int      auto_clear;
    int      waiters[BOTW_GUEST_MAX_THREADS];
    int      waiter_count;
} botw_hle_os_event;

typedef struct {
    int      used;
    void*    type;
    int      signaled;
    int      auto_clear;
    int      waiters[BOTW_GUEST_MAX_THREADS];
    int      waiter_count;
} botw_hle_os_light_event;

typedef struct {
    int      used;
    void*    type;
    int      count;
    int      maximum;
    int      waiters[BOTW_GUEST_MAX_THREADS];
    int      waiter_count;
} botw_hle_os_semaphore;

typedef struct {
    int      slot;
    uint64_t out;               /* guest uintptr_t* to fill before wake */
} botw_hle_os_mq_receiver;

typedef struct {
    int      slot;
    uint64_t message;           /* pending Send/Jam payload */
    int      jam;               /* insert at the front, not the back */
} botw_hle_os_mq_sender;

/* The guest supplies the ring storage.  Only the queue object pointer is an
 * identity token; `buffer` is data and is read/written as the real ring. */
typedef struct {
    int      used;
    int      index;
    void*    type;
    uint64_t* buffer;
    size_t   capacity;
    size_t   count;
    size_t   offset;
    botw_hle_os_mq_receiver receivers[BOTW_GUEST_MAX_THREADS];
    int      receiver_count;
    botw_hle_os_mq_sender senders[BOTW_GUEST_MAX_THREADS];
    int      sender_count;
} botw_hle_os_message_queue;

/* SystemEvent is an Event-like object keyed by the guest's SystemEventType
 * pointer.  The imported wait/try/timed-wait calls all observe this table. */
typedef struct {
    int      used;
    void*    type;
    int      signaled;
    int      auto_clear;
    int      waiters[BOTW_GUEST_MAX_THREADS];
    int      waiter_count;
} botw_hle_os_system_event;

/* A MultiWaitHolder points at one EventType; the holder itself is the object
 * WaitAny returns.  Link order is preserved because WaitAny must choose the
 * first signaled holder in the order the guest linked them. */
typedef struct {
    int      used;
    void*    type;
    int      event_index;
    int      owner;             /* multi-wait index, or -1 when unlinked */
    int      link_pos;
} botw_hle_os_multi_wait_holder;

typedef struct {
    int      used;
    void*    type;
    int      holders[BOTW_GUEST_MAX_THREADS];
    int      holder_count;
    int      waiter;            /* thread slot, or -1 */
} botw_hle_os_multi_wait;

typedef struct {
    botw_guest_run* run;
    int attached;
    botw_hle_os_thread threads[BOTW_GUEST_MAX_THREADS];
    botw_hle_os_mutex mutexes[BOTW_HLE_OS_MAX_MUTEX];
    botw_hle_os_event events[BOTW_HLE_OS_MAX_EVENT];
    botw_hle_os_light_event light_events[BOTW_HLE_OS_MAX_LIGHT_EVENT];
    botw_hle_os_semaphore semaphores[BOTW_HLE_OS_MAX_SEMAPHORE];
    botw_hle_os_message_queue message_queues[BOTW_HLE_OS_MAX_MESSAGE_QUEUE];
    botw_hle_os_system_event system_events[BOTW_HLE_OS_MAX_SYSTEM_EVENT];
    botw_hle_os_multi_wait multi_waits[BOTW_HLE_OS_MAX_MULTI_WAIT];
    botw_hle_os_multi_wait_holder multi_wait_holders[BOTW_HLE_OS_MAX_MULTI_WAIT_HOLDER];
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
    unsigned long wait_self;
    unsigned long wait_done;
    unsigned long wait_unknown;
    unsigned long thread_destroys;
    unsigned long yields;
    unsigned long sleeps;
    unsigned long sleep_blocks;
    unsigned long wait_blocks;
    unsigned long mutex_locks;
    unsigned long mutex_try_locks;
    unsigned long mutex_blocks;
    unsigned long mutex_unlocks;
    unsigned long event_waits;
    unsigned long event_blocks;
    unsigned long event_signals;
    unsigned long event_clears;
    unsigned long light_event_waits;
    unsigned long light_event_blocks;
    unsigned long light_event_signals;
    unsigned long light_event_timeouts;
    unsigned long semaphore_acquires;
    unsigned long semaphore_blocks;
    unsigned long semaphore_releases;
    unsigned long message_sends;
    unsigned long message_send_blocks;
    unsigned long message_jams;
    unsigned long message_jam_blocks;
    unsigned long message_receives;
    unsigned long message_receive_blocks;
    unsigned long message_try_sends;
    unsigned long message_try_receives;
    unsigned long message_peeks;
    unsigned long system_event_waits;
    unsigned long system_event_blocks;
    unsigned long system_event_signals;
    unsigned long system_event_timeouts;
    unsigned long system_event_destroys;
    unsigned long multi_wait_waits;
    unsigned long multi_wait_blocks;
    unsigned long multi_wait_wakes;
    unsigned long multi_wait_links;
    unsigned long block_allocs;
    unsigned long block_frees;
    void (*user_exception_handler)(void*);
    void*    user_exception_arg;
    void*    user_exception_stack;
    uint64_t user_exception_stack_size;
    unsigned long user_exception_sets;
    char     error[160];
} botw_hle_os_state;

/* Clears every mapping, TLS value and counter.  Does not free worker stacks;
 * the attached run owns those, and botw_guest_run_close() remains the final
 * owner. */
void botw_hle_os_reset(botw_hle_os_state* state);

/* The singleton used when register() is given NULL. */
botw_hle_os_state* botw_hle_os_default(void);

/* Registers the 78 table entries (all 74 title imports plus four helpers).  `state` may be NULL.  Returns 0,
 * or -1 with hle->error set. */
int botw_hle_os_register(botw_hle_table* hle, botw_hle_os_state* state);

/* Binds the state to the run whose thread table StartThread/GetCurrentThread
 * use.  Passing NULL detaches.  Worker mappings, sync objects and the timed-
 * wait hook are cleared; the run's saved frames are not.  Call after
 * botw_guest_run_init() and before the guest can reach an nn::os import. */
void botw_hle_os_attach_run(botw_hle_os_state* state, botw_guest_run* run);

/* Adds to the deterministic virtual clock and wakes any attached SLEEP
 * waiters whose deadline has arrived.  GetSystemTick() also advances one tick
 * per call so a polling loop makes progress without a host clock. */
void botw_hle_os_advance(botw_hle_os_state* state, uint64_t ticks);

/* Called by the AArch64 blocking wrapper, not by ordinary C code.  `frame`
 * points at the wrapper's complete saved registers; the function performs the
 * op, writes its return value into frame->x[0], and reports whether assembly
 * must resume this frame, jump to next_resume, or finish the run. */
int botw_hle_os_blocking_call(botw_guest_regs* frame, unsigned op,
                              const botw_guest_resume** next_resume);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_HLE_OS_H */

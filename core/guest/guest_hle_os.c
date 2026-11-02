#include "guest_hle_os.h"

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
/* The blocking wrappers live in guest_hle_os_arm64.S.  They are entered with
 * the guest's register file, not with a C signature, so their prototypes are
 * intentionally just symbols here. */
extern void botw_hle_os_guest_sleep(void);
extern void botw_hle_os_guest_wait_thread(void);
extern void botw_hle_os_guest_lock_mutex(void);
extern void botw_hle_os_guest_wait_event(void);
extern void botw_hle_os_guest_wait_light_event(void);
extern void botw_hle_os_guest_timed_wait_light_event(void);
extern void botw_hle_os_guest_acquire_semaphore(void);
extern void botw_hle_os_guest_send_message_queue(void);
extern void botw_hle_os_guest_receive_message_queue(void);
extern void botw_hle_os_guest_jam_message_queue(void);
#endif

/* The implementations are registered under their Itanium-mangled names,
 * because that is what the rewritten image imports.  The names are copied from
 * the 1.6.0 main image's undefined-symbol list; a wrong digit in a length
 * prefix does not fail here, it silently leaves the real import on the shared
 * zero-return stub.  The test checks every name individually, and reloc_run
 * --hle reports how many nn::os names are still stubs.
 */
typedef struct {
    const char* name;
    void* fn;
} os_entry;

#define OS_THREAD_DEFAULT_PRIORITY 0x2Cu
#define OS_THREAD_DEFAULT_CORE_MASK 0x7ull
#define OS_BLOCK_ALIGN ((uintptr_t)0x1000u)
#define OS_BLOCK_MAGIC 0x424F54574F53424Dull  /* "BOTWOSBM" */

static botw_hle_os_state* g_os;
static botw_hle_os_state g_os_default;
static int g_os_default_ready;
static char g_host_arg0[] = "botw-guest";
static char* g_host_argv[] = { g_host_arg0, NULL };

static void os_sync_timeout(void* user, int thread_slot, int wait_kind,
                            uint64_t wait_arg);

static uint32_t os_error(botw_hle_os_state* state, const char* fmt, ...) {
    va_list ap;
    if (state) {
        va_start(ap, fmt);
        vsnprintf(state->error, sizeof state->error, fmt, ap);
        va_end(ap);
    }
    return BOTW_HLE_OS_ERROR;
}

static botw_hle_os_thread* thread_by_slot(botw_hle_os_state* state, int slot) {
    if (!state || slot < 0 || slot >= (int)BOTW_GUEST_MAX_THREADS) return NULL;
    return state->threads[slot].used ? &state->threads[slot] : NULL;
}

static botw_hle_os_thread* thread_by_type(botw_hle_os_state* state, const void* type) {
    int i;
    if (!state || !type) return NULL;
    for (i = 0; i < (int)BOTW_GUEST_MAX_THREADS; i++) {
        if (state->threads[i].used && state->threads[i].type == type) {
            return &state->threads[i];
        }
    }
    return NULL;
}

static int current_slot(const botw_hle_os_state* state) {
    if (!state || !state->run) return 0;
    if (state->run->current < 0 || state->run->current >= (int)BOTW_GUEST_MAX_THREADS) return 0;
    return state->run->current;
}

static void sync_tick_from_run(botw_hle_os_state* state) {
    if (state && state->run) state->tick = botw_guest_run_clock(state->run);
}

static void init_main_thread(botw_hle_os_state* state) {
    botw_hle_os_thread* t = &state->threads[0];
    memset(&state->threads[0], 0, sizeof state->threads[0]);
    t->used = 1;
    t->type = state->main_type;
    t->slot = 0;
    t->priority = OS_THREAD_DEFAULT_PRIORITY;
    t->core_id = 0;
    t->core_mask = OS_THREAD_DEFAULT_CORE_MASK;
    t->id = BOTW_HLE_OS_MAIN_ID;
    memcpy(t->name, "main", 5);
}

void botw_hle_os_reset(botw_hle_os_state* state) {
    if (!state) return;
    if (state->run) botw_guest_run_set_timeout(state->run, NULL, NULL);
    memset(state, 0, sizeof *state);
    init_main_thread(state);
    state->next_thread_id = BOTW_HLE_OS_MAIN_ID + 1u;
}

botw_hle_os_state* botw_hle_os_default(void) {
    if (!g_os_default_ready) {
        botw_hle_os_reset(&g_os_default);
        g_os_default_ready = 1;
    }
    return &g_os_default;
}

void botw_hle_os_attach_run(botw_hle_os_state* state, botw_guest_run* run) {
    int i;
    if (!state) return;
    if (state->run) botw_guest_run_set_timeout(state->run, NULL, NULL);
    state->run = run;
    state->attached = run != NULL;
    memset(state->mutexes, 0, sizeof state->mutexes);
    memset(state->events, 0, sizeof state->events);
    memset(state->light_events, 0, sizeof state->light_events);
    memset(state->semaphores, 0, sizeof state->semaphores);
    memset(state->message_queues, 0, sizeof state->message_queues);
    for (i = 1; i < (int)BOTW_GUEST_MAX_THREADS; i++) {
        memset(&state->threads[i], 0, sizeof state->threads[i]);
    }
    memset(state->tls, 0, sizeof state->tls);
    memset(state->tls_used, 0, sizeof state->tls_used);
    memset(state->tls_dtor, 0, sizeof state->tls_dtor);
    state->tick = 0;
    if (run) run->clock = 0;
    if (run) botw_guest_run_set_timeout(run, os_sync_timeout, state);
    state->next_thread_id = BOTW_HLE_OS_MAIN_ID + 1u;
}

void botw_hle_os_advance(botw_hle_os_state* state, uint64_t ticks) {
    if (!state) return;
    if (state->run) {
        state->tick = botw_guest_run_advance_clock(state->run, ticks);
    } else {
        if (UINT64_MAX - state->tick < ticks) state->tick = UINT64_MAX;
        else state->tick += ticks;
    }
}

/* ------------------------------------------------------------------ */
/* Threads                                                            */
/* ------------------------------------------------------------------ */

static void* os_GetCurrentThread(void) {
    botw_hle_os_thread* t;
    if (!g_os) return NULL;
    g_os->calls++;
    t = thread_by_slot(g_os, current_slot(g_os));
    return t ? t->type : NULL;
}

static uint64_t os_GetThreadId(const void* type) {
    botw_hle_os_thread* t;
    if (!g_os) return 0;
    g_os->calls++;
    t = thread_by_type(g_os, type);
    return t ? t->id : 0;
}

static const char* os_GetThreadNamePointer(const void* type) {
    botw_hle_os_thread* t;
    if (!g_os) return "";
    g_os->calls++;
    t = thread_by_type(g_os, type);
    if (!t) return "";
    return t->name_ptr ? t->name_ptr : t->name;
}

static void os_SetThreadNamePointer(void* type, const char* name) {
    botw_hle_os_thread* t;
    if (!g_os) return;
    g_os->calls++;
    t = thread_by_type(g_os, type);
    if (t) t->name_ptr = name;
}

static void os_SetThreadName(void* type, const char* name) {
    botw_hle_os_thread* t;
    size_t n;
    if (!g_os) return;
    g_os->calls++;
    t = thread_by_type(g_os, type);
    if (!t) return;
    if (!name) {
        t->name[0] = '\0';
        t->name_ptr = NULL;
        return;
    }
    n = strlen(name);
    if (n >= BOTW_HLE_OS_NAME_MAX) n = BOTW_HLE_OS_NAME_MAX - 1u;
    memcpy(t->name, name, n);
    t->name[n] = '\0';
    t->name_ptr = NULL;
}

static int os_GetThreadPriority(const void* type) {
    botw_hle_os_thread* t;
    if (!g_os) return OS_THREAD_DEFAULT_PRIORITY;
    g_os->calls++;
    t = thread_by_type(g_os, type);
    return t ? t->priority : OS_THREAD_DEFAULT_PRIORITY;
}

static void os_ChangeThreadPriority(void* type, int priority) {
    botw_hle_os_thread* t;
    if (!g_os) return;
    g_os->calls++;
    t = thread_by_type(g_os, type);
    if (t) t->priority = priority;
}

static uint32_t os_GetThreadCoreMask(int* out_id, uint64_t* out_mask, const void* type) {
    botw_hle_os_thread* t;
    if (!g_os) return BOTW_HLE_OS_ERROR;
    g_os->calls++;
    t = thread_by_type(g_os, type);
    if (out_id) *out_id = t ? t->core_id : 0;
    if (out_mask) *out_mask = t ? t->core_mask : 0;
    return 0;
}

static uint32_t os_SetThreadCoreMask(void* type, int core_id, uint64_t mask) {
    botw_hle_os_thread* t;
    if (!g_os) return BOTW_HLE_OS_ERROR;
    g_os->calls++;
    t = thread_by_type(g_os, type);
    if (!t) return os_error(g_os, "SetThreadCoreMask: unknown ThreadType");
    t->core_id = core_id;
    t->core_mask = mask;
    return 0;
}

static uint64_t os_GetThreadAvailableCoreMask(void) {
    if (g_os) g_os->calls++;
    /* Core 0 is the one this cooperative guest actually runs on; the mask is
     * what the guest may ask for, not a promise of parallel execution. */
    return OS_THREAD_DEFAULT_CORE_MASK;
}

static int os_GetCurrentCoreNumber(void) {
    if (g_os) g_os->calls++;
    return 0;
}

static void os_GetCurrentStackInfo(size_t* out_base, size_t* out_size) {
    int slot;
    const botw_guest_thread* t;
    if (out_base) *out_base = 0;
    if (out_size) *out_size = 0;
    if (!g_os) return;
    g_os->calls++;
    if (!g_os->run) return;
    slot = current_slot(g_os);
    t = &g_os->run->threads[slot];
    if (t->stack) {
        if (out_base) *out_base = (size_t)(uintptr_t)(t->stack + t->guard_bytes);
        if (out_size) *out_size = t->stack_bytes - t->guard_bytes;
    }
}

static void* os_GetCurrentFiber(void) {
    return os_GetCurrentThread();
}

static void os_YieldThread(void) {
    if (!g_os) return;
    g_os->calls++;
    g_os->yields++;
    if (g_os->run) botw_guest_run_yield(g_os->run);
}

static uint64_t span_to_ticks(int64_t ns) {
    uint64_t u;
    if (ns <= 0) return 0;
    u = (uint64_t)ns;
    return (u / 1000000000ull) * BOTW_HLE_OS_TICK_FREQ +
           ((u % 1000000000ull) * BOTW_HLE_OS_TICK_FREQ) / 1000000000ull;
}

static int64_t ticks_to_span(uint64_t tick) {
    uint64_t sec = tick / BOTW_HLE_OS_TICK_FREQ;
    uint64_t rem = tick % BOTW_HLE_OS_TICK_FREQ;
    if (sec > (uint64_t)(INT64_MAX / 1000000000ll)) return INT64_MAX;
    return (int64_t)(sec * 1000000000ull + rem * 1000000000ull / BOTW_HLE_OS_TICK_FREQ);
}

static uint64_t sleep_prepare(int64_t ns) {
    uint64_t deadline;
    if (!g_os) return 0;
    g_os->calls++;
    g_os->sleeps++;
    g_os->last_sleep_ns = ns;
    sync_tick_from_run(g_os);
    deadline = g_os->tick + span_to_ticks(ns);
    g_os->sleep_deadline = deadline;
    return deadline;
}

#if !defined(__aarch64__)
/* Portable fallback used on hosts without the AArch64 blocking wrapper.  The
 * wrapper takes the real path on Apple arm64; this one can only request a
 * switch at the next gate. */
static void os_SleepThread(int64_t ns) {
    if (!g_os) return;
    (void)sleep_prepare(ns);
    if (g_os->run) botw_guest_run_yield(g_os->run);
}
#endif

static uint32_t os_CreateThread_common(void* type, void (*entry)(void*), void* arg,
                                       void* stack, size_t stack_size,
                                       int priority, int core) {
    int slot;
    botw_hle_os_thread* t;
    if (!g_os) return BOTW_HLE_OS_ERROR;
    g_os->calls++;
    if (!type || !entry) return os_error(g_os, "CreateThread: null ThreadType or entry");
    if (!g_os->run) return os_error(g_os, "CreateThread: no guest run is attached");
    if (thread_by_type(g_os, type)) return os_error(g_os, "CreateThread: ThreadType is already live");
    slot = botw_guest_run_thread_create(g_os->run, (const void*)entry,
                                        (uint64_t)(uintptr_t)arg, stack_size);
    if (slot < 0) {
        return os_error(g_os, "CreateThread: %s",
                        g_os->run->error[0] ? g_os->run->error : "run refused the thread");
    }
    t = &g_os->threads[slot];
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->type = type;
    t->slot = slot;
    t->priority = priority;
    t->core_id = core;
    t->core_mask = OS_THREAD_DEFAULT_CORE_MASK;
    t->id = g_os->next_thread_id++;
    t->guest_stack = (uint64_t)(uintptr_t)stack;
    t->guest_stack_size = (uint64_t)stack_size;
    (void)snprintf(t->name, sizeof t->name, "thread %d", slot);
    g_os->thread_creates++;
    return 0;
}

static uint32_t os_CreateThread6(void* type, void (*entry)(void*), void* arg,
                                 void* stack, size_t stack_size, int priority) {
    return os_CreateThread_common(type, entry, arg, stack, stack_size, priority, 0);
}

static uint32_t os_CreateThread7(void* type, void (*entry)(void*), void* arg,
                                 void* stack, size_t stack_size, int priority, int core) {
    return os_CreateThread_common(type, entry, arg, stack, stack_size, priority, core);
}

static void os_StartThread(void* type) {
    botw_hle_os_thread* t;
    if (!g_os) return;
    g_os->calls++;
    t = thread_by_type(g_os, type);
    if (!t) {
        (void)os_error(g_os, "StartThread: unknown ThreadType");
        return;
    }
    if (!g_os->run) {
        (void)os_error(g_os, "StartThread: no guest run is attached");
        return;
    }
    if (botw_guest_run_thread_start(g_os->run, t->slot) != 0) {
        (void)os_error(g_os, "StartThread: %s",
                       g_os->run->error[0] ? g_os->run->error : "run refused the start");
        return;
    }
    g_os->thread_starts++;
}

static int wait_prepare(void* type, int* out_slot) {
    botw_hle_os_thread* t;
    int cur;
    if (!g_os) return 0;
    g_os->calls++;
    t = thread_by_type(g_os, type);
    if (!t || !g_os->run) {
        g_os->wait_unknown++;
        return 0;
    }
    cur = current_slot(g_os);
    if (t->slot == cur) {
        g_os->wait_self++;
        return 0;
    }
    if (t->slot >= 0 && t->slot < (int)BOTW_GUEST_MAX_THREADS &&
        g_os->run->threads[t->slot].state == BOTW_GUEST_THREAD_DONE) {
        g_os->wait_done++;
        return 0;
    }
    g_os->thread_waits++;
    if (out_slot) *out_slot = t->slot;
    return 1;
}

#if !defined(__aarch64__)
static void os_WaitThread(void* type) {
    if (!g_os) return;
    if (!wait_prepare(type, NULL)) return;
    botw_guest_run_yield(g_os->run);
}
#endif

static void os_DestroyThread(void* type) {
    botw_hle_os_thread* t;
    if (!g_os) return;
    g_os->calls++;
    t = thread_by_type(g_os, type);
    if (!t) return;
    if (t->slot == 0) {
        (void)os_error(g_os, "DestroyThread: the main thread cannot be destroyed");
        return;
    }
    if (!g_os->run) {
        (void)os_error(g_os, "DestroyThread: no guest run is attached");
        return;
    }
    if (botw_guest_run_thread_reap(g_os->run, t->slot) != 0) {
        (void)os_error(g_os, "DestroyThread: %s",
                       g_os->run->error[0] ? g_os->run->error : "run refused the reap");
        return;
    }
    memset(t, 0, sizeof *t);
    g_os->thread_destroys++;
}

static int os_GetHostArgc(void) {
    if (g_os) g_os->calls++;
    return 1;
}

static char** os_GetHostArgv(void) {
    if (g_os) g_os->calls++;
    return g_host_argv;
}

/* ------------------------------------------------------------------ */
/* TLS                                                                */
/* ------------------------------------------------------------------ */

static uint32_t os_AllocateTlsSlot(uint32_t* out_slot, void (*dtor)(void*)) {
    unsigned i;
    if (!g_os) return BOTW_HLE_OS_ERROR;
    g_os->calls++;
    if (!out_slot) return os_error(g_os, "AllocateTlsSlot: null out pointer");
    for (i = 0; i < BOTW_HLE_OS_TLS_SLOTS; i++) {
        if (!g_os->tls_used[i]) {
            g_os->tls_used[i] = 1;
            g_os->tls_dtor[i] = (void*)(uintptr_t)dtor;
            *out_slot = i;
            return 0;
        }
    }
    return os_error(g_os, "AllocateTlsSlot: all %u slots are in use",
                    (unsigned)BOTW_HLE_OS_TLS_SLOTS);
}

static void os_FreeTlsSlot(uint32_t slot) {
    int i;
    if (!g_os) return;
    g_os->calls++;
    if (slot >= BOTW_HLE_OS_TLS_SLOTS) return;
    g_os->tls_used[slot] = 0;
    g_os->tls_dtor[slot] = NULL;
    for (i = 0; i < (int)BOTW_GUEST_MAX_THREADS; i++) g_os->tls[i][slot] = 0;
}

static uint64_t os_GetTlsValue(uint32_t slot) {
    int cur;
    if (!g_os) return 0;
    g_os->calls++;
    if (slot >= BOTW_HLE_OS_TLS_SLOTS || !g_os->tls_used[slot]) return 0;
    cur = current_slot(g_os);
    return g_os->tls[cur][slot];
}

static void os_SetTlsValue(uint32_t slot, uint64_t value) {
    int cur;
    if (!g_os) return;
    g_os->calls++;
    if (slot >= BOTW_HLE_OS_TLS_SLOTS || !g_os->tls_used[slot]) return;
    cur = current_slot(g_os);
    g_os->tls[cur][slot] = value;
}

/* ------------------------------------------------------------------ */
/* Virtual time                                                       */
/* ------------------------------------------------------------------ */

static uint64_t os_GetSystemTick(void) {
    if (!g_os) return 0;
    g_os->calls++;
    /* One tick per query keeps a guest poll loop moving without a host clock;
     * the shell may also call botw_hle_os_advance() in bigger steps. */
    if (g_os->run) g_os->tick = botw_guest_run_advance_clock(g_os->run, 1);
    else g_os->tick++;
    return g_os->tick;
}

static uint64_t os_GetSystemTickFrequency(void) {
    if (g_os) g_os->calls++;
    return BOTW_HLE_OS_TICK_FREQ;
}

static uint64_t os_ConvertToTick(int64_t ns) {
    if (g_os) g_os->calls++;
    return span_to_ticks(ns);
}

static int64_t os_ConvertToTimeSpan(uint64_t tick) {
    if (g_os) g_os->calls++;
    return ticks_to_span(tick);
}

/* ------------------------------------------------------------------ */
/* Memory blocks                                                      */
/* ------------------------------------------------------------------ */

static void os_SetMemoryHeapSize(size_t size) {
    if (!g_os) return;
    g_os->calls++;
    g_os->memory_heap_size = (uint64_t)size;
}

static uint32_t os_AllocateMemoryBlock(size_t* out, size_t size) {
    void* raw;
    uintptr_t p;
    uintptr_t aligned;
    size_t total;
    uint64_t* hdr;
    if (!g_os) return BOTW_HLE_OS_ERROR;
    g_os->calls++;
    if (!out) return os_error(g_os, "AllocateMemoryBlock: null out pointer");
    if (size == 0) size = 1;
    if (size > (size_t)(SIZE_MAX - 3u * OS_BLOCK_ALIGN)) {
        return os_error(g_os, "AllocateMemoryBlock: size overflow");
    }
    total = size + 3u * OS_BLOCK_ALIGN;
    raw = malloc(total);
    if (!raw) return os_error(g_os, "AllocateMemoryBlock: host allocation failed");
    p = (uintptr_t)raw;
    aligned = (p + OS_BLOCK_ALIGN + OS_BLOCK_ALIGN - 1u) & ~(OS_BLOCK_ALIGN - 1u);
    hdr = (uint64_t*)aligned;
    hdr[-2] = OS_BLOCK_MAGIC;
    hdr[-1] = (uint64_t)p;
    *out = (size_t)aligned;
    g_os->block_allocs++;
    return 0;
}

static void os_FreeMemoryBlock(size_t addr, size_t size) {
    uint64_t* hdr;
    (void)size;
    if (!g_os) return;
    g_os->calls++;
    if (!addr) return;
    hdr = (uint64_t*)(uintptr_t)addr;
    if (hdr[-2] != OS_BLOCK_MAGIC) {
        (void)os_error(g_os, "FreeMemoryBlock: pointer was not returned by AllocateMemoryBlock");
        return;
    }
    free((void*)(uintptr_t)hdr[-1]);
    g_os->block_frees++;
}

/* ------------------------------------------------------------------ */
/* Mutex / Event / LightEvent / Semaphore                             */
/* ------------------------------------------------------------------ */

/* The guest's object memory is never read or written.  The pointer is only a
 * key into these fixed tables, so an unknown layout cannot corrupt host state
 * and a table-full condition is reported instead of aliasing two objects. */

static int waiter_index(const int* waiters, int count, int slot) {
    int i;
    for (i = 0; i < count; i++) {
        if (waiters[i] == slot) return i;
    }
    return -1;
}

static int waiter_add(int* waiters, int* count, int slot) {
    if (waiter_index(waiters, *count, slot) >= 0) return 1;
    if (*count >= (int)BOTW_GUEST_MAX_THREADS) {
        (void)os_error(g_os, "sync object waiter list is full");
        return 0;
    }
    waiters[(*count)++] = slot;
    return 1;
}

static void waiter_remove_at(int* waiters, int* count, int pos) {
    int i;
    if (pos < 0 || pos >= *count) return;
    for (i = pos + 1; i < *count; i++) waiters[i - 1] = waiters[i];
    (*count)--;
}

static int thread_waiting_on(int slot, int kind, int index) {
    const botw_guest_thread* t;
    if (!g_os || !g_os->run) return 0;
    if (slot < 0 || slot >= (int)BOTW_GUEST_MAX_THREADS) return 0;
    t = &g_os->run->threads[slot];
    return t->state == BOTW_GUEST_THREAD_BLOCKED &&
           t->wait_kind == kind &&
           t->wait_arg == (uint64_t)(unsigned)index;
}

/* Drop entries whose thread timed out (or whose run is gone).  Timed waits
 * are completed by os_sync_timeout(), but compacting here also covers an HLE
 * state that was detached while an object still had queued waiters. */
static void waiter_compact(int* waiters, int* count, int kind, int index) {
    int i = 0;
    while (i < *count) {
        if (thread_waiting_on(waiters[i], kind, index)) {
            i++;
        } else {
            waiter_remove_at(waiters, count, i);
        }
    }
}

static int wake_sync_waiter(int slot, int result, int write_result) {
    botw_guest_thread* t;
    if (!g_os || !g_os->run) return 0;
    if (slot < 0 || slot >= (int)BOTW_GUEST_MAX_THREADS) return 0;
    t = &g_os->run->threads[slot];
    if (write_result && t->has_frame) t->frame.x[0] = (uint64_t)(unsigned)result;
    return botw_guest_run_wake(g_os->run, slot) > 0;
}

static botw_hle_os_mutex* mutex_find(void* type, int* out_index) {
    int i;
    if (!g_os || !type) return NULL;
    for (i = 0; i < (int)BOTW_HLE_OS_MAX_MUTEX; i++) {
        if (g_os->mutexes[i].used && g_os->mutexes[i].type == type) {
            if (out_index) *out_index = i;
            return &g_os->mutexes[i];
        }
    }
    return NULL;
}

static botw_hle_os_mutex* mutex_create(void* type, int recursive, int* out_index) {
    botw_hle_os_mutex* m;
    int existing = -1;
    int i;
    if (!g_os) return NULL;
    if (!type) {
        (void)os_error(g_os, "mutex: null MutexType");
        return NULL;
    }
    m = mutex_find(type, &existing);
    if (m) {
        waiter_compact(m->waiters, &m->waiter_count,
                       BOTW_GUEST_WAIT_MUTEX, existing);
        if (m->waiter_count != 0) {
            (void)os_error(g_os, "InitializeMutex: object still has waiters");
            return NULL;
        }
        memset(m, 0, sizeof *m);
        m->used = 1;
        m->type = type;
        if (out_index) *out_index = existing;
    } else {
        for (i = 0; i < (int)BOTW_HLE_OS_MAX_MUTEX; i++) {
            if (!g_os->mutexes[i].used) break;
        }
        if (i == (int)BOTW_HLE_OS_MAX_MUTEX) {
            (void)os_error(g_os, "InitializeMutex: mutex table is full");
            return NULL;
        }
        m = &g_os->mutexes[i];
        memset(m, 0, sizeof *m);
        m->used = 1;
        m->type = type;
        if (out_index) *out_index = i;
    }
    m->owner = -1;
    m->depth = 0;
    m->recursive = recursive != 0;
    return m;
}

static botw_hle_os_mutex* mutex_begin(void* type, int* out_index) {
    botw_hle_os_mutex* m;
    if (!g_os) return NULL;
    g_os->calls++;
    m = mutex_find(type, out_index);
    if (!m) (void)os_error(g_os, "LockMutex: MutexType was not initialized");
    return m;
}

static int mutex_acquire_now(botw_hle_os_mutex* m, int slot) {
    if (!m) return 0;
    if (m->owner < 0) {
        m->owner = slot;
        m->depth = 1;
        g_os->mutex_locks++;
        return 1;
    }
    if (m->owner == slot && m->recursive) {
        m->depth++;
        g_os->mutex_locks++;
        return 1;
    }
    return 0;
}

static void os_InitializeMutex(void* type, int recursive, int priority) {
    (void)priority;
    if (!g_os) return;
    g_os->calls++;
    (void)mutex_create(type, recursive, NULL);
}

static void os_FinalizeMutex(void* type) {
    botw_hle_os_mutex* m;
    int index = -1;
    if (!g_os) return;
    g_os->calls++;
    m = mutex_find(type, &index);
    if (!m) return;
    waiter_compact(m->waiters, &m->waiter_count, BOTW_GUEST_WAIT_MUTEX, index);
    if (m->waiter_count != 0) {
        (void)os_error(g_os, "FinalizeMutex: object still has waiters");
        return;
    }
    memset(m, 0, sizeof *m);
}

static int os_TryLockMutex(void* type) {
    botw_hle_os_mutex* m;
    int index = -1;
    if (!g_os) return 0;
    g_os->calls++;
    g_os->mutex_try_locks++;
    m = mutex_find(type, &index);
    if (!m) {
        (void)os_error(g_os, "TryLockMutex: MutexType was not initialized");
        return 0;
    }
    return mutex_acquire_now(m, current_slot(g_os));
}

static void os_UnlockMutex(void* type) {
    botw_hle_os_mutex* m;
    int index = -1;
    int cur;
    if (!g_os) return;
    g_os->calls++;
    m = mutex_find(type, &index);
    if (!m) {
        (void)os_error(g_os, "UnlockMutex: MutexType was not initialized");
        return;
    }
    cur = current_slot(g_os);
    if (m->owner != cur) {
        (void)os_error(g_os, "UnlockMutex: caller does not own the mutex");
        return;
    }
    g_os->mutex_unlocks++;
    if (m->depth > 1) {
        m->depth--;
        return;
    }
    m->owner = -1;
    m->depth = 0;
    waiter_compact(m->waiters, &m->waiter_count, BOTW_GUEST_WAIT_MUTEX, index);
    if (m->waiter_count > 0) {
        int next = m->waiters[0];
        waiter_remove_at(m->waiters, &m->waiter_count, 0);
        m->owner = next;
        m->depth = 1;
        g_os->mutex_locks++;
        (void)wake_sync_waiter(next, 0, 0);
    }
}

static botw_hle_os_event* event_find(void* type, int* out_index) {
    int i;
    if (!g_os || !type) return NULL;
    for (i = 0; i < (int)BOTW_HLE_OS_MAX_EVENT; i++) {
        if (g_os->events[i].used && g_os->events[i].type == type) {
            if (out_index) *out_index = i;
            return &g_os->events[i];
        }
    }
    return NULL;
}

static botw_hle_os_event* event_create(void* type, int signaled, int auto_clear,
                                       int* out_index) {
    botw_hle_os_event* e;
    int existing = -1;
    int i;
    if (!g_os) return NULL;
    if (!type) {
        (void)os_error(g_os, "event: null EventType");
        return NULL;
    }
    e = event_find(type, &existing);
    if (e) {
        waiter_compact(e->waiters, &e->waiter_count,
                       BOTW_GUEST_WAIT_EVENT, existing);
        if (e->waiter_count != 0) {
            (void)os_error(g_os, "InitializeEvent: object still has waiters");
            return NULL;
        }
        memset(e, 0, sizeof *e);
        e->used = 1;
        e->type = type;
        if (out_index) *out_index = existing;
    } else {
        for (i = 0; i < (int)BOTW_HLE_OS_MAX_EVENT; i++) {
            if (!g_os->events[i].used) break;
        }
        if (i == (int)BOTW_HLE_OS_MAX_EVENT) {
            (void)os_error(g_os, "InitializeEvent: event table is full");
            return NULL;
        }
        e = &g_os->events[i];
        memset(e, 0, sizeof *e);
        e->used = 1;
        e->type = type;
        if (out_index) *out_index = i;
    }
    e->signaled = signaled != 0;
    e->auto_clear = auto_clear != 0;
    return e;
}

static botw_hle_os_event* event_begin(void* type, int* out_index) {
    botw_hle_os_event* e;
    if (!g_os) return NULL;
    g_os->calls++;
    e = event_find(type, out_index);
    if (!e) (void)os_error(g_os, "WaitEvent: EventType was not initialized");
    return e;
}

static int event_consume(botw_hle_os_event* e) {
    if (!e || !e->signaled) return 0;
    if (e->auto_clear) e->signaled = 0;
    return 1;
}

static void os_InitializeEvent(void* type, int signaled, int clear_mode) {
    if (!g_os) return;
    g_os->calls++;
    (void)event_create(type, signaled, clear_mode, NULL);
}

static void os_FinalizeEvent(void* type) {
    botw_hle_os_event* e;
    int index = -1;
    if (!g_os) return;
    g_os->calls++;
    e = event_find(type, &index);
    if (!e) return;
    waiter_compact(e->waiters, &e->waiter_count, BOTW_GUEST_WAIT_EVENT, index);
    if (e->waiter_count != 0) {
        (void)os_error(g_os, "FinalizeEvent: object still has waiters");
        return;
    }
    memset(e, 0, sizeof *e);
}

static int event_get_result(void* type) {
    botw_hle_os_event* e;
    if (!g_os) return 0;
    g_os->calls++;
    g_os->event_waits++;
    e = event_find(type, NULL);
    if (!e) {
        (void)os_error(g_os, "WaitEvent: EventType was not initialized");
        return 0;
    }
    return event_consume(e);
}

static int os_TryWaitEvent(void* type) {
    return event_get_result(type);
}

static void os_SignalEvent(void* type) {
    botw_hle_os_event* e;
    int index = -1;
    if (!g_os) return;
    g_os->calls++;
    e = event_find(type, &index);
    if (!e) {
        (void)os_error(g_os, "SignalEvent: EventType was not initialized");
        return;
    }
    g_os->event_signals++;
    e->signaled = 1;
    waiter_compact(e->waiters, &e->waiter_count, BOTW_GUEST_WAIT_EVENT, index);
    if (e->auto_clear) {
        if (e->waiter_count > 0) {
            int next = e->waiters[0];
            waiter_remove_at(e->waiters, &e->waiter_count, 0);
            e->signaled = 0;
            (void)wake_sync_waiter(next, 1, 1);
        }
    } else {
        int i;
        for (i = 0; i < e->waiter_count; i++) {
            (void)wake_sync_waiter(e->waiters[i], 1, 1);
        }
        e->waiter_count = 0;
    }
}

static void os_ClearEvent(void* type) {
    botw_hle_os_event* e;
    if (!g_os) return;
    g_os->calls++;
    e = event_find(type, NULL);
    if (!e) {
        (void)os_error(g_os, "ClearEvent: EventType was not initialized");
        return;
    }
    e->signaled = 0;
    g_os->event_clears++;
}

static botw_hle_os_light_event* light_event_find(void* type, int* out_index) {
    int i;
    if (!g_os || !type) return NULL;
    for (i = 0; i < (int)BOTW_HLE_OS_MAX_LIGHT_EVENT; i++) {
        if (g_os->light_events[i].used && g_os->light_events[i].type == type) {
            if (out_index) *out_index = i;
            return &g_os->light_events[i];
        }
    }
    return NULL;
}

static botw_hle_os_light_event* light_event_create(void* type, int signaled,
                                                   int auto_clear, int* out_index) {
    botw_hle_os_light_event* e;
    int existing = -1;
    int i;
    if (!g_os) return NULL;
    if (!type) {
        (void)os_error(g_os, "light event: null LightEventType");
        return NULL;
    }
    e = light_event_find(type, &existing);
    if (e) {
        waiter_compact(e->waiters, &e->waiter_count,
                       BOTW_GUEST_WAIT_LIGHT_EVENT, existing);
        if (e->waiter_count != 0) {
            (void)os_error(g_os, "InitializeLightEvent: object still has waiters");
            return NULL;
        }
        memset(e, 0, sizeof *e);
        e->used = 1;
        e->type = type;
        if (out_index) *out_index = existing;
    } else {
        for (i = 0; i < (int)BOTW_HLE_OS_MAX_LIGHT_EVENT; i++) {
            if (!g_os->light_events[i].used) break;
        }
        if (i == (int)BOTW_HLE_OS_MAX_LIGHT_EVENT) {
            (void)os_error(g_os, "InitializeLightEvent: light-event table is full");
            return NULL;
        }
        e = &g_os->light_events[i];
        memset(e, 0, sizeof *e);
        e->used = 1;
        e->type = type;
        if (out_index) *out_index = i;
    }
    e->signaled = signaled != 0;
    e->auto_clear = auto_clear != 0;
    return e;
}

static int light_event_consume(botw_hle_os_light_event* e) {
    if (!e || !e->signaled) return 0;
    if (e->auto_clear) e->signaled = 0;
    return 1;
}

#if !defined(__aarch64__)
static int light_event_get_result(void* type) {
    botw_hle_os_light_event* e;
    if (!g_os) return 0;
    g_os->calls++;
    g_os->light_event_waits++;
    e = light_event_find(type, NULL);
    if (!e) {
        (void)os_error(g_os, "WaitLightEvent: LightEventType was not initialized");
        return 0;
    }
    return light_event_consume(e);
}
#endif

static void os_InitializeLightEvent(void* type, int signaled, int clear_mode) {
    if (!g_os) return;
    g_os->calls++;
    (void)light_event_create(type, signaled, clear_mode, NULL);
}

static void os_FinalizeLightEvent(void* type) {
    botw_hle_os_light_event* e;
    int index = -1;
    if (!g_os) return;
    g_os->calls++;
    e = light_event_find(type, &index);
    if (!e) return;
    waiter_compact(e->waiters, &e->waiter_count, BOTW_GUEST_WAIT_LIGHT_EVENT, index);
    if (e->waiter_count != 0) {
        (void)os_error(g_os, "FinalizeLightEvent: object still has waiters");
        return;
    }
    memset(e, 0, sizeof *e);
}

static void os_SignalLightEvent(void* type) {
    botw_hle_os_light_event* e;
    int index = -1;
    if (!g_os) return;
    g_os->calls++;
    e = light_event_find(type, &index);
    if (!e) {
        (void)os_error(g_os, "SignalLightEvent: LightEventType was not initialized");
        return;
    }
    g_os->light_event_signals++;
    e->signaled = 1;
    waiter_compact(e->waiters, &e->waiter_count,
                   BOTW_GUEST_WAIT_LIGHT_EVENT, index);
    if (e->auto_clear) {
        if (e->waiter_count > 0) {
            int next = e->waiters[0];
            waiter_remove_at(e->waiters, &e->waiter_count, 0);
            e->signaled = 0;
            (void)wake_sync_waiter(next, 1, 1);
        }
    } else {
        int i;
        for (i = 0; i < e->waiter_count; i++) {
            (void)wake_sync_waiter(e->waiters[i], 1, 1);
        }
        e->waiter_count = 0;
    }
}

static void os_ClearLightEvent(void* type) {
    botw_hle_os_light_event* e;
    if (!g_os) return;
    g_os->calls++;
    e = light_event_find(type, NULL);
    if (!e) {
        (void)os_error(g_os, "ClearLightEvent: LightEventType was not initialized");
        return;
    }
    e->signaled = 0;
}

static botw_hle_os_semaphore* semaphore_find(void* type, int* out_index) {
    int i;
    if (!g_os || !type) return NULL;
    for (i = 0; i < (int)BOTW_HLE_OS_MAX_SEMAPHORE; i++) {
        if (g_os->semaphores[i].used && g_os->semaphores[i].type == type) {
            if (out_index) *out_index = i;
            return &g_os->semaphores[i];
        }
    }
    return NULL;
}

static botw_hle_os_semaphore* semaphore_create(void* type, int count, int maximum,
                                               int* out_index) {
    botw_hle_os_semaphore* s;
    int existing = -1;
    int i;
    if (!g_os) return NULL;
    if (!type) {
        (void)os_error(g_os, "semaphore: null SemaphoreType");
        return NULL;
    }
    if (count < 0 || maximum <= 0 || count > maximum) {
        (void)os_error(g_os, "InitializeSemaphore: invalid count %d / maximum %d",
                       count, maximum);
        return NULL;
    }
    s = semaphore_find(type, &existing);
    if (s) {
        waiter_compact(s->waiters, &s->waiter_count,
                       BOTW_GUEST_WAIT_SEMAPHORE, existing);
        if (s->waiter_count != 0) {
            (void)os_error(g_os, "InitializeSemaphore: object still has waiters");
            return NULL;
        }
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->type = type;
        if (out_index) *out_index = existing;
    } else {
        for (i = 0; i < (int)BOTW_HLE_OS_MAX_SEMAPHORE; i++) {
            if (!g_os->semaphores[i].used) break;
        }
        if (i == (int)BOTW_HLE_OS_MAX_SEMAPHORE) {
            (void)os_error(g_os, "InitializeSemaphore: semaphore table is full");
            return NULL;
        }
        s = &g_os->semaphores[i];
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->type = type;
        if (out_index) *out_index = i;
    }
    s->count = count;
    s->maximum = maximum;
    return s;
}

static botw_hle_os_semaphore* semaphore_begin(void* type, int* out_index) {
    botw_hle_os_semaphore* s;
    if (!g_os) return NULL;
    g_os->calls++;
    s = semaphore_find(type, out_index);
    if (!s) (void)os_error(g_os, "AcquireSemaphore: SemaphoreType was not initialized");
    return s;
}

static int semaphore_acquire_now(botw_hle_os_semaphore* s) {
    if (!s || s->count <= 0) return 0;
    s->count--;
    g_os->semaphore_acquires++;
    return 1;
}

static void os_InitializeSemaphore(void* type, int count, int maximum) {
    if (!g_os) return;
    g_os->calls++;
    (void)semaphore_create(type, count, maximum, NULL);
}

static void os_FinalizeSemaphore(void* type) {
    botw_hle_os_semaphore* s;
    int index = -1;
    if (!g_os) return;
    g_os->calls++;
    s = semaphore_find(type, &index);
    if (!s) return;
    waiter_compact(s->waiters, &s->waiter_count, BOTW_GUEST_WAIT_SEMAPHORE, index);
    if (s->waiter_count != 0) {
        (void)os_error(g_os, "FinalizeSemaphore: object still has waiters");
        return;
    }
    memset(s, 0, sizeof *s);
}

static void os_ReleaseSemaphore_count(void* type, int count) {
    botw_hle_os_semaphore* s;
    int index = -1;
    if (!g_os) return;
    g_os->calls++;
    s = semaphore_find(type, &index);
    if (!s) {
        (void)os_error(g_os, "ReleaseSemaphore: SemaphoreType was not initialized");
        return;
    }
    if (count <= 0) {
        (void)os_error(g_os, "ReleaseSemaphore: count must be positive");
        return;
    }
    if (s->count > s->maximum - count) {
        (void)os_error(g_os, "ReleaseSemaphore: count %d exceeds maximum %d",
                       count, s->maximum);
        return;
    }
    g_os->semaphore_releases++;
    s->count += count;
    waiter_compact(s->waiters, &s->waiter_count, BOTW_GUEST_WAIT_SEMAPHORE, index);
    while (s->count > 0 && s->waiter_count > 0) {
        int next = s->waiters[0];
        waiter_remove_at(s->waiters, &s->waiter_count, 0);
        s->count--;
        g_os->semaphore_acquires++;
        (void)wake_sync_waiter(next, 0, 0);
    }
}

static void os_ReleaseSemaphore(void* type) {
    os_ReleaseSemaphore_count(type, 1);
}

static void os_ReleaseSemaphore_n(void* type, int count) {
    os_ReleaseSemaphore_count(type, count);
}

/* ------------------------------------------------------------------ */
/* MessageQueue                                                       */
/* ------------------------------------------------------------------ */

/* The ring itself is the guest's buffer.  The queue object remains an opaque
 * identity token, but the messages must be visible to the guest, so they are
 * copied through the real storage the caller supplied to Initialize.  A
 * blocked receiver is handed its value (and a blocked sender has its pending
 * value inserted) before it is made READY: the import wrapper resumes after
 * the call and never re-enters this C function. */

static botw_hle_os_message_queue* mq_find(const void* type, int* out_index) {
    int i;
    if (!g_os || !type) return NULL;
    for (i = 0; i < (int)BOTW_HLE_OS_MAX_MESSAGE_QUEUE; i++) {
        if (g_os->message_queues[i].used && g_os->message_queues[i].type == type) {
            if (out_index) *out_index = i;
            return &g_os->message_queues[i];
        }
    }
    return NULL;
}

static int mq_receiver_index(const botw_hle_os_message_queue* q, int slot) {
    int i;
    for (i = 0; i < q->receiver_count; i++) {
        if (q->receivers[i].slot == slot) return i;
    }
    return -1;
}

static int mq_sender_index(const botw_hle_os_message_queue* q, int slot) {
    int i;
    for (i = 0; i < q->sender_count; i++) {
        if (q->senders[i].slot == slot) return i;
    }
    return -1;
}

static void mq_receiver_remove_at(botw_hle_os_message_queue* q, int pos) {
    int i;
    if (pos < 0 || pos >= q->receiver_count) return;
    for (i = pos + 1; i < q->receiver_count; i++) {
        q->receivers[i - 1] = q->receivers[i];
    }
    q->receiver_count--;
}

static void mq_sender_remove_at(botw_hle_os_message_queue* q, int pos) {
    int i;
    if (pos < 0 || pos >= q->sender_count) return;
    for (i = pos + 1; i < q->sender_count; i++) {
        q->senders[i - 1] = q->senders[i];
    }
    q->sender_count--;
}

static int mq_receiver_add(botw_hle_os_message_queue* q, int slot, uint64_t out) {
    int pos = mq_receiver_index(q, slot);
    if (pos >= 0) {
        q->receivers[pos].out = out;
        return 1;
    }
    if (q->receiver_count >= (int)BOTW_GUEST_MAX_THREADS) {
        (void)os_error(g_os, "MessageQueue: receiver waiter list is full");
        return 0;
    }
    q->receivers[q->receiver_count].slot = slot;
    q->receivers[q->receiver_count].out = out;
    q->receiver_count++;
    return 1;
}

static int mq_sender_add(botw_hle_os_message_queue* q, int slot,
                         uint64_t message, int jam) {
    int pos = mq_sender_index(q, slot);
    if (pos >= 0) {
        q->senders[pos].message = message;
        q->senders[pos].jam = jam;
        return 1;
    }
    if (q->sender_count >= (int)BOTW_GUEST_MAX_THREADS) {
        (void)os_error(g_os, "MessageQueue: sender waiter list is full");
        return 0;
    }
    q->senders[q->sender_count].slot = slot;
    q->senders[q->sender_count].message = message;
    q->senders[q->sender_count].jam = jam;
    q->sender_count++;
    return 1;
}

static void mq_compact_waiters(botw_hle_os_message_queue* q) {
    int i = 0;
    while (i < q->receiver_count) {
        if (thread_waiting_on(q->receivers[i].slot,
                              BOTW_GUEST_WAIT_MESSAGE_QUEUE_RECEIVE,
                              q->index)) {
            i++;
        } else {
            mq_receiver_remove_at(q, i);
        }
    }
    i = 0;
    while (i < q->sender_count) {
        if (thread_waiting_on(q->senders[i].slot,
                              BOTW_GUEST_WAIT_MESSAGE_QUEUE_SEND,
                              q->index)) {
            i++;
        } else {
            mq_sender_remove_at(q, i);
        }
    }
}

static int mq_insert(botw_hle_os_message_queue* q, uint64_t message, int jam) {
    size_t ind;
    if (!q || q->count >= q->capacity) return 0;
    if (jam) {
        ind = (q->offset + q->capacity - 1u) % q->capacity;
        q->offset = ind;
    } else {
        ind = (q->offset + q->count) % q->capacity;
    }
    q->buffer[ind] = message;
    q->count++;
    return 1;
}

static int mq_remove(botw_hle_os_message_queue* q, uint64_t* out) {
    if (!q || q->count == 0) return 0;
    *out = q->buffer[q->offset];
    q->offset = (q->offset + 1u) % q->capacity;
    q->count--;
    return 1;
}

static int mq_fill_senders(botw_hle_os_message_queue* q) {
    int admitted = 0;
    while (q->count < q->capacity && q->sender_count > 0) {
        botw_hle_os_mq_sender sender = q->senders[0];
        if (!thread_waiting_on(sender.slot, BOTW_GUEST_WAIT_MESSAGE_QUEUE_SEND,
                               q->index)) {
            mq_sender_remove_at(q, 0);
            continue;
        }
        if (!mq_insert(q, sender.message, sender.jam)) break;
        mq_sender_remove_at(q, 0);
        (void)wake_sync_waiter(sender.slot, 0, 0);
        admitted++;
    }
    return admitted;
}

static int mq_deliver_receivers(botw_hle_os_message_queue* q) {
    int delivered = 0;
    while (q->count > 0 && q->receiver_count > 0) {
        botw_hle_os_mq_receiver receiver = q->receivers[0];
        uint64_t message;
        if (!thread_waiting_on(receiver.slot, BOTW_GUEST_WAIT_MESSAGE_QUEUE_RECEIVE,
                               q->index)) {
            mq_receiver_remove_at(q, 0);
            continue;
        }
        if (!mq_remove(q, &message)) break;
        mq_receiver_remove_at(q, 0);
        if (receiver.out) *(uint64_t*)(uintptr_t)receiver.out = message;
        (void)wake_sync_waiter(receiver.slot, 0, 0);
        delivered++;
    }
    return delivered;
}

/* Admit blocked senders into freed slots, hand queued messages to blocked
 * receivers, and repeat if one step makes the other possible. */
static void mq_service(botw_hle_os_message_queue* q) {
    int progress;
    do {
        progress = mq_fill_senders(q);
        progress += mq_deliver_receivers(q);
    } while (progress > 0);
}

static int mq_try_insert(botw_hle_os_message_queue* q, uint64_t message, int jam) {
    if (!mq_insert(q, message, jam)) return 0;
    mq_service(q);
    return 1;
}

static int mq_try_remove(botw_hle_os_message_queue* q, uint64_t* out) {
    if (!mq_remove(q, out)) return 0;
    mq_service(q);
    return 1;
}

static botw_hle_os_message_queue* mq_create(void* type, uint64_t* buffer,
                                           size_t capacity, int* out_index) {
    botw_hle_os_message_queue* q;
    int existing = -1;
    int i;
    if (!g_os) return NULL;
    if (!type) {
        (void)os_error(g_os, "InitializeMessageQueue: null MessageQueueType");
        return NULL;
    }
    if (!buffer) {
        (void)os_error(g_os, "InitializeMessageQueue: null message buffer");
        return NULL;
    }
    if (capacity == 0) {
        (void)os_error(g_os, "InitializeMessageQueue: capacity must be positive");
        return NULL;
    }
    if (capacity > (size_t)INT_MAX) {
        (void)os_error(g_os, "InitializeMessageQueue: capacity %zu is too large",
                       capacity);
        return NULL;
    }
    q = mq_find(type, &existing);
    if (q) {
        mq_compact_waiters(q);
        if (q->receiver_count != 0 || q->sender_count != 0) {
            (void)os_error(g_os, "InitializeMessageQueue: object still has waiters");
            return NULL;
        }
        memset(q, 0, sizeof *q);
        q->index = existing;
        if (out_index) *out_index = existing;
    } else {
        for (i = 0; i < (int)BOTW_HLE_OS_MAX_MESSAGE_QUEUE; i++) {
            if (!g_os->message_queues[i].used) break;
        }
        if (i == (int)BOTW_HLE_OS_MAX_MESSAGE_QUEUE) {
            (void)os_error(g_os, "InitializeMessageQueue: queue table is full");
            return NULL;
        }
        q = &g_os->message_queues[i];
        memset(q, 0, sizeof *q);
        q->index = i;
        if (out_index) *out_index = i;
    }
    q->used = 1;
    q->type = type;
    q->buffer = buffer;
    q->capacity = capacity;
    return q;
}

static botw_hle_os_message_queue* mq_begin(const char* op, const void* type,
                                           int* out_index) {
    botw_hle_os_message_queue* q;
    if (!g_os) return NULL;
    g_os->calls++;
    q = mq_find(type, out_index);
    if (!q) {
        (void)os_error(g_os, "%s: MessageQueueType was not initialized", op);
    }
    return q;
}

static void os_InitializeMessageQueue(void* type, size_t* buffer, size_t capacity) {
    if (!g_os) return;
    g_os->calls++;
    (void)mq_create(type, (uint64_t*)(uintptr_t)buffer, capacity, NULL);
}

static void os_FinalizeMessageQueue(void* type) {
    botw_hle_os_message_queue* q;
    if (!g_os) return;
    g_os->calls++;
    q = mq_find(type, NULL);
    if (!q) return;
    mq_compact_waiters(q);
    if (q->receiver_count != 0 || q->sender_count != 0) {
        (void)os_error(g_os, "FinalizeMessageQueue: object still has waiters");
        return;
    }
    memset(q, 0, sizeof *q);
}

static int os_TrySendMessageQueue(void* type, size_t message) {
    botw_hle_os_message_queue* q;
    if (!g_os) return 0;
    q = mq_begin("TrySendMessageQueue", type, NULL);
    if (!q) return 0;
    g_os->message_try_sends++;
    return mq_try_insert(q, (uint64_t)message, 0);
}

static int os_TryReceiveMessageQueue(size_t* out, void* type) {
    botw_hle_os_message_queue* q;
    if (!g_os) return 0;
    q = mq_begin("TryReceiveMessageQueue", type, NULL);
    if (!q) return 0;
    g_os->message_try_receives++;
    if (!out) {
        (void)os_error(g_os, "TryReceiveMessageQueue: null output pointer");
        return 0;
    }
    return mq_try_remove(q, (uint64_t*)(uintptr_t)out);
}

static int os_TryPeekMessageQueue(size_t* out, const void* type) {
    botw_hle_os_message_queue* q;
    if (!g_os) return 0;
    q = mq_begin("TryPeekMessageQueue", type, NULL);
    if (!q) return 0;
    g_os->message_peeks++;
    if (!out) {
        (void)os_error(g_os, "TryPeekMessageQueue: null output pointer");
        return 0;
    }
    if (q->count == 0) return 0;
    *(uint64_t*)(uintptr_t)out = q->buffer[q->offset];
    return 1;
}

/* Called from the scheduler's timed-wake path.  The thread is still BLOCKED,
 * so its saved frame is the one the import wrapper will restore. */
static void os_sync_timeout(void* user, int thread_slot, int wait_kind,
                            uint64_t wait_arg) {
    botw_hle_os_state* state = (botw_hle_os_state*)user;
    int index;
    botw_guest_thread* t;
    if (!state || !state->run) return;
    if (thread_slot < 0 || thread_slot >= (int)BOTW_GUEST_MAX_THREADS) return;
    t = &state->run->threads[thread_slot];
    if (wait_kind == BOTW_GUEST_WAIT_LIGHT_EVENT) {
        botw_hle_os_light_event* e;
        index = (int)wait_arg;
        if (index >= 0 && index < (int)BOTW_HLE_OS_MAX_LIGHT_EVENT) {
            e = &state->light_events[index];
            if (e->used) {
                int pos = waiter_index(e->waiters, e->waiter_count, thread_slot);
                waiter_remove_at(e->waiters, &e->waiter_count, pos);
            }
        }
        state->light_event_timeouts++;
        if (t->has_frame) t->frame.x[0] = 0;
    }
}

/* Portable entry points.  They cannot park a guest thread without the
 * AArch64 wrapper; a contended operation reports that instead of returning
 * success without mutual exclusion. */
#if !defined(__aarch64__)
static void os_LockMutex(void* type) {
    botw_hle_os_mutex* m;
    int index = -1;
    m = mutex_begin(type, &index);
    if (!m) return;
    if (!mutex_acquire_now(m, current_slot(g_os))) {
        (void)os_error(g_os, "LockMutex: would block on this host");
    }
}

static int os_WaitEvent_portable(void* type) {
    int got = event_get_result(type);
    if (!got) (void)os_error(g_os, "WaitEvent: would block on this host");
    return got;
}

static void os_WaitLightEvent_portable(void* type) {
    int got = light_event_get_result(type);
    if (!got) (void)os_error(g_os, "WaitLightEvent: would block on this host");
}

static int os_TimedWaitLightEvent_portable(void* type, int64_t ns) {
    int got = light_event_get_result(type);
    (void)ns;
    if (!got) (void)os_error(g_os, "TimedWaitLightEvent: would block on this host");
    return got;
}

static void os_AcquireSemaphore_portable(void* type) {
    botw_hle_os_semaphore* s;
    int index = -1;
    s = semaphore_begin(type, &index);
    if (!s) return;
    if (!semaphore_acquire_now(s)) {
        (void)os_error(g_os, "AcquireSemaphore: would block on this host");
    }
}

static void os_SendMessageQueue(void* type, size_t message) {
    botw_hle_os_message_queue* q = mq_begin("SendMessageQueue", type, NULL);
    if (!q) return;
    g_os->message_sends++;
    if (!mq_try_insert(q, (uint64_t)message, 0)) {
        (void)os_error(g_os, "SendMessageQueue: would block on this host");
    }
}

static void os_JamMessageQueue(void* type, size_t message) {
    botw_hle_os_message_queue* q = mq_begin("JamMessageQueue", type, NULL);
    if (!q) return;
    g_os->message_jams++;
    if (!mq_try_insert(q, (uint64_t)message, 1)) {
        (void)os_error(g_os, "JamMessageQueue: would block on this host");
    }
}

static void os_ReceiveMessageQueue(size_t* out, void* type) {
    botw_hle_os_message_queue* q = mq_begin("ReceiveMessageQueue", type, NULL);
    if (!q) return;
    g_os->message_receives++;
    if (!out) {
        (void)os_error(g_os, "ReceiveMessageQueue: null output pointer");
        return;
    }
    if (!mq_try_remove(q, (uint64_t*)(uintptr_t)out)) {
        (void)os_error(g_os, "ReceiveMessageQueue: would block on this host");
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Blocking imports                                                   */
/* ------------------------------------------------------------------ */

/* This is the C half of guest_hle_os_arm64.S.  The wrapper has already saved
 * every guest register and reports the return address as frame->pc.  If a
 * wait cannot block (no attached executing run, or the target is already
 * done), it returns to the caller exactly like the portable C entry point. */
int botw_hle_os_blocking_call(botw_guest_regs* frame, unsigned op,
                              const botw_guest_resume** next_resume) {
    botw_guest_run* run;
    const botw_guest_resume* next;
    uint64_t arg0;
    uint64_t arg1;
    int cur;

    if (!frame || !next_resume) return BOTW_HLE_OS_STOPPED;
    *next_resume = NULL;
    /* x0 is both the import argument and the return-value slot.  Read it
     * before replacing it, or SleepThread always sees zero nanoseconds and
     * WaitThread always sees a null ThreadType. */
    arg0 = frame->x[0];
    arg1 = frame->x[1];
    if (!g_os) {
        frame->x[0] = 0;
        return BOTW_HLE_OS_RETURN;
    }
    frame->x[0] = 0;
    run = g_os->run;

    switch (op) {
    case BOTW_HLE_OS_OP_SLEEP: {
        uint64_t deadline = sleep_prepare((int64_t)arg0);
        if (run && botw_guest_run_is_executing(run)) {
            next = botw_guest_run_block_and_switch(run, frame,
                                                   BOTW_GUEST_WAIT_SLEEP,
                                                   0, deadline);
            if (!next) return BOTW_HLE_OS_STOPPED;
            g_os->sleep_blocks++;
            sync_tick_from_run(g_os);
            *next_resume = next;
            return BOTW_HLE_OS_SWITCHED;
        }
        if (run) botw_guest_run_yield(run);
        return BOTW_HLE_OS_RETURN;
    }
    case BOTW_HLE_OS_OP_WAIT_THREAD: {
        int target = -1;
        if (!wait_prepare((void*)(uintptr_t)arg0, &target) || !run) {
            return BOTW_HLE_OS_RETURN;
        }
        if (botw_guest_run_is_executing(run)) {
            next = botw_guest_run_block_and_switch(run, frame,
                                                   BOTW_GUEST_WAIT_THREAD,
                                                   (uint64_t)(unsigned)target,
                                                   BOTW_GUEST_NO_DEADLINE);
            if (!next) return BOTW_HLE_OS_STOPPED;
            g_os->wait_blocks++;
            *next_resume = next;
            return BOTW_HLE_OS_SWITCHED;
        }
        botw_guest_run_yield(run);
        return BOTW_HLE_OS_RETURN;
    }
    case BOTW_HLE_OS_OP_LOCK_MUTEX: {
        botw_hle_os_mutex* m;
        int index = -1;
        m = mutex_begin((void*)(uintptr_t)arg0, &index);
        if (!m) return BOTW_HLE_OS_RETURN;
        cur = current_slot(g_os);
        if (mutex_acquire_now(m, cur)) return BOTW_HLE_OS_RETURN;
        if (!run) {
            (void)os_error(g_os, "LockMutex: no guest run is attached");
            return BOTW_HLE_OS_RETURN;
        }
        if (!botw_guest_run_is_executing(run)) {
            (void)os_error(g_os, "LockMutex: would block without an executing run");
            return BOTW_HLE_OS_RETURN;
        }
        if (!waiter_add(m->waiters, &m->waiter_count, cur)) {
            return BOTW_HLE_OS_RETURN;
        }
        next = botw_guest_run_block_and_switch(run, frame,
                                               BOTW_GUEST_WAIT_MUTEX,
                                               (uint64_t)(unsigned)index,
                                               BOTW_GUEST_NO_DEADLINE);
        if (!next) {
            int pos = waiter_index(m->waiters, m->waiter_count, cur);
            waiter_remove_at(m->waiters, &m->waiter_count, pos);
            return BOTW_HLE_OS_STOPPED;
        }
        g_os->mutex_blocks++;
        *next_resume = next;
        return BOTW_HLE_OS_SWITCHED;
    }
    case BOTW_HLE_OS_OP_WAIT_EVENT: {
        botw_hle_os_event* e;
        int index = -1;
        e = event_begin((void*)(uintptr_t)arg0, &index);
        if (!e) return BOTW_HLE_OS_RETURN;
        g_os->event_waits++;
        if (event_consume(e)) {
            frame->x[0] = 1;
            return BOTW_HLE_OS_RETURN;
        }
        if (!run) {
            (void)os_error(g_os, "WaitEvent: no guest run is attached");
            return BOTW_HLE_OS_RETURN;
        }
        if (!botw_guest_run_is_executing(run)) {
            (void)os_error(g_os, "WaitEvent: would block without an executing run");
            return BOTW_HLE_OS_RETURN;
        }
        cur = current_slot(g_os);
        if (!waiter_add(e->waiters, &e->waiter_count, cur)) {
            return BOTW_HLE_OS_RETURN;
        }
        next = botw_guest_run_block_and_switch(run, frame,
                                               BOTW_GUEST_WAIT_EVENT,
                                               (uint64_t)(unsigned)index,
                                               BOTW_GUEST_NO_DEADLINE);
        if (!next) {
            int pos = waiter_index(e->waiters, e->waiter_count, cur);
            waiter_remove_at(e->waiters, &e->waiter_count, pos);
            return BOTW_HLE_OS_STOPPED;
        }
        g_os->event_blocks++;
        *next_resume = next;
        return BOTW_HLE_OS_SWITCHED;
    }
    case BOTW_HLE_OS_OP_WAIT_LIGHT_EVENT: {
        botw_hle_os_light_event* e;
        int index = -1;
        e = light_event_find((void*)(uintptr_t)arg0, &index);
        g_os->calls++;
        if (!e) {
            (void)os_error(g_os, "WaitLightEvent: LightEventType was not initialized");
            return BOTW_HLE_OS_RETURN;
        }
        g_os->light_event_waits++;
        if (light_event_consume(e)) return BOTW_HLE_OS_RETURN;
        if (!run) {
            (void)os_error(g_os, "WaitLightEvent: no guest run is attached");
            return BOTW_HLE_OS_RETURN;
        }
        if (!botw_guest_run_is_executing(run)) {
            (void)os_error(g_os, "WaitLightEvent: would block without an executing run");
            return BOTW_HLE_OS_RETURN;
        }
        cur = current_slot(g_os);
        if (!waiter_add(e->waiters, &e->waiter_count, cur)) {
            return BOTW_HLE_OS_RETURN;
        }
        next = botw_guest_run_block_and_switch(run, frame,
                                               BOTW_GUEST_WAIT_LIGHT_EVENT,
                                               (uint64_t)(unsigned)index,
                                               BOTW_GUEST_NO_DEADLINE);
        if (!next) {
            int pos = waiter_index(e->waiters, e->waiter_count, cur);
            waiter_remove_at(e->waiters, &e->waiter_count, pos);
            return BOTW_HLE_OS_STOPPED;
        }
        g_os->light_event_blocks++;
        *next_resume = next;
        return BOTW_HLE_OS_SWITCHED;
    }
    case BOTW_HLE_OS_OP_TIMED_WAIT_LIGHT_EVENT: {
        botw_hle_os_light_event* e;
        uint64_t ticks;
        uint64_t deadline;
        int index = -1;
        e = light_event_find((void*)(uintptr_t)arg0, &index);
        g_os->calls++;
        if (!e) {
            (void)os_error(g_os, "TimedWaitLightEvent: LightEventType was not initialized");
            return BOTW_HLE_OS_RETURN;
        }
        g_os->light_event_waits++;
        if (light_event_consume(e)) {
            frame->x[0] = 1;
            return BOTW_HLE_OS_RETURN;
        }
        ticks = span_to_ticks((int64_t)arg1);
        if (ticks == 0) return BOTW_HLE_OS_RETURN;
        if (!run) {
            (void)os_error(g_os, "TimedWaitLightEvent: no guest run is attached");
            return BOTW_HLE_OS_RETURN;
        }
        if (!botw_guest_run_is_executing(run)) {
            (void)os_error(g_os, "TimedWaitLightEvent: would block without an executing run");
            return BOTW_HLE_OS_RETURN;
        }
        cur = current_slot(g_os);
        if (!waiter_add(e->waiters, &e->waiter_count, cur)) {
            return BOTW_HLE_OS_RETURN;
        }
        deadline = botw_guest_run_clock(run);
        if (UINT64_MAX - deadline < ticks) deadline = UINT64_MAX - 1u;
        else deadline += ticks;
        next = botw_guest_run_block_and_switch(run, frame,
                                               BOTW_GUEST_WAIT_LIGHT_EVENT,
                                               (uint64_t)(unsigned)index,
                                               deadline);
        if (!next) {
            int pos = waiter_index(e->waiters, e->waiter_count, cur);
            waiter_remove_at(e->waiters, &e->waiter_count, pos);
            return BOTW_HLE_OS_STOPPED;
        }
        g_os->light_event_blocks++;
        *next_resume = next;
        return BOTW_HLE_OS_SWITCHED;
    }
    case BOTW_HLE_OS_OP_SEND_MESSAGE_QUEUE:
    case BOTW_HLE_OS_OP_JAM_MESSAGE_QUEUE: {
        botw_hle_os_message_queue* q;
        int index = -1;
        int jam = op == BOTW_HLE_OS_OP_JAM_MESSAGE_QUEUE;
        q = mq_begin(jam ? "JamMessageQueue" : "SendMessageQueue",
                     (void*)(uintptr_t)arg0, &index);
        if (!q) return BOTW_HLE_OS_RETURN;
        if (jam) g_os->message_jams++;
        else g_os->message_sends++;
        if (mq_try_insert(q, arg1, jam)) return BOTW_HLE_OS_RETURN;
        if (!run) {
            (void)os_error(g_os, "%s: no guest run is attached",
                           jam ? "JamMessageQueue" : "SendMessageQueue");
            return BOTW_HLE_OS_RETURN;
        }
        if (!botw_guest_run_is_executing(run)) {
            (void)os_error(g_os, "%s: would block without an executing run",
                           jam ? "JamMessageQueue" : "SendMessageQueue");
            return BOTW_HLE_OS_RETURN;
        }
        cur = current_slot(g_os);
        if (!mq_sender_add(q, cur, arg1, jam)) return BOTW_HLE_OS_RETURN;
        next = botw_guest_run_block_and_switch(run, frame,
                                               BOTW_GUEST_WAIT_MESSAGE_QUEUE_SEND,
                                               (uint64_t)(unsigned)index,
                                               BOTW_GUEST_NO_DEADLINE);
        if (!next) {
            int pos = mq_sender_index(q, cur);
            mq_sender_remove_at(q, pos);
            return BOTW_HLE_OS_STOPPED;
        }
        if (jam) g_os->message_jam_blocks++;
        else g_os->message_send_blocks++;
        *next_resume = next;
        return BOTW_HLE_OS_SWITCHED;
    }
    case BOTW_HLE_OS_OP_RECEIVE_MESSAGE_QUEUE: {
        botw_hle_os_message_queue* q;
        int index = -1;
        q = mq_begin("ReceiveMessageQueue", (void*)(uintptr_t)arg1, &index);
        if (!q) return BOTW_HLE_OS_RETURN;
        g_os->message_receives++;
        if (!arg0) {
            (void)os_error(g_os, "ReceiveMessageQueue: null output pointer");
            return BOTW_HLE_OS_RETURN;
        }
        if (mq_try_remove(q, (uint64_t*)(uintptr_t)arg0)) {
            return BOTW_HLE_OS_RETURN;
        }
        if (!run) {
            (void)os_error(g_os, "ReceiveMessageQueue: no guest run is attached");
            return BOTW_HLE_OS_RETURN;
        }
        if (!botw_guest_run_is_executing(run)) {
            (void)os_error(g_os, "ReceiveMessageQueue: would block without an executing run");
            return BOTW_HLE_OS_RETURN;
        }
        cur = current_slot(g_os);
        if (!mq_receiver_add(q, cur, arg0)) return BOTW_HLE_OS_RETURN;
        next = botw_guest_run_block_and_switch(run, frame,
                                               BOTW_GUEST_WAIT_MESSAGE_QUEUE_RECEIVE,
                                               (uint64_t)(unsigned)index,
                                               BOTW_GUEST_NO_DEADLINE);
        if (!next) {
            int pos = mq_receiver_index(q, cur);
            mq_receiver_remove_at(q, pos);
            return BOTW_HLE_OS_STOPPED;
        }
        g_os->message_receive_blocks++;
        *next_resume = next;
        return BOTW_HLE_OS_SWITCHED;
    }
    case BOTW_HLE_OS_OP_ACQUIRE_SEMAPHORE: {
        botw_hle_os_semaphore* s;
        int index = -1;
        s = semaphore_begin((void*)(uintptr_t)arg0, &index);
        if (!s) return BOTW_HLE_OS_RETURN;
        if (semaphore_acquire_now(s)) return BOTW_HLE_OS_RETURN;
        if (!run) {
            (void)os_error(g_os, "AcquireSemaphore: no guest run is attached");
            return BOTW_HLE_OS_RETURN;
        }
        if (!botw_guest_run_is_executing(run)) {
            (void)os_error(g_os, "AcquireSemaphore: would block without an executing run");
            return BOTW_HLE_OS_RETURN;
        }
        cur = current_slot(g_os);
        if (!waiter_add(s->waiters, &s->waiter_count, cur)) {
            return BOTW_HLE_OS_RETURN;
        }
        next = botw_guest_run_block_and_switch(run, frame,
                                               BOTW_GUEST_WAIT_SEMAPHORE,
                                               (uint64_t)(unsigned)index,
                                               BOTW_GUEST_NO_DEADLINE);
        if (!next) {
            int pos = waiter_index(s->waiters, s->waiter_count, cur);
            waiter_remove_at(s->waiters, &s->waiter_count, pos);
            return BOTW_HLE_OS_STOPPED;
        }
        g_os->semaphore_blocks++;
        *next_resume = next;
        return BOTW_HLE_OS_SWITCHED;
    }
    default:
        (void)os_error(g_os, "blocking import %u is not implemented", op);
        return BOTW_HLE_OS_STOPPED;
    }
}

/* ------------------------------------------------------------------ */
/* Registry                                                           */
/* ------------------------------------------------------------------ */

#if defined(__aarch64__)
#  define OS_SLEEP_ENTRY ((void*)(uintptr_t)&botw_hle_os_guest_sleep)
#  define OS_WAIT_THREAD_ENTRY ((void*)(uintptr_t)&botw_hle_os_guest_wait_thread)
#  define OS_LOCK_MUTEX_ENTRY ((void*)(uintptr_t)&botw_hle_os_guest_lock_mutex)
#  define OS_WAIT_EVENT_ENTRY ((void*)(uintptr_t)&botw_hle_os_guest_wait_event)
#  define OS_WAIT_LIGHT_EVENT_ENTRY ((void*)(uintptr_t)&botw_hle_os_guest_wait_light_event)
#  define OS_TIMED_WAIT_LIGHT_EVENT_ENTRY ((void*)(uintptr_t)&botw_hle_os_guest_timed_wait_light_event)
#  define OS_ACQUIRE_SEMAPHORE_ENTRY ((void*)(uintptr_t)&botw_hle_os_guest_acquire_semaphore)
#  define OS_SEND_MESSAGE_QUEUE_ENTRY ((void*)(uintptr_t)&botw_hle_os_guest_send_message_queue)
#  define OS_JAM_MESSAGE_QUEUE_ENTRY ((void*)(uintptr_t)&botw_hle_os_guest_jam_message_queue)
#  define OS_RECEIVE_MESSAGE_QUEUE_ENTRY ((void*)(uintptr_t)&botw_hle_os_guest_receive_message_queue)
#else
#  define OS_SLEEP_ENTRY ((void*)(uintptr_t)&os_SleepThread)
#  define OS_WAIT_THREAD_ENTRY ((void*)(uintptr_t)&os_WaitThread)
#  define OS_LOCK_MUTEX_ENTRY ((void*)(uintptr_t)&os_LockMutex)
#  define OS_WAIT_EVENT_ENTRY ((void*)(uintptr_t)&os_WaitEvent_portable)
#  define OS_WAIT_LIGHT_EVENT_ENTRY ((void*)(uintptr_t)&os_WaitLightEvent_portable)
#  define OS_TIMED_WAIT_LIGHT_EVENT_ENTRY ((void*)(uintptr_t)&os_TimedWaitLightEvent_portable)
#  define OS_ACQUIRE_SEMAPHORE_ENTRY ((void*)(uintptr_t)&os_AcquireSemaphore_portable)
#  define OS_SEND_MESSAGE_QUEUE_ENTRY ((void*)(uintptr_t)&os_SendMessageQueue)
#  define OS_JAM_MESSAGE_QUEUE_ENTRY ((void*)(uintptr_t)&os_JamMessageQueue)
#  define OS_RECEIVE_MESSAGE_QUEUE_ENTRY ((void*)(uintptr_t)&os_ReceiveMessageQueue)
#endif

static const os_entry g_os_entries[BOTW_HLE_OS_COUNT] = {
    { "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE", OS_WAIT_THREAD_ENTRY },
    { "_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE", (void*)(uintptr_t)&os_FreeTlsSlot },
    { "_ZN2nn2os11GetHostArgcEv", (void*)(uintptr_t)&os_GetHostArgc },
    { "_ZN2nn2os11GetHostArgvEv", (void*)(uintptr_t)&os_GetHostArgv },
    { "_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE", (void*)(uintptr_t)&os_GetThreadId },
    { "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE", (void*)(uintptr_t)&os_GetTlsValue },
    { "_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm", (void*)(uintptr_t)&os_SetTlsValue },
    { "_ZN2nn2os11SleepThreadENS_8TimeSpanE", OS_SLEEP_ENTRY },
    { "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE", (void*)(uintptr_t)&os_StartThread },
    { "_ZN2nn2os11YieldThreadEv", (void*)(uintptr_t)&os_YieldThread },
    { "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi", (void*)(uintptr_t)&os_CreateThread6 },
    { "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii", (void*)(uintptr_t)&os_CreateThread7 },
    { "_ZN2nn2os13ConvertToTickENS_8TimeSpanE", (void*)(uintptr_t)&os_ConvertToTick },
    { "_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE", (void*)(uintptr_t)&os_DestroyThread },
    { "_ZN2nn2os13GetSystemTickEv", (void*)(uintptr_t)&os_GetSystemTick },
    { "_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc", (void*)(uintptr_t)&os_SetThreadName },
    { "_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE", (void*)(uintptr_t)&os_AllocateTlsSlot },
    { "_ZN2nn2os15FreeMemoryBlockEmm", (void*)(uintptr_t)&os_FreeMemoryBlock },
    { "_ZN2nn2os15GetCurrentFiberEv", (void*)(uintptr_t)&os_GetCurrentFiber },
    { "_ZN2nn2os16GetCurrentThreadEv", (void*)(uintptr_t)&os_GetCurrentThread },
    { "_ZN2nn2os17ConvertToTimeSpanENS0_4TickE", (void*)(uintptr_t)&os_ConvertToTimeSpan },
    { "_ZN2nn2os17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE", (void*)(uintptr_t)&os_GetThreadCoreMask },
    { "_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE", (void*)(uintptr_t)&os_GetThreadPriority },
    { "_ZN2nn2os17SetMemoryHeapSizeEm", (void*)(uintptr_t)&os_SetMemoryHeapSize },
    { "_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim", (void*)(uintptr_t)&os_SetThreadCoreMask },
    { "_ZN2nn2os19AllocateMemoryBlockEPmm", (void*)(uintptr_t)&os_AllocateMemoryBlock },
    { "_ZN2nn2os19GetCurrentStackInfoEPmS1_", (void*)(uintptr_t)&os_GetCurrentStackInfo },
    { "_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi", (void*)(uintptr_t)&os_ChangeThreadPriority },
    { "_ZN2nn2os20GetCurrentCoreNumberEv", (void*)(uintptr_t)&os_GetCurrentCoreNumber },
    { "_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE", (void*)(uintptr_t)&os_GetThreadNamePointer },
    { "_ZN2nn2os20SetThreadNamePointerEPNS0_10ThreadTypeEPKc", (void*)(uintptr_t)&os_SetThreadNamePointer },
    { "_ZN2nn2os22GetSystemTickFrequencyEv", (void*)(uintptr_t)&os_GetSystemTickFrequency },
    { "_ZN2nn2os26GetThreadAvailableCoreMaskEv", (void*)(uintptr_t)&os_GetThreadAvailableCoreMask },
    { "_ZN2nn2os10ClearEventEPNS0_9EventTypeE", (void*)(uintptr_t)&os_ClearEvent },
    { "_ZN2nn2os11SignalEventEPNS0_9EventTypeE", (void*)(uintptr_t)&os_SignalEvent },
    { "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", (void*)(uintptr_t)&os_UnlockMutex },
    { "_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE", (void*)(uintptr_t)&os_TryLockMutex },
    { "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE", (void*)(uintptr_t)&os_TryWaitEvent },
    { "_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE", (void*)(uintptr_t)&os_FinalizeEvent },
    { "_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE", (void*)(uintptr_t)&os_FinalizeMutex },
    { "_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE", OS_WAIT_LIGHT_EVENT_ENTRY },
    { "_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE", (void*)(uintptr_t)&os_ClearLightEvent },
    { "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE", (void*)(uintptr_t)&os_InitializeEvent },
    { "_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi", (void*)(uintptr_t)&os_InitializeMutex },
    { "_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE", OS_ACQUIRE_SEMAPHORE_ENTRY },
    { "_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE", (void*)(uintptr_t)&os_ReleaseSemaphore },
    { "_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeEi", (void*)(uintptr_t)&os_ReleaseSemaphore_n },
    { "_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE", (void*)(uintptr_t)&os_SignalLightEvent },
    { "_ZN2nn2os17FinalizeSemaphoreEPNS0_13SemaphoreTypeE", (void*)(uintptr_t)&os_FinalizeSemaphore },
    { "_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE", (void*)(uintptr_t)&os_FinalizeLightEvent },
    { "_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii", (void*)(uintptr_t)&os_InitializeSemaphore },
    { "_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE", OS_TIMED_WAIT_LIGHT_EVENT_ENTRY },
    { "_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE", (void*)(uintptr_t)&os_InitializeLightEvent },
    { "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", OS_LOCK_MUTEX_ENTRY },
    { "_ZN2nn2os9WaitEventEPNS0_9EventTypeE", OS_WAIT_EVENT_ENTRY },
    { "_ZN2nn2os20FinalizeMessageQueueEPNS0_16MessageQueueTypeE", (void*)(uintptr_t)&os_FinalizeMessageQueue },
    { "_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm", OS_SEND_MESSAGE_QUEUE_ENTRY },
    { "_ZN2nn2os19TrySendMessageQueueEPNS0_16MessageQueueTypeEm", (void*)(uintptr_t)&os_TrySendMessageQueue },
    { "_ZN2nn2os15JamMessageQueueEPNS0_16MessageQueueTypeEm", OS_JAM_MESSAGE_QUEUE_ENTRY },
    { "_ZN2nn2os19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE", OS_RECEIVE_MESSAGE_QUEUE_ENTRY },
    { "_ZN2nn2os22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE", (void*)(uintptr_t)&os_TryReceiveMessageQueue },
    { "_ZN2nn2os19TryPeekMessageQueueEPmPKNS0_16MessageQueueTypeE", (void*)(uintptr_t)&os_TryPeekMessageQueue },
    { "_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm", (void*)(uintptr_t)&os_InitializeMessageQueue }
};

int botw_hle_os_register(botw_hle_table* hle, botw_hle_os_state* state) {
    size_t i;
    if (!hle) return -1;
    if (!state) {
        botw_hle_os_reset(&g_os_default);
        g_os_default_ready = 1;
        state = &g_os_default;
    }
    g_os = state;
    for (i = 0; i < BOTW_HLE_OS_COUNT; i++) {
        if (botw_hle_register(hle, g_os_entries[i].name, g_os_entries[i].fn) != 0) return -1;
    }
    return 0;
}

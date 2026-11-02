#include "guest_hle_os.h"

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
    state->run = run;
    state->attached = run != NULL;
    for (i = 1; i < (int)BOTW_GUEST_MAX_THREADS; i++) {
        memset(&state->threads[i], 0, sizeof state->threads[i]);
    }
    memset(state->tls, 0, sizeof state->tls);
    memset(state->tls_used, 0, sizeof state->tls_used);
    memset(state->tls_dtor, 0, sizeof state->tls_dtor);
    state->tick = 0;
    if (run) run->clock = 0;
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
    if (!t || !g_os->run) return 0;
    cur = current_slot(g_os);
    if (t->slot == cur) return 0;
    if (t->slot >= 0 && t->slot < (int)BOTW_GUEST_MAX_THREADS &&
        g_os->run->threads[t->slot].state == BOTW_GUEST_THREAD_DONE) {
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

    if (!frame || !next_resume) return BOTW_HLE_OS_STOPPED;
    *next_resume = NULL;
    frame->x[0] = 0;
    if (!g_os) return BOTW_HLE_OS_RETURN;
    run = g_os->run;

    switch (op) {
    case BOTW_HLE_OS_OP_SLEEP: {
        uint64_t deadline = sleep_prepare((int64_t)frame->x[0]);
        if (run && botw_guest_run_is_executing(run)) {
            next = botw_guest_run_block_and_switch(run, frame,
                                                   BOTW_GUEST_WAIT_SLEEP,
                                                   deadline);
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
        if (!wait_prepare((void*)(uintptr_t)frame->x[0], &target) || !run) {
            return BOTW_HLE_OS_RETURN;
        }
        if (botw_guest_run_is_executing(run)) {
            next = botw_guest_run_block_and_switch(run, frame,
                                                   BOTW_GUEST_WAIT_THREAD,
                                                   (uint64_t)(unsigned)target);
            if (!next) return BOTW_HLE_OS_STOPPED;
            g_os->wait_blocks++;
            *next_resume = next;
            return BOTW_HLE_OS_SWITCHED;
        }
        botw_guest_run_yield(run);
        return BOTW_HLE_OS_RETURN;
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
#else
#  define OS_SLEEP_ENTRY ((void*)(uintptr_t)&os_SleepThread)
#  define OS_WAIT_THREAD_ENTRY ((void*)(uintptr_t)&os_WaitThread)
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
    { "_ZN2nn2os26GetThreadAvailableCoreMaskEv", (void*)(uintptr_t)&os_GetThreadAvailableCoreMask }
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

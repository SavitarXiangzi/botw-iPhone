/* Cooperative guest threads: two extra threads, three stacks, one scheduler.
 *
 * The image and gate block are synthetic, but the code that runs is real
 * AArch64.  Each thread calls the nn::os::YieldThread HLE import, then the
 * gate block; the yield request is honoured at the gate and the trampoline
 * resumes a different thread's saved frame on its own stack.  The thread
 * bodies are the same assembly probe called three times.
 *
 * On x86 the whole test skips: the entry and switch sequences are AArch64.
 */
#include "guest_gate.h"
#include "guest_map.h"
#include "guest_run.h"
#include "guest_slot.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32) || !defined(__aarch64__)

int main() {
    std::printf("guest_thread_test: skipped (needs AArch64 + mmap)\n");
    return 0;
}

#else

#include "guest_hle.h"
#include "guest_hle_os.h"

#include <unistd.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_thread_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

static const size_t kKeyOff = 0x8000;
static const size_t kPatchOff = 0x10000;
static const size_t kBlockLen = 32;
static const size_t kFileLen = kPatchOff + kBlockLen;

static const char* kHeader =
    "# PATCH_OFF\t0x10000\n"
    "# KEY_PAGE_OFF\t0x8000\n"
    "# KEY_PAGE_SIZE\t0x4000\n"
    "# KEY_K18_IDX\t0\n"
    "# KEY_KTLS_IDX\t1\n"
    "# KEY_KTLR_IDX\t2\n"
    "# KEY_KGATE_IDX\t3\n"
    "# TEXT_END\t0x400\n"
    "# RO_OFF\t0x400\n"
    "# DATA_OFF\t0x4000\n"
    "# BUILD_ID\t8e9978d50bdd20b4c8395a106c27ffde\n"
    "# SLOT_BASE\tabs\n";

/* The gate block reads slot[KGATE] through the mapped key page.  The final
 * `ret` stands in for the rewriter's `b <site+4>`: this probe only needs the
 * call to come back. */
static uint32_t adrp_word(unsigned rd, size_t pc, size_t target) {
    int64_t imm = ((int64_t)(target & ~(uint64_t)0xFFFu) -
                   (int64_t)(pc & ~(uint64_t)0xFFFu)) >> 12;
    uint32_t u = (uint32_t)imm;
    return 0x90000000u | ((u & 3u) << 29) | (((u >> 2) & 0x7FFFFu) << 5) | rd;
}

static void emit_gate_block(unsigned char* at) {
    uint32_t w[8];
    w[0] = 0xA9800000u | ((0x7Eu) << 15) | (30u << 10) | (31u << 5) | 29u;
    w[1] = adrp_word(30, kPatchOff + 4, kKeyOff);
    w[2] = 0xF9400000u | (3u << 10) | (30u << 5) | 30u;
    w[3] = 0xF9400000u | (30u << 5) | 30u;
    w[4] = 0xF9400000u | (31u << 5) | 29u;
    w[5] = 0xD63F0000u | (30u << 5);
    w[6] = 0xA8C00000u | (2u << 15) | (30u << 10) | (31u << 5) | 29u;
    w[7] = 0xD65F03C0u;
    std::memcpy(at, w, sizeof w);
}

static std::vector<unsigned char> make_image() {
    std::vector<unsigned char> v(kFileLen, 0);
    auto put32 = [&](size_t at, uint32_t x) { std::memcpy(&v[at], &x, 4); };
    put32(4, 8);
    std::memcpy(&v[8], "MOD0", 4);
    put32(0x0c, 0x100 - 8);
    emit_gate_block(&v[kPatchOff]);
    return v;
}

extern "C" void botw_guest_thread_probe_entry(void* args);

struct probe_args {
    uint64_t* counter;      /* [0] iterations, [1] id, [2] sp */
    uint64_t  id;
    uint64_t  iterations;
    void*     block;
    void*     yield_fn;     /* nn::os::YieldThread import */
};

struct gate_ctx {
    std::vector<uint64_t>* ids;
    unsigned long* calls;
};

typedef uint32_t (*os_create7_fn)(void*, void (*)(void*), void*, void*, size_t, int, int);
typedef void (*os_start_fn)(void*);

static void gate_cb(void* user, botw_guest_regs* regs) {
    gate_ctx* c = (gate_ctx*)user;
    c->ids->push_back(regs->x[0]);
    (*c->calls)++;
}

static bool inside(const botw_guest_thread& t, uint64_t sp) {
    uintptr_t lo = (uintptr_t)t.stack;
    uintptr_t hi = lo + t.stack_bytes;
    return sp >= lo && sp < hi;
}

int main() {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    std::vector<unsigned char> file = make_image();
    botw_guest_mapping map;
    botw_guest_image img;
    alignas(64) unsigned char slot_mem[BOTW_GUEST_SLOT_MIN * 2];
    botw_guest_slot slot;
    botw_guest_run run;
    uint64_t main_counter[3] = {0, 0, 0};
    uint64_t a_counter[3] = {0, 0, 0};
    uint64_t b_counter[3] = {0, 0, 0};
    uint64_t type_a[64] = {0};
    uint64_t type_b[64] = {0};
    botw_hle_table hle;
    botw_hle_os_state os;
    std::vector<uint64_t> ids;
    unsigned long calls = 0;
    int a, b, i;

    CHECK(botw_guest_map_open(&map, &img, file.data(), file.size(), kHeader,
                              nullptr, nullptr, page) == 0);
    if (map.base == nullptr) {
        std::printf("guest_thread_test: %s\n", map.error);
        return 1;
    }
    CHECK(botw_guest_slot_init(&slot, slot_mem, sizeof slot_mem,
                               (void*)(uintptr_t)&botw_guest_gate_entry) == 0);
    CHECK(botw_guest_map_publish_slot(&map, &img, &slot) == 0);

    void* block = (unsigned char*)map.base + kPatchOff;
    __builtin___clear_cache((char*)block, (char*)block + kBlockLen);

    CHECK(botw_guest_run_init(&run, map.base, img.size, 0x8000) == 0);
    CHECK(run.thread_count == 1);
    CHECK(run.threads[0].state == BOTW_GUEST_THREAD_CREATED);
    CHECK(run.threads[0].stack == run.stack);
    CHECK(run.current == -1);

    /* A created thread must be started explicitly; the main slot cannot be
     * started through the worker API. */
    CHECK(botw_guest_run_thread_start(&run, 1) == -1);
    CHECK(botw_guest_run_thread_start(&run, 0) == -1);
    CHECK(botw_guest_run_thread_create(&run, nullptr, 0, 0) == -1);

    botw_hle_init(&hle);
    CHECK(hle.error[0] == '\0');
    botw_hle_os_reset(&os);
    CHECK(botw_hle_os_register(&hle, &os) == 0);
    botw_hle_os_attach_run(&os, &run);
    void* yield_fn = botw_hle_lookup(&hle, "_ZN2nn2os11YieldThreadEv");
    CHECK(yield_fn != nullptr);

    probe_args a_args = { a_counter, 1, 2, block, yield_fn };
    probe_args b_args = { b_counter, 2, 2, block, yield_fn };
    os_create7_fn create = (os_create7_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii");
    os_start_fn start = (os_start_fn)botw_hle_lookup(
        &hle, "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    CHECK(create != nullptr);
    CHECK(start != nullptr);
    CHECK(create(type_a, &botw_guest_thread_probe_entry, &a_args, nullptr, 0x8000, 0x20, 0) == 0);
    CHECK(create(type_b, &botw_guest_thread_probe_entry, &b_args, nullptr, 0x8000, 0x20, 0) == 0);
    CHECK(os.thread_creates == 2);
    CHECK(os.threads[1].used);
    CHECK(os.threads[2].used);
    a = 1;
    b = 2;
    CHECK(run.thread_count == 3);
    CHECK(run.threads[a].state == BOTW_GUEST_THREAD_CREATED);
    CHECK(run.threads[b].state == BOTW_GUEST_THREAD_CREATED);
    CHECK(run.threads[a].stack != run.threads[b].stack);
    CHECK(run.threads[a].stack != run.stack);
    CHECK(run.threads[a].guard_bytes == page);
    start(type_a);
    start(type_b);
    CHECK(os.thread_starts == 2);
    CHECK(run.threads[a].state == BOTW_GUEST_THREAD_READY);
    CHECK(run.threads[b].state == BOTW_GUEST_THREAD_READY);

    gate_ctx gc = { &ids, &calls };
    botw_guest_run_set_gate(&run, gate_cb, &gc);

    probe_args main_args = { main_counter, 0, 4, block, yield_fn };
    run.args[0] = (uint64_t)(uintptr_t)&main_args;
    CHECK(botw_guest_run_enter_at(&run, (const void*)&botw_guest_thread_probe_entry) == 0);

    CHECK(run.reason == BOTW_GUEST_EXIT_RETURNED);
    CHECK(run.gates == 8);
    CHECK(calls == 8);
    CHECK(run.switches == 9);
    CHECK(run.thread_exits == 2);
    CHECK(main_counter[0] == 4);
    CHECK(a_counter[0] == 2);
    CHECK(b_counter[0] == 2);
    CHECK(main_counter[1] == 0);
    CHECK(a_counter[1] == 1);
    CHECK(b_counter[1] == 2);
    CHECK(run.threads[a].state == BOTW_GUEST_THREAD_DONE);
    CHECK(run.threads[b].state == BOTW_GUEST_THREAD_DONE);
    CHECK(run.threads[0].state == BOTW_GUEST_THREAD_DONE);
    CHECK(run.threads[0].yields == 4);
    CHECK(run.threads[a].yields == 2);
    CHECK(run.threads[b].yields == 2);
    CHECK(os.yields == 8);
    CHECK(run.current == -1);

    const uint64_t expected[8] = {0, 1, 2, 0, 1, 2, 0, 0};
    CHECK(ids.size() == 8);
    for (i = 0; i < 8 && i < (int)ids.size(); i++) CHECK(ids[(size_t)i] == expected[i]);

    CHECK(inside(run.threads[0], main_counter[2]));
    CHECK(inside(run.threads[a], a_counter[2]));
    CHECK(inside(run.threads[b], b_counter[2]));
    CHECK(main_counter[2] != a_counter[2]);
    CHECK(a_counter[2] != b_counter[2]);
    CHECK(main_counter[2] % 16 == 0);
    CHECK(a_counter[2] % 16 == 0);
    CHECK(b_counter[2] % 16 == 0);

    CHECK(botw_guest_gate_get_handler() == nullptr);
    botw_hle_os_attach_run(&os, nullptr);

    botw_guest_run_close(&run);
    CHECK(run.stack == nullptr);
    CHECK(run.threads[a].stack == nullptr);
    CHECK(run.threads[b].stack == nullptr);

    botw_guest_map_close(&map, &img);

    if (g_fail) {
        std::printf("guest_thread_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_thread_test: ok (3 threads, %zu gates, 9 switches)\n", ids.size());
    return 0;
}

#endif

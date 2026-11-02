/* Entering a guest: stack, guard page, gates, and the exit sentinel.
 *
 * The image is synthetic (the same split as guest_map_test), but the guest
 * program is a real AArch64 function that runs on the stack this module maps,
 * crosses a real gate block, and returns.  On x86 the whole test skips: the
 * entry sequence is AArch64 assembly.
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
    std::printf("guest_run_test: skipped (needs AArch64 + mmap)\n");
    return 0;
}

#else

#include <unistd.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_run_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

static const size_t kTextEnd = 0x400;
static const size_t kRoOff = 0x400;
static const size_t kDataOff = 0x4000;
static const size_t kKeyOff = 0x8000;
static const size_t kKeySize = 0x4000;
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

extern "C" void botw_guest_run_probe_entry(uint64_t* counter, void* block, uint64_t value);

static uint32_t adrp_word(unsigned rd, size_t pc, size_t target) {
    int64_t imm = ((int64_t)(target & ~(uint64_t)0xFFFu) -
                   (int64_t)(pc & ~(uint64_t)0xFFFu)) >> 12;
    uint32_t u = (uint32_t)imm;
    return 0x90000000u | ((u & 3u) << 29) | (((u >> 2) & 0x7FFFFu) << 5) | rd;
}

/* The rewriter's gate block, with `ret` where the site branch would go. */
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

static int g_calls;
static uint64_t g_seen_arg;

extern "C" void run_gate_cb(void* user, botw_guest_regs* regs) {
    (void)user;
    g_calls++;
    g_seen_arg = regs->x[0];
    regs->x[0] = 0xdead0001u;
}

int main() {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    std::vector<unsigned char> file = make_image();
    botw_guest_mapping map;
    botw_guest_image img;
    alignas(64) unsigned char slot_mem[BOTW_GUEST_SLOT_MIN * 2];
    botw_guest_slot slot;
    botw_guest_run run;
    uint64_t counter[2] = {0, 0};

    CHECK(botw_guest_map_open(&map, &img, file.data(), file.size(), kHeader,
                              nullptr, nullptr, page) == 0);
    if (map.base == nullptr) {
        std::printf("guest_run_test: %s\n", map.error);
        return 1;
    }
    CHECK(botw_guest_slot_init(&slot, slot_mem, sizeof slot_mem,
                               (void*)(uintptr_t)&botw_guest_gate_entry) == 0);
    CHECK(botw_guest_map_publish_slot(&map, &img, &slot) == 0);

    void* block = (unsigned char*)map.base + kPatchOff;
    __builtin___clear_cache((char*)block, (char*)block + kBlockLen);

    CHECK(botw_guest_run_init(&run, map.base, img.size, 0x8000) == 0);
    CHECK(run.stack != nullptr);
    CHECK(run.guard_bytes == page);
    CHECK(run.stack_bytes == page + 0x8000);
    CHECK(run.stack_top % 16 == 0);
    CHECK(run.reason == BOTW_GUEST_EXIT_NONE);

    /* Refusals happen before a single instruction runs. */
    CHECK(botw_guest_run_enter(&run, kFileLen) == -1);
    CHECK(run.reason == BOTW_GUEST_EXIT_REFUSED);
    CHECK(run.error[0] != '\0');

    g_calls = 0;
    g_seen_arg = 0;
    botw_guest_run_set_gate(&run, run_gate_cb, nullptr);
    run.args[0] = (uint64_t)(uintptr_t)counter;
    run.args[1] = (uint64_t)(uintptr_t)block;
    run.args[2] = 0x1234;

    CHECK(botw_guest_run_enter_at(&run, (const void*)&botw_guest_run_probe_entry) == 0);
    CHECK(run.reason == BOTW_GUEST_EXIT_RETURNED);
    CHECK(counter[0] == 3);
    CHECK(counter[1] == 0xdead0001u);
    CHECK(g_calls == 1);
    CHECK(run.gates == 1);
    CHECK(g_seen_arg == 0x1234);
    /* The probe balanced its own frame, so the exit saw the stack top. */
    CHECK(run.exit_sp == run.stack_top);
    CHECK(run.exit_x0 == 0xdead0001u);

    /* A second entry on the same run starts from a clean register file and
     * re-installs the handler; nothing from the first run leaks. */
    run.args[2] = 0x5678;
    CHECK(botw_guest_run_enter_at(&run, (const void*)&botw_guest_run_probe_entry) == 0);
    CHECK(counter[0] == 6);
    CHECK(counter[1] == 0xdead0001u);
    CHECK(g_calls == 2);
    CHECK(run.gates == 1);
    CHECK(g_seen_arg == 0x5678);

    /* The handler is put back when the run finishes. */
    CHECK(botw_guest_gate_get_handler() == nullptr);

    botw_guest_run_close(&run);
    CHECK(run.stack == nullptr);

    botw_guest_map_close(&map, &img);

    if (g_fail) {
        std::printf("guest_run_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_run_test: ok (page %zu, stack+guard, entry, %d gates, "
                "exit sentinel, re-entry)\n", page, g_calls);
    return 0;
}

#endif

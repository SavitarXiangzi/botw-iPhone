#include "botw_core.h"

#include "guest_gate.h"
#include "guest_map.h"
#include "guest_run.h"
#include "guest_slot.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__aarch64__)
#include <unistd.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif
extern "C" void botw_app_guest_probe(void*);
extern "C" void botw_app_guest_map_probe(uint64_t* out, void* block, uint64_t value);
#endif

/* The selftest report is exposed separately so the Swift shell can put the
 * reason in selftest.txt even when the process otherwise exits cleanly. */
static char s_image_report[256] = "not run";

static void set_image_report(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_image_report, sizeof s_image_report, fmt, ap);
    va_end(ap);
}

/* This is deliberately an app-facing debug hook, not part of the game path.
 * It proves that the iOS binary can enter native AArch64 guest code through
 * botw_guest_run, use the mapped guest stack, and return through the sentinel.
 * The actual title image and HLE bootstrap are separate milestones. */
extern "C" int botw_core_guest_selftest(void) {
#if defined(__aarch64__)
    botw_guest_run run{};
    uint64_t words[4] = {};
    const uint64_t magic = 0x5457424Full; /* "BOTW" little-endian */

    if (botw_guest_run_init(&run, nullptr, 0, 0x8000) != 0) {
        std::printf("botw_guest_selftest: init failed: %s\n", run.error);
        return 2;
    }

    run.args[0] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(words));
    const int entered = botw_guest_run_enter_at(
        &run, reinterpret_cast<const void*>(&botw_app_guest_probe));

    const bool ok = entered == 0 &&
                    run.reason == BOTW_GUEST_EXIT_RETURNED &&
                    words[0] == magic &&
                    words[1] == magic + 0x101u &&
                    words[2] == 0x11Fu &&
                    words[3] == run.stack_top - 16u &&
                    run.exit_x0 == magic;

    std::printf(
        "botw_guest_selftest: %s (reason=%d words=%llx/%llx/%llx sp=%llx exit_x0=%llx)\n",
        ok ? "ok" : "FAIL", run.reason,
        static_cast<unsigned long long>(words[0]),
        static_cast<unsigned long long>(words[1]),
        static_cast<unsigned long long>(words[2]),
        static_cast<unsigned long long>(words[3]),
        static_cast<unsigned long long>(run.exit_x0));

    botw_guest_run_close(&run);
    return ok ? 0 : 1;
#else
    std::printf("botw_guest_selftest: skipped (needs AArch64)\n");
    return -1;
#endif
}

#if defined(__aarch64__)
namespace {

/* A synthetic patched.bin with the same file layout the 1.6.0 rewriter emits
 * for a module.  The split at 0x4000 is intentional: the first 16 KiB page is
 * RX text plus R rodata, the data page is RW.  That keeps the plan valid on
 * both the 4 KiB x86 host and Apple's 16 KiB pages. */
constexpr size_t kTextEnd = 0x400;
constexpr size_t kRoOff = 0x400;
constexpr size_t kDataOff = 0x4000;
constexpr size_t kKeyOff = 0x8000;
constexpr size_t kKeySize = 0x4000;
constexpr size_t kPatchOff = 0x10000;
constexpr size_t kBlockLen = 32;
constexpr size_t kFileLen = kPatchOff + kBlockLen;

constexpr const char* kHeader =
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

int g_map_calls;
botw_guest_regs g_map_seen;
uint64_t g_map_return;

extern "C" void image_gate(void*, botw_guest_regs* regs) {
    g_map_calls++;
    g_map_seen = *regs;
    regs->x[0] = g_map_return;
}

void put32(unsigned char* at, size_t off, uint32_t value) {
    std::memcpy(at + off, &value, sizeof value);
}

void put64(unsigned char* at, size_t off, uint64_t value) {
    std::memcpy(at + off, &value, sizeof value);
}

/* ADRP encoding, matching the rewrite the gate block expects. */
uint32_t adrp_word(unsigned rd, size_t pc, size_t target) {
    const int64_t imm = ((static_cast<int64_t>(target & ~static_cast<size_t>(0xFFFu)) -
                          static_cast<int64_t>(pc & ~static_cast<size_t>(0xFFFu))) >> 12);
    const uint32_t u = static_cast<uint32_t>(imm);
    return 0x90000000u | ((u & 3u) << 29) | (((u >> 2) & 0x7FFFFu) << 5) | rd;
}

void make_image(unsigned char* image) {
    uint32_t w[8];

    std::memset(image, 0, kFileLen);
    put32(image, 4, 8);                    /* MOD0 header at image + 8 */
    std::memcpy(image + 8, "MOD0", 4);
    put32(image, 0x0c, 0x100 - 8);         /* dynamic table offset */

    /* DT_INIT at 0x30: this is code that really lives in the mapped RX text
     * page.  It writes a marker to the RW data page, branches into the patch
     * area, stores the gate's answer, and returns through the exit sentinel. */
    put32(image, 0x30, 0xA9800000u | (0x7Cu << 15) | (30u << 10) | (31u << 5) | 29u);
    put32(image, 0x34, 0x910003FDu);                       /* mov x29, sp */
    put32(image, 0x38, adrp_word(9, 0x38, kDataOff));      /* adrp x9, data */
    put32(image, 0x3c, 0xD2800000u | (0x1234u << 5) | 10u);/* mov x10, #0x1234 */
    put32(image, 0x40, 0xF9000000u | ((0x100u / 8u) << 10) | (9u << 5) | 10u);
    put32(image, 0x44, 0xD2800000u | (0x55u << 5));        /* mov x0, #0x55 */
    put32(image, 0x48, 0x94000000u |
          (static_cast<uint32_t>((kPatchOff - 0x48) / 4) & 0x03FFFFFFu));
    put32(image, 0x4c, adrp_word(9, 0x4c, kDataOff));      /* adrp x9, data */
    put32(image, 0x50, 0xF9000000u | ((0x108u / 8u) << 10) | (9u << 5) | 0u);
    put32(image, 0x54, 0xA8C00000u | (4u << 15) | (30u << 10) | (31u << 5) | 29u);
    put32(image, 0x58, 0xD65F03C0u);                       /* ret */
    put64(image, 0x100, 12);                               /* DT_INIT */
    put64(image, 0x108, 0x30);
    put64(image, 0x110, 0);                                /* DT_NULL */

    w[0] = 0xA9800000u | (0x7Eu << 15) | (30u << 10) | (31u << 5) | 29u; /* stp x29,x30,[sp,#-16]! */
    w[1] = adrp_word(30, kPatchOff + 4, kKeyOff);
    w[2] = 0xF9400000u | (3u << 10) | (30u << 5) | 30u; /* ldr x30,[x30,#24] */
    w[3] = 0xF9400000u | (30u << 5) | 30u;              /* ldr x30,[x30] */
    w[4] = 0xF9400000u | (31u << 5) | 29u;              /* ldr x29,[sp] */
    w[5] = 0xD63F0000u | (30u << 5);                    /* blr x30 */
    w[6] = 0xA8C00000u | (2u << 15) | (30u << 10) | (31u << 5) | 29u; /* ldp x29,x30,[sp],#16 */
    w[7] = 0xD65F03C0u;                                 /* ret */
    std::memcpy(image + kPatchOff, w, sizeof w);
}

#define IMAGE_CHECK(cond, why) do { \
    if (!(cond)) { \
        set_image_report("FAIL %s: %s", why, #cond); \
        goto done; \
    } \
} while (0)

} /* namespace */
#endif /* __aarch64__ */

/* Debug hook: maps a synthetic patched.bin into executable memory, publishes
 * a thread slot, enters a gate block on a real guest stack, and verifies the
 * dispatcher and exit path.  This is the app-side proof of the loader ->
 * mapping -> gate chain; the retail title image is still a separate step.
 *
 * Returns 0 on success, 1 when a load/mapping/gate check fails, 2 when the
 * guest run cannot be set up, or -1 on non-AArch64 hosts. */
extern "C" int botw_core_guest_image_selftest(void) {
#if defined(__aarch64__)
    botw_guest_mapping map{};
    botw_guest_image img{};
    botw_guest_run run{};
    alignas(64) unsigned char slot_mem[BOTW_GUEST_SLOT_MIN * 2] = {};
    botw_guest_slot slot{};
    unsigned char file[kFileLen] = {};
    uint64_t out[4] = {};
    const uint64_t value = 0x1234;
    size_t page;
    size_t entry = 0;
    void* block = nullptr;
    int rc = 1;

    g_map_calls = 0;
    std::memset(&g_map_seen, 0, sizeof g_map_seen);
    g_map_return = 0x47415445u; /* "GATE" */

    page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    if (page == 0) {
        set_image_report("FAIL: sysconf(_SC_PAGESIZE) returned 0");
        goto done;
    }

    make_image(file);
    if (botw_guest_map_open(&map, &img, file, sizeof file, kHeader,
                            nullptr, nullptr, page) != 0) {
        set_image_report("FAIL map: %s", map.error);
        goto done;
    }

    IMAGE_CHECK(map.base != nullptr, "mapping");
    IMAGE_CHECK(map.page == page, "page size");
    IMAGE_CHECK(map.runs >= 1 && map.runs <= BOTW_GUEST_MAP_MAX_SEGMENTS, "run count");
    IMAGE_CHECK(img.base == static_cast<uint8_t*>(map.base), "image base");
    IMAGE_CHECK(img.owns_base == 0, "image ownership");
    IMAGE_CHECK(img.text_end == kTextEnd && img.ro_off == kRoOff &&
                img.data_off == kDataOff, "image layout");
    IMAGE_CHECK(img.key_page_off == kKeyOff && img.key_page_size == kKeySize,
                "key page");
    IMAGE_CHECK(img.patch_off == kPatchOff && img.patch_len == kBlockLen,
                "patch range");
    IMAGE_CHECK(img.slot_abs == 1, "absolute slot mode");
    IMAGE_CHECK(img.kgate_idx == 3, "KGATE index");
    {
        /* A BL encodes its displacement from the instruction's own address,
         * not from the next instruction.  An off-by-four here jumps into the
         * read-only padding before the patch area and faults before the gate
         * ever runs; decode the word back instead of trusting the encoder. */
        uint32_t bl = 0;
        std::memcpy(&bl, static_cast<const unsigned char*>(map.base) + 0x48, sizeof bl);
        const int64_t disp = static_cast<int64_t>(static_cast<int32_t>(bl << 6) >> 6);
        IMAGE_CHECK(0x48 + disp * 4 == static_cast<int64_t>(kPatchOff),
                    "DT_INIT branch target");
    }

    IMAGE_CHECK(botw_guest_slot_init(&slot, slot_mem, sizeof slot_mem,
                                     reinterpret_cast<void*>(&botw_guest_gate_entry)) == 0,
                "slot init");
    IMAGE_CHECK(botw_guest_map_publish_slot(&map, &img, &slot) == 0,
                "slot publish");

    {
        const uint64_t* keys = reinterpret_cast<const uint64_t*>(
            static_cast<const unsigned char*>(map.base) + kKeyOff);
        uint64_t handler = 0;
        std::memcpy(&handler, slot_mem + slot.gate_off, sizeof handler);
        IMAGE_CHECK(keys[img.kgate_idx] ==
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(slot_mem + slot.gate_off)),
                    "KGATE key");
        IMAGE_CHECK(handler == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&botw_guest_gate_entry)),
                    "gate handler");
    }

    block = static_cast<unsigned char*>(map.base) + kPatchOff;
#if defined(__APPLE__)
    /* __builtin___clear_cache emits a call to ___clear_cache, which the iOS
     * device libSystem does not export.  This is the supported spelling. */
    sys_icache_invalidate(block, kBlockLen);
#else
    __builtin___clear_cache(static_cast<char*>(block),
                            static_cast<char*>(block) + kBlockLen);
#endif

    if (botw_guest_run_init(&run, map.base, img.size, 0x8000) != 0) {
        set_image_report("FAIL run init: %s", run.error);
        rc = 2;
        goto done;
    }
    IMAGE_CHECK(run.stack != nullptr && run.stack_bytes == page + 0x8000,
                "guest stack");
    IMAGE_CHECK(run.stack_top % 16 == 0, "stack alignment");

    botw_guest_run_set_gate(&run, image_gate, nullptr);
    run.args[0] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(out));
    run.args[1] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(block));
    run.args[2] = value;

    if (botw_guest_run_enter_at(&run,
                                reinterpret_cast<const void*>(&botw_app_guest_map_probe)) != 0) {
        set_image_report("FAIL enter: %s", run.error);
        rc = 2;
        goto done;
    }

    IMAGE_CHECK(run.reason == BOTW_GUEST_EXIT_RETURNED, "exit reason");
    IMAGE_CHECK(run.gates == 1, "gate count");
    IMAGE_CHECK(g_map_calls == 1, "dispatcher calls");
    IMAGE_CHECK(out[0] == value + 1u && out[3] == value, "probe setup");
    IMAGE_CHECK(out[1] == g_map_return, "dispatcher return");
    IMAGE_CHECK(out[2] == run.stack_top - 64u, "probe stack");
    IMAGE_CHECK(g_map_seen.x[0] == value, "guest x0 at gate");
    IMAGE_CHECK(g_map_seen.pc == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(block)) + 24u,
                "gate pc");
    IMAGE_CHECK(g_map_seen.sp == run.stack_top - 64u, "gate sp");
    IMAGE_CHECK(g_map_seen.x[30] != g_map_seen.pc, "guest x30");
    IMAGE_CHECK(run.exit_x0 == g_map_return, "exit x0");
    IMAGE_CHECK(run.exit_sp == run.stack_top, "exit sp");

    /* Now enter the image itself through its DT_INIT, not a host probe.  This
     * proves the loader's entry offset and that text executes from the RX page
     * while the data page stays writable. */
    entry = botw_guest_image_entry(&img);
    IMAGE_CHECK(entry == 0x30, "DT_INIT entry");
    g_map_calls = 0;
    std::memset(&g_map_seen, 0, sizeof g_map_seen);
    run.args[0] = 0;
    run.args[1] = 0;
    run.args[2] = 0;
    if (botw_guest_run_enter(&run, entry) != 0) {
        set_image_report("FAIL image entry: %s", run.error);
        rc = 2;
        goto done;
    }
    IMAGE_CHECK(run.reason == BOTW_GUEST_EXIT_RETURNED, "image exit reason");
    IMAGE_CHECK(run.gates == 1, "image gate count");
    IMAGE_CHECK(g_map_calls == 1, "image dispatcher calls");
    IMAGE_CHECK(g_map_seen.x[0] == 0x55u, "image gate x0");
    IMAGE_CHECK(g_map_seen.pc == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(block)) + 24u,
                "image gate pc");
    IMAGE_CHECK(g_map_seen.sp == run.stack_top - 32u, "image gate sp");
    IMAGE_CHECK(*reinterpret_cast<const uint64_t*>(
                    static_cast<const unsigned char*>(map.base) + kDataOff + 0x100) == 0x1234u,
                "image data marker");
    IMAGE_CHECK(*reinterpret_cast<const uint64_t*>(
                    static_cast<const unsigned char*>(map.base) + kDataOff + 0x108) == g_map_return,
                "image data result");
    IMAGE_CHECK(run.exit_x0 == g_map_return, "image exit x0");
    IMAGE_CHECK(run.exit_sp == run.stack_top, "image exit sp");

    set_image_report("ok (page=%zu span=%#zx runs=%zu gates=%lu entry=%#zx exit_x0=%#llx)",
                     page, map.span, map.runs, run.gates, entry,
                     static_cast<unsigned long long>(run.exit_x0));
    rc = 0;

done:
    botw_guest_run_close(&run);
    botw_guest_map_close(&map, &img);
    std::printf("botw_guest_image_selftest: %s\n", s_image_report);
    return rc;
#else
    set_image_report("skipped (needs AArch64 + mmap)");
    std::printf("botw_guest_image_selftest: %s\n", s_image_report);
    return -1;
#endif
}

extern "C" const char* botw_core_guest_image_selftest_report(void) {
    return s_image_report;
}

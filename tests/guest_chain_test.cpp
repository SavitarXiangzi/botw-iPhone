/* Full loader chain on a real mapping: map -> slot publish -> KGATE ->
 * trampoline -> dispatcher -> block ldp -> back to the caller.
 *
 * This is the first test that joins every piece built so far:
 *
 *   - guest_map.c maps a synthetic patched.bin with real page protections,
 *     applies its (empty) relocation table and mprotects text/data/key/patch;
 *   - guest_slot.c lays out a thread slot and guest_map_publish_slot() stores
 *     its absolute field addresses in the writable key page;
 *   - the patch area holds the exact instruction sequence the rewriter emits
 *     around an svc, including the adrp/ldr indirection through KGATE;
 *   - guest_gate_arm64.S spills the register frame, calls the dispatcher,
 *     restores it and returns to the block's ldp;
 *   - the probe checks that the dispatcher saw the guest registers and that
 *     the caller got back the values the dispatcher wrote.
 *
 * It is still synthetic: the block is hand-encoded here, not a gateway into
 * the retail main module, and no HLE service is involved.
 */
#include "guest_gate.h"
#include "guest_map.h"
#include "guest_slot.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32) || !defined(__aarch64__)

int main() {
    std::printf("guest_chain_test: skipped (needs AArch64 + mmap)\n");
    return 0;
}

#else

#include <unistd.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_chain_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

/* Same layout as guest_map_test: cleanly split at 4 KiB and 16 KiB. */
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

/* tests/guest_chain_probe_arm64.S */
extern "C" void botw_guest_chain_probe(void* block, uint64_t* out);

/* ADRP, as scripts/guest/third_party/nxnative/nso_asm.py computes it.  The
 * mapping base is page-aligned, so the delta between two file offsets is the
 * same delta between their runtime addresses. */
static uint32_t adrp_word(unsigned rd, size_t pc, size_t target) {
    int64_t imm = ((int64_t)(target & ~(uint64_t)0xFFFu) -
                   (int64_t)(pc & ~(uint64_t)0xFFFu)) >> 12;
    uint32_t u;
    if (imm < -(1 << 20) || imm >= (1 << 20)) {
        std::printf("guest_chain_test: adrp target out of range\n");
        return 0;
    }
    u = (uint32_t)imm;
    return 0x90000000u | ((u & 3u) << 29) | (((u >> 2) & 0x7FFFFu) << 5) | rd;
}

/* The gate block the rewriter emits for an svc, with `ret` in place of the
 * final branch to the site (the probe called us with blr, so the block's ldp
 * restores the probe's link register). */
static void emit_gate_block(unsigned char* at) {
    uint32_t w[8];
    w[0] = 0xA9800000u | ((0x7Eu) << 15) | (30u << 10) | (31u << 5) | 29u; /* stp x29,x30,[sp,#-16]! */
    w[1] = adrp_word(30, kPatchOff + 4, kKeyOff);
    w[2] = 0xF9400000u | (3u << 10) | (30u << 5) | 30u;  /* ldr x30,[x30,#24] */
    w[3] = 0xF9400000u | (30u << 5) | 30u;               /* ldr x30,[x30] */
    w[4] = 0xF9400000u | (31u << 5) | 29u;               /* ldr x29,[sp] */
    w[5] = 0xD63F0000u | (30u << 5);                     /* blr x30 */
    w[6] = 0xA8C00000u | ((2u) << 15) | (30u << 10) | (31u << 5) | 29u; /* ldp x29,x30,[sp],#16 */
    w[7] = 0xD65F03C0u;                                  /* ret */
    std::memcpy(at, w, sizeof w);
}

static std::vector<unsigned char> make_image() {
    std::vector<unsigned char> v(kFileLen, 0);
    auto put32 = [&](size_t at, uint32_t x) { std::memcpy(&v[at], &x, 4); };

    put32(4, 8);                       /* MOD0 at image+8 */
    std::memcpy(&v[8], "MOD0", 4);
    put32(0x0c, 0x100 - 8);            /* dynamic table offset */
    /* The dynamic table at 0x100 needs only DT_NULL: no relocations. */
    emit_gate_block(&v[kPatchOff]);
    return v;
}

static botw_guest_regs g_seen;
static int g_calls;

extern "C" void chain_dispatch(botw_guest_regs* regs) {
    g_seen = *regs;
    g_calls++;
    regs->x[0] = 0xdead0001u;
    regs->x[2] = 0x2222u;
    regs->x[19] = 0x3333u;
}

int main() {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    std::vector<unsigned char> file = make_image();
    botw_guest_mapping map;
    botw_guest_image img;
    alignas(64) unsigned char slot_mem[BOTW_GUEST_SLOT_MIN * 2];
    botw_guest_slot slot;
    uint64_t out[16];

    CHECK(botw_guest_map_open(&map, &img, file.data(), file.size(), kHeader,
                              nullptr, nullptr, page) == 0);
    if (map.base == nullptr) {
        std::printf("guest_chain_test: %s\n", map.error);
        return 1;
    }
    CHECK(img.base == (uint8_t*)map.base);
    CHECK(img.owns_base == 0);
    CHECK(img.text_end == kTextEnd && img.ro_off == kRoOff && img.data_off == kDataOff);
    CHECK(img.key_page_off == kKeyOff && img.key_page_size == kKeySize);
    CHECK(img.slot_abs == 1);
    CHECK(img.kgate_idx == 3);
    CHECK(img.patch_off == kPatchOff && img.patch_len == kBlockLen);

    CHECK(botw_guest_slot_init(&slot, slot_mem, sizeof slot_mem,
                               (void*)(uintptr_t)&botw_guest_gate_entry) == 0);
    CHECK(botw_guest_map_publish_slot(&map, &img, &slot) == 0);

    /* The key page now points at this thread's slot fields, absolutely. */
    {
        const uint64_t* keys = (const uint64_t*)((const unsigned char*)map.base + kKeyOff);
        uint64_t handler = 0;
        CHECK(keys[img.k18_idx] == (uint64_t)(uintptr_t)(slot_mem + slot.x18_off));
        CHECK(keys[img.ktls_idx] == (uint64_t)(uintptr_t)(slot_mem + slot.tls_off));
        CHECK(keys[img.ktlr_idx] == (uint64_t)(uintptr_t)(slot_mem + slot.tlr_off));
        CHECK(keys[img.kgate_idx] == (uint64_t)(uintptr_t)(slot_mem + slot.gate_off));
        std::memcpy(&handler, slot_mem + slot.gate_off, sizeof handler);
        CHECK(handler == (uint64_t)(uintptr_t)&botw_guest_gate_entry);
    }

    void* block = (unsigned char*)map.base + kPatchOff;
    __builtin___clear_cache((char*)block, (char*)block + kBlockLen);

    g_calls = 0;
    std::memset(&g_seen, 0, sizeof g_seen);
    std::memset(out, 0, sizeof out);
    botw_guest_gate_set_handler(chain_dispatch);
    botw_guest_chain_probe(block, out);
    botw_guest_gate_set_handler(nullptr);

    CHECK(g_calls == 1);
    for (int i = 0; i <= 8; i++) CHECK(g_seen.x[i] == (uint64_t)(0x100 + i));
    CHECK(g_seen.x[19] == 0x113);
    CHECK(g_seen.x[20] == 0x114);
    CHECK(g_seen.x[29] == 0x11d);
    CHECK(g_seen.sp == out[14]);
    CHECK(g_seen.pc == (uint64_t)(uintptr_t)block + 24); /* the block's ldp */
    CHECK(g_seen.x[30] != g_seen.pc);                    /* guest LR, not pc */
    CHECK(g_seen.nzcv == 0xa0000000u);

    CHECK(out[0] == 0xdead0001u);    /* the dispatcher's x0 */
    CHECK(out[1] == 0x101);          /* every other register restored */
    CHECK(out[2] == 0x2222u);
    CHECK(out[9] == 0x3333u);
    CHECK(out[10] == 0x114);
    CHECK(out[11] == 0x11d);         /* the block's ldp took guest x29 */
    CHECK(out[12] == g_seen.x[30]);  /* ... and guest x30 */
    CHECK(out[13] == 0xa0000000u);   /* NZCV round-tripped */
    CHECK(out[14] == g_seen.sp);     /* sp returned above the block's pair */

    botw_guest_map_close(&map, &img);
    CHECK(map.base == nullptr && img.base == nullptr);

    if (g_fail) {
        std::printf("guest_chain_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_chain_test: ok (page %zu, map -> slot -> KGATE -> "
                "trampoline -> block, registers round-trip)\n", page);
    return 0;
}

#endif

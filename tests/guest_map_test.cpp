/* Maps a synthetic image with real page protections and checks the result.
 *
 * POSIX only: it needs mmap/mprotect, and a child process to prove that a
 * write to an executable page really dies.  On Windows it prints "skipped" -
 * the malloc-backed loader is covered by guest_image_test there.
 *
 * The layout is chosen so it splits cleanly at both 4 KiB and 16 KiB pages,
 * and the test asks the host for its real page size.  That matters: mprotect
 * only accepts host-page-aligned ranges, and macOS would reject 4 KiB runs.
 *
 * What it proves, in order:
 *   - the plan produced by guest_segments.c is accepted by mprotect;
 *   - relocations landed while the mapping was writable;
 *   - text and the patch area are readable and executing the patch area runs
 *     (a two-instruction stub that returns 0x1234);
 *   - writing to text or to the patch area kills the process;
 *   - writing to data and to the key page does not;
 *   - a layout that would need one page writable and executable is refused;
 *   - after mprotect, the loader can still publish a thread slot into the key
 *     page, and the page holds absolute slot field addresses.
 */
#include "guest_map.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)

int main() {
    std::printf("guest_map_test: skipped (needs mmap/mprotect)\n");
    return 0;
}

#else

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_map_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

/* Layout: text+rodata in the first page group, data at 0x4000, one 16 KiB key
 * page at 0x8000, padding, then the patch area at 0x10000. */
static const size_t kTextEnd = 0x400;
static const size_t kRoOff = 0x400;
static const size_t kDataOff = 0x4000;
static const size_t kKeyOff = 0x8000;
static const size_t kKeySize = 0x4000;
static const size_t kPatchOff = 0x10000;
static const size_t kFileLen = 0x10100;

static const size_t kSymtab = 0x420;
static const size_t kStrtab = 0x4c0;
static const size_t kRela = 0x500;
static const size_t kJmpRel = 0x530;
static const size_t kSlotRelative = 0x4000;
static const size_t kSlotImport = 0x4008;

static void* g_import_value = (void*)(uintptr_t)0xdead0000u;

static void* resolver(void*, const char* name) {
    return std::strcmp(name, "fake_import") == 0 ? g_import_value : nullptr;
}

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

static std::vector<unsigned char> make_image() {
    std::vector<unsigned char> v(kFileLen, 0);
    auto put32 = [&](size_t at, uint32_t x) { std::memcpy(&v[at], &x, 4); };
    auto put64 = [&](size_t at, uint64_t x) { std::memcpy(&v[at], &x, 8); };

    put32(4, 8);                       /* MOD0 at image+8 */
    std::memcpy(&v[8], "MOD0", 4);
    put32(0x0c, 0x100 - 8);            /* dynamic table offset */
    size_t d = 0x100;
    auto dt = [&](uint64_t tag, uint64_t val) { put64(d, tag); put64(d + 8, val); d += 16; };
    dt(7, kRela);      /* DT_RELA */
    dt(8, 24);         /* DT_RELASZ: one entry */
    dt(9, 24);         /* DT_RELAENT */
    dt(5, kStrtab);    /* DT_STRTAB */
    dt(6, kSymtab);    /* DT_SYMTAB */
    dt(10, 32);        /* DT_STRSZ */
    dt(11, 24);        /* DT_SYMENT */
    dt(23, kJmpRel);   /* DT_JMPREL */
    dt(2, 24);         /* DT_PLTRELSZ: one entry */
    dt(20, 7);         /* DT_PLTREL = RELA */
    dt(12, 0x30);      /* DT_INIT */
    dt(0, 0);          /* DT_NULL */

    std::memcpy(&v[kStrtab], "\0fake_import\0", 14);
    /* symbol 1: "fake_import", undefined */
    put32(kSymtab + 24, 1);
    v[kSymtab + 24 + 4] = 0x12;
    v[kSymtab + 24 + 5] = 0;
    std::memcpy(&v[kSymtab + 24 + 6], "\0\0", 2);
    put64(kSymtab + 24 + 8, 0);

    /* R_RELATIVE at kSlotRelative */
    put64(kRela, kSlotRelative);
    put64(kRela + 8, 1027);
    put64(kRela + 16, 0x1234);
    /* R_JUMP_SLOT at kSlotImport */
    put64(kJmpRel, kSlotImport);
    put64(kJmpRel + 8, (1ull << 32) | 1026);
    put64(kJmpRel + 16, 0);

    /* Patch stub: return 0x1234. */
#if defined(__aarch64__)
    static const unsigned char kStub[] = {0x80, 0x46, 0x82, 0x52, 0xc0, 0x03, 0x5f, 0xd6};
#elif defined(__x86_64__) || defined(__i386__)
    static const unsigned char kStub[] = {0xb8, 0x34, 0x12, 0x00, 0x00, 0xc3};
#else
    static const unsigned char kStub[] = {0};
#endif
    std::memcpy(&v[kPatchOff], kStub, sizeof kStub);
    return v;
}

/* The child writes one byte and must die from the protection fault; if it
 * survives, the page was not actually read-only. */
static bool write_dies(volatile unsigned char* at, unsigned char value) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        *at = value;
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return false;
    if (!WIFSIGNALED(status)) return false;
    int sig = WTERMSIG(status);
    return sig == SIGSEGV || sig == SIGBUS;
}

int main() {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    std::vector<unsigned char> file = make_image();
    botw_guest_mapping map;
    botw_guest_image img;
    size_t span = (kFileLen + page - 1) / page * page;

    CHECK(botw_guest_map_open(&map, &img, file.data(), file.size(), kHeader,
                              resolver, nullptr, page) == 0);
    if (map.base == nullptr) {
        std::printf("guest_map_test: %s\n", map.error);
        return 1;
    }
    CHECK(map.span == span);
    CHECK(map.page == page);
    CHECK(img.base == (uint8_t*)map.base);
    CHECK(img.owns_base == 0);
    CHECK(img.patch_off == kPatchOff && img.patch_len == kFileLen - kPatchOff);
    CHECK(img.text_end == kTextEnd && img.ro_off == kRoOff && img.data_off == kDataOff);
    CHECK(img.key_page_off == kKeyOff && img.key_page_size == kKeySize);

    /* Relocations were applied while the mapping was writable. */
    uint64_t slot_rel = 0, slot_imp = 0;
    std::memcpy(&slot_rel, (unsigned char*)map.base + kSlotRelative, 8);
    std::memcpy(&slot_imp, (unsigned char*)map.base + kSlotImport, 8);
    CHECK(slot_rel == (uint64_t)(uintptr_t)map.base + 0x1234u);
    CHECK(slot_imp == 0xdead0000u);
    CHECK(img.relocs == 1);
    CHECK(img.hle == 1);

    /* Read-only is readable: MOD0 sits at image+8. */
    CHECK(((unsigned char*)map.base)[8] == 'M');
    CHECK(((unsigned char*)map.base)[9] == 'O');

    /* The patch area is executable: the stub returns 0x1234. */
    typedef int (*stub_fn)(void);
    void* stub_addr = (unsigned char*)map.base + kPatchOff;
    stub_fn stub = (stub_fn)stub_addr;
    CHECK(stub() == 0x1234);

    /* Data and the key page stay writable. */
    volatile uint64_t* slot = (volatile uint64_t*)((unsigned char*)map.base + kSlotRelative);
    *slot = 0xaaaa;
    CHECK(*slot == 0xaaaa);
    volatile unsigned char* key = (volatile unsigned char*)((unsigned char*)map.base + kKeyOff);
    *key = 0x5a;
    CHECK(*key == 0x5a);

    /* The loader step between mapping and the first gate: lay out the thread
     * slot and publish its absolute field addresses into the key page. */
    {
        alignas(64) unsigned char slot_mem[BOTW_GUEST_SLOT_MIN * 2];
        botw_guest_slot slot;
        uint64_t handler = 0;
        uint64_t* keys = (uint64_t*)(void*)((unsigned char*)map.base + kKeyOff);
        CHECK(botw_guest_slot_init(&slot, slot_mem, sizeof slot_mem,
                                   (void*)(uintptr_t)0x12345678u) == 0);
        CHECK(botw_guest_map_publish_slot(&map, &img, &slot) == 0);
        CHECK(keys[0] == (uint64_t)(uintptr_t)(slot_mem + slot.x18_off));
        CHECK(keys[1] == (uint64_t)(uintptr_t)(slot_mem + slot.tls_off));
        CHECK(keys[2] == (uint64_t)(uintptr_t)(slot_mem + slot.tlr_off));
        CHECK(keys[3] == (uint64_t)(uintptr_t)(slot_mem + slot.gate_off));
        std::memcpy(&handler, slot_mem + slot.gate_off, sizeof handler);
        CHECK(handler == 0x12345678u);
        /* mprotect left the key page writable, not just writable before it. */
        keys[4] = 0xfeedfaceu;
        CHECK(keys[4] == 0xfeedfaceu);

        /* A key page the plan did not make writable is refused before any
         * write: the padding page at 0xc000 is read-only. */
        {
            botw_guest_image saved = img;
            img.key_page_off = 0xc000;
            img.key_page_size = 0x1000;
            CHECK(botw_guest_map_publish_slot(&map, &img, &slot) != 0);
            CHECK(std::strstr(map.error, "not writable") != nullptr);
            img = saved;
        }
    }

    /* Text and the patch area are not. */
    CHECK(write_dies((volatile unsigned char*)map.base + 0x30, 0));
    CHECK(write_dies((volatile unsigned char*)map.base + kPatchOff, 0));
    /* The data page is, so the same check must not fire there. */
    CHECK(!write_dies((volatile unsigned char*)map.base + kSlotRelative, 0));

    botw_guest_map_close(&map, &img);
    CHECK(map.base == nullptr && map.span == 0);
    CHECK(img.base == nullptr);

    /* A layout that cannot be split: text, rodata and data in one page. */
    {
        std::vector<unsigned char> tight(kFileLen, 0);
        const char* bad =
            "# PATCH_OFF\t0x8000\n"
            "# KEY_PAGE_OFF\t0x4000\n"
            "# KEY_PAGE_SIZE\t0x4000\n"
            "# TEXT_END\t0x100\n"
            "# RO_OFF\t0x100\n"
            "# DATA_OFF\t0x200\n"
            "# BUILD_ID\t8e9978d50bdd20b4c8395a106c27ffde\n";
        botw_guest_mapping m2;
        botw_guest_image i2;
        CHECK(botw_guest_map_open(&m2, &i2, tight.data(), tight.size(), bad,
                                  resolver, nullptr, page) != 0);
        CHECK(std::strstr(m2.error, "writable and executable") != nullptr);
        CHECK(m2.base == nullptr);
    }

    if (g_fail) {
        std::printf("guest_map_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_map_test: ok (page %zu, %zu KiB mapped, RX executes, RW writes, "
                "text write dies, conflict refused)\n",
                page, span / 1024);
    return 0;
}

#endif /* _WIN32 */

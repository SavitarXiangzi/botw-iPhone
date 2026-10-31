/* Synthetic-image tests for core/guest/guest_image.c.
 *
 * No title data: the image is assembled in this file.  It covers every
 * relocation the 1.6.0 modules actually contain:
 *
 *   R_RELATIVE                  base + addend
 *   R_ABS64 with symbol 0       base + addend
 *   R_ABS64 with a module-local base + st_value + addend  (sdk's 13,453)
 *   R_ABS64 with an undefined   HLE address + addend      (main's 959)
 *   R_JUMP_SLOT / R_GLOB_DAT    resolved import address
 *
 * plus the refusal paths: unresolved imports, symbol-less PLT slots, symbols
 * pointing past the image, a relocation outside the image, and a bad fixups
 * header.
 */
#include "guest_image.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("guest_image_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void put16(std::vector<uint8_t>& v, size_t off, uint16_t x) {
    std::memcpy(&v[off], &x, 2);
}
static void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    std::memcpy(&v[off], &x, 4);
}
static void put64(std::vector<uint8_t>& v, size_t off, uint64_t x) {
    std::memcpy(&v[off], &x, 8);
}
static uint64_t get64(const std::vector<uint8_t>& v, size_t off) {
    uint64_t x;
    std::memcpy(&x, &v[off], 8);
    return x;
}

static const char* kHeader =
    "# PATCH_OFF\t0x5000\n"
    "# KEY_PAGE_OFF\t0x1000\n"
    "# KEY_PAGE_SIZE\t0x4000\n"
    "# TEXT_END\t0x800\n"
    "# RO_OFF\t0x800\n"
    "# DATA_OFF\t0x800\n"
    "# BUILD_ID\t8e9978d50bdd20b4c8395a106c27ffde\n";

static const size_t kSymtab = 0x300;
static const size_t kStrtab = 0x400;
static const size_t kRela = 0x200;
static const size_t kJmpRel = 0x500;

static const size_t kSlotRelative = 0x600;
static const size_t kSlotJump = 0x608;
static const size_t kSlotAbsBase = 0x610;
static const size_t kSlotAbsLocal = 0x618;
static const size_t kSlotAbsImport = 0x620;
static const size_t kSlotGlobDat = 0x628;

static void put_sym(std::vector<uint8_t>& v, unsigned index, uint32_t name_off,
                    uint8_t info, uint16_t shndx, uint64_t value) {
    size_t at = kSymtab + index * 24u;
    put32(v, at, name_off);
    v[at + 4] = info;
    v[at + 5] = 0;
    put16(v, at + 6, shndx);
    put64(v, at + 8, value);
    put64(v, at + 16, 0);
}

static void put_rela(std::vector<uint8_t>& v, size_t at, size_t target,
                     uint64_t type, uint64_t sym, int64_t addend) {
    put64(v, at, target);
    put64(v, at + 8, (sym << 32) | type);
    put64(v, at + 16, (uint64_t)addend);
}

static std::vector<uint8_t> make_image() {
    std::vector<uint8_t> v(0x5800, 0);
    put32(v, 4, 8);                       // MOD0 at image+8
    std::memcpy(&v[8], "MOD0", 4);
    put32(v, 0x0c, 0x100 - 8);            // dynamic table offset
    size_t d = 0x100;
    auto dt = [&](uint64_t tag, uint64_t val) {
        put64(v, d, tag);
        put64(v, d + 8, val);
        d += 16;
    };
    dt(7, kRela);    // DT_RELA
    dt(8, 24 * 4);   // DT_RELASZ: four entries
    dt(9, 24);       // DT_RELAENT
    dt(5, kStrtab);  // DT_STRTAB
    dt(6, kSymtab);  // DT_SYMTAB
    dt(10, 48);      // DT_STRSZ
    dt(11, 24);      // DT_SYMENT
    dt(23, kJmpRel); // DT_JMPREL
    dt(2, 24 * 2);   // DT_PLTRELSZ: two entries
    dt(20, 7);       // DT_PLTREL = RELA
    dt(12, 0x30);    // DT_INIT
    dt(0, 0);        // DT_NULL

    // String table: "\0fake_import\0local_sym\0import_abs\0"
    std::memcpy(&v[kStrtab], "\0fake_import\0local_sym\0import_abs\0", 34);
    put_sym(v, 1, 1, 0x12, 0, 0);            // undefined import
    put_sym(v, 2, 13, 0x12, 1, 0x700);       // defined in this module
    put_sym(v, 3, 23, 0x12, 0, 0);           // undefined, reached via ABS64

    put_rela(v, kRela + 0, kSlotRelative, 1027, 0, 0x1234);
    put_rela(v, kRela + 24, kSlotAbsBase, 257, 0, 0x40);
    put_rela(v, kRela + 48, kSlotAbsLocal, 257, 2, 0x8);
    put_rela(v, kRela + 72, kSlotAbsImport, 257, 3, 0x4);

    put_rela(v, kJmpRel + 0, kSlotJump, 1026, 1, 0);
    put_rela(v, kJmpRel + 24, kSlotGlobDat, 1025, 2, 0);  // module-local symbol
    return v;
}

static void* resolver(void*, const char* name) {
    if (std::strcmp(name, "fake_import") == 0) return (void*)(uintptr_t)0x12345678u;
    if (std::strcmp(name, "import_abs") == 0) return (void*)(uintptr_t)0xcafebabeu;
    return nullptr;
}
static void* null_resolver(void*, const char*) { return nullptr; }

int main() {
    std::vector<uint8_t> file = make_image();
    botw_guest_image img;

    CHECK(botw_guest_image_open(&img, file.data(), file.size(), kHeader, resolver, nullptr) == 0);
    CHECK(img.error[0] == '\0');
    CHECK(img.relocs == 4);          // one R_RELATIVE + three R_ABS64
    CHECK(img.symbols == 2);         // the two R_ABS64 entries with a symbol
    CHECK(img.hle == 2);             // undefined R_ABS64 + undefined JUMP_SLOT
    CHECK(img.imports == 2);         // JUMP_SLOT (import) + GLOB_DAT (local)
    CHECK(img.size == 0x1000);
    CHECK(img.text_end == 0x800 && img.ro_off == 0x800 && img.data_off == 0x800);
    CHECK(img.key_page_off == 0x1000 && img.key_page_size == 0x4000);
    CHECK(img.patch_off == 0x5000 && img.patch_len == 0x800);
    CHECK(img.build_id[0] == 0x8e && img.build_id[15] == 0xde);
    CHECK(botw_guest_image_entry(&img) == 0x30);

    // R_RELATIVE and R_ABS64/0 are both base-relative.
    CHECK(get64(file, kSlotRelative) == 0);  // the source image is not modified in place
    CHECK(*(const uint64_t*)(img.base + kSlotRelative) == (uint64_t)(uintptr_t)img.base + 0x1234u);
    CHECK(*(const uint64_t*)(img.base + kSlotAbsBase) == (uint64_t)(uintptr_t)img.base + 0x40u);
    // Module-local symbol: base + st_value + addend, no HLE call.
    CHECK(*(const uint64_t*)(img.base + kSlotAbsLocal) == (uint64_t)(uintptr_t)img.base + 0x700u + 8u);
    // Undefined symbol: HLE address + addend.
    CHECK(*(const uint64_t*)(img.base + kSlotAbsImport) == 0xcafebabeu + 4u);
    CHECK(*(const uint64_t*)(img.base + kSlotJump) == 0x12345678u);
    // A GOT slot may name a symbol this module defines; it must not go to HLE.
    CHECK(*(const uint64_t*)(img.base + kSlotGlobDat) == (uint64_t)(uintptr_t)img.base + 0x700u);
    botw_guest_image_close(&img);
    CHECK(img.base == nullptr);

    // An unknown import must fail, not write NULL.
    CHECK(botw_guest_image_open(&img, file.data(), file.size(), kHeader, null_resolver, nullptr) != 0);
    CHECK(img.error[0] != '\0');
    CHECK(std::strstr(img.error, "import") != nullptr);
    CHECK(img.base == nullptr);

    // No resolver at all is the same failure.
    CHECK(botw_guest_image_open(&img, file.data(), file.size(), kHeader, nullptr, nullptr) != 0);
    CHECK(std::strstr(img.error, "no resolver") != nullptr || std::strstr(img.error, "import") != nullptr);

    // A relocation outside the image must fail.
    std::vector<uint8_t> bad = make_image();
    put64(bad, kRela, 0x1000);
    CHECK(botw_guest_image_open(&img, bad.data(), bad.size(), kHeader, resolver, nullptr) != 0);
    CHECK(std::strstr(img.error, "outside") != nullptr);

    // A module-local symbol pointing past the image must fail, not truncate.
    std::vector<uint8_t> far = make_image();
    put64(far, kSymtab + 2 * 24u + 8, 0x2000);
    CHECK(botw_guest_image_open(&img, far.data(), far.size(), kHeader, resolver, nullptr) != 0);
    CHECK(std::strstr(img.error, "past the image") != nullptr);

    // A PLT/GOT slot without a symbol is a broken table, not NULL.
    std::vector<uint8_t> nosym = make_image();
    put64(nosym, kJmpRel + 24 + 8, 1025u);
    CHECK(botw_guest_image_open(&img, nosym.data(), nosym.size(), kHeader, resolver, nullptr) != 0);
    CHECK(std::strstr(img.error, "no symbol") != nullptr);

    // A bad fixups header must fail before allocating.
    CHECK(botw_guest_image_open(&img, file.data(), file.size(), "# PATCH_OFF\t0x5000\n", resolver, nullptr) != 0);
    CHECK(img.base == nullptr);

    if (g_fail) {
        std::printf("guest_image_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_image_test: ok (relative, abs64 base/local/import, imports, refusals)\n");
    return 0;
}

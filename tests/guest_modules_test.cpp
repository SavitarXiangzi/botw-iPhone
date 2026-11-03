/* Synthetic multi-module linkage tests for core/guest/guest_modules.c.
 *
 * No title data.  Two small images reproduce the shape of the 1.6.0 set:
 * `provider` defines `exported` and imports `external`; `consumer` imports
 * both `exported` (which must bind to the provider's address) and `external`
 * (which must fall through to HLE).  The cycle is the same one that makes a
 * one-module-at-a-time loader wrong for main/sdk/subsdk0/rtld.
 */
#include "guest_modules.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("guest_modules_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static const char* kHeader =
    "# PATCH_OFF\t0x5000\n"
    "# KEY_PAGE_OFF\t0x1000\n"
    "# KEY_PAGE_SIZE\t0x4000\n"
    "# KEY_K18_IDX\t0\n"
    "# KEY_KTLS_IDX\t1\n"
    "# KEY_KTLR_IDX\t2\n"
    "# KEY_KGATE_IDX\t3\n"
    "# TEXT_END\t0x800\n"
    "# RO_OFF\t0x800\n"
    "# DATA_OFF\t0x800\n"
    "# BUILD_ID\t8e9978d50bdd20b4c8395a106c27ffde\n";

static const size_t kFileLen = 0x5800;
static const size_t kSymtab = 0x300;
static const size_t kStrtab = 0x360;
static const size_t kRela = 0x200;
static const size_t kJmpRel = 0x500;
static const size_t kSlotRelative = 0x600;
static const size_t kSlotLocal = 0x610;
static const size_t kSlotImportAbs = 0x618;
static const size_t kSlotImportJump = 0x620;
static const size_t kSlotLocalJump = 0x628;


static void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    std::memcpy(&v[off], &x, 4);
}
static void put64(std::vector<uint8_t>& v, size_t off, uint64_t x) {
    std::memcpy(&v[off], &x, 8);
}
static uint64_t get64(const uint8_t* p, size_t off) {
    uint64_t x;
    std::memcpy(&x, p + off, 8);
    return x;
}
static void put_sym(std::vector<uint8_t>& v, unsigned index, uint32_t name_off,
                    uint8_t info, uint16_t shndx, uint64_t value) {
    size_t at = kSymtab + index * 24u;
    put32(v, at, name_off);
    v[at + 4] = info;
    v[at + 5] = 0;
    std::memcpy(&v[at + 6], &shndx, 2);
    put64(v, at + 8, value);
    put64(v, at + 16, 0);
}
static void put_rela(std::vector<uint8_t>& v, size_t at, size_t target,
                     uint64_t type, uint64_t sym, int64_t addend) {
    put64(v, at, target);
    put64(v, at + 8, (sym << 32) | type);
    put64(v, at + 16, (uint64_t)addend);
}

/* `provider` defines `exported`; `consumer` imports `import_name`.  Both
 * import `external`, which only HLE can answer. */
static std::vector<uint8_t> make_image(const char* import_name, bool provider) {
    std::vector<uint8_t> v(kFileLen, 0);
    std::string names;
    names.push_back('\0');
    size_t off_external = names.size();
    names += "external";
    names.push_back('\0');
    size_t off_exported = names.size();
    names += "exported";
    names.push_back('\0');
    size_t off_import = names.size();
    names += import_name;
    names.push_back('\0');

    put32(v, 4, 8);                 // MOD0 at image+8
    std::memcpy(&v[8], "MOD0", 4);
    put32(v, 0x0c, 0x100 - 8);      // dynamic table offset
    size_t d = 0x100;
    auto dt = [&](uint64_t tag, uint64_t val) {
        put64(v, d, tag);
        put64(v, d + 8, val);
        d += 16;
    };
    dt(7, kRela);        // DT_RELA
    dt(8, 24 * 4);       // DT_RELASZ: four entries
    dt(9, 24);           // DT_RELAENT
    dt(5, kStrtab);      // DT_STRTAB
    dt(6, kSymtab);      // DT_SYMTAB
    dt(10, names.size()); // DT_STRSZ
    dt(11, 24);          // DT_SYMENT
    dt(23, kJmpRel);     // DT_JMPREL
    dt(2, 24);           // DT_PLTRELSZ: one entry
    dt(20, 7);           // DT_PLTREL = RELA
    dt(12, 0x30);        // DT_INIT
    dt(0, 0);            // DT_NULL

    std::memcpy(&v[kStrtab], names.data(), names.size());
    put_sym(v, 1, (uint32_t)off_external, 0x12, 0, 0);       // undefined external
    put_sym(v, 2, (uint32_t)off_exported, 0x12, 1, 0x700);   // defined here
    put_sym(v, 3, (uint32_t)off_import, 0x12, 0, 0);         // undefined import

    put_rela(v, kRela + 0, kSlotRelative, 1027, 0, 0x1234);
    if (provider) {
        // local definition + two imports
        put_rela(v, kRela + 24, kSlotLocal, 257, 2, 0x8);
        put_rela(v, kRela + 48, kSlotImportAbs, 257, 1, 0x4);
        put_rela(v, kRela + 72, kSlotLocalJump, 1026, 2, 0);
    } else {
        // two references to another module's symbol + one HLE import
        put_rela(v, kRela + 24, kSlotLocal, 257, 3, 0x10);
        put_rela(v, kRela + 48, kSlotImportAbs, 1026, 3, 0);
        put_rela(v, kRela + 72, kSlotImportJump, 257, 1, 0);
    }
    put_rela(v, kJmpRel + 0, kSlotImportJump, 1026, 1, 0);
    return v;
}

static unsigned g_hle_calls;
static void* hle_resolver(void*, const char* name) {
    if (std::strcmp(name, "external") == 0) {
        g_hle_calls++;
        return (void*)(uintptr_t)0xabc000u;
    }
    return nullptr;
}

int main() {
    std::vector<uint8_t> provider = make_image("exported", true);
    std::vector<uint8_t> consumer = make_image("exported", false);
    botw_guest_modules set;
    std::memset(&set, 0, sizeof set);

    CHECK(botw_guest_modules_add(&set, "provider", provider.data(), provider.size(), kHeader) == 0);
    CHECK(botw_guest_modules_add(&set, "consumer", consumer.data(), consumer.size(), kHeader) == 0);
    CHECK(set.count == 2);
    CHECK(set.linked == 0);
    CHECK(botw_guest_modules_link(&set, hle_resolver, nullptr) == 0);
    CHECK(set.linked == 1);
    CHECK(set.cross == 2);
    CHECK(set.hle == 4);
    CHECK(g_hle_calls == 4);
    CHECK(set.mods[0].cross == 0);
    CHECK(set.mods[1].cross == 2);

    const botw_guest_image* a = botw_guest_modules_find_const(&set, "provider");
    const botw_guest_image* b = botw_guest_modules_find_const(&set, "consumer");
    CHECK(a != nullptr && b != nullptr);
    if (a && b) {
        CHECK(a->base != b->base);
        CHECK(botw_guest_image_symbol_count(a) == 4);
        const char* name = nullptr;
        uint64_t value = 0;
        unsigned shndx = 0, bind = 0;
        CHECK(botw_guest_image_symbol_at(a, 2, &name, &value, &shndx, &bind) == 1);
        CHECK(name && std::strcmp(name, "exported") == 0);
        CHECK(shndx == 1 && bind == 1 && value == 0x700);

        uint64_t addr = 0;
        CHECK(botw_guest_image_find_symbol(a, "exported", &addr) == 1);
        CHECK(addr == (uint64_t)(uintptr_t)(a->base + 0x700));
        CHECK(botw_guest_modules_find_symbol(&set, "exported", &addr) == 1);
        CHECK(addr == (uint64_t)(uintptr_t)(a->base + 0x700));
        CHECK(botw_guest_modules_find_symbol(&set, "external", &addr) == 0);

        CHECK(get64(b->base, 0x610) ==
              (uint64_t)(uintptr_t)(a->base + 0x700 + 0x10));
        CHECK(get64(b->base, 0x618) ==
              (uint64_t)(uintptr_t)(a->base + 0x700));
        CHECK(get64(b->base, 0x620) == 0xabc000u);
        CHECK(get64(a->base, 0x618) == 0xabc004u);
        CHECK(get64(a->base, 0x620) == 0xabc000u);
    }

    // Linking is idempotent.
    CHECK(botw_guest_modules_link(&set, hle_resolver, nullptr) == 0);

    // A missing import must fail, not bind NULL.
    {
        botw_guest_modules bad;
        std::memset(&bad, 0, sizeof bad);
        std::vector<uint8_t> missing = make_image("missing", false);
        CHECK(botw_guest_modules_add(&bad, "provider", provider.data(), provider.size(), kHeader) == 0);
        CHECK(botw_guest_modules_add(&bad, "consumer", missing.data(), missing.size(), kHeader) == 0);
        CHECK(botw_guest_modules_link(&bad, hle_resolver, nullptr) != 0);
        CHECK(std::strstr(bad.error, "unresolved import: missing") != nullptr);
        botw_guest_modules_close(&bad);
    }

    // Duplicate names and adding after link are refused.
    {
        botw_guest_modules dup;
        std::memset(&dup, 0, sizeof dup);
        CHECK(botw_guest_modules_add(&dup, "provider", provider.data(), provider.size(), kHeader) == 0);
        CHECK(botw_guest_modules_add(&dup, "provider", provider.data(), provider.size(), kHeader) != 0);
        CHECK(std::strstr(dup.error, "duplicate module name") != nullptr);
        botw_guest_modules_close(&dup);
    }
    CHECK(botw_guest_modules_add(&set, "late", provider.data(), provider.size(), kHeader) != 0);
    CHECK(std::strstr(set.error, "after linking") != nullptr);

    botw_guest_modules_close(&set);
    if (g_fail) {
        std::printf("guest_modules_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_modules_test: ok (cross=2, hle=4, duplicate/missing refusals)\n");
    return 0;
}
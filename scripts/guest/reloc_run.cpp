/* Host-only probe: run the portable loader against a real rewritten module.
 *
 * It never executes guest code.  The resolver hands out one distinct 16-byte
 * block per import name, so a successful open proves that every relocation in
 * the image was applied, every symbol was readable, and no import silently
 * became NULL.  Real module paths are passed on the command line; nothing
 * title-derived is written by this tool.
 *
 *   reloc_run <module.patched.bin> <module.fixups.tsv> [page_size] [--slot]
 *
 *   reloc_run --multi <dir> [page_size] [--hle] [--names]
 *
 * --multi loads main/sdk/subsdk0/rtld from <dir>, binds them in that order,
 * and lets the linker resolve cross-module symbols before falling back to HLE.
 * That is the shape the real four-module set needs; the single-image mode
 * cannot resolve the 430 sdk imports of main or the malloc/free imports of sdk.
 *
 * It also prints the page protection plan the image would need, which is the
 * input to the executable mapping.  The page size defaults to 16 KiB (iOS);
 * pass 0x1000 to see how the same module looks on a 4 KiB host.  `--slot`
 * publishes a synthetic thread slot into the image's key page and checks the
 * four entries; the image is malloc-backed here, so this validates the real
 * module's indices and addresses, not the mapping protections.  `--hle` uses
 * the HLE registry instead of the sentinel resolver and reports how many
 * imports got a real implementation and how many got the zero-return stub.
 * `--names` additionally lists every import the image asked for and, at the
 * end, how many of them still land on a stub per C++ namespace.  That tally
 * is the to-do list for the HLE layers: it is measured from the image, not
 * from a hand-written table.
 */
#include "guest_hle.h"
#include "guest_image.h"
#include "guest_modules.h"
#include "guest_slot.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

static std::vector<char*> g_sentinel_blocks;
static std::unordered_map<std::string, void*> g_sentinels;
static botw_hle_table g_hle;

static void* sentinel_resolve(void*, const char* name) {
    auto it = g_sentinels.find(name);
    if (it != g_sentinels.end()) return it->second;
    char* block = new char[16];
    std::memset(block, 0, 16);
    g_sentinel_blocks.push_back(block);
    g_sentinels.emplace(name, block);
    return block;
}

static bool slurp(const char* path, std::vector<unsigned char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize((size_t)size);
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), (std::streamsize)size);
    return (bool)in;
}

/* "_ZN2nn2oe10InitializeEv" -> "nn::oe".  Itanium nested names only; anything
 * with a substitution or a template parameter returns "" so the caller can
 * bucket it as (global) instead of guessing. */
static std::string mangled_namespace(const std::string& n) {
    std::vector<std::string> parts;
    size_t i;
    if (n.rfind("_ZN", 0) == 0) {
        i = 3;
    } else if (n.rfind("_Z", 0) == 0) {
        i = 2;
    } else {
        return std::string();
    }
    while (i < n.size() && n[i] >= '0' && n[i] <= '9') {
        size_t len = 0;
        while (i < n.size() && n[i] >= '0' && n[i] <= '9') {
            len = len * 10u + (size_t)(n[i] - '0');
            i++;
        }
        if (len == 0 || i + len > n.size()) return std::string();
        std::string comp = n.substr(i, len);
        i += len;
        if (comp[0] == 'E') break;
        if (comp.size() > 1 && (comp[0] == 'S' || comp[1] == 'I')) return std::string();
        parts.push_back(comp);
    }
    if (parts.size() < 2) return std::string();
    parts.pop_back(); /* last component is the function name */
    std::string out;
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) out += "::";
        out += parts[k];
    }
    return out;
}

static void report_names(void) {
    std::map<std::string, size_t> stubbed;
    for (size_t i = 0; i < BOTW_HLE_SLOTS; i++) {
        if (!g_hle.kind[i]) continue;
        const bool impl = g_hle.kind[i] == 1;
        std::printf("reloc_run: %s %s\n", impl ? "impl" : "stub", g_hle.name[i]);
        if (!impl) {
            std::string ns = mangled_namespace(g_hle.name[i]);
            stubbed[ns.empty() ? "(other)" : ns]++;
        }
    }
    std::vector<std::pair<size_t, std::string>> rows;
    for (const auto& kv : stubbed) rows.emplace_back(kv.second, kv.first);
    std::sort(rows.rbegin(), rows.rend());
    std::printf("reloc_run: remaining stubs by namespace:\n");
    for (const auto& r : rows) {
        std::printf("reloc_run:   %5zu  %s\n", r.first, r.second.c_str());
    }
}

/* Publishes a synthetic slot into the real image's key page.  The slot lives
 * on this stack frame only long enough to verify the four entries; nothing
 * executes the image. */
static int publish_slot(botw_guest_image* img) {
    alignas(64) unsigned char slot_mem[BOTW_GUEST_SLOT_MIN * 2];
    botw_guest_slot slot;
    const uint64_t* key;
    int ok = 1;

    if (botw_guest_slot_init(&slot, slot_mem, sizeof slot_mem,
                             (void*)(uintptr_t)0x12345678u) != 0) {
        std::printf("reloc_run: slot init failed: %s\n", slot.error);
        return 1;
    }
    if (botw_guest_slot_publish(img, &slot) != 0) {
        std::printf("reloc_run: slot publish failed: %s\n",
                    slot.error[0] ? slot.error : "unknown");
        return 1;
    }
    key = (const uint64_t*)(const void*)botw_guest_image_key_page(img);
    if (!key) {
        std::printf("reloc_run: key page unavailable after publish\n");
        return 1;
    }
    std::printf("reloc_run: slot k18=%u ktls=%u ktlr=%u kgate=%u abs=%d "
                "x18_off=%#zx gate_off=%#zx\n",
                img->k18_idx, img->ktls_idx, img->ktlr_idx, img->kgate_idx,
                img->slot_abs, slot.x18_off, slot.gate_off);
    ok &= key[img->k18_idx] == (uint64_t)(uintptr_t)(slot.base + slot.x18_off);
    ok &= key[img->ktls_idx] == (uint64_t)(uintptr_t)(slot.base + slot.tls_off);
    ok &= key[img->ktlr_idx] == (uint64_t)(uintptr_t)(slot.base + slot.tlr_off);
    ok &= key[img->kgate_idx] == (uint64_t)(uintptr_t)(slot.base + slot.gate_off);
    std::printf("reloc_run: key page %s\n",
                ok ? "ok (4 absolute field addresses)" : "MISMATCH");
    return ok ? 0 : 1;
}

static void print_plan(botw_guest_image* img, size_t page) {
    botw_guest_segment seg[16];
    size_t n = botw_guest_image_plan(img, page, seg, 16);
    size_t span = botw_guest_image_map_size(img, page);
    if (n == 0) {
        std::printf("reloc_run: plan page=%#zx FAIL: %s\n", page, img->error);
        return;
    }
    size_t shown = n < 16 ? n : 16;
    std::printf("reloc_run: plan page=%#zx span=%#zx runs=%zu conflict=%d\n",
                page, span, n, botw_guest_image_plan_conflict(img, page));
    for (size_t i = 0; i < shown; i++) {
        char prot[4];
        prot[0] = (seg[i].prot & BOTW_GUEST_PROT_READ) ? 'r' : '-';
        prot[1] = (seg[i].prot & BOTW_GUEST_PROT_WRITE) ? 'w' : '-';
        prot[2] = (seg[i].prot & BOTW_GUEST_PROT_EXEC) ? 'x' : '-';
        prot[3] = '\0';
        std::printf("  [%#zx, %#zx) %s\n", seg[i].off, seg[i].off + seg[i].len, prot);
    }
}

static bool file_exists(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return (bool)in;
}

static int run_multi(const char* dir, size_t page, int want_hle, int want_names) {
    static const char* const kOrder[] = {"main", "sdk", "subsdk0", "rtld"};
    botw_guest_modules set;
    std::memset(&set, 0, sizeof set);
    if (want_hle) botw_hle_init(&g_hle);
    for (size_t i = 0; i < sizeof kOrder / sizeof kOrder[0]; ++i) {
        std::string bin = std::string(dir) + "/" + kOrder[i] + ".patched.bin";
        std::string fix = std::string(dir) + "/" + kOrder[i] + ".fixups.tsv";
        if (!file_exists(bin)) {
            if (i == 0) {
                std::printf("reloc_run: cannot read %s\n", bin.c_str());
                return 2;
            }
            continue;
        }
        std::vector<unsigned char> image, fixups;
        if (!slurp(bin.c_str(), image) || !slurp(fix.c_str(), fixups)) {
            std::printf("reloc_run: cannot read %s or %s\n", bin.c_str(), fix.c_str());
            return 2;
        }
        std::string header((const char*)fixups.data(), fixups.size());
        if (botw_guest_modules_add(&set, kOrder[i], image.data(), image.size(),
                                   header.c_str()) != 0) {
            std::printf("reloc_run: FAIL %s: %s\n", kOrder[i], set.error);
            botw_guest_modules_close(&set);
            return 1;
        }
    }
    auto start = std::chrono::steady_clock::now();
    int rc = botw_guest_modules_link(&set,
                                     want_hle ? botw_hle_resolve : sentinel_resolve,
                                     want_hle ? (void*)&g_hle : nullptr);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start).count();
    if (rc != 0) {
        std::printf("reloc_run: FAIL multi: %s\n", set.error);
        botw_guest_modules_close(&set);
        return 1;
    }
    for (unsigned i = 0; i < set.count; ++i) {
        const botw_guest_image& img = set.mods[i].image;
        unsigned fallthrough = img.hle > set.mods[i].cross ? img.hle - set.mods[i].cross : 0;
        std::printf("reloc_run: module %-7s size=%.1f MiB relocs=%u symbols=%u "
                    "resolver=%u cross=%u hle=%u imports=%u entry=%#zx\n",
                    set.mods[i].name, (double)img.file_size / (1024.0 * 1024.0),
                    img.relocs, img.symbols, img.hle, set.mods[i].cross, fallthrough,
                    img.imports, botw_guest_image_entry(&img));
    }
    std::printf("reloc_run: linked modules=%u symbols=%u cross=%u hle=%u load=%lldms\n",
                set.count, set.nsyms, set.cross, set.hle, (long long)ms);
    if (want_hle) {
        std::printf("reloc_run: hle implementations=%zu stubs=%zu%s%s\n",
                    botw_hle_implementation_count(&g_hle),
                    botw_hle_stub_count(&g_hle),
                    g_hle.error[0] ? " error=" : "",
                    g_hle.error[0] ? g_hle.error : "");
        if (want_names) report_names();
    }
    for (unsigned i = 0; i < set.count; ++i) print_plan(&set.mods[i].image, page);
    botw_guest_modules_close(&set);
    for (char* block : g_sentinel_blocks) delete[] block;
    g_sentinel_blocks.clear();
    return 0;
}

int main(int argc, char** argv) {
    size_t page = 0x4000;
    int want_slot = 0;
    int want_hle = 0;
    int want_names = 0;
    if (argc >= 2 && std::strcmp(argv[1], "--multi") == 0) {
        if (argc < 3) {
            std::printf("usage: reloc_run --multi <dir> [page_size] [--hle] [--names]\n");
            return 2;
        }
        for (int i = 3; i < argc; i++) {
            if (std::strcmp(argv[i], "--hle") == 0) {
                want_hle = 1;
            } else if (std::strcmp(argv[i], "--names") == 0) {
                want_hle = 1;
                want_names = 1;
            } else {
                char* end = nullptr;
                unsigned long long v = std::strtoull(argv[i], &end, 0);
                if (!end || *end != '\0' || v < 0x1000) {
                    std::printf("reloc_run: bad page size '%s'\n", argv[i]);
                    return 2;
                }
                page = (size_t)v;
            }
        }
        return run_multi(argv[2], page, want_hle, want_names);
    }
    if (argc < 3) {
        std::printf("usage: reloc_run <module.patched.bin> <module.fixups.tsv> "
                    "[page_size] [--slot] [--hle] [--names]\n"
                    "       reloc_run --multi <dir> [page_size] [--hle] [--names]\n");
        return 2;
    }
    for (int i = 3; i < argc; i++) {
        if (std::strcmp(argv[i], "--slot") == 0) {
            want_slot = 1;
        } else if (std::strcmp(argv[i], "--hle") == 0) {
            want_hle = 1;
        } else if (std::strcmp(argv[i], "--names") == 0) {
            want_hle = 1;
            want_names = 1;
        } else {
            char* end = nullptr;
            unsigned long long v = std::strtoull(argv[i], &end, 0);
            if (!end || *end != '\0' || v < 0x1000) {
                std::printf("reloc_run: bad page size '%s'\n", argv[i]);
                return 2;
            }
            page = (size_t)v;
        }
    }
    std::vector<unsigned char> image;
    std::vector<unsigned char> fixups;
    if (!slurp(argv[1], image) || !slurp(argv[2], fixups)) {
        std::printf("reloc_run: cannot read %s\n", image.empty() ? argv[1] : argv[2]);
        return 2;
    }
    std::string header((const char*)fixups.data(), fixups.size());

    if (want_hle) botw_hle_init(&g_hle);
    auto start = std::chrono::steady_clock::now();
    botw_guest_image img;
    int rc = botw_guest_image_open(&img, image.data(), image.size(), header.c_str(),
                                   want_hle ? botw_hle_resolve : sentinel_resolve,
                                   want_hle ? (void*)&g_hle : nullptr);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start).count();
    if (rc != 0) {
        std::printf("reloc_run: FAIL %s: %s\n", argv[1], img.error);
        return 1;
    }
    std::printf("reloc_run: ok %s size=%.1f MiB relocs=%u symbols=%u hle=%u imports=%u "
                "distinct=%zu entry=%#zx load=%lldms\n",
                argv[1], (double)image.size() / (1024.0 * 1024.0), img.relocs, img.symbols,
                img.hle, img.imports, want_hle ? g_hle.used : g_sentinels.size(),
                botw_guest_image_entry(&img),
                (long long)ms);
    if (want_hle) {
        std::printf("reloc_run: hle implementations=%zu stubs=%zu%s%s\n",
                    botw_hle_implementation_count(&g_hle),
                    botw_hle_stub_count(&g_hle),
                    g_hle.error[0] ? " error=" : "",
                    g_hle.error[0] ? g_hle.error : "");
        if (want_names) report_names();
    }
    print_plan(&img, page);
    if (want_slot && publish_slot(&img) != 0) {
        botw_guest_image_close(&img);
        for (char* block : g_sentinel_blocks) delete[] block;
        g_sentinel_blocks.clear();
        return 1;
    }
    botw_guest_image_close(&img);
    for (char* block : g_sentinel_blocks) delete[] block;
    g_sentinel_blocks.clear();
    return 0;
}

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
 * It also prints the page protection plan the image would need, which is the
 * input to the executable mapping.  The page size defaults to 16 KiB (iOS);
 * pass 0x1000 to see how the same module looks on a 4 KiB host.  `--slot`
 * publishes a synthetic thread slot into the image's key page and checks the
 * four entries; the image is malloc-backed here, so this validates the real
 * module's indices and addresses, not the mapping protections.  `--hle` uses
 * the HLE registry instead of the sentinel resolver and reports how many
 * imports got a real implementation and how many got the zero-return stub.
 */
#include "guest_hle.h"
#include "guest_image.h"
#include "guest_slot.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

int main(int argc, char** argv) {
    size_t page = 0x4000;
    int want_slot = 0;
    int want_hle = 0;
    if (argc < 3) {
        std::printf("usage: reloc_run <module.patched.bin> <module.fixups.tsv> "
                    "[page_size] [--slot]\n");
        return 2;
    }
    for (int i = 3; i < argc; i++) {
        if (std::strcmp(argv[i], "--slot") == 0) {
            want_slot = 1;
        } else if (std::strcmp(argv[i], "--hle") == 0) {
            want_hle = 1;
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
                img.hle, img.imports, g_sentinels.size(), botw_guest_image_entry(&img),
                (long long)ms);
    if (want_hle) {
        std::printf("reloc_run: hle implementations=%zu stubs=%zu%s%s\n",
                    botw_hle_implementation_count(&g_hle),
                    botw_hle_stub_count(&g_hle),
                    g_hle.error[0] ? " error=" : "",
                    g_hle.error[0] ? g_hle.error : "");
    }
    {
        botw_guest_segment seg[16];
        size_t n = botw_guest_image_plan(&img, page, seg, 16);
        size_t span = botw_guest_image_map_size(&img, page);
        if (n == 0) {
            std::printf("reloc_run: plan page=%#zx FAIL: %s\n", page, img.error);
        } else {
            size_t shown = n < 16 ? n : 16;
            std::printf("reloc_run: plan page=%#zx span=%#zx runs=%zu conflict=%d\n",
                        page, span, n, botw_guest_image_plan_conflict(&img, page));
            for (size_t i = 0; i < shown; i++) {
                char prot[4];
                prot[0] = (seg[i].prot & BOTW_GUEST_PROT_READ) ? 'r' : '-';
                prot[1] = (seg[i].prot & BOTW_GUEST_PROT_WRITE) ? 'w' : '-';
                prot[2] = (seg[i].prot & BOTW_GUEST_PROT_EXEC) ? 'x' : '-';
                prot[3] = '\0';
                std::printf("  [%#zx, %#zx) %s\n", seg[i].off, seg[i].off + seg[i].len, prot);
            }
        }
    }
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

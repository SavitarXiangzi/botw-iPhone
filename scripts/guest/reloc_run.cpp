/* Host-only probe: run the portable loader against a real rewritten module.
 *
 * It never executes guest code.  The resolver hands out one distinct 16-byte
 * block per import name, so a successful open proves that every relocation in
 * the image was applied, every symbol was readable, and no import silently
 * became NULL.  Real module paths are passed on the command line; nothing
 * title-derived is written by this tool.
 *
 *   reloc_run <module.patched.bin> <module.fixups.tsv> [page_size]
 *
 * It also prints the page protection plan the image would need, which is the
 * input to the executable mapping.  The page size defaults to 16 KiB (iOS);
 * pass 0x1000 to see how the same module looks on a 4 KiB host.
 */
#include "guest_image.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

static std::vector<char*> g_sentinel_blocks;
static std::unordered_map<std::string, void*> g_sentinels;

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

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::printf("usage: reloc_run <module.patched.bin> <module.fixups.tsv> [page_size]\n");
        return 2;
    }
    std::vector<unsigned char> image;
    std::vector<unsigned char> fixups;
    if (!slurp(argv[1], image) || !slurp(argv[2], fixups)) {
        std::printf("reloc_run: cannot read %s\n", image.empty() ? argv[1] : argv[2]);
        return 2;
    }
    std::string header((const char*)fixups.data(), fixups.size());

    auto start = std::chrono::steady_clock::now();
    botw_guest_image img;
    int rc = botw_guest_image_open(&img, image.data(), image.size(), header.c_str(),
                                   sentinel_resolve, nullptr);
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
    {
        size_t page = argc == 4 ? (size_t)std::strtoull(argv[3], nullptr, 0) : (size_t)0x4000;
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
    botw_guest_image_close(&img);
    for (char* block : g_sentinel_blocks) delete[] block;
    g_sentinel_blocks.clear();
    return 0;
}

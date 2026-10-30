/* keycheck -- does nvngfx_shader_key_from_guest() reproduce nxnative's key?

 * The offline chain files an MSL blob under
 *     sha256(control[0:nctl] || code[0:ncode])[0..8]
 * and the runtime recomputes that from the guest's NVNshaderData at
 * nvnProgramSetShaders.  Python (nx_shader.py), the C runtime
 * (nvn_shader_key.c) and nxnative (hle_nvn.c keys_hash) each implement it, so
 * a drift in one of them orphans every blob without any error message.  This
 * tool replays the runtime's function over real shaders and compares the
 * result with the file names the Python side produced.
 *
 * Input: the --out directory of
 *     nx_shader.py extract <pack> --keep-full-code
 * i.e. code/<key>_<stage>.ctl + code/<key>_<stage>.code.  The .code file must
 * be the *whole* code area, 0x30 head included; the .bin files next to it are
 * stripped for the translator and are skipped.
 *
 * Build and run (no game data needed to build):
 *     clang++ -std=c++20 -O1 scripts/shader_chain/keycheck.cpp \
 *         ios/nvn/src/nvn_shader_key.c -I ios/nvn/include -o build/keycheck
 *     build/keycheck build/shader-chain/work/code
 *
 * Exit code 0 = every shader matched.  Nothing is written anywhere.
 */
#include "nvn_shader.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static bool slurp(const std::string& path, std::vector<unsigned char>* out) {
    // ifstream, not fopen: MSVC marks fopen deprecated and the CI builds this
    // translation unit with -Wall -Wextra -Werror.
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out->empty();
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <extract --out>/code\n", argv[0]);
        return 2;
    }
    const fs::path dir = argv[1];
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "keycheck: %s is not a directory\n", argv[1]);
        return 2;
    }

    int total = 0, bad = 0, skipped = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string p = e.path().string();
        if (p.size() < 5 || p.compare(p.size() - 5, 5, ".code") != 0) continue;
        const std::string base = p.substr(0, p.size() - 5);
        std::string want = fs::path(base).filename().string();
        const size_t cut = want.rfind('_');
        if (cut == std::string::npos) { ++skipped; continue; }
        want.resize(cut);

        std::vector<unsigned char> code, ctl;
        if (!slurp(p, &code) || !slurp(base + ".ctl", &ctl)) { ++skipped; continue; }
        ++total;

        char key[17];
        const int rc = nvngfx_shader_key_from_guest(ctl.data(), code.data(), key);
        if (rc != 0 || want != key) {
            if (bad < 10) {
                std::printf("FAIL %s: %s\n", want.c_str(),
                            rc != 0 ? "no key (control section has no code length)"
                                    : key);
            }
            ++bad;
        }
    }

    std::printf("keycheck: %d shader(s), %d match, %d bad, %d skipped\n",
                total, total - bad, bad, skipped);
    if (total == 0) {
        std::fprintf(stderr,
                     "keycheck: no <key>_<stage>.code in %s -- run "
                     "nx_shader.py extract ... --keep-full-code first\n", argv[1]);
        return 2;
    }
    return bad == 0 ? 0 : 1;
}
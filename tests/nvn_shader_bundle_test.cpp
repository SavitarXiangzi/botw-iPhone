/* The file form of the blob table: manifest parsing, merging with the
 * generated table, and the rule that a bad bundle never disturbs the one that
 * is already installed.
 *
 * Runs on Windows and Linux (no Apple headers), so a manifest typo shows up
 * here instead of as a black screen on a phone. */
#include "nvn_shader.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    ++g_failures;
}

static void write_text(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

static void write_bytes(const fs::path& p, const void* data, size_t n) {
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
}

static const char* kPs =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "fragment float4 nvngfx_main_fs() { return float4(1.0, 0.0, 0.0, 1.0); }\n";

static const char* kVs =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "vertex float4 nvngfx_main_vs(uint i [[vertex_id]]) { return float4(0.0); }\n";

/* One bundle directory: two shaders, one with a layout and c1 bytes, one with
 * an explicit empty c1, plus a comment line. */
static void make_good_bundle(const fs::path& dir) {
    fs::create_directories(dir);
    write_text(dir / "ps.msl", kPs);
    write_text(dir / "vs.msl", kVs);
    const unsigned char c1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    write_bytes(dir / "ps.c1", c1, sizeof c1);
    write_bytes(dir / "empty.c1", nullptr, 0);
    write_text(dir / "manifest.tsv",
               "# key\tstage\tmsl\tc1\targs\n"
               "00000000000000b1\tps\tps.msl\tps.c1\t5:3:0:1,9:0:1:2\n"
               "00000000000000b2\tvs\tvs.msl\tempty.c1\t-\n");
}

static void make_bad_manifest(const fs::path& dir, const std::string& body) {
    fs::remove_all(dir);
    fs::create_directories(dir);
    write_text(dir / "ps.msl", kPs);
    write_text(dir / "manifest.tsv", body);
}

int main() {
    const fs::path root =
        fs::temp_directory_path() / "nvn_shader_bundle_test";
    fs::remove_all(root);
    fs::create_directories(root);

    /* A table that was installed before the bundle: the generated one in a
     * real build.  It must survive a load and come back on unload. */
    static const nvngfx_shader_blob kBase[] = {
        {"00000000000000c0", NVNGFX_STAGE_FRAGMENT, kPs, nullptr, 0, nullptr, 0},
    };
    check(nvngfx_shader_set_blobs(kBase, 1) == 0, "base table installs");

    const fs::path good = root / "good";
    make_good_bundle(good);

    char why[512] = {0};
    check(nvngfx_shader_load_bundle(good.string().c_str(), why, sizeof why) == 0,
          "good bundle loads");
    if (g_failures) std::printf("  why: %s\n", why);
    check(nvngfx_shader_bundle_count() == 2, "bundle contributes two entries");
    check(nvngfx_shader_blob_count() == 3, "installed table has base + bundle");

    const nvngfx_shader_blob* ps =
        nvngfx_shader_lookup_blob("00000000000000b1", NVNGFX_STAGE_FRAGMENT,
                                  nullptr);
    check(ps != nullptr, "fragment blob is found by key");
    if (ps) {
        check(std::strcmp(ps->msl, kPs) == 0, "fragment MSL came from the file");
        check(ps->num_args == 2, "layout has two entries");
        check(ps->c1 != nullptr && ps->c1_size == 8, "c1 sidecar is loaded");
        check(ps->c1 && ps->c1[0] == 1 && ps->c1[7] == 8,
              "c1 bytes are in guest order");
        if (ps->num_args == 2) {
            check(ps->args[0].id == 5 && ps->args[0].slot == 3 &&
                      ps->args[0].packed == 0 &&
                      ps->args[0].kind == NVNGFX_ARG_UBO,
                  "first argument is ubo id 5 -> slot 3");
            check(ps->args[1].id == 9 && ps->args[1].slot == 0 &&
                      ps->args[1].packed == 1 &&
                      ps->args[1].kind == NVNGFX_ARG_TEXTURE,
                  "second argument is texture id 9 -> packed 1");
        }
    }
    const nvngfx_shader_blob* vs =
        nvngfx_shader_lookup_blob("00000000000000b2", NVNGFX_STAGE_VERTEX,
                                  nullptr);
    check(vs != nullptr, "vertex blob is found by key");
    if (vs) {
        check(vs->c1 != nullptr && vs->c1_size == 0,
              "an empty c1 file is an explicit empty c1");
        check(vs->args == nullptr && vs->num_args == 0, "no layout is NULL");
    }
    check(nvngfx_shader_lookup("00000000000000b1", NVNGFX_STAGE_VERTEX,
                               nullptr) == nullptr,
          "a fragment key does not satisfy the vertex stage");
    check(nvngfx_shader_lookup("00000000000000c0", NVNGFX_STAGE_FRAGMENT,
                               nullptr) != nullptr,
          "the generated table is still reachable");

    /* Loading the same directory twice must not stack duplicates. */
    check(nvngfx_shader_load_bundle(good.string().c_str(), why, sizeof why) == 0,
          "a second load of the same bundle succeeds");
    check(nvngfx_shader_blob_count() == 3, "the second load replaces, not adds");
    check(nvngfx_shader_bundle_count() == 2, "bundle count stays two");

    /* Failures leave the installed table exactly as it was. */
    {
        const fs::path bad = root / "missing";
        make_bad_manifest(bad, "00000000000000b3\tps\tnope.msl\t-\t-\n");
        check(nvngfx_shader_load_bundle(bad.string().c_str(), why, sizeof why) != 0,
              "a manifest naming a missing file is refused");
        check(std::strstr(why, "nope.msl") != nullptr,
              "the failure names the missing file");
    }
    {
        const fs::path bad = root / "badkey";
        make_bad_manifest(bad, "xyz\tps\tps.msl\t-\t-\n");
        check(nvngfx_shader_load_bundle(bad.string().c_str(), why, sizeof why) != 0,
              "a malformed key is refused");
    }
    {
        const fs::path bad = root / "badstage";
        make_bad_manifest(bad, "00000000000000b4\tcs\tps.msl\t-\t-\n");
        check(nvngfx_shader_load_bundle(bad.string().c_str(), why, sizeof why) != 0,
              "a compute stage is refused");
    }
    {
        const fs::path bad = root / "badargs";
        make_bad_manifest(bad, "00000000000000b5\tps\tps.msl\t-\t5:x:0:1\n");
        check(nvngfx_shader_load_bundle(bad.string().c_str(), why, sizeof why) != 0,
              "a malformed layout entry is refused");
    }
    {
        const fs::path bad = root / "duplicate";
        make_bad_manifest(bad,
                          "00000000000000c0\tps\tps.msl\t-\t-\n");
        check(nvngfx_shader_load_bundle(bad.string().c_str(), why, sizeof why) != 0,
              "a key that collides with the generated table is refused");
    }
    check(nvngfx_shader_blob_count() == 3, "failed loads changed nothing");
    check(nvngfx_shader_lookup("00000000000000b1", NVNGFX_STAGE_FRAGMENT,
                               nullptr) != nullptr,
          "the previous bundle is still installed");

    /* Bootup_Graphics alone is already 5,102 stages and the whole title is
     * about 19,595, so a 4,096-entry manifest cap would make every real
     * bundle fail to load.  One MSL can be shared by many keys; this test is
     * about the manifest cap, not about Metal. */
    {
        const fs::path many = root / "many";
        fs::remove_all(many);
        fs::create_directories(many);
        write_text(many / "ps.msl", kPs);
        constexpr unsigned kMany = 4097;
        std::string manifest;
        for (unsigned i = 0; i < kMany; ++i) {
            char key[32];
            std::snprintf(key, sizeof key, "%016llx",
                          0x1000000000000000ull + i);
            manifest += key;
            manifest += "\tps\tps.msl\t-\t-\n";
        }
        write_text(many / "manifest.tsv", manifest);
        check(nvngfx_shader_load_bundle(many.string().c_str(), why,
                                        sizeof why) == 0,
              "a bundle larger than 4,096 entries loads");
        check(nvngfx_shader_bundle_count() == kMany,
              "the large bundle contributes every entry");
        check(nvngfx_shader_lookup("1000000000001000", NVNGFX_STAGE_FRAGMENT,
                                   nullptr) != nullptr,
              "the last entry of a large bundle is reachable");
    }

    nvngfx_shader_unload_bundle();
    check(nvngfx_shader_bundle_count() == 0, "unload drops the bundle");
    check(nvngfx_shader_blob_count() == 1, "unload restores the generated table");
    check(nvngfx_shader_lookup("00000000000000b1", NVNGFX_STAGE_FRAGMENT,
                               nullptr) == nullptr,
          "bundle keys are gone after unload");
    check(nvngfx_shader_lookup("00000000000000c0", NVNGFX_STAGE_FRAGMENT,
                               nullptr) != nullptr,
          "generated keys survive unload");

    nvngfx_shader_set_blobs(nullptr, 0);
    fs::remove_all(root);

    if (g_failures) {
        std::printf("nvn_shader_bundle_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvn_shader_bundle_test: ok (manifest, merge, c1, refusals)\n");
    return 0;
}
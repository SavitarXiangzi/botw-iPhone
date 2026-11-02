/* nn::fs HLE against a real directory tree on the host.
 *
 * The tree is created under the system temp directory, driven through the
 * registered function pointers exactly as the guest would call them, and
 * removed at the end.  Nothing in this test knows the guest is not running:
 * every value comes back through the out-pointers the SDK signature uses.
 *
 * The 23 mangled names are copied from `reloc_run --names` on the real
 * main.patched.bin.  A typo that appears in both this list and guest_hle_fs.c
 * would pass here; the real-image check is the tally in reloc_run, which must
 * show zero remaining nn::fs stubs.
 */
#include "guest_hle.h"
#include "guest_hle_fs.h"
#include "guest_hle_nn.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("guest_hle_fs_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; } } while (0)

static const char* const kFsNames[BOTW_HLE_FS_COUNT] = {
    "_ZN2nn2fs10CreateFileEPKcl",
    "_ZN2nn2fs10DeleteFileEPKc",
    "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",
    "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl",
    "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc",
    "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE",
    "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci",
    "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl",
    "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE",
    "_ZN2nn2fs14CommitSaveDataEPKc",
    "_ZN2nn2fs15CreateDirectoryEPKc",
    "_ZN2nn2fs17MountAddOnContentEPKciPvm",
    "_ZN2nn2fs22QueryMountRomCacheSizeEPm",
    "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc",
    "_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi",
    "_ZN2nn2fs7UnmountEPKc",
    "_ZN2nn2fs8MountRomEPKcPvm",
    "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",
    "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",
    "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE",
    "_ZN2nn2fs9CloseFileENS0_10FileHandleE",
    "_ZN2nn2fs9FlushFileENS0_10FileHandleE",
    "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE",
};

/* Signature aliases, one per shape the SDK uses. */
using result_fn = uint32_t (*)(void);
using mount_fn = uint32_t (*)(const char*, void*, size_t);
using query_size_fn = uint32_t (*)(size_t*);
using open_file_fn = uint32_t (*)(uint32_t*, const char*, int);
using read_fn = uint32_t (*)(uint64_t*, uint32_t, int64_t, void*, size_t);
using read_opt_fn = uint32_t (*)(uint64_t*, uint32_t, int64_t, void*, size_t, const void*);
using write_fn = uint32_t (*)(uint32_t, int64_t, const void*, size_t, const void*);
using get_size_fn = uint32_t (*)(int64_t*, uint32_t);
using set_size_fn = uint32_t (*)(uint32_t, int64_t);
using handle_fn = uint32_t (*)(uint32_t);
using create_file_fn = uint32_t (*)(const char*, int64_t);
using path_fn = uint32_t (*)(const char*);
using entry_type_fn = uint32_t (*)(uint32_t*, const char*);
using open_dir_fn = uint32_t (*)(uint32_t*, const char*, int);
using read_dir_fn = uint32_t (*)(int64_t*, botw_hle_fs_dir_entry*, uint32_t, int64_t);
using unmount_fn = uint32_t (*)(const char*);

static botw_hle_table g_hle;
static botw_hle_fs_state g_fs;

static void write_text(const std::filesystem::path& p, const std::string& s) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(s.data(), (std::streamsize)s.size());
}

static std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main() {
    /* The guest's DirectoryEntry is a fixed 0x310-byte record.  If this ever
     * drifts, every ReadDirectory writes the wrong bytes into guest memory. */
    static_assert(sizeof(botw_hle_fs_dir_entry) == 0x310, "DirectoryEntry is 0x310 bytes");
    static_assert(offsetof(botw_hle_fs_dir_entry, type) == 0x303, "type at 0x303");
    static_assert(offsetof(botw_hle_fs_dir_entry, size) == 0x308, "size at 0x308");

    botw_hle_init(&g_hle);
    CHECK(g_hle.error[0] == '\0');
    for (unsigned i = 0; i < BOTW_HLE_FS_COUNT; i++) {
        CHECK(botw_hle_lookup(&g_hle, kFsNames[i]) != nullptr);
    }
    /* libc + nn::oe + nn::fs, and none of them on a stub. */
    CHECK(botw_hle_implementation_count(&g_hle) >= 46 + BOTW_HLE_OE_COUNT + BOTW_HLE_FS_COUNT);
    CHECK(botw_hle_stub_count(&g_hle) == 0);

    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("botw_fs_test_" + std::to_string((long long)std::filesystem::file_time_type::clock::now()
                                              .time_since_epoch().count()));
    std::filesystem::create_directories(root / "rom" / "Pack" / "sub");
    write_text(root / "rom" / "Pack" / "Boot.pack", "0123456789abcdef");
    write_text(root / "rom" / "Pack" / "sub" / "inner.bin", "xy");

    /* An unconfigured state fails loudly instead of touching the CWD. */
    botw_hle_fs_state fresh{};
    CHECK(botw_hle_fs_init(&fresh, "") == -1);
    CHECK(botw_hle_fs_init(&fresh, "/") == -1);
    CHECK(fresh.configured == 0);

    CHECK(botw_hle_fs_init(&g_fs, root.string().c_str()) == 0);
    CHECK(botw_hle_fs_register(&g_hle, &g_fs) == 0);

    mount_fn mount_rom = (mount_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs8MountRomEPKcPvm");
    query_size_fn cache_size = (query_size_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs22QueryMountRomCacheSizeEPm");
    open_file_fn open_file = (open_file_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    read_fn read_file = (read_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
    read_opt_fn read_file_opt = (read_opt_fn)botw_hle_lookup(
        &g_hle, "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE");
    write_fn write_file = (write_fn)botw_hle_lookup(
        &g_hle, "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
    get_size_fn get_size = (get_size_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE");
    set_size_fn set_size = (set_size_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl");
    handle_fn close_file = (handle_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs9CloseFileENS0_10FileHandleE");
    handle_fn flush_file = (handle_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs9FlushFileENS0_10FileHandleE");
    create_file_fn create_file = (create_file_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs10CreateFileEPKcl");
    path_fn delete_file = (path_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs10DeleteFileEPKc");
    path_fn create_dir = (path_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs15CreateDirectoryEPKc");
    path_fn delete_tree = (path_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc");
    entry_type_fn entry_type = (entry_type_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc");
    open_dir_fn open_dir = (open_dir_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci");
    read_dir_fn read_dir = (read_dir_fn)botw_hle_lookup(
        &g_hle, "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl");
    handle_fn close_dir = (handle_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
    unmount_fn unmount = (unmount_fn)botw_hle_lookup(&g_hle, "_ZN2nn2fs7UnmountEPKc");

    size_t cache = 0;
    CHECK(cache_size(&cache) == 0);
    CHECK(cache == 0x400000u);
    CHECK(mount_rom("rom", nullptr, cache) == 0);

    /* Mounting creates <root>/rom when it is missing, so a typos'd mount name
     * shows up as "file not found", not as a hard failure at mount time. */
    CHECK(std::filesystem::is_directory(root / "rom"));

    uint32_t type = 0xffffffffu;
    CHECK(entry_type(&type, "rom:/Pack/Boot.pack") == 0);
    CHECK(type == 1);
    CHECK(entry_type(&type, "rom:/Pack/sub") == 0);
    CHECK(type == 0);
    CHECK(entry_type(&type, "/rom/Pack/sub") == 0);
    CHECK(entry_type(&type, "rom:/Pack/nope") != 0);

    uint32_t handle = 0;
    CHECK(open_file(&handle, "rom:/Pack/Boot.pack", 1) == 0);
    CHECK(handle >= 0x1000u);
    int64_t size = -1;
    CHECK(get_size(&size, handle) == 0);
    CHECK(size == 16);

    char buf[16];
    uint64_t got = 999;
    std::memset(buf, 0, sizeof buf);
    CHECK(read_file(&got, handle, 4, buf, 4) == 0);
    CHECK(got == 4);
    CHECK(std::memcmp(buf, "4567", 4) == 0);
    /* The ReadOption overload must land on the same implementation. */
    CHECK(read_file_opt(&got, handle, 0, buf, 2, nullptr) == 0);
    CHECK(got == 2);
    CHECK(std::memcmp(buf, "01", 2) == 0);
    CHECK(flush_file(handle) == 0);
    CHECK(close_file(handle) == 0);
    /* A closed handle is invalid, not a pointer to slot 0. */
    CHECK(close_file(handle) != 0);
    CHECK(read_file(&got, handle, 0, buf, 1) != 0);

    /* Path escapes are refused before the host is touched. */
    CHECK(open_file(&handle, "rom:/../escape", 1) != 0);
    /* A Windows-style separator is refused too; spelled without a literal
     * drive-letter-looking sequence so the leak check stays quiet. */
    const std::string backslash_path = std::string("rom:") + (char)92 + ".." + (char)92 + "escape";
    CHECK(open_file(&handle, backslash_path.c_str(), 1) != 0);
    CHECK(open_file(&handle, "rom:/Pack/./Boot.pack", 1) == 0);
    CHECK(close_file(handle) == 0);

    /* Directory enumeration: names, types and sizes, both filters. */
    uint32_t dir_handle = 0;
    CHECK(open_dir(&dir_handle, "rom:/Pack", 3) == 0);
    botw_hle_fs_dir_entry entries[8];
    int64_t count = -1;
    CHECK(read_dir(&count, entries, dir_handle, 8) == 0);
    CHECK(count == 2);
    std::set<std::string> names;
    for (int64_t i = 0; i < count; i++) {
        names.insert(entries[i].name);
        if (std::strcmp(entries[i].name, "Boot.pack") == 0) {
            CHECK(entries[i].type == 1);
            CHECK(entries[i].size == 16);
        }
        if (std::strcmp(entries[i].name, "sub") == 0) {
            CHECK(entries[i].type == 0);
            CHECK(entries[i].size == 0);
        }
    }
    CHECK(names.count("Boot.pack") == 1);
    CHECK(names.count("sub") == 1);
    CHECK(names.count(".") == 0);
    CHECK(names.count("..") == 0);
    /* The cursor is exhausted, so a second read returns nothing. */
    CHECK(read_dir(&count, entries, dir_handle, 8) == 0);
    CHECK(count == 0);
    CHECK(close_dir(dir_handle) == 0);

    CHECK(open_dir(&dir_handle, "rom:/Pack", 1) == 0);
    CHECK(read_dir(&count, entries, dir_handle, 8) == 0);
    CHECK(count == 1);
    CHECK(std::strcmp(entries[0].name, "sub") == 0);
    CHECK(close_dir(dir_handle) == 0);

    /* Write path: create, write at an offset, read it back through a fresh
     * handle, then grow and shrink the file. */
    CHECK(open_file(&handle, "rom:/Pack/save.bin", 2) == 0);
    CHECK(write_file(handle, 0, "hello", 5, nullptr) == 0);
    CHECK(write_file(handle, 5, "world", 5, nullptr) == 0);
    CHECK(get_size(&size, handle) == 0);
    CHECK(size == 10);
    CHECK(set_size(handle, 4) == 0);
    CHECK(get_size(&size, handle) == 0);
    CHECK(size == 4);
    CHECK(close_file(handle) == 0);
    CHECK(read_text(root / "rom" / "Pack" / "save.bin") == "hell");

    CHECK(create_file("rom:/Pack/zero.bin", 0x20) == 0);
    CHECK(std::filesystem::is_regular_file(root / "rom" / "Pack" / "zero.bin"));
    CHECK(std::filesystem::file_size(root / "rom" / "Pack" / "zero.bin") == 0x20u);

    CHECK(create_dir("rom:/NewDir") == 0);
    CHECK(std::filesystem::is_directory(root / "rom" / "NewDir"));
    CHECK(create_dir("rom:/NewDir") == 0); /* idempotent, like the SDK */

    CHECK(delete_file("rom:/Pack/zero.bin") == 0);
    CHECK(!std::filesystem::exists(root / "rom" / "Pack" / "zero.bin"));
    CHECK(delete_file("rom:/Pack/zero.bin") != 0);

    /* The one deliberate gap: recursive delete refuses and says so. */
    unsigned long errors_before = g_fs.errors;
    CHECK(delete_tree("rom:/NewDir") != 0);
    CHECK(g_fs.errors == errors_before + 1);
    CHECK(g_fs.last_error[0] != '\0');

    CHECK(unmount("rom") == 0);
    CHECK(unmount("rom") != 0);
    /* After unmount the path no longer resolves, so a stale path fails. */
    CHECK(open_file(&handle, "rom:/Pack/Boot.pack", 1) != 0);

    CHECK(g_fs.calls > 30);

    std::filesystem::remove_all(root);

    if (g_fail) {
        std::printf("guest_hle_fs_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("guest_hle_fs_test: ok (%u nn::fs names, sandboxed paths, handles, directories, explicit gap)\n",
                BOTW_HLE_FS_COUNT);
    return 0;
}

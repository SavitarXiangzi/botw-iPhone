/* nn::fs HLE: a small virtual filesystem over one host directory.
 *
 * The 23 nn::fs imports in the 1.6.0 main image are the game's only way to
 * read its own data.  Every path the guest passes is relative to a mount
 * (`rom:/Pack/...`, `/rom/Pack/...`, or a bare relative path), so the whole
 * namespace can be redirected into one host directory:
 *
 *     <root>/rom/Pack/Boot.pack
 *     <root>/save/...
 *
 * Rules, in the order they are enforced:
 *
 *   - `root` must be configured before anything else; an empty root is not
 *     "the current directory", it is a hard failure.
 *   - `root` may not be a filesystem root: a bare "/" or a drive letter
 *     with or without a trailing separator. The mount table creates
 *     directories, so a misconfigured root must never turn DeleteFile
 *     into "delete anywhere".
 *
 * Handles are opaque u32s: 0x1000|n for files, 0x2000|n for directories.  A
 * handle that was never opened, or was closed, fails instead of aliasing
 * slot 0.  Nothing here dereferences guest memory except the out-pointers the
 * caller supplied.
 *
 * Not implemented on purpose: DeleteDirectoryRecursively.  A recursive delete
 * needs lstat plus a reparse-point check to be safe on all hosts, and the
 * game only reaches it from the "delete save data" menu.  It returns an
 * explicit failure and increments `errors` so the call shows up.
 */
#ifndef BOTW_GUEST_HLE_FS_H
#define BOTW_GUEST_HLE_FS_H

#include <stddef.h>
#include <stdint.h>

#include "guest_hle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOTW_HLE_FS_PATH_MAX    768
#define BOTW_HLE_FS_MAX_MOUNTS    8
#define BOTW_HLE_FS_MAX_FILES    64
#define BOTW_HLE_FS_MAX_DIRS     16
#define BOTW_HLE_FS_COUNT        23u

/* nn::Result-shaped failure: bit31 set, module 2 (fs), description 0x7FF.
 * Callers only test "zero or not"; the exact module/description is ours and
 * is not a Nintendo code. */
#define BOTW_HLE_FS_ERROR 0x804FFE00u

/* Horizon's nn::fs::DirectoryEntry as the guest sees it: a 0x300-byte name,
 * three bytes of padding, a one-byte type and a 64-bit size at 0x308.  The
 * type is 0 for a directory and 1 for a file. */
typedef struct {
    char     name[0x300];
    uint8_t  pad[3];
    uint8_t  type;
    uint32_t pad2;
    int64_t  size;
} botw_hle_fs_dir_entry;

typedef struct {
    char     name[32];
    char     host[BOTW_HLE_FS_PATH_MAX];
    unsigned used;
} botw_hle_fs_mount;

typedef struct {
    unsigned used;
    void*    file;                      /* FILE*, host-side only */
    char     path[BOTW_HLE_FS_PATH_MAX];
} botw_hle_fs_file;

typedef struct {
    unsigned used;
    void*    dir;                       /* DIR*, host-side only */
    char     path[BOTW_HLE_FS_PATH_MAX];
    int      mode;                      /* 1 dirs, 2 files, 3 both */
} botw_hle_fs_dir;

typedef struct {
    char root[BOTW_HLE_FS_PATH_MAX];
    int  configured;
    botw_hle_fs_mount mounts[BOTW_HLE_FS_MAX_MOUNTS];
    botw_hle_fs_file  files[BOTW_HLE_FS_MAX_FILES];
    botw_hle_fs_dir   dirs[BOTW_HLE_FS_MAX_DIRS];
    unsigned long calls;
    unsigned long errors;
    char last_error[160];
} botw_hle_fs_state;

/* Closes every open handle and drops every mount, then clears the root. */
void botw_hle_fs_reset(botw_hle_fs_state* state);

/* Points the filesystem at `host_root` and clears all state.  Returns 0, or
 * -1 with last_error set (empty root, filesystem root, or path too long). */
int botw_hle_fs_init(botw_hle_fs_state* state, const char* host_root);

/* The singleton used when register() is given NULL.  Unconfigured until
 * botw_hle_fs_init() is called on it. */
botw_hle_fs_state* botw_hle_fs_default(void);

/* Registers the 23 nn::fs implementations.  `state` may be NULL.  Returns 0,
 * or -1 with hle->error set. */
int botw_hle_fs_register(botw_hle_table* hle, botw_hle_fs_state* state);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_HLE_FS_H */

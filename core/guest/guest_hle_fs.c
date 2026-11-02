#if defined(_WIN32)
#  define _CRT_SECURE_NO_WARNINGS
#  define WIN32_LEAN_AND_MEAN
#  include <direct.h>
#  include <io.h>
#  include <windows.h>
#  define fs_mkdir(p) _mkdir(p)
typedef struct _stat64 fs_stat_t;
#  define fs_stat(p, sb) _stat64((p), (sb))
#  define fs_isdir(m) (((m) & _S_IFMT) == _S_IFDIR)
#else
/* Strict -std=c11 hides fileno/ftruncate/mkdir behind this.  Linux CI is the
 * reason: without it the build fails exactly like strnlen did. */
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#  include <dirent.h>
#  include <unistd.h>
#  define fs_mkdir(p) mkdir((p), 0777)
typedef struct stat fs_stat_t;
#  define fs_stat(p, sb) stat((p), (sb))
#  define fs_isdir(m) S_ISDIR(m)
#endif

#include "guest_hle_fs.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Opaque handle encoding.  The ranges are far enough apart that a file handle
 * can never look like a directory handle, and slot 0 is never used so a zero
 * handle (the stub's answer) is always invalid. */
#define FS_FILE_HANDLE_BASE 0x1000u
#define FS_DIR_HANDLE_BASE  0x2000u

static botw_hle_fs_state g_default_state;
static botw_hle_fs_state* g_state = &g_default_state;

/* MSVC has no <dirent.h>, so enumeration is a two-branch iterator rather than
 * a portable wrapper around opendir().  Both branches report a name, whether
 * it is a directory, and its size, which is everything DirectoryEntry needs. */
typedef struct {
    char path[BOTW_HLE_FS_PATH_MAX];
#if defined(_WIN32)
    HANDLE find;
    WIN32_FIND_DATAA data;
    int first;
#else
    DIR* dir;
#endif
} fs_dir_iter;

static fs_dir_iter* fs_dir_open(const char* host) {
    fs_dir_iter* it = (fs_dir_iter*)calloc(1, sizeof *it);
    if (!it) return NULL;
    if (strlen(host) + 1 > sizeof it->path) {
        free(it);
        return NULL;
    }
    snprintf(it->path, sizeof it->path, "%s", host);
#if defined(_WIN32)
    {
        char pattern[BOTW_HLE_FS_PATH_MAX + 3];
        if (strlen(host) + 3 > sizeof pattern) {
            free(it);
            return NULL;
        }
        snprintf(pattern, sizeof pattern, "%s\\*", host);
        it->find = FindFirstFileA(pattern, &it->data);
        if (it->find == INVALID_HANDLE_VALUE) {
            free(it);
            return NULL;
        }
        it->first = 1;
    }
#else
    it->dir = opendir(host);
    if (!it->dir) {
        free(it);
        return NULL;
    }
#endif
    return it;
}

/* Returns 1 with a name, 0 at the end of the directory. */
static int fs_dir_next(fs_dir_iter* it, char* name, size_t cap,
                       int* is_dir, int64_t* size) {
#if defined(_WIN32)
    if (it->first) it->first = 0;
    else if (!FindNextFileA(it->find, &it->data)) return 0;
    snprintf(name, cap, "%s", it->data.cFileName);
    if (is_dir) *is_dir = (it->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (size) {
        *size = ((int64_t)it->data.nFileSizeHigh << 32) | (int64_t)it->data.nFileSizeLow;
    }
    return 1;
#else
    for (;;) {
        struct dirent* e = readdir(it->dir);
        char host[BOTW_HLE_FS_PATH_MAX];
        fs_stat_t sb;
        if (!e) return 0;
        if (strlen(it->path) + 1 + strlen(e->d_name) + 1 > sizeof host) continue;
        snprintf(host, sizeof host, "%s/%s", it->path, e->d_name);
        if (fs_stat(host, &sb) != 0) continue;
        snprintf(name, cap, "%s", e->d_name);
        if (is_dir) *is_dir = fs_isdir(sb.st_mode) ? 1 : 0;
        if (size) *size = (int64_t)sb.st_size;
        return 1;
    }
#endif
}

static void fs_dir_close(fs_dir_iter* it) {
    if (!it) return;
#if defined(_WIN32)
    if (it->find != INVALID_HANDLE_VALUE) FindClose(it->find);
#else
    if (it->dir) closedir(it->dir);
#endif
    free(it);
}

static void fs_error(botw_hle_fs_state* st, const char* fmt, ...) {
    va_list ap;
    st->errors++;
    va_start(ap, fmt);
    vsnprintf(st->last_error, sizeof st->last_error, fmt, ap);
    va_end(ap);
}

static uint32_t fs_fail(botw_hle_fs_state* st, const char* what, const char* path) {
    fs_error(st, "%s%s%s", what, path ? ": " : "", path ? path : "");
    return BOTW_HLE_FS_ERROR;
}

static int fs_seek(FILE* f, int64_t off) {
    if (off < 0) return -1;
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseek(f, (long)off, SEEK_SET);
#endif
}

static int64_t fs_tell(FILE* f) {
#if defined(_WIN32)
    return (int64_t)_ftelli64(f);
#else
    return (int64_t)ftell(f);
#endif
}

static int fs_truncate(FILE* f, int64_t size) {
    if (size < 0) return -1;
    if (fflush(f) != 0) return -1;
#if defined(_WIN32)
    return _chsize_s(_fileno(f), (long long)size);
#else
    return ftruncate(fileno(f), (off_t)size);
#endif
}

/* --- state -------------------------------------------------------------- */

static void fs_close_all(botw_hle_fs_state* st) {
    size_t i;
    for (i = 0; i < BOTW_HLE_FS_MAX_FILES; i++) {
        if (st->files[i].used && st->files[i].file) fclose((FILE*)st->files[i].file);
        memset(&st->files[i], 0, sizeof st->files[i]);
    }
    for (i = 0; i < BOTW_HLE_FS_MAX_DIRS; i++) {
        if (st->dirs[i].used && st->dirs[i].dir) fs_dir_close((fs_dir_iter*)st->dirs[i].dir);
        memset(&st->dirs[i], 0, sizeof st->dirs[i]);
    }
    memset(st->mounts, 0, sizeof st->mounts);
}

void botw_hle_fs_reset(botw_hle_fs_state* state) {
    if (!state) state = &g_default_state;
    fs_close_all(state);
    memset(state->root, 0, sizeof state->root);
    state->configured = 0;
    state->calls = 0;
    state->errors = 0;
    state->last_error[0] = '\0';
}

botw_hle_fs_state* botw_hle_fs_default(void) {
    return &g_default_state;
}

/* A filesystem root is not a data directory.  "" is rejected too: an empty
 * root would silently mean the process working directory. */
static int fs_root_is_safe(const char* root) {
    size_t n = strlen(root);
    if (n == 0 || n >= BOTW_HLE_FS_PATH_MAX) return 0;
    if (n == 1 && (root[0] == '/' || root[0] == '\\')) return 0;
#if defined(_WIN32)
    if (n == 2 && root[1] == ':') return 0;
    if (n == 3 && root[1] == ':' && (root[2] == '\\' || root[2] == '/')) return 0;
#endif
    return 1;
}

int botw_hle_fs_init(botw_hle_fs_state* state, const char* host_root) {
    if (!state) state = &g_default_state;
    /* init() must accept a struct that was never initialized, so it cannot
     * walk the handle tables first: fclose() on garbage would corrupt the
     * host heap.  Callers that are re-pointing a live state call reset()
     * first, which does close the handles. */
    memset(state, 0, sizeof *state);
    if (!host_root || !fs_root_is_safe(host_root)) {
        fs_error(state, "host root must be a directory, not a filesystem root");
        return -1;
    }
    snprintf(state->root, sizeof state->root, "%s", host_root);
    state->configured = 1;
    return 0;
}

/* --- paths -------------------------------------------------------------- */

/* Collapses separators, drops "." components, and rejects "..", backslashes
 * and an empty result.  `dst` never starts or ends with a separator. */
static int normalize_rel(const char* src, char* dst, size_t cap) {
    size_t o = 0;
    int any = 0;
    const char* p = src;
    while (*p) {
        const char* start;
        size_t len;
        if (*p == '\\') return -1;
        while (*p == '/') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != '/') p++;
        len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') return -1;
        if (any) {
            if (o + 1 >= cap) return -1;
            dst[o++] = '/';
        }
        if (o + len >= cap) return -1;
        memcpy(dst + o, start, len);
        o += len;
        any = 1;
    }
    if (!any) return -1;
    dst[o] = '\0';
    return 0;
}

static botw_hle_fs_mount* fs_find_mount(botw_hle_fs_state* st, const char* name) {
    size_t i;
    for (i = 0; i < BOTW_HLE_FS_MAX_MOUNTS; i++) {
        if (st->mounts[i].used && strcmp(st->mounts[i].name, name) == 0) return &st->mounts[i];
    }
    return NULL;
}

/* Guest path -> host path.  Accepts `<mount>:/rest`, `/<mount>/rest` and a
 * bare relative path, which lands in the first mount. */
static int fs_resolve(botw_hle_fs_state* st, const char* guest_path,
                      char* out, size_t cap) {
    const botw_hle_fs_mount* m = NULL;
    const char* rest = NULL;
    char rel[BOTW_HLE_FS_PATH_MAX];
    size_t i;

    if (!st->configured) return -1;
    if (!guest_path || !guest_path[0]) return -1;

    for (i = 0; i < BOTW_HLE_FS_MAX_MOUNTS && !m; i++) {
        size_t n;
        if (!st->mounts[i].used) continue;
        n = strlen(st->mounts[i].name);
        if (strncmp(guest_path, st->mounts[i].name, n) == 0 &&
            guest_path[n] == ':' && guest_path[n + 1] == '/') {
            m = &st->mounts[i];
            rest = guest_path + n + 2;
        } else if (guest_path[0] == '/' &&
                   strncmp(guest_path + 1, st->mounts[i].name, n) == 0 &&
                   (guest_path[1 + n] == '/' || guest_path[1 + n] == '\0')) {
            m = &st->mounts[i];
            rest = guest_path + 1 + n;
        }
    }
    if (!m) {
        for (i = 0; i < BOTW_HLE_FS_MAX_MOUNTS; i++) {
            if (st->mounts[i].used) { m = &st->mounts[i]; break; }
        }
        if (!m) return -1;
        rest = guest_path;
    }
    if (normalize_rel(rest, rel, sizeof rel) != 0) return -1;
    if (strlen(m->host) + 1 + strlen(rel) + 1 > cap) return -1;
    snprintf(out, cap, "%s/%s", m->host, rel);
    return 0;
}

static int fs_mount_add(botw_hle_fs_state* st, const char* name) {
    botw_hle_fs_mount* m;
    fs_stat_t sb;
    size_t i, n;

    if (!name || !name[0]) return -1;
    n = strlen(name);
    if (n >= sizeof st->mounts[0].name) return -1;
    for (i = 0; i < n; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return -1;
    }
    m = fs_find_mount(st, name);
    if (!m) {
        for (i = 0; i < BOTW_HLE_FS_MAX_MOUNTS; i++) {
            if (!st->mounts[i].used) { m = &st->mounts[i]; break; }
        }
    }
    if (!m) return -1;
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    if (strlen(st->root) + 1 + n + 1 > sizeof m->host) return -1;
    snprintf(m->host, sizeof m->host, "%s/%s", st->root, name);
    m->used = 1;
    if (fs_stat(m->host, &sb) != 0 || !fs_isdir(sb.st_mode)) {
        if (fs_mkdir(m->host) != 0) {
            fs_error(st, "cannot create mount directory %s", m->host);
            m->used = 0;
            return -1;
        }
    }
    return 0;
}

/* --- handles ------------------------------------------------------------ */

static botw_hle_fs_file* fs_file_slot(botw_hle_fs_state* st, uint32_t handle) {
    uint32_t i;
    if (handle <= FS_FILE_HANDLE_BASE) return NULL;
    i = handle - FS_FILE_HANDLE_BASE - 1u;
    if (i >= BOTW_HLE_FS_MAX_FILES) return NULL;
    if (!st->files[i].used) return NULL;
    return &st->files[i];
}

static botw_hle_fs_dir* fs_dir_slot(botw_hle_fs_state* st, uint32_t handle) {
    uint32_t i;
    if (handle <= FS_DIR_HANDLE_BASE) return NULL;
    i = handle - FS_DIR_HANDLE_BASE - 1u;
    if (i >= BOTW_HLE_FS_MAX_DIRS) return NULL;
    if (!st->dirs[i].used) return NULL;
    return &st->dirs[i];
}

static int fs_alloc_file(botw_hle_fs_state* st, FILE* f, const char* path, uint32_t* out) {
    size_t i;
    for (i = 0; i < BOTW_HLE_FS_MAX_FILES; i++) {
        if (!st->files[i].used) {
            st->files[i].used = 1;
            st->files[i].file = f;
            snprintf(st->files[i].path, sizeof st->files[i].path, "%s", path);
            *out = FS_FILE_HANDLE_BASE + (uint32_t)i + 1u;
            return 0;
        }
    }
    return -1;
}

/* --- nn::fs ------------------------------------------------------------- */

static uint32_t fs_MountRom(const char* name, void* cache, size_t cache_size) {
    botw_hle_fs_state* st = g_state;
    (void)cache;
    (void)cache_size;
    st->calls++;
    if (!st->configured) return fs_fail(st, "MountRom before init", name);
    if (fs_mount_add(st, name) != 0) return fs_fail(st, "cannot mount", name);
    return 0;
}

static uint32_t fs_MountAddOnContent(const char* name, int index, void* cache, size_t cache_size) {
    botw_hle_fs_state* st = g_state;
    (void)index; /* the host directory is the mount name; index is not encoded */
    (void)cache;
    (void)cache_size;
    st->calls++;
    if (!st->configured) return fs_fail(st, "MountAddOnContent before init", name);
    if (fs_mount_add(st, name) != 0) return fs_fail(st, "cannot mount add-on", name);
    return 0;
}

static uint32_t fs_MountSaveData(const char* name, const void* uid) {
    botw_hle_fs_state* st = g_state;
    (void)uid; /* nn::account::Uid is never dereferenced: there is one user */
    st->calls++;
    if (!st->configured) return fs_fail(st, "MountSaveData before init", name);
    if (fs_mount_add(st, name) != 0) return fs_fail(st, "cannot mount save data", name);
    return 0;
}

static uint32_t fs_CommitSaveData(const char* name) {
    botw_hle_fs_state* st = g_state;
    st->calls++;
    /* Writes go straight to the host file; there is no journal to commit. */
    if (!st->configured) return fs_fail(st, "CommitSaveData before init", name);
    return 0;
}

static uint32_t fs_Unmount(const char* name) {
    botw_hle_fs_state* st = g_state;
    botw_hle_fs_mount* m;
    st->calls++;
    m = name ? fs_find_mount(st, name) : NULL;
    if (!m) return fs_fail(st, "unmount of an unknown mount", name);
    memset(m, 0, sizeof *m);
    return 0;
}

static uint32_t fs_QueryMountRomCacheSize(size_t* out_size) {
    botw_hle_fs_state* st = g_state;
    st->calls++;
    /* The guest allocates this and hands it back to MountRom, which ignores
     * it: the host never copies the romfs image into guest memory. */
    if (out_size) *out_size = 0x400000u;
    return 0;
}

static uint32_t fs_QueryMountAddOnContentCacheSize(size_t* out_size, int index) {
    botw_hle_fs_state* st = g_state;
    (void)index;
    st->calls++;
    if (out_size) *out_size = 0x100000u;
    return 0;
}

static uint32_t fs_OpenFile(uint32_t* out_handle, const char* path, int mode) {
    botw_hle_fs_state* st = g_state;
    const char* fmode;
    char host[BOTW_HLE_FS_PATH_MAX];
    FILE* f;

    st->calls++;
    if (!out_handle) return fs_fail(st, "OpenFile without an out handle", path);
    switch (mode) {
        case 1: fmode = "rb"; break;                 /* Read */
        case 2: fmode = "wb"; break;                 /* Write */
        case 3: fmode = "ab+"; break;                /* Append */
        default: return fs_fail(st, "OpenFile with an unknown mode", path);
    }
    if (fs_resolve(st, path, host, sizeof host) != 0) return fs_fail(st, "bad path", path);
    f = fopen(host, fmode);
    if (!f) return fs_fail(st, "cannot open", path);
    if (fs_alloc_file(st, f, host, out_handle) != 0) {
        fclose(f);
        return fs_fail(st, "out of file handles", path);
    }
    return 0;
}

static uint32_t fs_CloseFile(uint32_t handle) {
    botw_hle_fs_state* st = g_state;
    botw_hle_fs_file* slot = fs_file_slot(st, handle);
    st->calls++;
    if (!slot) return fs_fail(st, "CloseFile on an invalid handle", NULL);
    if (slot->file) fclose((FILE*)slot->file);
    memset(slot, 0, sizeof *slot);
    return 0;
}

static uint32_t fs_ReadFile(uint64_t* out_read, uint32_t handle, int64_t offset,
                            void* buffer, size_t size) {
    botw_hle_fs_state* st = g_state;
    botw_hle_fs_file* slot = fs_file_slot(st, handle);
    size_t got;

    st->calls++;
    if (!slot) return fs_fail(st, "ReadFile on an invalid handle", NULL);
    if (fs_seek((FILE*)slot->file, offset) != 0) return fs_fail(st, "seek failed", slot->path);
    got = (buffer && size) ? fread(buffer, 1, size, (FILE*)slot->file) : 0;
    if (out_read) *out_read = (uint64_t)got;
    return 0;
}

static uint32_t fs_WriteFile(uint32_t handle, int64_t offset, const void* buffer,
                             size_t size, const void* option) {
    botw_hle_fs_state* st = g_state;
    botw_hle_fs_file* slot = fs_file_slot(st, handle);
    size_t put;

    (void)option; /* nn::fs::WriteOption is deliberately never dereferenced */
    st->calls++;
    if (!slot) return fs_fail(st, "WriteFile on an invalid handle", NULL);
    if (fs_seek((FILE*)slot->file, offset) != 0) return fs_fail(st, "seek failed", slot->path);
    put = (buffer && size) ? fwrite(buffer, 1, size, (FILE*)slot->file) : 0;
    if (fflush((FILE*)slot->file) != 0) return fs_fail(st, "flush failed", slot->path);
    if (put != size) return fs_fail(st, "short write", slot->path);
    return 0;
}

static uint32_t fs_GetFileSize(int64_t* out_size, uint32_t handle) {
    botw_hle_fs_state* st = g_state;
    botw_hle_fs_file* slot = fs_file_slot(st, handle);
    FILE* f;
    int64_t here;

    st->calls++;
    if (!slot) return fs_fail(st, "GetFileSize on an invalid handle", NULL);
    f = (FILE*)slot->file;
    here = fs_tell(f);
    if (fs_seek(f, 0)) return fs_fail(st, "seek failed", slot->path);
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) return fs_fail(st, "seek failed", slot->path);
    if (out_size) *out_size = (int64_t)_ftelli64(f);
#else
    if (fseek(f, 0, SEEK_END) != 0) return fs_fail(st, "seek failed", slot->path);
    if (out_size) *out_size = (int64_t)ftell(f);
#endif
    fs_seek(f, here);
    return 0;
}

static uint32_t fs_SetFileSize(uint32_t handle, int64_t size) {
    botw_hle_fs_state* st = g_state;
    botw_hle_fs_file* slot = fs_file_slot(st, handle);
    st->calls++;
    if (!slot) return fs_fail(st, "SetFileSize on an invalid handle", NULL);
    if (size < 0) return fs_fail(st, "negative file size", slot->path);
    if (fs_truncate((FILE*)slot->file, size) != 0) return fs_fail(st, "truncate failed", slot->path);
    return 0;
}

static uint32_t fs_FlushFile(uint32_t handle) {
    botw_hle_fs_state* st = g_state;
    botw_hle_fs_file* slot = fs_file_slot(st, handle);
    st->calls++;
    if (!slot) return fs_fail(st, "FlushFile on an invalid handle", NULL);
    if (fflush((FILE*)slot->file) != 0) return fs_fail(st, "flush failed", slot->path);
    return 0;
}

static uint32_t fs_CreateFile(const char* path, int64_t size) {
    botw_hle_fs_state* st = g_state;
    char host[BOTW_HLE_FS_PATH_MAX];
    FILE* f;

    st->calls++;
    if (fs_resolve(st, path, host, sizeof host) != 0) return fs_fail(st, "bad path", path);
    if (size < 0) return fs_fail(st, "negative file size", path);
    f = fopen(host, "wb");
    if (!f) return fs_fail(st, "cannot create", path);
    if (size > 0 && fs_truncate(f, size) != 0) {
        fclose(f);
        return fs_fail(st, "cannot size new file", path);
    }
    fclose(f);
    return 0;
}

static uint32_t fs_DeleteFile(const char* path) {
    botw_hle_fs_state* st = g_state;
    char host[BOTW_HLE_FS_PATH_MAX];
    st->calls++;
    if (fs_resolve(st, path, host, sizeof host) != 0) return fs_fail(st, "bad path", path);
    if (remove(host) != 0) return fs_fail(st, "cannot delete", path);
    return 0;
}

static uint32_t fs_CreateDirectory(const char* path) {
    botw_hle_fs_state* st = g_state;
    char host[BOTW_HLE_FS_PATH_MAX];
    fs_stat_t sb;

    st->calls++;
    if (fs_resolve(st, path, host, sizeof host) != 0) return fs_fail(st, "bad path", path);
    if (fs_stat(host, &sb) == 0) {
        if (fs_isdir(sb.st_mode)) return 0;
        return fs_fail(st, "a file is in the way", path);
    }
    if (fs_mkdir(host) != 0) return fs_fail(st, "cannot create directory", path);
    return 0;
}

static uint32_t fs_DeleteDirectoryRecursively(const char* path) {
    botw_hle_fs_state* st = g_state;
    st->calls++;
    /* Deliberately not implemented: see the header comment. */
    return fs_fail(st, "DeleteDirectoryRecursively is not implemented", path);
}

static uint32_t fs_GetEntryType(uint32_t* out_type, const char* path) {
    botw_hle_fs_state* st = g_state;
    char host[BOTW_HLE_FS_PATH_MAX];
    fs_stat_t sb;

    st->calls++;
    if (!out_type) return fs_fail(st, "GetEntryType without an out value", path);
    if (fs_resolve(st, path, host, sizeof host) != 0) return fs_fail(st, "bad path", path);
    if (fs_stat(host, &sb) != 0) return fs_fail(st, "cannot stat", path);
    *out_type = fs_isdir(sb.st_mode) ? 0u : 1u;
    return 0;
}

static uint32_t fs_OpenDirectory(uint32_t* out_handle, const char* path, int mode) {
    botw_hle_fs_state* st = g_state;
    char host[BOTW_HLE_FS_PATH_MAX];
    fs_dir_iter* it;
    size_t i;

    st->calls++;
    if (!out_handle) return fs_fail(st, "OpenDirectory without an out handle", path);
    if (mode < 1 || mode > 3) return fs_fail(st, "OpenDirectory with an unknown mode", path);
    if (fs_resolve(st, path, host, sizeof host) != 0) return fs_fail(st, "bad path", path);
    it = fs_dir_open(host);
    if (!it) return fs_fail(st, "cannot open directory", path);
    for (i = 0; i < BOTW_HLE_FS_MAX_DIRS; i++) {
        if (!st->dirs[i].used) {
            st->dirs[i].used = 1;
            st->dirs[i].dir = it;
            st->dirs[i].mode = mode;
            snprintf(st->dirs[i].path, sizeof st->dirs[i].path, "%s", host);
            *out_handle = FS_DIR_HANDLE_BASE + (uint32_t)i + 1u;
            return 0;
        }
    }
    fs_dir_close(it);
    return fs_fail(st, "out of directory handles", path);
}

static uint32_t fs_ReadDirectory(int64_t* out_count, botw_hle_fs_dir_entry* entries,
                                 uint32_t handle, int64_t max_entries) {
    botw_hle_fs_state* st = g_state;
    botw_hle_fs_dir* slot = fs_dir_slot(st, handle);
    int64_t n = 0;

    st->calls++;
    if (!slot) return fs_fail(st, "ReadDirectory on an invalid handle", NULL);
    if (max_entries < 0) return fs_fail(st, "negative entry count", NULL);
    if (max_entries > 0 && !entries) return fs_fail(st, "ReadDirectory without entries", NULL);

    while (n < max_entries) {
        char name[0x300];
        int is_dir = 0;
        int64_t size = 0;
        size_t name_len;

        if (!fs_dir_next((fs_dir_iter*)slot->dir, name, sizeof name, &is_dir, &size)) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (slot->mode == 1 && !is_dir) continue;
        if (slot->mode == 2 && is_dir) continue;

        memset(&entries[n], 0, sizeof entries[n]);
        name_len = strlen(name);
        if (name_len > 0x2FF) name_len = 0x2FF;
        memcpy(entries[n].name, name, name_len);
        entries[n].type = (uint8_t)(is_dir ? 0 : 1);
        entries[n].size = is_dir ? 0 : size;
        n++;
    }
    if (out_count) *out_count = n;
    return 0;
}

static uint32_t fs_CloseDirectory(uint32_t handle) {
    botw_hle_fs_state* st = g_state;
    botw_hle_fs_dir* slot = fs_dir_slot(st, handle);
    st->calls++;
    if (!slot) return fs_fail(st, "CloseDirectory on an invalid handle", NULL);
    if (slot->dir) fs_dir_close((fs_dir_iter*)slot->dir);
    memset(slot, 0, sizeof *slot);
    return 0;
}

/* --- registration ------------------------------------------------------- */

typedef struct {
    const char* name;
    void* fn;
} fs_entry;

/* Mangled names exactly as main.patched.bin asks for them.  The two ReadFile
 * overloads differ only by the trailing ReadOption reference, so both are
 * listed. */
static const fs_entry g_fs_entries[BOTW_HLE_FS_COUNT] = {
    { "_ZN2nn2fs10CreateFileEPKcl", (void*)(uintptr_t)&fs_CreateFile },
    { "_ZN2nn2fs10DeleteFileEPKc", (void*)(uintptr_t)&fs_DeleteFile },
    { "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", (void*)(uintptr_t)&fs_GetFileSize },
    { "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", (void*)(uintptr_t)&fs_SetFileSize },
    { "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", (void*)(uintptr_t)&fs_GetEntryType },
    { "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", (void*)(uintptr_t)&fs_MountSaveData },
    { "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", (void*)(uintptr_t)&fs_OpenDirectory },
    { "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", (void*)(uintptr_t)&fs_ReadDirectory },
    { "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", (void*)(uintptr_t)&fs_CloseDirectory },
    { "_ZN2nn2fs14CommitSaveDataEPKc", (void*)(uintptr_t)&fs_CommitSaveData },
    { "_ZN2nn2fs15CreateDirectoryEPKc", (void*)(uintptr_t)&fs_CreateDirectory },
    { "_ZN2nn2fs17MountAddOnContentEPKciPvm", (void*)(uintptr_t)&fs_MountAddOnContent },
    { "_ZN2nn2fs22QueryMountRomCacheSizeEPm", (void*)(uintptr_t)&fs_QueryMountRomCacheSize },
    { "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", (void*)(uintptr_t)&fs_DeleteDirectoryRecursively },
    { "_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi", (void*)(uintptr_t)&fs_QueryMountAddOnContentCacheSize },
    { "_ZN2nn2fs7UnmountEPKc", (void*)(uintptr_t)&fs_Unmount },
    { "_ZN2nn2fs8MountRomEPKcPvm", (void*)(uintptr_t)&fs_MountRom },
    { "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", (void*)(uintptr_t)&fs_OpenFile },
    { "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", (void*)(uintptr_t)&fs_ReadFile },
    { "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", (void*)(uintptr_t)&fs_ReadFile },
    { "_ZN2nn2fs9CloseFileENS0_10FileHandleE", (void*)(uintptr_t)&fs_CloseFile },
    { "_ZN2nn2fs9FlushFileENS0_10FileHandleE", (void*)(uintptr_t)&fs_FlushFile },
    { "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", (void*)(uintptr_t)&fs_WriteFile },
};

int botw_hle_fs_register(botw_hle_table* hle, botw_hle_fs_state* state) {
    size_t i;
    if (!hle) return -1;
    if (!state) state = &g_default_state;
    g_state = state;
    for (i = 0; i < BOTW_HLE_FS_COUNT; i++) {
        if (botw_hle_register(hle, g_fs_entries[i].name, g_fs_entries[i].fn) != 0) return -1;
    }
    return 0;
}

/* Offline MSL bundles: the file form of the blob table.
 *
 * The generated table in nvn_shader_blobs.gen.c is a C array, which is fine
 * for a handful of shaders and wrong for the ~19,000 a title ships: the text
 * alone is tens of megabytes and every regeneration is a recompile.  A bundle
 * is the same data as files, read at startup:
 *
 *     <dir>/manifest.tsv      one shader per line
 *     <dir>/<file>.msl        the translated MSL, entry points renamed
 *     <dir>/<file>.c1         optional embedded constant segment
 *
 * manifest.tsv is tab separated; '#' starts a comment line:
 *
 *     key <TAB> stage <TAB> msl-file <TAB> c1-file <TAB> args
 *
 *   key      16 hex digits, the guest shader key (case insensitive)
 *   stage    vs | ps (fs is accepted as the spirv-cross spelling)
 *   msl      file name inside the bundle; the text is read at load time
 *   c1       '-' for none, otherwise a file.  An empty file is an explicit
 *            empty c1, which is not the same thing as "no c1".
 *   args     '-' for none, otherwise comma separated
 *            id:slot:packed:kind[:offset], where kind is the numeric
 *            nvngfx_arg_kind (1..12), not the C enum name
 *
 * Everything is validated by nvngfx_shader_set_blobs() before it goes live, so
 * a bad manifest fails the load and leaves the previously installed table
 * alone.  A loaded bundle is installed *after* the generated table, so a build
 * can ship both.
 *
 * Nothing here knows about Metal: this compiles on Windows and Linux and is
 * exercised by tests/nvn_shader_bundle_test.cpp.  On Apple platforms the
 * directory is <app>/shaders (see nvn_metal.mm); NVNGFX_SHADER_DIR overrides
 * it, which is how a translated set is tried without rebuilding the app.
 */
/* fopen/sscanf are the portable spelling; the MSVC CRT only calls them
 * deprecated, and this file has to build with /W4 /WX on Windows too. */
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "nvn_shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A single shader this large is already suspicious; the cap keeps a damaged
 * manifest from asking for an unbounded allocation. */
#define BUNDLE_MAX_MSL (8u * 1024u * 1024u)
/* The whole title is ~19,595 stages (Bootup_Graphics alone is 5,102), so
 * 4,096 would reject every real bundle.  The cap is only a damaged-manifest
 * guard; it must stay above the title's stage count with room to spare. */
#define BUNDLE_MAX_ENTRIES 65536u
#define BUNDLE_LINE_MAX 4096u
#define KEY_TEXT_MAX 32u

#define WHY(...)                                  \
    do {                                          \
        if (why && why_size)                      \
            snprintf(why, why_size, __VA_ARGS__);  \
    } while (0)

typedef struct Owners {
    void** items;
    size_t count;
    size_t cap;
} Owners;

typedef struct Bundle {
    nvngfx_shader_blob* blobs; /* the installed table (base + ours) */
    size_t installed;          /* entries in blobs */
    size_t own_count;          /* entries this bundle contributed */
    Owners owned;
} Bundle;

static Bundle* g_bundle;
/* The table installed before the first bundle (the generated one).  A load
 * installs base + bundle, so both can be used at once. */
static const nvngfx_shader_blob* g_base;
static size_t g_base_count;

static void* own(Owners* o, void* p) {
    if (!p) return NULL;
    if (o->count == o->cap) {
        const size_t cap = o->cap ? o->cap * 2 : 16;
        void** grown = (void**)realloc(o->items, cap * sizeof *grown);
        if (!grown) {
            free(p);
            return NULL;
        }
        o->items = grown;
        o->cap = cap;
    }
    o->items[o->count++] = p;
    return p;
}

static void owners_free(Owners* o) {
    size_t i;
    for (i = 0; i < o->count; ++i) free(o->items[i]);
    free(o->items);
    o->items = NULL;
    o->count = 0;
    o->cap = 0;
}

static char* join_path(const char* dir, const char* name) {
    const size_t dl = strlen(dir), nl = strlen(name);
    const int slash = dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\';
    char* out = (char*)malloc(dl + (size_t)slash + nl + 1);
    if (!out) return NULL;
    memcpy(out, dir, dl);
    if (slash) out[dl] = '/';
    memcpy(out + dl + (size_t)slash, name, nl);
    out[dl + (size_t)slash + nl] = '\0';
    return out;
}

/* Reads a whole file, NUL terminated.  A zero length file is allowed (c1 uses
 * that to mean "declared, empty"); the caller decides whether that is legal. */
static char* read_file(const char* dir, const char* name, size_t limit,
                       size_t* out_len, char* why, size_t why_size) {
    char* path = join_path(dir, name);
    FILE* f;
    long size;
    char* data;
    if (!path) {
        WHY("out of memory reading %s", name);
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f) {
        WHY("cannot open %s (named by manifest.tsv)", path);
        free(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        WHY("cannot size %s", path);
        fclose(f);
        free(path);
        return NULL;
    }
    if ((unsigned long)size > (unsigned long)limit) {
        WHY("%s is larger than the bundle limit", path);
        fclose(f);
        free(path);
        return NULL;
    }
    data = (char*)malloc((size_t)size + 1);
    if (!data) {
        WHY("out of memory reading %s", path);
        fclose(f);
        free(path);
        return NULL;
    }
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        WHY("short read on %s", path);
        free(data);
        fclose(f);
        free(path);
        return NULL;
    }
    data[size] = '\0';
    fclose(f);
    free(path);
    if (out_len) *out_len = (size_t)size;
    return data;
}

static int parse_stage(const char* s) {
    if (!strcmp(s, "vs") || !strcmp(s, "vertex")) return NVNGFX_STAGE_VERTEX;
    if (!strcmp(s, "ps") || !strcmp(s, "fs") || !strcmp(s, "fragment"))
        return NVNGFX_STAGE_FRAGMENT;
    return -1;
}

/* id:slot:packed:kind[:offset], comma separated.  The layout is validated by
 * nvngfx_shader_set_blobs(); this only refuses what cannot even be parsed. */
static nvngfx_shader_arg* parse_args(const char* text, unsigned* out_count,
                                     Owners* o, char* why, size_t why_size) {
    nvngfx_shader_arg* args;
    unsigned n = 1, i = 0;
    const char* p;
    if (!strcmp(text, "-") || text[0] == '\0') {
        *out_count = 0;
        return NULL;
    }
    for (p = text; *p; ++p)
        if (*p == ',') ++n;
    if (n > NVNGFX_MAX_ARGS) {
        WHY("manifest lists more than %u arguments", (unsigned)NVNGFX_MAX_ARGS);
        return NULL;
    }
    args = (nvngfx_shader_arg*)calloc(n, sizeof *args);
    if (!args) {
        WHY("out of memory for the argument layout");
        return NULL;
    }
    p = text;
    while (i < n) {
        const char* end = strchr(p, ',');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        char item[64];
        int id = 0, slot = 0, packed = 0, kind = 0, off = 0;
        if (len == 0 || len >= sizeof item) {
            WHY("malformed argument entry '%.*s'", (int)len, p);
            free(args);
            return NULL;
        }
        memcpy(item, p, len);
        item[len] = '\0';
        {
            const int parsed = sscanf(item, "%d:%d:%d:%d:%d", &id, &slot,
                                      &packed, &kind, &off);
            if (parsed < 4 || id < 0 || id > 255 || slot < -128 || slot > 127 ||
                packed < 0 || packed > 255 || kind < 0 || off < 0 ||
                off > 65535) {
                WHY("argument entry '%s' is not id:slot:packed:kind[:offset]",
                    item);
                free(args);
                return NULL;
            }
        }
        args[i].id = (unsigned char)id;
        args[i].slot = (signed char)slot;
        args[i].packed = (unsigned char)packed;
        args[i].kind = (unsigned char)kind;
        args[i].offset = (unsigned short)off;
        ++i;
        if (!end) break;
        p = end + 1;
    }
    if (!own(o, args)) {
        WHY("out of memory for the argument layout");
        return NULL;
    }
    *out_count = n;
    return args;
}

/* Splits one manifest line in place by tabs.  Returns the field count, or -1
 * when the line has more than `cap` fields. */
static int split_tabs(char* line, char** fields, int cap) {
    int n = 0;
    char* p = line;
    for (;;) {
        char* tab = strchr(p, '\t');
        if (n == cap) return -1;
        fields[n++] = p;
        if (!tab) return n;
        *tab = '\0';
        p = tab + 1;
    }
}

static void bundle_free(Bundle* b) {
    if (!b) return;
    owners_free(&b->owned);
    free(b->blobs);
    free(b);
}

int nvngfx_shader_load_bundle(const char* dir, char* why, size_t why_size) {
    Bundle* fresh;
    FILE* mf = NULL;
    char* mpath = NULL;
    char line[BUNDLE_LINE_MAX];
    size_t cap = 0, used = 0;
    unsigned lineno = 0;

    if (why && why_size) why[0] = '\0';
    if (!dir || dir[0] == '\0') {
        WHY("empty bundle directory");
        return -1;
    }
    fresh = (Bundle*)calloc(1, sizeof *fresh);
    if (!fresh) {
        WHY("out of memory");
        return -1;
    }
    mpath = join_path(dir, "manifest.tsv");
    mf = mpath ? fopen(mpath, "rb") : NULL;
    if (!mf) {
        WHY("no readable manifest.tsv in %s", dir);
        free(mpath);
        bundle_free(fresh);
        return -1;
    }

    while (fgets(line, (int)sizeof line, mf)) {
        char* fields[6];
        int nf;
        int stage;
        char* msl;
        size_t msl_len = 0;
        const unsigned char* c1 = NULL;
        unsigned c1_size = 0;
        const char* c1_name;
        const char* args_text;
        nvngfx_shader_blob* b;

        ++lineno;
        {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = '\0';
            if (len == 0 || line[0] == '#') continue;
            if (len + 1 == sizeof line) {
                WHY("manifest line %u is too long", lineno);
                goto fail;
            }
        }
        nf = split_tabs(line, fields, 6);
        if (nf < 3) {
            WHY("manifest line %u needs key, stage and file", lineno);
            goto fail;
        }
        stage = parse_stage(fields[1]);
        if (stage < 0) {
            WHY("manifest line %u: unknown stage '%s'", lineno, fields[1]);
            goto fail;
        }
        c1_name = nf > 3 ? fields[3] : "-";
        args_text = nf > 4 ? fields[4] : "-";

        msl = read_file(dir, fields[2], BUNDLE_MAX_MSL, &msl_len, why, why_size);
        if (!msl) goto fail;
        if (msl_len == 0) {
            WHY("manifest line %u: %s is empty", lineno, fields[2]);
            free(msl);
            goto fail;
        }
        if (!own(&fresh->owned, msl)) {
            WHY("out of memory");
            goto fail;
        }
        if (strcmp(c1_name, "-") != 0 && c1_name[0] != '\0') {
            size_t got = 0;
            char* bytes = read_file(dir, c1_name, NVNGFX_MAX_C1_BYTES, &got,
                                    why, why_size);
            if (!bytes) goto fail;
            if (got == 0) {
                /* Explicit empty c1: the pointer has to stay non-NULL. */
                char* grown = (char*)realloc(bytes, 1);
                if (!grown) {
                    free(bytes);
                    WHY("out of memory");
                    goto fail;
                }
                bytes = grown;
            }
            if (!own(&fresh->owned, bytes)) {
                WHY("out of memory");
                goto fail;
            }
            c1 = (const unsigned char*)bytes;
            c1_size = (unsigned)got;
        }
        if (used == cap) {
            const size_t next = cap ? cap * 2 : 8;
            nvngfx_shader_blob* grown;
            if (next > BUNDLE_MAX_ENTRIES) {
                WHY("bundle has more than %u shaders",
                    (unsigned)BUNDLE_MAX_ENTRIES);
                goto fail;
            }
            grown = (nvngfx_shader_blob*)realloc(fresh->blobs,
                                                 next * sizeof *grown);
            if (!grown) {
                WHY("out of memory");
                goto fail;
            }
            fresh->blobs = grown;
            cap = next;
        }
        {
            /* fields[] points into `line`, which the next fgets reuses. */
            char* key = (char*)malloc(KEY_TEXT_MAX);
            if (!key) {
                WHY("out of memory");
                goto fail;
            }
            snprintf(key, KEY_TEXT_MAX, "%s", fields[0]);
            if (!own(&fresh->owned, key)) {
                WHY("out of memory");
                goto fail;
            }
            b = &fresh->blobs[used];
            memset(b, 0, sizeof *b);
            b->key = key;
        }
        b->stage = stage;
        b->msl = msl;
        b->args = parse_args(args_text, &b->num_args, &fresh->owned, why, why_size);
        /* NULL is legal only for "no layout": parse_args sets no count when it
         * fails, so the text decides whether that NULL is an error. */
        if (!b->args && strcmp(args_text, "-") != 0 && args_text[0] != '\0')
            goto fail;
        b->c1 = c1;
        b->c1_size = c1_size;
        ++used;
    }
    fclose(mf);
    free(mpath);

    if (used == 0) {
        WHY("manifest.tsv in %s lists no shader", dir);
        bundle_free(fresh);
        return -1;
    }
    fresh->own_count = used;

    if (!g_bundle) g_base = nvngfx_shader_blob_table(&g_base_count);
    {
        nvngfx_shader_blob* all =
            (nvngfx_shader_blob*)calloc(g_base_count + used, sizeof *all);
        size_t i;
        int rc;
        if (!all) {
            WHY("out of memory");
            bundle_free(fresh);
            return -1;
        }
        for (i = 0; i < g_base_count; ++i) all[i] = g_base[i];
        for (i = 0; i < used; ++i) all[g_base_count + i] = fresh->blobs[i];
        rc = nvngfx_shader_set_blobs(all, g_base_count + used);
        if (rc != 0) {
            const long idx = -rc - 1;
            if (idx >= 0 && (size_t)idx < used)
                WHY("manifest entry %ld (key %.16s) is not a valid blob", idx + 1,
                    fresh->blobs[idx].key);
            else if (idx >= 0)
                WHY("entry %ld collides with the generated table", idx + 1);
            else
                WHY("shader table rejected (%d)", rc);
            free(all);
            if (!g_bundle) {
                g_base = NULL;
                g_base_count = 0;
            }
            bundle_free(fresh);
            return -1;
        }
        free(fresh->blobs);
        fresh->blobs = all;
        fresh->installed = g_base_count + used;
    }

    bundle_free(g_bundle);
    g_bundle = fresh;
    return 0;

fail:
    if (mf) fclose(mf);
    free(mpath);
    bundle_free(fresh);
    return -1;
}

size_t nvngfx_shader_bundle_count(void) {
    return g_bundle ? g_bundle->own_count : 0;
}

void nvngfx_shader_unload_bundle(void) {
    if (!g_bundle) return;
    bundle_free(g_bundle);
    g_bundle = NULL;
    nvngfx_shader_set_blobs(g_base, g_base_count);
    g_base = NULL;
    g_base_count = 0;
}
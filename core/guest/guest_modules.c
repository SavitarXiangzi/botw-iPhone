#include "guest_modules.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_LOCAL  0u
#define SHN_ABS    0xfff1u

typedef struct {
    const char* name;  /* aliases the module image; the image outlives the index */
    uint64_t addr;
} sym_entry;

typedef struct {
    unsigned cap;      /* power of two */
    sym_entry* entries;
} sym_index;

static void set_error(botw_guest_modules* set, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(set->error, sizeof set->error, fmt, ap);
    va_end(ap);
}

static uint32_t name_hash(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static sym_index* index_of(const botw_guest_modules* set) {
    return (sym_index*)set->syms;
}

static int index_lookup(const botw_guest_modules* set, const char* name,
                        uint64_t* addr) {
    const sym_index* idx = index_of(set);
    uint32_t at;
    if (!idx || !idx->entries) return 0;
    at = name_hash(name) & (idx->cap - 1u);
    for (;;) {
        const sym_entry* e = &idx->entries[at];
        if (!e->name) return 0;
        if (strcmp(e->name, name) == 0) {
            *addr = e->addr;
            return 1;
        }
        at = (at + 1u) & (idx->cap - 1u);
    }
}

static int index_insert(botw_guest_modules* set, const char* name, uint64_t addr) {
    sym_index* idx = index_of(set);
    uint32_t at = name_hash(name) & (idx->cap - 1u);
    for (;;) {
        sym_entry* e = &idx->entries[at];
        if (!e->name) {
            e->name = name;
            e->addr = addr;
            set->nsyms++;
            return 0;
        }
        /* Earlier-added modules win, matching Horizon's load-order binding. */
        if (strcmp(e->name, name) == 0) return 0;
        at = (at + 1u) & (idx->cap - 1u);
    }
}

static int definition_count(const botw_guest_modules* set, unsigned* out) {
    unsigned total = 0;
    unsigned m;
    for (m = 0; m < set->count; ++m) {
        const botw_guest_image* img = &set->mods[m].image;
        unsigned n = botw_guest_image_symbol_count(img);
        unsigned i;
        for (i = 1; i < n; ++i) {
            const char* name = NULL;
            uint64_t value = 0;
            unsigned shndx = 0, bind = 0;
            if (!botw_guest_image_symbol_at(img, i, &name, &value, &shndx, &bind)) continue;
            if (shndx == 0 || !name || !name[0] || bind == STB_LOCAL) continue;
            if (total == 0x7fffffffu) return -1;
            total++;
        }
    }
    *out = total;
    return 0;
}

static int build_index(botw_guest_modules* set) {
    unsigned total = 0;
    unsigned cap = 16;
    unsigned m;
    sym_index* idx;

    if (definition_count(set, &total) != 0) {
        set_error(set, "symbol count overflows");
        return -1;
    }
    if (total > 0x20000000u) {
        set_error(set, "too many module symbols (%u)", total);
        return -1;
    }
    while (cap < total * 2u) cap <<= 1;
    idx = (sym_index*)calloc(1, sizeof *idx);
    if (!idx) {
        set_error(set, "out of memory for the symbol index");
        return -1;
    }
    idx->cap = cap;
    idx->entries = (sym_entry*)calloc(cap, sizeof *idx->entries);
    if (!idx->entries) {
        free(idx);
        set_error(set, "out of memory for %u symbol slots", cap);
        return -1;
    }
    set->syms = idx;
    set->nsyms = 0;

    for (m = 0; m < set->count; ++m) {
        const botw_guest_image* img = &set->mods[m].image;
        unsigned n = botw_guest_image_symbol_count(img);
        unsigned i;
        for (i = 1; i < n; ++i) {
            const char* name = NULL;
            uint64_t value = 0;
            unsigned shndx = 0, bind = 0;
            uint64_t addr;
            if (!botw_guest_image_symbol_at(img, i, &name, &value, &shndx, &bind)) continue;
            if (shndx == 0 || !name || !name[0] || bind == STB_LOCAL) continue;
            if (shndx == SHN_ABS) {
                addr = value;
            } else {
                if (value > (uint64_t)img->size) {
                    set_error(set, "%s: symbol %s sits past the image (%#llx)",
                              set->mods[m].name, name, (unsigned long long)value);
                    return -1;
                }
                addr = (uint64_t)(uintptr_t)img->base + value;
            }
            if (index_insert(set, name, addr) != 0) {
                set_error(set, "%s: symbol index is full", set->mods[m].name);
                return -1;
            }
        }
    }
    return 0;
}

static void* resolve_cb(void* user, const char* name) {
    botw_guest_modules* set = (botw_guest_modules*)user;
    uint64_t addr = 0;
    if (index_lookup(set, name, &addr)) {
        set->cross++;
        if (set->current) set->current->cross++;
        return (void*)(uintptr_t)addr;
    }
    if (set->hle_resolve) {
        void* p = set->hle_resolve(set->hle_user, name);
        if (p) {
            set->hle++;
            return p;
        }
    }
    return NULL;
}

int botw_guest_modules_add(botw_guest_modules* set, const char* name,
                           const void* file, size_t file_len,
                           const char* fixups_text) {
    botw_guest_module* mod;
    void* copy;
    unsigned i;

    if (!set || !name || !*name || !file) return -1;
    if (strlen(name) >= BOTW_GUEST_MODULE_NAME) {
        set_error(set, "module name is too long: %s", name);
        return -1;
    }
    if (set->linked) {
        set_error(set, "cannot add %s after linking", name);
        return -1;
    }
    if (set->count >= BOTW_GUEST_MAX_MODULES) {
        set_error(set, "too many modules (max %d)", BOTW_GUEST_MAX_MODULES);
        return -1;
    }
    for (i = 0; i < set->count; ++i) {
        if (strcmp(set->mods[i].name, name) == 0) {
            set_error(set, "duplicate module name: %s", name);
            return -1;
        }
    }

    mod = &set->mods[set->count];
    memset(mod, 0, sizeof *mod);
    snprintf(mod->name, sizeof mod->name, "%s", name);
    if (botw_guest_image_parse(&mod->image, file_len, fixups_text) != 0) {
        set_error(set, "%s: %s", name, mod->image.error);
        return -1;
    }
    copy = malloc(file_len);
    if (!copy) {
        set_error(set, "%s: out of memory (%zu bytes)", name, file_len);
        return -1;
    }
    memcpy(copy, file, file_len);
    mod->image.owns_base = 1;
    if (botw_guest_image_bind(&mod->image, copy) != 0) {
        set_error(set, "%s: %s", name, mod->image.error);
        botw_guest_image_close(&mod->image);
        return -1;
    }
    set->count++;
    set->error[0] = '\0';
    return 0;
}

int botw_guest_modules_link(botw_guest_modules* set,
                            botw_guest_resolve_fn hle, void* user) {
    unsigned m;
    if (!set) return -1;
    if (set->linked) return 0;
    if (set->count == 0) {
        set_error(set, "no modules to link");
        return -1;
    }
    set->cross = 0;
    set->hle = 0;
    if (build_index(set) != 0) return -1;
    set->hle_resolve = hle;
    set->hle_user = user;
    for (m = 0; m < set->count; ++m) {
        set->current = &set->mods[m];
        if (botw_guest_image_relocate(&set->mods[m].image, resolve_cb, set) != 0) {
            set_error(set, "%s: %s", set->mods[m].name, set->mods[m].image.error);
            set->current = NULL;
            return -1;
        }
    }
    set->current = NULL;
    set->linked = 1;
    set->error[0] = '\0';
    return 0;
}

botw_guest_image* botw_guest_modules_find(botw_guest_modules* set, const char* name) {
    unsigned i;
    if (!set || !name) return NULL;
    for (i = 0; i < set->count; ++i) {
        if (strcmp(set->mods[i].name, name) == 0) return &set->mods[i].image;
    }
    return NULL;
}

const botw_guest_image* botw_guest_modules_find_const(const botw_guest_modules* set,
                                                      const char* name) {
    unsigned i;
    if (!set || !name) return NULL;
    for (i = 0; i < set->count; ++i) {
        if (strcmp(set->mods[i].name, name) == 0) return &set->mods[i].image;
    }
    return NULL;
}

int botw_guest_modules_find_symbol(const botw_guest_modules* set, const char* name,
                                   uint64_t* addr) {
    if (!set || !name || !addr) return 0;
    return index_lookup(set, name, addr);
}

void botw_guest_modules_close(botw_guest_modules* set) {
    unsigned i;
    sym_index* idx;
    if (!set) return;
    for (i = 0; i < set->count; ++i) botw_guest_image_close(&set->mods[i].image);
    idx = index_of(set);
    if (idx) {
        free(idx->entries);
        free(idx);
    }
    memset(set, 0, sizeof *set);
}
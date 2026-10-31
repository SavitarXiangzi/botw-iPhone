#include "guest_image.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DT_NULL       0
#define DT_PLTRELSZ   2
#define DT_STRTAB     5
#define DT_SYMTAB     6
#define DT_RELA       7
#define DT_RELASZ     8
#define DT_RELAENT    9
#define DT_STRSZ      10
#define DT_SYMENT     11
#define DT_INIT       12
#define DT_PLTREL     20
#define DT_JMPREL     23

#define R_NONE        0
#define R_ABS64       257
#define R_GLOB_DAT    1025
#define R_JUMP_SLOT   1026
#define R_RELATIVE    1027

#define ELF_RELA_SIZE 24u
#define ELF_SYM_SIZE  24u

static void set_error(botw_guest_image* img, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(img->error, sizeof img->error, fmt, ap);
    va_end(ap);
}

static uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static int64_t rd64(const uint8_t* p) {
    int64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint64_t rdu64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static void wr64(uint8_t* p, uint64_t v) {
    memcpy(p, &v, sizeof v);
}

static int parse_int(const char* text, const char* key, int64_t* out) {
    const char* p = text ? strstr(text, key) : NULL;
    char* end = NULL;
    long long v;
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == ':') ++p;
    v = strtoll(p, &end, 0);
    if (end == p) return 0;
    *out = (int64_t)v;
    return 1;
}

static int parse_build_id(const char* text, uint8_t out[16]) {
    const char* p = text ? strstr(text, "BUILD_ID") : NULL;
    int i;
    if (!p) return 0;
    p += strlen("BUILD_ID");
    while (*p == ' ' || *p == '\t' || *p == ':') ++p;
    for (i = 0; i < 16; ++i) {
        unsigned v = 0;
        int k;
        for (k = 0; k < 2; ++k) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return 0;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}

typedef struct {
    uint64_t rela_off, rela_sz, rela_ent;
    uint64_t jmp_off, jmp_sz, jmp_ent;
    uint64_t symtab, strtab, strsz, syment;
    uint64_t init;
    int have_rela, have_jmp;
} dyn_info;

static int read_dynamic(botw_guest_image* img, dyn_info* d) {
    uint32_t m0;
    int32_t dyn_delta;
    int64_t dyn;
    unsigned i;
    memset(d, 0, sizeof *d);
    d->rela_ent = ELF_RELA_SIZE;
    d->jmp_ent = ELF_RELA_SIZE;
    d->syment = ELF_SYM_SIZE;
    if (img->size < 16) {
        set_error(img, "image is too small for a MOD0 header");
        return 0;
    }
    m0 = rd32(img->base + 4);
    if ((size_t)m0 + 8 > img->size || memcmp(img->base + m0, "MOD0", 4) != 0) {
        set_error(img, "MOD0 header not found at %#x", m0);
        return 0;
    }
    memcpy(&dyn_delta, img->base + m0 + 4, sizeof dyn_delta);
    dyn = (int64_t)m0 + (int64_t)dyn_delta;
    if (dyn < 0 || (uint64_t)dyn + 16 > img->size) {
        set_error(img, "dynamic table offset %#llx is outside the image", (unsigned long long)dyn);
        return 0;
    }
    for (i = 0; i < 4096; ++i) {
        uint64_t tag, val;
        uint64_t at = (uint64_t)dyn + (uint64_t)i * 16u;
        if (at + 16 > img->size) {
            set_error(img, "dynamic table is not terminated inside the image");
            return 0;
        }
        tag = (uint64_t)rd64(img->base + at);
        val = rdu64(img->base + at + 8);
        if (tag == DT_NULL) break;
        switch (tag) {
        case DT_RELA:      d->rela_off = val; d->have_rela = 1; break;
        case DT_RELASZ:    d->rela_sz = val; break;
        case DT_RELAENT:   d->rela_ent = val; break;
        case DT_JMPREL:    d->jmp_off = val; d->have_jmp = 1; break;
        case DT_PLTRELSZ:  d->jmp_sz = val; break;
        case DT_PLTREL:    if (val != 7) { set_error(img, "DT_PLTREL=%llu is not RELA", (unsigned long long)val); return 0; } break;
        case DT_SYMTAB:    d->symtab = val; break;
        case DT_STRTAB:    d->strtab = val; break;
        case DT_STRSZ:     d->strsz = val; break;
        case DT_SYMENT:    d->syment = val; break;
        case DT_INIT:      d->init = val; break;
        default: break;
        }
    }
    if (i == 4096) {
        set_error(img, "dynamic table has no DT_NULL");
        return 0;
    }
    if (d->rela_ent < ELF_RELA_SIZE || d->jmp_ent < ELF_RELA_SIZE || d->syment < ELF_SYM_SIZE) {
        set_error(img, "ELF entry sizes are too small (rela %llu, plt %llu, sym %llu)",
                  (unsigned long long)d->rela_ent, (unsigned long long)d->jmp_ent,
                  (unsigned long long)d->syment);
        return 0;
    }
    return 1;
}

/* Reads one dynamic symbol.  `name` may be NULL when only shndx/value matter;
 * when it is non-NULL the returned pointer aliases the image's own string
 * table (NUL-terminated, validated inside DT_STRSZ), so symbol names have no
 * copy limit.  The 1.6.0 sdk has names up to 767 bytes. */
static int symbol_lookup(const botw_guest_image* img, const dyn_info* d, uint64_t sym_index,
                         unsigned* shndx, uint64_t* value, const char** name) {
    uint64_t at, name_off;
    size_t n;

    if (d->symtab == 0) return 0;
    if (sym_index > (UINT64_MAX - d->symtab) / d->syment) return 0;
    at = d->symtab + sym_index * d->syment;
    if (at + ELF_SYM_SIZE > img->size) return 0;
    *shndx = rd16(img->base + at + 6);
    *value = rdu64(img->base + at + 8);
    if (name == NULL) return 1;

    if (d->strtab == 0 || d->strsz == 0) return 0;
    name_off = rd32(img->base + at);
    if (name_off >= d->strsz || d->strtab + name_off >= img->size) return 0;
    n = (size_t)(img->size - (d->strtab + name_off));
    if (n > (size_t)(d->strsz - name_off)) n = (size_t)(d->strsz - name_off);
    if (memchr(img->base + d->strtab + name_off, 0, n) == NULL) return 0;
    *name = (const char*)(img->base + d->strtab + name_off);
    return 1;
}

/* Resolves a dynamic symbol to S.  Undefined symbols go through the HLE
 * resolver; defined ones are base + st_value.  Both R_ABS64 and the GOT/PLT
 * slots need this: the 1.6.0 sdk points 8,822 of its 8,838 GOT/PLT slots at
 * symbols it defines itself, so treating those as imports silently wrote
 * sentinel addresses into its vtables. */
static int symbol_address(botw_guest_image* img, const dyn_info* d, uint64_t sym,
                          botw_guest_resolve_fn resolve, void* user, uint64_t* out) {
    unsigned shndx = 0;
    uint64_t st_value = 0;
    const char* name = NULL;

    if (!symbol_lookup(img, d, sym, &shndx, &st_value, &name)) {
        set_error(img, "symbol %llu is not readable", (unsigned long long)sym);
        return 0;
    }
    if (shndx == 0) {
        void* p;
        if (!resolve) {
            set_error(img, "image imports %s but no resolver was supplied", name);
            return 0;
        }
        p = resolve(user, name);
        if (!p) {
            set_error(img, "unresolved import: %s", name);
            return 0;
        }
        *out = (uint64_t)(uintptr_t)p;
        img->hle++;
    } else if (shndx == 0xfff1u) { /* SHN_ABS */
        *out = st_value;
    } else {
        if (st_value >= (uint64_t)img->size) {
            set_error(img, "symbol %s sits past the image (%#llx)",
                      name, (unsigned long long)st_value);
            return 0;
        }
        *out = (uint64_t)(uintptr_t)img->base + st_value;
    }
    return 1;
}

static int apply_one(botw_guest_image* img, const dyn_info* d, uint64_t off, uint64_t info,
                     int64_t addend, botw_guest_resolve_fn resolve, void* user) {
    uint32_t type = (uint32_t)(info & 0xffffffffu);
    uint64_t sym = info >> 32;
    if (off > img->size || img->size - off < 8) {
        set_error(img, "relocation at %#llx is outside the %#zx-byte image",
                  (unsigned long long)off, img->size);
        return 0;
    }
    switch (type) {
    case R_NONE:
        return 1;
    case R_RELATIVE:
        wr64(img->base + off, (uint64_t)(uintptr_t)img->base + (uint64_t)addend);
        img->relocs++;
        return 1;
    case R_ABS64: {
        /* R_AARCH64_ABS64 is S + A.  The 1.6.0 modules use all three
         * flavours: symbol 0 (base-relative), an undefined symbol that has to
         * come from the HLE table, and a symbol defined inside the module. */
        uint64_t s;
        if (sym == 0) {
            s = (uint64_t)(uintptr_t)img->base;
        } else {
            if (!symbol_address(img, d, sym, resolve, user, &s)) return 0;
            img->symbols++;
        }
        wr64(img->base + off, s + (uint64_t)addend);
        img->relocs++;
        return 1;
    }
    case R_GLOB_DAT:
    case R_JUMP_SLOT: {
        uint64_t s;
        if (sym == 0) {
            /* A PLT/GOT slot with no symbol is not "NULL"; it is a broken
             * table.  Writing 0 here is what turns a load failure into a
             * mysterious jump to address 0 much later. */
            set_error(img, "%s relocation has no symbol",
                      type == R_JUMP_SLOT ? "R_JUMP_SLOT" : "R_GLOB_DAT");
            return 0;
        }
        if (!symbol_address(img, d, sym, resolve, user, &s)) return 0;
        wr64(img->base + off, s + (uint64_t)addend);
        img->imports++;
        return 1;
    }
    case 1028: case 1029: case 1030: case 1031:
    case 1032: case 1033: case 1034:
        set_error(img, "TLS relocation type %u is not supported yet", type);
        return 0;
    default:
        set_error(img, "unsupported relocation type %u", type);
        return 0;
    }
}

static int apply_table(botw_guest_image* img, const dyn_info* d, uint64_t off, uint64_t sz,
                       uint64_t ent, botw_guest_resolve_fn resolve, void* user) {
    uint64_t n, i;
    if (sz == 0) return 1;
    if (off > img->size || img->size - off < sz) {
        set_error(img, "relocation table %#llx+%#llx is outside the image",
                  (unsigned long long)off, (unsigned long long)sz);
        return 0;
    }
    n = sz / ent;
    for (i = 0; i < n; ++i) {
        uint64_t at = off + i * ent;
        uint64_t r_off, r_info;
        int64_t r_add;
        if (at + ELF_RELA_SIZE > img->size) {
            set_error(img, "relocation entry %llu is truncated", (unsigned long long)i);
            return 0;
        }
        r_off = rdu64(img->base + at);
        r_info = rdu64(img->base + at + 8);
        r_add = rd64(img->base + at + 16);
        if (!apply_one(img, d, r_off, r_info, r_add, resolve, user)) return 0;
    }
    return 1;
}

int botw_guest_image_open(botw_guest_image* img, const void* file, size_t file_len,
                          const char* fixups_text, botw_guest_resolve_fn resolve,
                          void* user) {
    int64_t patch_off, key_off, key_sz, text_end, ro_off, data_off;
    dyn_info d;
    if (!img) return -1;
    memset(img, 0, sizeof *img);
    if (!file || file_len == 0) {
        set_error(img, "empty image");
        return -1;
    }
    if (!parse_int(fixups_text, "PATCH_OFF", &patch_off) ||
        !parse_int(fixups_text, "KEY_PAGE_OFF", &key_off) ||
        !parse_int(fixups_text, "KEY_PAGE_SIZE", &key_sz) ||
        !parse_int(fixups_text, "TEXT_END", &text_end) ||
        !parse_int(fixups_text, "RO_OFF", &ro_off) ||
        !parse_int(fixups_text, "DATA_OFF", &data_off)) {
        set_error(img, "fixups header is missing a required field");
        return -1;
    }
    if (patch_off < 0 || key_off <= 0 || key_sz != 0x4000 ||
        text_end <= 0 || ro_off <= 0 || data_off <= 0) {
        set_error(img, "fixups header has an unsupported layout");
        return -1;
    }
    if ((uint64_t)patch_off >= file_len ||
        (uint64_t)key_off + (uint64_t)key_sz > (uint64_t)patch_off ||
        (uint64_t)text_end > (uint64_t)ro_off ||
        (uint64_t)ro_off > (uint64_t)data_off ||
        (uint64_t)data_off > (uint64_t)key_off) {
        set_error(img, "fixups ranges are out of order or past the file");
        return -1;
    }
    img->base = (uint8_t*)malloc(file_len);
    if (!img->base) {
        set_error(img, "out of memory (%zu bytes)", file_len);
        return -1;
    }
    memcpy(img->base, file, file_len);
    img->owns_base = 1;
    img->size = (size_t)key_off;
    img->text_end = (size_t)text_end;
    img->ro_off = (size_t)ro_off;
    img->data_off = (size_t)data_off;
    img->key_page_off = (size_t)key_off;
    img->key_page_size = (size_t)key_sz;
    img->patch_off = (size_t)patch_off;
    img->patch_len = file_len - (size_t)patch_off;
    if (!parse_build_id(fixups_text, img->build_id)) {
        set_error(img, "fixups header has no BUILD_ID");
        botw_guest_image_close(img);
        return -1;
    }
    if (!read_dynamic(img, &d)) {
        botw_guest_image_close(img);
        return -1;
    }
    if (d.have_rela && d.rela_sz &&
        !apply_table(img, &d, d.rela_off, d.rela_sz, d.rela_ent, resolve, user)) {
        botw_guest_image_close(img);
        return -1;
    }
    if (d.have_jmp && d.jmp_sz &&
        !apply_table(img, &d, d.jmp_off, d.jmp_sz, d.jmp_ent, resolve, user)) {
        botw_guest_image_close(img);
        return -1;
    }
    img->error[0] = '\0';
    return 0;
}

void botw_guest_image_close(botw_guest_image* img) {
    if (!img) return;
    if (img->owns_base) free(img->base);
    img->base = NULL;
    img->owns_base = 0;
}

size_t botw_guest_image_entry(const botw_guest_image* img) {
    dyn_info d;
    if (!img || !img->base) return 0;
    if (!read_dynamic((botw_guest_image*)img, &d)) return 0;
    return (size_t)d.init;
}

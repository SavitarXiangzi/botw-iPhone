#if defined(_WIN32)
/* strcpy/strcat are deliberately in the passthrough table; the host build
 * must not turn the CRT's deprecation warning into an error. */
#  define _CRT_SECURE_NO_WARNINGS
#endif
#include "guest_hle.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#  include <strings.h>   /* strcasecmp / strncasecmp */
#endif

static void hle_error(botw_hle_table* hle, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(hle->error, sizeof hle->error, fmt, ap);
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

/* Open addressing.  Returns the matching slot, or the first empty slot when
 * the name is absent.  Callers must not use this on a full table: with no
 * empty slot the probe would never terminate. */
static size_t find_slot(const botw_hle_table* hle, const char* name, int* found) {
    size_t mask = BOTW_HLE_SLOTS - 1u;
    size_t i = (size_t)(name_hash(name) & (uint32_t)mask);
    while (hle->kind[i] != 0) {
        if (strcmp(hle->name[i], name) == 0) {
            *found = 1;
            return i;
        }
        i = (i + 1u) & mask;
    }
    *found = 0;
    return i;
}

/* A full table has no empty slot for the open-addressing probe, so the rare
 * full case falls back to a linear scan: replacing an existing name still
 * works, and a genuinely new name fails instead of spinning. */
static int find_full(const botw_hle_table* hle, const char* name, size_t* slot) {
    size_t i;
    for (i = 0; i < BOTW_HLE_SLOTS; i++) {
        if (hle->kind[i] != 0 && strcmp(hle->name[i], name) == 0) {
            *slot = i;
            return 1;
        }
    }
    return 0;
}

int botw_hle_register(botw_hle_table* hle, const char* name, void* fn) {
    int found;
    size_t i;

    if (!hle || !name || !name[0] || !fn) return -1;
    hle->error[0] = '\0';
    if (hle->used == BOTW_HLE_SLOTS) {
        if (!find_full(hle, name, &i)) {
            hle_error(hle, "HLE table is full (%u slots)", (unsigned)BOTW_HLE_SLOTS);
            return -1;
        }
        found = 1;
    } else {
        i = find_slot(hle, name, &found);
    }
    if (found) {
        if (hle->kind[i] == 2) {
            hle->stubs--;
            hle->implementations++;
        }
        hle->kind[i] = 1;
        hle->fn[i] = fn;
        return 0;
    }
    hle->name[i] = name;
    hle->fn[i] = fn;
    hle->kind[i] = 1;
    hle->used++;
    hle->implementations++;
    return 0;
}

void* botw_hle_lookup(const botw_hle_table* hle, const char* name) {
    int found;
    size_t i;
    if (!hle || !name) return NULL;
    i = find_slot(hle, name, &found);
    return (found && hle->kind[i] == 1) ? hle->fn[i] : NULL;
}

uint64_t botw_hle_stub(void) {
    return 0;
}

void* botw_hle_resolve(void* user, const char* name) {
    botw_hle_table* hle = (botw_hle_table*)user;
    int found;
    size_t i;

    if (!hle || !name) return NULL;
    hle->error[0] = '\0';
    if (hle->used == BOTW_HLE_SLOTS) {
        if (find_full(hle, name, &i)) return hle->fn[i];
        hle_error(hle, "HLE table is full (%u slots)", (unsigned)BOTW_HLE_SLOTS);
        return NULL;
    }
    i = find_slot(hle, name, &found);
    if (found) return hle->fn[i];
    hle->name[i] = name;
    hle->fn[i] = (void*)(uintptr_t)&botw_hle_stub;
    hle->kind[i] = 2;
    hle->used++;
    hle->stubs++;
    return hle->fn[i];
}

size_t botw_hle_stub_count(const botw_hle_table* hle) {
    return hle ? hle->stubs : 0;
}

size_t botw_hle_implementation_count(const botw_hle_table* hle) {
    return hle ? hle->implementations : 0;
}

/* Functions whose argument and return registers match Apple AAPCS64.  The
 * list intentionally excludes variadic calls and anything with a struct
 * layout in its signature. */
static void register_libc(botw_hle_table* hle) {
#define REG(name) do { \
    if (botw_hle_register(hle, #name, (void*)(uintptr_t)&name) != 0) return; \
} while (0)
    REG(memcpy);
    REG(memmove);
    REG(memset);
    REG(memcmp);
    REG(memchr);
    REG(strlen);
    REG(strnlen);
    REG(strcmp);
    REG(strncmp);
    REG(strcpy);
    REG(strncpy);
    REG(strcat);
    REG(strncat);
    REG(strchr);
    REG(strrchr);
    REG(strstr);
    REG(strspn);
    REG(strcspn);
    REG(strpbrk);
    REG(atoi);
    REG(atol);
    REG(strtol);
    REG(strtoul);
    REG(strtoll);
    REG(strtoull);
    REG(strtod);
    REG(strtof);
    REG(sqrtf);
    REG(sinf);
    REG(cosf);
    REG(tanf);
    REG(atanf);
    REG(atan2f);
    REG(powf);
    REG(fmodf);
    REG(expf);
    REG(logf);
    REG(log10f);
    REG(floorf);
    REG(ceilf);
    REG(roundf);
    REG(truncf);
    REG(fabsf);
    REG(ldexpf);
#if !defined(_WIN32)
    REG(strcasecmp);
    REG(strncasecmp);
#endif
#undef REG
}

void botw_hle_init(botw_hle_table* hle) {
    if (!hle) return;
    memset(hle, 0, sizeof *hle);
    register_libc(hle);
}

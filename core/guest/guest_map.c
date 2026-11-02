#include "guest_map.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
/* Windows keeps the malloc-backed loader; there is no mmap/mprotect here and
 * pretending otherwise would make the tests lie. */
int botw_guest_map_open(botw_guest_mapping* map, botw_guest_image* img,
                        const void* file, size_t file_len, const char* fixups_text,
                        botw_guest_resolve_fn resolve, void* user, size_t page_size) {
    (void)file; (void)file_len; (void)fixups_text; (void)resolve; (void)user;
    (void)page_size;
    if (map) {
        memset(map, 0, sizeof *map);
        snprintf(map->error, sizeof map->error,
                 "no mmap on this host; use botw_guest_image_open()");
    }
    if (img) memset(img, 0, sizeof *img);
    return -1;
}

int botw_guest_map_publish_slot(botw_guest_mapping* map, botw_guest_image* img,
                                botw_guest_slot* slot) {
    (void)img;
    (void)slot;
    if (map) {
        snprintf(map->error, sizeof map->error,
                 "no mmap on this host; use botw_guest_slot_publish()");
    }
    return -1;
}

void botw_guest_map_close(botw_guest_mapping* map, botw_guest_image* img) {
    if (img) img->base = NULL;
    if (map) memset(map, 0, sizeof *map);
}

#else /* POSIX */

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

static int map_fail(botw_guest_mapping* map, botw_guest_image* img,
                    const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(map->error, sizeof map->error, fmt, ap);
    va_end(ap);
    if (map->base) munmap(map->base, map->span);
    map->base = NULL;
    map->span = 0;
    map->runs = 0;
    /* The image points into the mapping; it must not be left dangling. */
    if (img && !img->owns_base) img->base = NULL;
    return -1;
}

static int prot_to_native(unsigned prot) {
    int out = 0;
    if (prot & BOTW_GUEST_PROT_READ) out |= PROT_READ;
    if (prot & BOTW_GUEST_PROT_WRITE) out |= PROT_WRITE;
    if (prot & BOTW_GUEST_PROT_EXEC) out |= PROT_EXEC;
    return out;
}

int botw_guest_map_open(botw_guest_mapping* map, botw_guest_image* img,
                        const void* file, size_t file_len, const char* fixups_text,
                        botw_guest_resolve_fn resolve, void* user, size_t page_size) {
    size_t span, i;
    uint8_t* p;

    if (!map || !img) return -1;
    memset(map, 0, sizeof *map);
    map->page = page_size;
    if (!file || file_len == 0) return map_fail(map, img, "empty image");

    if (botw_guest_image_parse(img, file_len, fixups_text) != 0) {
        return map_fail(map, img, "layout: %s", img->error);
    }
    span = botw_guest_image_map_size(img, page_size);
    if (span == 0) return map_fail(map, img, "plan: %s", img->error);
    if (botw_guest_image_plan_conflict(img, page_size) != 0) {
        return map_fail(map, img,
                        "page size %#zx needs one page writable and executable "
                        "(text %#zx, data %#zx): refuse rather than fake W^X",
                        page_size, img->text_end, img->data_off);
    }
    map->runs = botw_guest_image_plan(img, page_size, map->seg,
                                      BOTW_GUEST_MAP_MAX_SEGMENTS);
    if (map->runs == 0 || map->runs > BOTW_GUEST_MAP_MAX_SEGMENTS) {
        return map_fail(map, img, "plan: %s", img->error);
    }

    p = (uint8_t*)mmap(NULL, span, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return map_fail(map, img, "mmap(%zu) failed: errno %d", span, errno);
    }
    map->base = p;
    map->span = span;
    memcpy(p, file, file_len);
    if (span > file_len) memset(p + file_len, 0, span - file_len);

    if (botw_guest_image_attach(img, p, resolve, user) != 0) {
        return map_fail(map, img, "load: %s", img->error);
    }

    for (i = 0; i < map->runs; i++) {
        if (mprotect(p + map->seg[i].off, map->seg[i].len,
                     prot_to_native(map->seg[i].prot)) != 0) {
            return map_fail(map, img,
                            "mprotect(%#zx, %#zx, %u) failed: errno %d%s",
                            map->seg[i].off, map->seg[i].len, map->seg[i].prot, errno,
                            errno == EPERM ? " (executable anonymous memory needs the JIT entitlement)" : "");
        }
    }
    return 0;
}

int botw_guest_map_publish_slot(botw_guest_mapping* map, botw_guest_image* img,
                                botw_guest_slot* slot) {
    size_t i, begin, end;

    if (!map || !img || !slot) return -1;
    if (!map->base || map->span == 0 || map->runs == 0) {
        snprintf(map->error, sizeof map->error, "mapping is closed");
        return -1;
    }
    if (img->base != (uint8_t*)map->base) {
        snprintf(map->error, sizeof map->error,
                 "image does not live in this mapping");
        return -1;
    }
    if (!slot->base) {
        snprintf(map->error, sizeof map->error, "slot is not initialized");
        return -1;
    }
    if (img->key_page_size == 0) {
        snprintf(map->error, sizeof map->error, "image has no key page");
        return -1;
    }
    begin = img->key_page_off;
    end = begin + img->key_page_size;
    if (begin >= map->span || end > map->span) {
        snprintf(map->error, sizeof map->error,
                 "key page [%#zx, %#zx) is outside the %#zx-byte mapping",
                 begin, end, map->span);
        return -1;
    }
    for (i = 0; i < map->runs; i++) {
        if (map->seg[i].off <= begin && end <= map->seg[i].off + map->seg[i].len) break;
    }
    if (i == map->runs) {
        snprintf(map->error, sizeof map->error,
                 "key page [%#zx, %#zx) is not covered by one protection run",
                 begin, end);
        return -1;
    }
    if ((map->seg[i].prot & BOTW_GUEST_PROT_WRITE) == 0) {
        snprintf(map->error, sizeof map->error,
                 "key page [%#zx, %#zx) is not writable in the page plan",
                 begin, end);
        return -1;
    }
    if (botw_guest_slot_publish(img, slot) != 0) {
        snprintf(map->error, sizeof map->error, "slot: %s",
                 slot->error[0] ? slot->error : "publish failed");
        return -1;
    }
    return 0;
}

void botw_guest_map_close(botw_guest_mapping* map, botw_guest_image* img) {
    if (img && !img->owns_base) img->base = NULL;
    if (!map) return;
    if (map->base) munmap(map->base, map->span);
    memset(map, 0, sizeof *map);
}

#endif /* _WIN32 */

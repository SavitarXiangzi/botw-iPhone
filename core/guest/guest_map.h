/* Executable mapping for a rewritten image (POSIX hosts).
 *
 * guest_segments.c computes what each page needs; this file is the only place
 * that talks to the OS.  The order matters and is the whole point:
 *
 *   1. parse the fixups header (no bytes touched) so the page plan is known
 *      before anything is allocated;
 *   2. refuse outright when a page would need to be writable and executable -
 *      there is no protection that makes that page correct;
 *   3. mmap the whole span read/write and copy patched.bin into it, preserving
 *      the file offsets the rewritten `adrp` instructions depend on;
 *   4. apply the relocations while the mapping is still writable (text has
 *      none of them, but a loader that protects first would turn a future text
 *      relocation into a crash instead of an error);
 *   5. mprotect each run from the plan, so text and the patch area end up
 *      read-execute and data/key page stay read-write.
 *
 * The image returned in *img points into this mapping and does not own it:
 * closing either one is not enough, botw_guest_map_close() does both.
 *
 * iOS note: anonymous memory can only become executable with the JIT
 * entitlement (dynamic-codesigning).  Without it step 5 fails with EPERM and
 * the error says so; nothing here pretends otherwise.
 */
#ifndef BOTW_GUEST_MAP_H
#define BOTW_GUEST_MAP_H

#include <stddef.h>

#include "guest_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/* main at 16 KiB pages needs 5 runs; 16 leaves room for a future layout. */
#define BOTW_GUEST_MAP_MAX_SEGMENTS 16

typedef struct {
    void*  base;
    size_t span;
    size_t page;
    char   error[192];
} botw_guest_mapping;

/* Maps `file` (the whole patched.bin) and fills *img with a relocated image
 * living inside the mapping.  `page_size` must be the host's page size;
 * sysconf(_SC_PAGESIZE) is the usual way to get it.  Returns 0 or -1 with
 * map->error set. */
int botw_guest_map_open(botw_guest_mapping* map, botw_guest_image* img,
                        const void* file, size_t file_len, const char* fixups_text,
                        botw_guest_resolve_fn resolve, void* user, size_t page_size);

/* Drops the image's pointer and unmaps.  Safe on a zeroed mapping. */
void botw_guest_map_close(botw_guest_mapping* map, botw_guest_image* img);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_MAP_H */

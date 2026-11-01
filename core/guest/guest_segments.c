/* Page-level protection plan for a rewritten image.
 *
 * The rewritten code addresses the key page and the patch area with `adrp`,
 * i.e. by a delta fixed at rewrite time.  Text, rodata, data, the key page and
 * the patch area therefore have to stay at their file offsets relative to each
 * other: the whole patched.bin has to be one contiguous mapping.  The only
 * freedom left is the protection of each page, and that is what this file
 * computes.
 *
 * Two facts from the 1.6.0 rewrite make the plan simple:
 *
 *   - no relocation is applied to text (probe_relocs: 0 of 262,837), so text
 *     can be read-execute and never written again after the copy;
 *   - the key page is the only part of the image the guest writes to, and the
 *     loader writes it before the first gate.
 *
 * Page size is a parameter, not a constant: iOS and Apple silicon use 16 KiB,
 * x86 hosts 4 KiB.  The same module can be fine at 4 KiB and impossible at
 * 16 KiB - rtld's 0x1a00 bytes of text, rodata and data all land in the first
 * 16 KiB page - which is exactly why the conflict is reported instead of
 * being silently resolved into one of the two permissions.
 */
#include "guest_image.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define REGION_MAX 7

typedef struct {
    size_t   off;
    size_t   end;
    unsigned prot;
} region;

static void plan_error(botw_guest_image* img, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(img->error, sizeof img->error, fmt, ap);
    va_end(ap);
}

static int is_power_of_two(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

static size_t round_up(size_t v, size_t page) {
    return (v + page - 1) / page * page;
}

/* Region order is the file order; botw_guest_image_open() already refused a
 * header whose ranges disagree. */
static int build_regions(const botw_guest_image* img, region* out) {
    size_t key_end = img->key_page_off + img->key_page_size;
    int n = 0;
    if (img->key_page_size == 0 || key_end > img->patch_off) return 0;
    if (img->patch_off >= img->file_size) return 0;
    if (!(img->text_end <= img->ro_off && img->ro_off <= img->data_off &&
          img->data_off <= img->key_page_off)) {
        return 0;
    }
    out[n].off = 0;                     out[n].end = img->text_end;      out[n].prot = BOTW_GUEST_PROT_READ | BOTW_GUEST_PROT_EXEC; n++;
    out[n].off = img->text_end;         out[n].end = img->ro_off;        out[n].prot = BOTW_GUEST_PROT_READ; n++;
    out[n].off = img->ro_off;           out[n].end = img->data_off;      out[n].prot = BOTW_GUEST_PROT_READ; n++;
    out[n].off = img->data_off;         out[n].end = img->key_page_off;  out[n].prot = BOTW_GUEST_PROT_READ | BOTW_GUEST_PROT_WRITE; n++;
    out[n].off = img->key_page_off;     out[n].end = key_end;            out[n].prot = BOTW_GUEST_PROT_READ | BOTW_GUEST_PROT_WRITE; n++;
    /* Padding between the key page and the patch area: zeros, read-only. */
    out[n].off = key_end;               out[n].end = img->patch_off;     out[n].prot = BOTW_GUEST_PROT_READ; n++;
    out[n].off = img->patch_off;        out[n].end = img->file_size;     out[n].prot = BOTW_GUEST_PROT_READ | BOTW_GUEST_PROT_EXEC; n++;
    return n;
}

static unsigned page_prot(const region* regions, int n, size_t off, size_t end) {
    unsigned prot = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (regions[i].off < end && regions[i].end > off) prot |= regions[i].prot;
    }
    return prot;
}

size_t botw_guest_image_map_size(botw_guest_image* img, size_t page_size) {
    if (!img) return 0;
    img->error[0] = '\0';
    if (img->file_size == 0) {
        plan_error(img, "image has no file size");
        return 0;
    }
    if (!is_power_of_two(page_size) || page_size < 0x1000) {
        plan_error(img, "page size %#zx is not a power of two >= 0x1000", page_size);
        return 0;
    }
    return round_up(img->file_size, page_size);
}

/* One pass over the pages: counts the runs, and fills `out` while there is
 * room.  Returning the needed count even when the caller's array is too small
 * makes the two-call pattern work without an allocation. */
size_t botw_guest_image_plan(botw_guest_image* img, size_t page_size,
                             botw_guest_segment* out, size_t cap) {
    region regions[REGION_MAX];
    size_t span, pages, i, runs = 0;
    size_t run_off = 0, run_len = 0;
    unsigned run_prot = 0;
    int nreg;

    if (!img) return 0;
    img->error[0] = '\0';
    span = botw_guest_image_map_size(img, page_size);
    if (span == 0) return 0;
    nreg = build_regions(img, regions);
    if (nreg == 0) {
        plan_error(img, "image layout is not usable (text %#zx ro %#zx data %#zx key %#zx patch %#zx of %#zx)",
                   img->text_end, img->ro_off, img->data_off, img->key_page_off,
                   img->patch_off, img->file_size);
        return 0;
    }
    pages = span / page_size;
    for (i = 0; i <= pages; i++) {
        unsigned prot = i < pages ? page_prot(regions, nreg, i * page_size, (i + 1) * page_size) : 0;
        if (i == 0 || (i < pages && prot == run_prot)) {
            if (i < pages) run_prot = prot;
            run_len += page_size;
            continue;
        }
        /* flush the run that ended at page i */
        if (out && runs < cap) {
            out[runs].off = run_off;
            out[runs].len = run_len;
            out[runs].prot = run_prot;
        }
        runs++;
        if (i == pages) break;
        run_off = i * page_size;
        run_len = page_size;
        run_prot = prot;
    }
    if (out && runs > cap) {
        plan_error(img, "plan needs %zu runs, the caller offered %zu", runs, cap);
    }
    return runs;
}

int botw_guest_image_plan_conflict(botw_guest_image* img, size_t page_size) {
    region regions[REGION_MAX];
    size_t span, pages, i;
    int nreg;
    if (!img) return -1;
    img->error[0] = '\0';
    span = botw_guest_image_map_size(img, page_size);
    if (span == 0) return -1;
    nreg = build_regions(img, regions);
    if (nreg == 0) return -1;
    pages = span / page_size;
    for (i = 0; i < pages; i++) {
        unsigned prot = page_prot(regions, nreg, i * page_size, (i + 1) * page_size);
        if ((prot & BOTW_GUEST_PROT_WRITE) && (prot & BOTW_GUEST_PROT_EXEC)) return 1;
    }
    return 0;
}

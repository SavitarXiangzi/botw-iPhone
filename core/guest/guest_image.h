/* Minimal loader for a rewritten 1.6.0 module image.
 *
 * This is the portable half of the iOS guest core.  It does not execute
 * guest code: it maps a patched.bin produced by scripts/guest/rewrite_botw.py,
 * applies its ELF-style relocations, and resolves imported symbols through a
 * caller-supplied HLE table.  On an ARM64 host the caller can then enter the
 * image's MOD0 init function; on Windows the same code is exercised by
 * tests/guest_image_test.cpp with a synthetic image.
 *
 * Relocations follow real ELF semantics, which the 1.6.0 images need:
 *   R_RELATIVE  -> base + addend
 *   R_ABS64     -> sym == 0 ? base + addend : S + addend, where S is the HLE
 *                  address for an undefined symbol or base + st_value for a
 *                  symbol defined inside the module
 *   R_GLOB_DAT / R_JUMP_SLOT -> the resolved import address (symbol required)
 *
 * The loader refuses unknown imports instead of writing NULL: a silently
 * missing HLE entry is exactly the class of bug that made draws=0 look like a
 * game that never submitted geometry.
 */
#ifndef BOTW_GUEST_IMAGE_H
#define BOTW_GUEST_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* (*botw_guest_resolve_fn)(void* user, const char* name);

typedef struct {
    uint8_t* base;              /* owned copy of the image, mapped at runtime later */
    size_t   size;              /* text + ro + data + bss (relocations live here) */
    size_t   file_size;         /* whole patched.bin, including key page + patch */
    size_t   text_end;
    size_t   ro_off;
    size_t   data_off;
    size_t   key_page_off;
    size_t   key_page_size;
    size_t   patch_off;
    size_t   patch_len;
    int      slot_abs;          /* key page holds absolute slot addresses */
    unsigned k18_idx;           /* key-page indices, from the fixups header */
    unsigned ktls_idx;
    unsigned ktlr_idx;
    unsigned kgate_idx;
    uint8_t  build_id[16];
    unsigned relocs;            /* R_RELATIVE / R_ABS64 applied */
    unsigned symbols;           /* R_ABS64 entries whose symbol index is non-zero */
    unsigned hle;               /* resolver calls: undefined symbols from ABS64 and GOT/PLT */
    unsigned imports;           /* R_GLOB_DAT / R_JUMP_SLOT slots applied (local or imported) */
    int      owns_base;
    char     error[192];
} botw_guest_image;

/* `file` is the whole patched.bin; `fixups_text` is the matching
 * <module>.fixups.tsv (only the '#' header lines are read).  On success the
 * image owns a private copy; on failure `error` says which check failed.
 * `resolve` may be NULL only when the image has no imports. */
int botw_guest_image_open(botw_guest_image* img, const void* file, size_t file_len,
                          const char* fixups_text, botw_guest_resolve_fn resolve,
                          void* user);

/* The two halves of open(), for callers that own the buffer.
 *
 * parse() reads only the fixups header: it validates the layout, fills every
 * field in botw_guest_image except `base`, and never touches the bytes.  An
 * executable mapping needs this first, because the page plan comes from the
 * header alone.
 *
 * attach() adopts `buffer` (file_size bytes, already a copy of patched.bin),
 * reads MOD0/dynamic from it, and applies the relocations in place.  It never
 * frees or copies the buffer; whoever set owns_base keeps that duty. */
int botw_guest_image_parse(botw_guest_image* img, size_t file_len,
                           const char* fixups_text);
int botw_guest_image_attach(botw_guest_image* img, void* buffer,
                            botw_guest_resolve_fn resolve, void* user);
void botw_guest_image_close(botw_guest_image* img);

/* The MOD0 init entry (virtual offset), or 0 when the image has no init. */
size_t botw_guest_image_entry(const botw_guest_image* img);

/* The key page the rewritten code reads with `adrp/ldr`.  It sits between the
 * image proper and the patch area and is writable: guest_slot_publish() stores
 * the thread-slot offsets here before the first guest instruction runs. */
uint8_t* botw_guest_image_key_page(botw_guest_image* img);
size_t botw_guest_image_key_page_size(const botw_guest_image* img);

/* Protection plan for the mapping the guest will run from.
 *
 * Text, rodata, data, the key page and the patch area are addressed relative
 * to each other by `adrp`, so the whole patched.bin has to be one contiguous
 * mapping and only its page protections can differ:
 *
 *   [0, TEXT_END)                RX   code, never written after the copy
 *   [TEXT_END, DATA_OFF)         R    rodata
 *   [DATA_OFF, KEY_PAGE_OFF)     RW   data + bss; relocations land here
 *   [KEY_PAGE_OFF, +0x4000)      RW   the key page guest_slot publishes into
 *   [.., PATCH_OFF)              R    padding
 *   [PATCH_OFF, file_len)        RX   rewritten gate blocks
 *
 * Page size is a parameter because iOS and Apple silicon use 16 KiB while
 * x86 hosts use 4 KiB, and a page can be shared by a read-only and a writable
 * region (then it gets the union).  When a page would need to be both
 * writable and executable, no protection is correct: plan_conflict() says so
 * and the caller has to split or dual-map that page.
 *
 * Offsets in `out` are page-aligned runs covering [0, map_size()); the runs
 * are contiguous and in order.  Returns the number of runs the image needs.
 * With out == NULL and cap == 0 this is a pure size query; if cap is too
 * small the count is still returned, `out` holds a prefix, and img->error
 * says how many were needed. */
#define BOTW_GUEST_PROT_READ  1u
#define BOTW_GUEST_PROT_WRITE 2u
#define BOTW_GUEST_PROT_EXEC  4u

typedef struct {
    size_t   off;
    size_t   len;
    unsigned prot;
} botw_guest_segment;

size_t botw_guest_image_map_size(botw_guest_image* img, size_t page_size);
size_t botw_guest_image_plan(botw_guest_image* img, size_t page_size,
                             botw_guest_segment* out, size_t cap);
/* 1 when some page needs write and execute at once, 0 when not, -1 on a bad
 * layout or page size (img->error says which). */
int botw_guest_image_plan_conflict(botw_guest_image* img, size_t page_size);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_IMAGE_H */

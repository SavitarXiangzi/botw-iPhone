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
    size_t   text_end;
    size_t   ro_off;
    size_t   data_off;
    size_t   key_page_off;
    size_t   key_page_size;
    size_t   patch_off;
    size_t   patch_len;
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
void botw_guest_image_close(botw_guest_image* img);

/* The MOD0 init entry (virtual offset), or 0 when the image has no init. */
size_t botw_guest_image_entry(const botw_guest_image* img);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_IMAGE_H */

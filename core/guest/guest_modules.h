/* Multi-module linking for the rewritten 1.6.0 images.
 *
 * Horizon's rtld loads main, sdk, subsdk0 and rtld into one address space and
 * binds their relocations against each other before any init function runs.
 * A one-module-at-a-time loader cannot do that: main imports 430 symbols from
 * sdk, sdk imports malloc/free/calloc from main, subsdk0 imports 640 symbols
 * from sdk, and rtld imports from both.  The cycle is resolved by binding all
 * images first, building one symbol index, and only then relocating each one.
 *
 * The set does not execute code and does not map pages.  It owns private
 * malloc-backed copies, so it is portable and usable from the Windows tests
 * and from `reloc_run --multi`, while the iOS executable mapping remains a
 * separate step.
 *
 * Resolution order matches the loader used by nxnative: a definition in an
 * earlier-added module wins, then the caller's HLE resolver.  An unresolved
 * import is an error, never a silent NULL.
 */
#ifndef BOTW_GUEST_MODULES_H
#define BOTW_GUEST_MODULES_H

#include "guest_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOTW_GUEST_MAX_MODULES 8
#define BOTW_GUEST_MODULE_NAME 16

typedef struct {
    char name[BOTW_GUEST_MODULE_NAME];
    botw_guest_image image;   /* owns image.base */
    unsigned cross;           /* relocations this module got from another module */
} botw_guest_module;

typedef struct {
    botw_guest_module mods[BOTW_GUEST_MAX_MODULES];
    unsigned count;
    int linked;
    unsigned cross;           /* total cross-module resolutions */
    unsigned hle;             /* total fall-through resolver calls that returned a host address */
    unsigned nsyms;           /* entries in the private symbol index */
    void* syms;               /* private symbol index, freed by close */
    botw_guest_resolve_fn hle_resolve;
    void* hle_user;
    botw_guest_module* current; /* module being relocated, for per-module counts */
    char error[192];
} botw_guest_modules;

/* Parses `fixups_text` and adopts a private copy of `file`.  The bytes are
 * bound but not relocated.  Module names are caller-chosen; the usual order is
 * main, sdk, subsdk0, rtld.  Returns 0 on success. */
int botw_guest_modules_add(botw_guest_modules* set, const char* name,
                           const void* file, size_t file_len,
                           const char* fixups_text);

/* Builds the cross-module symbol index and relocates every module.  `hle` is
 * called only for names no module defines; it may be NULL when every import is
 * internal.  Returns 0 on success and leaves a diagnostic in set->error. */
int botw_guest_modules_link(botw_guest_modules* set,
                            botw_guest_resolve_fn hle, void* user);

botw_guest_image* botw_guest_modules_find(botw_guest_modules* set, const char* name);
const botw_guest_image* botw_guest_modules_find_const(const botw_guest_modules* set,
                                                      const char* name);

/* First definition of `name` in the linked set, as a runtime address. */
int botw_guest_modules_find_symbol(const botw_guest_modules* set, const char* name,
                                   uint64_t* addr);

void botw_guest_modules_close(botw_guest_modules* set);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_MODULES_H */
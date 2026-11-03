/* Host-side HLE (high-level emulation) registry.
 *
 * The rewritten image imports symbols by name; guest_image.c asks a resolver
 * for every undefined symbol.  This table maps those names to host functions.
 * Unknown names get a shared stub that returns 0, so a relocation never
 * becomes NULL; the stub count is the coverage signal.
 *
 * Names are not copied: the table stores the pointer it is given.  The image
 * loader passes a pointer into the image's string table, which stays alive
 * while the image does.  Keep the image (or another stable string) alive for
 * as long as the table entry is needed.
 *
 * The table is deliberately plain data: no allocation, no OS calls, and no
 * execution.  It has 4,096 slots.  After the 1.6.0 four modules are linked
 * against each other, only 25 distinct names still fall through to this table
 * (the glslc* group and the linker-provided __rel_dyn/__EX/__got markers), so
 * the old 1,137-name estimate was counting cross-module imports as HLE.
 *
 * The built-in entries are libc passthroughs whose AAPCS64 signatures are
 * identical on the Switch and on Apple silicon.  Variadic functions are
 * deliberately absent: the Switch passes variadic arguments in registers
 * while Apple's ABI puts them on the stack.
 */
#ifndef BOTW_GUEST_HLE_H
#define BOTW_GUEST_HLE_H

#include <stddef.h>
#include <stdint.h>

#include "guest_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOTW_HLE_SLOTS 4096u

typedef struct {
    const char* name[BOTW_HLE_SLOTS];
    void* fn[BOTW_HLE_SLOTS];
    unsigned char kind[BOTW_HLE_SLOTS];  /* 0 empty, 1 implementation, 2 stub */
    size_t used;
    size_t implementations;
    size_t stubs;
    char error[128];
} botw_hle_table;

/* Clears the table and registers the built-in libc passthroughs. */
void botw_hle_init(botw_hle_table* hle);

/* Registers or replaces an implementation.  Returns 0, or -1 with error set. */
int botw_hle_register(botw_hle_table* hle, const char* name, void* fn);

/* Returns the registered implementation, or NULL.  A stub is not returned. */
void* botw_hle_lookup(const botw_hle_table* hle, const char* name);

/* botw_guest_resolve_fn adapter: an implementation, otherwise the shared
 * zero-return stub.  The same unknown name always gets the same pointer, and
 * a later botw_hle_register() replaces it. */
void* botw_hle_resolve(void* user, const char* name);

/* The shared stub, so tests and callers can recognize it.  It returns 0 in
 * x0/w0; unimplemented float-returning calls should not be allowed to run. */
uint64_t botw_hle_stub(void);

size_t botw_hle_stub_count(const botw_hle_table* hle);
size_t botw_hle_implementation_count(const botw_hle_table* hle);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_HLE_H */

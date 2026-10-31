/* Per-thread slot for the rewritten guest code.
 *
 * The rewrite replaces x18 with a memory location and replaces every svc with
 * a gate block that does:
 *
 *     stp  x29, x30, [sp, #-16]!
 *     mrs  x30, tpidrro_el0          ; slot base
 *     adrp x29, #key_page
 *     ldr  x29, [x29, #(KGATE*8)]    ; offset inside the slot
 *     add  x30, x30, x29
 *     ldr  x30, [x30]                ; handler pointer
 *     blr  x30
 *
 * so the *image* only carries key-page indices; the offsets themselves have to
 * be published at runtime.  This module owns both halves: it lays out a slot
 * and writes the offsets into the image's key page.
 *
 * Nothing here executes guest code, and the layout is deliberately plain
 * offsets so it can be tested on any host.
 */
#ifndef BOTW_GUEST_SLOT_H
#define BOTW_GUEST_SLOT_H

#include <stddef.h>
#include <stdint.h>

#include "guest_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOTW_GUEST_SLOT_MIN 0x40u
#define BOTW_GUEST_SLOT_ALIGN 16u

typedef struct {
    uint8_t* base;
    size_t   size;
    size_t   x18_off;    /* guest x18 storage */
    size_t   tls_off;    /* tpidr_el0 storage */
    size_t   tlr_off;    /* tpidrro_el0 storage */
    size_t   gate_off;   /* gate handler pointer */
    char     error[128];
} botw_guest_slot;

/* Zeroes `memory` and installs `gate_handler` at the gate offset.  `memory`
 * must be BOTW_GUEST_SLOT_ALIGN-aligned and at least BOTW_GUEST_SLOT_MIN
 * bytes.  Returns 0 on success, -1 with `error` filled otherwise. */
int botw_guest_slot_init(botw_guest_slot* slot, void* memory, size_t size,
                         void* gate_handler);

/* Copies the slot's offsets into the image's key page, at the indices the
 * fixups header names.  The image must stay alive for as long as the guest
 * runs; the rewritten code reads this page directly. */
int botw_guest_slot_publish(botw_guest_image* img, botw_guest_slot* slot);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_GUEST_SLOT_H */

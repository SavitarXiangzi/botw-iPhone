#include "guest_slot.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void slot_error(botw_guest_slot* slot, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(slot->error, sizeof slot->error, fmt, ap);
    va_end(ap);
}

int botw_guest_slot_init(botw_guest_slot* slot, void* memory, size_t size,
                         void* gate_handler) {
    uintptr_t addr;
    uint8_t* mem = (uint8_t*)memory;

    if (!slot) return -1;
    memset(slot, 0, sizeof *slot);
    if (!mem) {
        slot_error(slot, "slot memory is NULL");
        return -1;
    }
    addr = (uintptr_t)mem;
    if (addr % BOTW_GUEST_SLOT_ALIGN != 0) {
        slot_error(slot, "slot memory %p is not %u-byte aligned", memory,
                   (unsigned)BOTW_GUEST_SLOT_ALIGN);
        return -1;
    }
    if (size < BOTW_GUEST_SLOT_MIN) {
        slot_error(slot, "slot is %zu bytes, need at least %u", size,
                   (unsigned)BOTW_GUEST_SLOT_MIN);
        return -1;
    }
    if (!gate_handler) {
        slot_error(slot, "gate handler is NULL");
        return -1;
    }

    memset(mem, 0, size);
    slot->base = mem;
    slot->size = size;
    slot->x18_off = 0;
    slot->tls_off = 8;
    slot->tlr_off = 16;
    slot->gate_off = 24;
    memcpy(mem + slot->gate_off, &gate_handler, sizeof gate_handler);
    slot->error[0] = '\0';
    return 0;
}

int botw_guest_slot_publish(botw_guest_image* img, botw_guest_slot* slot) {
    uint8_t* page;
    size_t page_size, entries;
    uint64_t* key;

    if (!img || !slot || !slot->base) return -1;
    if (slot->gate_off + sizeof(void*) > slot->size) {
        slot_error(slot, "slot layout does not fit in %zu bytes", slot->size);
        return -1;
    }
    page = botw_guest_image_key_page(img);
    page_size = botw_guest_image_key_page_size(img);
    if (!page || page_size < 8 * 4) {
        slot_error(slot, "image has no usable key page");
        return -1;
    }
    entries = page_size / 8;
    key = (uint64_t*)(void*)page;
    if (img->k18_idx >= entries || img->ktls_idx >= entries ||
        img->ktlr_idx >= entries || img->kgate_idx >= entries) {
        slot_error(slot, "key index out of range (k18=%u ktls=%u ktlr=%u kgate=%u, %zu entries)",
                   img->k18_idx, img->ktls_idx, img->ktlr_idx, img->kgate_idx, entries);
        return -1;
    }
    key[img->k18_idx] = (uint64_t)slot->x18_off;
    key[img->ktls_idx] = (uint64_t)slot->tls_off;
    key[img->ktlr_idx] = (uint64_t)slot->tlr_off;
    key[img->kgate_idx] = (uint64_t)slot->gate_off;
    return 0;
}

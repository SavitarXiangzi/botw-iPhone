/* NVN descriptor pools: base assignment and handle packing.
 *
 * No Apple headers: this file has to compile with MSVC, and the unit test
 * runs the same arithmetic the Metal backend uses.
 */
#include "nvn_pool.h"

void nvngfx_pool_map_init(nvngfx_pool_map* m) {
    if (!m) return;
    m->count = 0;
    m->next_base = 0;
}

int nvngfx_pool_map_add(nvngfx_pool_map* m, uint64_t pool, int size, int* base) {
    int i;
    if (base) *base = 0;
    if (!m || !pool) return 0;
    for (i = 0; i < m->count; ++i) {
        if (m->entries[i].pool == pool) {
            if (base) *base = m->entries[i].base;
            return 1;
        }
    }
    if (m->count >= NVNGFX_POOL_MAX) return 0;
    if (size < 0) size = 0;
    nvngfx_pool_entry* e = &m->entries[m->count++];
    e->pool = pool;
    e->base = m->next_base;
    e->size = size;
    /* Round up so the next pool never shares a 256-entry block, even when
     * this one is tiny.  Same rule as nxnative's dpool_add. */
    m->next_base += (size + NVNGFX_POOL_ALIGN - 1) & ~(NVNGFX_POOL_ALIGN - 1);
    if (base) *base = e->base;
    return 1;
}

int nvngfx_pool_map_base(const nvngfx_pool_map* m, uint64_t pool) {
    int i;
    if (!m || !pool) return -1;
    for (i = 0; i < m->count; ++i)
        if (m->entries[i].pool == pool) return m->entries[i].base;
    return -1;
}

int nvngfx_pool_slot(const nvngfx_pool_map* m, uint64_t pool, int id) {
    int i;
    if (!m || id < 0) return -1;
    for (i = 0; i < m->count; ++i) {
        if (m->entries[i].pool != pool) continue;
        if (id >= m->entries[i].size) return -1;
        return m->entries[i].base + id;
    }
    return -1;
}

uint64_t nvngfx_texture_handle(unsigned tex, unsigned samp) {
    return (uint64_t)(tex & NVNGFX_POOL_INDEX_MASK) |
           ((uint64_t)(samp & NVNGFX_POOL_INDEX_MASK) << NVNGFX_POOL_INDEX_BITS);
}

unsigned nvngfx_texture_handle_tex(uint64_t h) {
    return (unsigned)(h & NVNGFX_POOL_INDEX_MASK);
}

unsigned nvngfx_texture_handle_samp(uint64_t h) {
    return (unsigned)((h >> NVNGFX_POOL_INDEX_BITS) & NVNGFX_POOL_INDEX_MASK);
}

uint64_t nvngfx_separate_sampler_handle(unsigned id) {
    return (uint64_t)(id & NVNGFX_POOL_INDEX_MASK) << NVNGFX_POOL_INDEX_BITS;
}

unsigned nvngfx_separate_sampler_index(uint64_t h) {
    return nvngfx_texture_handle_samp(h);
}
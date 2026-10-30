/* NVN descriptor pools, in host terms.
 *
 * A texture or sampler ID is not a global name: it indexes whichever pool the
 * command buffer last bound (nvnCommandBufferSetTexturePool /
 * SetSamplerPool).  Two pools may register different resources under the same
 * ID, and both must keep working after the command buffer switches pools.
 *
 * This mirrors nxnative's hle_nvn.c: every pool gets a base when it is
 * initialized, bases are handed out in creation order and rounded up to 256
 * entries, and a registration goes to base + id.  A game with one pool per
 * kind therefore sees base 0 and unchanged IDs, while a game with several
 * pools (Fire Emblem: Three Houses uses a 12,544-entry scene pool plus two
 * 4,096-entry UI pools) does not have its UI textures overwritten by the
 * scene pool's IDs.
 *
 * Everything here is arithmetic over integers, so it compiles and is tested
 * on Windows; the Metal side only stores host objects at the resulting slots.
 */
#ifndef NVN_POOL_H
#define NVN_POOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* nxnative's NDPOOL.  A 17th pool in one process does not get its own
 * numbering; the backend reports that instead of silently aliasing pools. */
#define NVNGFX_POOL_MAX 16
#define NVNGFX_POOL_ALIGN 256

/* The low 20 bits of an NVNtextureHandle are the texture pool index and the
 * next bits are the sampler pool index -- nvnDeviceGetTextureHandle packs them
 * that way in nxnative's hle_nvn.c, and separate handles use the same fields. */
#define NVNGFX_POOL_INDEX_BITS 20
#define NVNGFX_POOL_INDEX_MASK ((1u << NVNGFX_POOL_INDEX_BITS) - 1u)

typedef struct {
    uint64_t pool;   /* guest address of the NVNtexturePool/NVNsamplerPool */
    int base;        /* first host slot owned by this pool */
    int size;        /* entries the guest asked for */
} nvngfx_pool_entry;

typedef struct {
    nvngfx_pool_entry entries[NVNGFX_POOL_MAX];
    int count;
    int next_base;
} nvngfx_pool_map;

void nvngfx_pool_map_init(nvngfx_pool_map* m);

/* Assigns (or returns) the pool's base.  Writing *base is optional.
 * Returns 1 on success, 0 when the table is full; on failure *base is 0 and
 * the caller must not register anything. */
int nvngfx_pool_map_add(nvngfx_pool_map* m, uint64_t pool, int size, int* base);

/* -1 when this pool was never initialized. */
int nvngfx_pool_map_base(const nvngfx_pool_map* m, uint64_t pool);

/* Handle packing, identical to nxnative.  `tex` and `samp` are pool indices
 * (already including the pool base), not guest object addresses. */
uint64_t nvngfx_texture_handle(unsigned tex, unsigned samp);
unsigned nvngfx_texture_handle_tex(uint64_t h);
unsigned nvngfx_texture_handle_samp(uint64_t h);

/* nvnDeviceGetSeparateSamplerHandle(id) == id << 20; the texture half stays 0. */
uint64_t nvngfx_separate_sampler_handle(unsigned id);
unsigned nvngfx_separate_sampler_index(uint64_t h);

/* The slot a registration lands in, or -1 when the pool is unknown or the ID
 * is outside the pool. */
int nvngfx_pool_slot(const nvngfx_pool_map* m, uint64_t pool, int id);

#ifdef __cplusplus
}
#endif
#endif /* NVN_POOL_H */

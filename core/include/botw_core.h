/* Minimal C ABI between the (future) ARM64 HLE core and the iOS shell.
 *
 * Design note: on device the core does NOT run a JIT. The Windows-side harness
 * (tools/m1_kern.py, Unicorn) exists to *discover* the HLE boundary and to record
 * the NVN call stream. That recording is replayed here as a static C++ backend.
 */
#ifndef BOTW_CORE_H
#define BOTW_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct botw_core botw_core;

typedef struct {
    uint32_t width;
    uint32_t height;
    void*    metal_device;      /* id<MTLDevice>, bridged */
    void*    metal_command_queue;
    const char* romfs_root;     /* path to the mounted content/ tree */
} botw_core_desc;

botw_core* botw_core_create(const botw_core_desc* desc);
void       botw_core_destroy(botw_core* core);

/* Advance one frame. Returns 0 on success, negative on fatal error. */
int        botw_core_frame(botw_core* core, double dt);

uint64_t   botw_core_frame_index(const botw_core* core);
const char* botw_core_last_error(const botw_core* core);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_CORE_H */

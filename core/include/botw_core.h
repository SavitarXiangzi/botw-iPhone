/* Minimal C ABI between the ARM64 HLE core and the iOS shell.
 *
 * Design note: on device the core does NOT run a JIT. The Windows-side harness
 * (tools/m1_kern.py, Unicorn) exists to *discover* the HLE boundary and to
 * record the NVN call stream.  On iOS the title's own ARM64 code runs natively
 * and every NVN entry point resolves through the host table in
 * ios/nvn/src/nvn_dispatch.cpp -- see docs/IOS_PORTING.md.
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

/* --- NVN host-HLE surface ------------------------------------------- */

/* How much of the title's NVN API the installed backend actually covers.
 * `total` is the registry size extracted from the title's own getproc name
 * strings; `implemented` is what the backend resolves for real.  The gap is
 * the honest work queue, also written to docs/NVN_COVERAGE.md. */
typedef struct {
    uint32_t total;
    uint32_t implemented;
    const char* backend;   /* "metal" or "null" */
} botw_core_nvn_stats;

void botw_core_nvn_stats_get(const botw_core* core, botw_core_nvn_stats* out);

/* 1 if the backend implements `proc_name` (useful from the debug HUD). */
int botw_core_nvn_has(const botw_core* core, const char* proc_name);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_CORE_H */

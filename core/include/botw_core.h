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
    /* CAMetalLayer* the NVN window presents into.  This must be the layer the
     * app actually displays, not a private one: otherwise the guest's frames
     * go to a surface nobody can see.  NULL lets the backend make its own. */
    void*    native_layer;
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
    /* Frames the host swapchain actually handed to the display.  0 means the
     * layer was never presentable (not attached, wrong size, no drawable). */
    uint32_t presents;
} botw_core_nvn_stats;

void botw_core_nvn_stats_get(const botw_core* core, botw_core_nvn_stats* out);

/* 1 if the backend implements `proc_name` (useful from the debug HUD). */
int botw_core_nvn_has(const botw_core* core, const char* proc_name);

/* Runs the Metal replay layer's self-test on the system default device: one
 * frame through the NVN entry points (texture, render target, clear, program,
 * pipeline, draw) followed by a readback of the centre pixel.
 *
 * Returns 0 on success, a non-zero bitmask naming the failed step, or -1 when
 * the backend has no self-test (non-Apple builds).
 *
 * This is a debug hook, not part of the game path.  The iOS shell calls it at
 * launch only when BOTW_SELFTEST=1, which is how CI executes the renderer
 * instead of only compiling it. */
int botw_core_selftest(void);

/* Debug hook: enters a tiny built-in AArch64 probe through the real
 * botw_guest_run path (private stack, exit sentinel, host return).  It
 * proves that the iOS binary can execute native guest code; it does not
 * load the title image yet.  Returns 0 on success, 1 when the probe
 * result differs, 2 when run setup fails, or -1 on non-AArch64 hosts. */
int botw_core_guest_selftest(void);

/* Debug hook: maps a synthetic patched.bin into executable memory with
 * botw_guest_map_open(), publishes a thread slot, enters a rewritten gate
 * block on a real guest stack, and verifies the dispatcher plus the exit
 * sentinel.  It exercises the app-side loader/mapping/gate chain without any
 * title data.  Returns 0 on success, 1 on a loader/mapping/gate failure, 2
 * when the guest run cannot be set up, or -1 on non-AArch64 hosts. */
int botw_core_guest_image_selftest(void);

/* Human-readable result of the last botw_core_guest_image_selftest(), for
 * the debug report the Swift shell writes to selftest.txt. */
const char* botw_core_guest_image_selftest_report(void);

#ifdef __cplusplus
}
#endif
#endif /* BOTW_CORE_H */

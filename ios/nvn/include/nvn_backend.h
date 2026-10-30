#ifndef NVN_BACKEND_H
#define NVN_BACKEND_H

#include "nvn_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A backend maps NVN entry-point names to host implementations.
 *
 * There are two of them:
 *   null   -- resolves nothing.  Portable; used by the ABI test and by
 *             headless CI so the registry/dispatch can be exercised with no
 *             Apple frameworks around.
 *   metal  -- resolves whatever the Metal translation implements.
 *
 * `resolve` returns NULL for anything it does not own; the dispatcher then
 * installs a logging stub, so a missing proc shows up as one stderr line
 * instead of a crash.
 */
typedef struct nvngfx_backend {
    const char* name;
    void* user;
    nvngfx_proc (*resolve)(void* user, const char* name);
} nvngfx_backend;

/* Installs `backend` and binds every registry entry.  Must be called once,
 * before nvngfx_lookup().  NULL installs the null backend. */
void nvngfx_install(const nvngfx_backend* backend);

/* 1 if the installed backend really implements `name`. */
int nvngfx_is_implemented(const char* name);

/* Number of known entry points (== NVN_PROC_COUNT). */
int nvngfx_count(void);

/* Where "resolved a proc nobody implements" goes.  Called at most once per
 * name per install(); NULL restores the default (one stderr line each).
 * Note it fires on *resolve*, which is the only point where the name is
 * still known -- the platform ABI is signature-agnostic, so the stub itself
 * cannot tell which entry point it stands for. */
void nvngfx_set_missing_reporter(void (*fn)(const char* name));

/* --- backends ------------------------------------------------------- */
/* Borrowed pointers to static storage; never NULL. */
const nvngfx_backend* nvngfx_null_backend(void);

#if defined(__APPLE__)
const nvngfx_backend* nvngfx_metal_backend(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NVN_BACKEND_H */


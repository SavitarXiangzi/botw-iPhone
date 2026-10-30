#ifndef NVN_METAL_H
#define NVN_METAL_H

#include "nvn_api.h"
#include "nvn_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Apple-only entry points into the Metal backend.
 *
 * Order at startup:
 *   nvngfx_ctx* ctx = nvngfx_metal_create();
 *   nvngfx_install(nvngfx_metal_backend());
 *   ... run the title, passing `ctx` as the first argument of every proc ...
 *   nvngfx_metal_destroy(ctx);
 *
 * The context owns the MTLDevice and MTLCommandQueue.  It is deliberately
 * opaque: nothing outside nvn_metal.mm may depend on its layout. */
nvngfx_ctx* nvngfx_metal_create(void);
void nvngfx_metal_destroy(nvngfx_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* NVN_METAL_H */

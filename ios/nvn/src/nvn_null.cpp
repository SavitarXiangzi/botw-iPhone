/* Null backend: knows no entry points.
 *
 * Purpose is narrow and useful: it lets the dispatch table, the generated
 * name list and the "missing proc" path be compiled and tested on any host
 * (including Windows, where the iOS toolchain is unavailable).  If the ABI
 * test passes against this, the registry is consistent; whether Metal
 * implements a given proc is a separate, human-tracked question.
 */
#include "nvn_backend.h"

static nvngfx_proc null_resolve(void* user, const char* name) {
    (void)user;
    (void)name;
    return 0;
}

static const nvngfx_backend k_null = {"null", 0, null_resolve};

extern "C" const nvngfx_backend* nvngfx_null_backend(void) {
    return &k_null;
}

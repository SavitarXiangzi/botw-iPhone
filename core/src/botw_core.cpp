#include "botw_core.h"

#include "nvn_backend.h"

#include <cstdio>
#include <cstring>

#if defined(__APPLE__)
#include "nvn_metal.h"
#endif

struct botw_core {
    botw_core_desc desc{};
    uint64_t frame = 0;
    int nvn_total = 0;
    int nvn_ready = 0;
    const char* backend = "none";
    char error[256] = "core stub: no title data linked yet";

#if defined(__APPLE__)
    nvngfx_ctx* nvn_ctx = nullptr;
#endif
};

extern "C" botw_core* botw_core_create(const botw_core_desc* d) {
    if (!d) return nullptr;
    auto* c = new botw_core();
    c->desc = *d;

#if defined(__APPLE__)
    c->nvn_ctx = nvngfx_metal_create();
    nvngfx_install(nvngfx_metal_backend());
    c->backend = "metal";
#else
    nvngfx_install(nvngfx_null_backend());
    c->backend = "null";
#endif

    c->nvn_total = nvngfx_count();
    int ready = 0;
    for (int i = 0; i < c->nvn_total; ++i) {
        if (nvngfx_is_implemented(nvngfx_proc_name(static_cast<nvn_proc_index>(i)))) ++ready;
    }
    c->nvn_ready = ready;

    std::snprintf(c->error, sizeof(c->error),
                  "core stub: %d/%d NVN procs on %s backend", ready, c->nvn_total, c->backend);
    return c;
}

extern "C" void botw_core_destroy(botw_core* c) {
    if (!c) return;
#if defined(__APPLE__)
    nvngfx_metal_destroy(c->nvn_ctx);
#endif
    delete c;
}

extern "C" int botw_core_frame(botw_core* c, double) {
    if (!c) return -1;
    c->frame++;
    return 0;
}

extern "C" uint64_t botw_core_frame_index(const botw_core* c) { return c ? c->frame : 0; }
extern "C" const char* botw_core_last_error(const botw_core* c) { return c ? c->error : "null"; }

extern "C" void botw_core_nvn_stats_get(const botw_core* c, botw_core_nvn_stats* out) {
    if (!out) return;
    out->total = c ? static_cast<uint32_t>(c->nvn_total) : 0u;
    out->implemented = c ? static_cast<uint32_t>(c->nvn_ready) : 0u;
    out->backend = c ? c->backend : "none";
}

extern "C" int botw_core_nvn_has(const botw_core* c, const char* name) {
    if (!c || !name) return 0;
    return nvngfx_is_implemented(name) ? 1 : 0;
}

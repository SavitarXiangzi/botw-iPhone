#include "botw_core.h"

#include "nvn_backend.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(__APPLE__)
#include "nvn_metal.h"
#endif

struct botw_core {
    botw_core_desc desc{};
    uint64_t frame = 0;
    unsigned presents = 0;
    int nvn_total = 0;
    int nvn_ready = 0;
    const char* backend = "none";
    char error[256] = "core stub: no title data linked yet";

#if defined(__APPLE__)
    nvngfx_ctx* nvn_ctx = nullptr;
    float phase = 0.0f;
#endif
};

extern "C" botw_core* botw_core_create(const botw_core_desc* d) {
    if (!d) return nullptr;
    auto* c = new botw_core();
    c->desc = *d;

#if defined(__APPLE__)
    c->nvn_ctx = nvngfx_metal_create();
    /* Share the app's device and queue, so guest textures and drawables live on
     * the same device the view renders with.  Then hand the backend the layer
     * the app actually shows -- without it the guest's swapchain would present
     * into a surface nobody can see. */
    nvngfx_metal_set_device(c->nvn_ctx, d->metal_device, d->metal_command_queue);
    nvngfx_metal_set_layer(c->nvn_ctx, d->native_layer);
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

extern "C" int botw_core_frame(botw_core* c, double dt) {
    if (!c) return -1;
    c->frame++;
#if defined(__APPLE__)
    /* No title code is running yet, so the only honest frame source is the
     * host: clear the swapchain and present it.  This proves
     * layer -> drawable -> command buffer -> display works on the device and
     * makes `presents` a number the HUD can show.  When the ARM64 core is
     * linked in, this branch is what it replaces. */
    if (c->nvn_ctx) {
        c->phase += static_cast<float>(dt) * 0.7f;
        const float r = 0.5f + 0.5f * static_cast<float>(std::sin(c->phase));
        const float g = 0.5f + 0.5f * static_cast<float>(std::sin(c->phase + 2.0944));
        const float b = 0.5f + 0.5f * static_cast<float>(std::sin(c->phase + 4.1888));
        if (nvngfx_metal_present_clear(c->nvn_ctx, r, g, b)) c->presents++;
    }
#else
    (void)dt;
#endif
    return 0;
}

extern "C" uint64_t botw_core_frame_index(const botw_core* c) { return c ? c->frame : 0; }
extern "C" const char* botw_core_last_error(const botw_core* c) { return c ? c->error : "null"; }

extern "C" void botw_core_nvn_stats_get(const botw_core* c, botw_core_nvn_stats* out) {
    if (!out) return;
    out->total = c ? static_cast<uint32_t>(c->nvn_total) : 0u;
    out->implemented = c ? static_cast<uint32_t>(c->nvn_ready) : 0u;
    out->backend = c ? c->backend : "none";
    out->presents = c ? c->presents : 0u;
}

extern "C" int botw_core_nvn_has(const botw_core* c, const char* name) {
    if (!c || !name) return 0;
    return nvngfx_is_implemented(name) ? 1 : 0;
}

extern "C" int botw_core_selftest(void) {
#if defined(__APPLE__)
    return nvngfx_metal_selftest();
#else
    return -1;
#endif
}

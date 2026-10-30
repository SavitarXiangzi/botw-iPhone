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

/* Adopts the app's MTLDevice and MTLCommandQueue (bridged, not retained: the
 * app owns them).  Call this before nvngfx_metal_set_layer so commands and
 * drawables come from the same device the view uses.  Passing NULL for the
 * queue makes the backend create its own from the device. */
void nvngfx_metal_set_device(nvngfx_ctx* ctx, void* mtlDevice, void* mtlCommandQueue);

/* Binds the CAMetalLayer the app displays (bridged, not retained: the view
 * owns it).  The NVN window then presents into it instead of a private layer.
 * May be called before or after nvnWindowInitialize. */
void nvngfx_metal_set_layer(nvngfx_ctx* ctx, void* caMetalLayer);

/* Host-side swapchain self-test: clear + present one frame with no guest
 * command stream at all.  Returns 1 if a frame reached the display.
 *
 * This exists to prove the layer / drawable / command-buffer / present path on
 * a real device before the title produces any commands.  It is not the game
 * rendering; it is the harness that says "the swapchain works, the rest is
 * missing". */
int nvngfx_metal_present_clear(nvngfx_ctx* ctx, float r, float g, float b);

/* Executes one complete frame through the guest-facing entry points:
 * texture builder -> nvnTextureInitialize -> BeginRecording -> SetRenderTargets
 * -> ClearColor -> BindProgram -> DrawArrays -> EndRecording, then blits the
 * target into shared memory and checks the centre pixel.
 *
 * Returns 0 on success, otherwise a bitmask of the step that failed.  Runs on
 * any Metal device, the iOS Simulator included, which is what makes it a CI
 * regression test instead of a manual check.
 *
 * The program it draws is the built-in MSL from nvn_shader.h reached through
 * the same nvngfx_shader_to_msl() seam a translator will use -- it proves the
 * Metal half, not that a guest shader can be translated. */
int nvngfx_metal_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* NVN_METAL_H */

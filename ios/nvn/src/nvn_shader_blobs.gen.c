/* GENERATED FILE -- written by scripts/shader_chain/gen_blobs.py.
 *
 * The committed copy installs no blobs: this repository never contains game
 * data.  With a game at hand, run the chain and this file:
 *
 *   python scripts/shader_chain/nx_shader.py extract <pack> --out build/shader-chain/work
 *   python scripts/shader_chain/nx_shader.py translate --work build/shader-chain/work \
 *       --out build/shader-chain/msl --xlate ... --spirv-cross ...
 *   python scripts/shader_chain/gen_blobs.py --msl build/shader-chain/msl \
 *       --out ios/nvn/src/nvn_shader_blobs.gen.c
 *
 * Keep the generated file out of commits: it is derived from the title.
 */
#include "nvn_shader.h"

void nvngfx_shader_install_offline_blobs(void) {
    nvngfx_shader_set_blobs(0, 0);
}

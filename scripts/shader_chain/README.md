# scripts/shader_chain — 离线着色器链

把游戏里的 Maxwell 机器码翻成 MSL。完整原理、已验证的事实和还缺的步骤见
[`docs/SHADER_CHAIN.md`](../../docs/SHADER_CHAIN.md)。

| 文件 | 作用 |
|---|---|
| `build_tools.ps1` | 在本机构建 `dmsh_xlate`（.NET 10）和 `spirv-cross`（固定 commit） |
| `nx_shader.py` | `extract` / `translate` / `check-tools` 三个子命令 |
| `gen_blobs.py` | 把 `<key>_<stage>.metal` 目录写成 C 表或可替换的文件 bundle |
| `keycheck.cpp` | 用运行时的 C 函数重算真实 shader 的 key，和文件名逐个对账 |

```powershell
powershell -File scripts/shader_chain/build_tools.ps1 -Src <nxnative checkout>

C:\Python314\python.exe scripts\shader_chain\nx_shader.py extract <pack|dir> --out build\shader-chain\work `
  --keep-full-code
C:\Python314\python.exe scripts\shader_chain\nx_shader.py translate `
  --work build\shader-chain\work --out build\shader-chain\msl `
  --xlate build\shader-chain\tools\xlate\dmsh_xlate.exe `
  --spirv-cross build\shader-chain\tools\spirv-cross.exe

# 3) 对账：运行时的 C 函数算出的 key 必须和文件名一致（5102/5102）
clang++ -std=c++20 -O1 scripts/shader_chain/keycheck.cpp `
  ios/nvn/src/nvn_shader_key.c -I ios/nvn/include -o build/keycheck
build\keycheck.exe build\shader-chain\work\code

# 4a) 让 App 按 key 找到这些 MSL（覆盖掉入库的空表）
C:\Python314\python.exe scripts\shader_chain\gen_blobs.py --msl build\shader-chain\msl `
  --out ios\nvn\src\nvn_shader_blobs.gen.c

# 4b) 或者生成可在设备上替换的 bundle（M3.90）
C:\Python314\python.exe scripts\shader_chain\gen_blobs.py --msl build\shader-chain\msl `
  --bundle-out build\shader-chain\shaders
# 把 shaders/ 整个目录拷到 BotW.app/shaders，或设 NVNGFX_SHADER_DIR 指向它。
```

约定：

* 产物只在 `build/`（已 gitignore）里，**任何游戏内容都不入库**；
* 入口名由脚本改成 `nvngfx_main_vs` / `nvngfx_main_fs`，与 `nvn_shader.h` 的宏一致；
* `--keep-full-code` 额外写 `<key>_<stage>.code`（**带** 0x30 头的完整 code area）。
  key 哈希的是带头的整段，`keycheck.cpp` 需要它；交给翻译器的 `.bin` 仍然是
  去掉头的，两者别混；
* `gen_blobs.py --bundle-out` 写出的 `manifest.tsv` / `.msl` / `.c1` 也只在
  `build/` 里使用，**不要提交**；`tests/nvn_shader_bundle_test.cpp` 使用合成 MSL，
  可以入库；
* `gen_blobs.py` 写出的 `ios/nvn/src/nvn_shader_blobs.gen.c` **同样不要提交**：
  入库的那一份必须是空表（只调用 `nvngfx_shader_set_blobs(0, 0)`），
  提交前用 `git checkout -- ios/nvn/src/nvn_shader_blobs.gen.c` 还原；
* 最后一步 `xcrun metal` 只能在 macOS 上做，本脚本不做、也不假装做了。

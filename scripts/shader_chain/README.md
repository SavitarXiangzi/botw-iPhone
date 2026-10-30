# scripts/shader_chain — 离线着色器链

把游戏里的 Maxwell 机器码翻成 MSL。完整原理、已验证的事实和还缺的步骤见
[`docs/SHADER_CHAIN.md`](../../docs/SHADER_CHAIN.md)。

| 文件 | 作用 |
|---|---|
| `build_tools.ps1` | 在本机构建 `dmsh_xlate`（.NET 10）和 `spirv-cross`（固定 commit） |
| `nx_shader.py` | `extract` / `translate` / `check-tools` 三个子命令 |

```powershell
powershell -File scripts/shader_chain/build_tools.ps1 -Src <nxnative checkout>

C:\Python314\python.exe scripts\shader_chain\nx_shader.py extract <pack|dir> --out build\shader-chain\work
C:\Python314\python.exe scripts\shader_chain\nx_shader.py translate `
  --work build\shader-chain\work --out build\shader-chain\msl `
  --xlate build\shader-chain\tools\xlate\dmsh_xlate.exe `
  --spirv-cross build\shader-chain\tools\spirv-cross.exe
```

约定：

* 产物只在 `build/`（已 gitignore）里，**任何游戏内容都不入库**；
* 入口名由脚本改成 `nvngfx_main_vs` / `nvngfx_main_fs`，与 `nvn_shader.h` 的宏一致；
* 最后一步 `xcrun metal` 只能在 macOS 上做，本脚本不做、也不假装做了。

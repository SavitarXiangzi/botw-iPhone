# BOTW iOS 移植 · 本机软件与工具链清单

> 核查日期：2026-11-03
> 主机：Windows，无 Apple Silicon Mac、无 Xcode
> 仓库：`E:\平台移植\botw-iPhone`，分支 `m4/guest-run`
> 用途：下一次开工前确认“哪些软件必须保留、哪些是本次新增、哪些可以删除或重装”。

## 0. 先看结论

1. **E 盘没有通过 Windows 安装程序安装的软件。** 按注册表 `InstallLocation` 查询，位于 `E:` 的安装项为 0。E 盘只有源码、密钥、便携工具和其他项目文件。
2. **C 盘才是工具链所在。** 项目实际使用 Visual Studio 2022、Windows SDK、Python 3.14、CMake、Git、LLVM、GnuWin32 Make 和 WinLibs。
3. **本次项目期间新增的大头**：LLVM 约 2.98 GiB、WinLibs 约 0.90 GiB、`.NET 10` 本地 SDK 约 0.75 GiB、nxnative/winchain 约 0.72 GiB，合计约 5.4 GiB。这里不含游戏数据，也不含此前已安装的 VS/Python/CMake/Git。
4. **必须保留**：`C:\botw-work\data\switch\1.6.0\secure`、`C:\botw-work\keys`、`E:\平台移植\ProdKeys.NET-v22.5.0`；以及 VS2022 + Windows SDK + Python + CMake + Git。
5. **可以清理或重下**：Sideloadly 残留、`C:\botw-work\repo` / `port` 早期快照、`logs`、构建产物；如果暂时不做 shader 链，`.NET 10` 和 `C:\botw-work\nxsrc` 也可以重下。
6. **`C:\msys64` 已经不存在。** 旧交接文档里“约 0.30 GiB、可卸载”的说法已过时，不要再找它。
7. **当前没有可用的签名/IPA 工具链。** 没有 Xcode、`codesign`、`ldid`、iTunes/iCloud、`ideviceinstaller`；Sideloadly 只有空目录和 AppData 残留。

当前磁盘：

| 盘符 | 已用 | 剩余 |
|---|---:|---:|
| C: | 191.71 GiB | 6.94 GiB |
| E: | 22.28 GiB | 28.03 GiB |

## 1. 统计口径

这份清单把“软件”分为四类，避免把游戏数据和源码误认为安装软件：

| 类别 | 例子 | 是否计入“已安装软件” |
|---|---|---|
| 系统级安装 | VS2022、Python、CMake、Git、LLVM、WinLibs | 是 |
| 项目局部运行时/工具 | `C:\botw-work\dotnet10`、`nxsrc\winchain` 里的 `dmsh_xlate.exe` / `spirv-cross.exe` | 否，但必须单独记录 |
| 便携工具 | `E:\tools`、MobaXterm、`ProdKeys.NET` 目录里的密钥 | 否 |
| 源码/数据/脚本 | `botw-iPhone`、`C:\botw-work\data`、`C:\botw-work\tools` | 否 |

安装日期来自 Windows 卸载注册表；项目局部目录的日期来自文件系统创建时间，只能说明“何时下载/生成”，不等于安装程序写入时间。

## 2. C 盘：系统级安装

### 2.1 与本项目直接相关

| 软件 | 版本 | 位置 | 安装日期 | 占用 | 项目用途 | 建议 |
|---|---|---|---:|---:|---|---|
| Visual Studio Community 2022 | 17.14.39 | `C:\Program Files\Microsoft Visual Studio\2022\Community` | 2025-09-18 | 7.39 GiB | `cl.exe`、`link.exe`、`vcvars64.bat`，Windows portable 门禁 | **保留** |
| Windows SDK | 10.1.26100.7705 | `C:\Program Files (x86)\Windows Kits\10` | 2026-08-21 | 2.25 GiB | Windows 头文件/库/调试工具 | **保留** |
| Python | 3.14.2 | `C:\Python314\python.exe` | 2025-12-11 | 1.86 GiB | 所有 `scripts/*.py`、门禁、NSO 分析 | **保留** |
| CMake | 4.1.1 | `C:\Program Files\CMake\bin\cmake.exe` | 2025-09-19 | 0.12 GiB | 构建 `spirv-cross`（shader 链） | **保留** |
| Git | 2.54.0 | `C:\Program Files\Git\cmd\git.exe` | 2026-04-23 | 0.40 GiB | 仓库、拉取、提交 | **保留** |
| LLVM | 23.1.4 | `C:\Program Files\LLVM\bin\clang.exe` | **2026-10-30** | 2.98 GiB | `clang++` 做 `nvn_metal.mm` 语法检查；也带 `llvm-objdump`、`ld.lld` | 开发期保留；空间紧张可卸载，CI 仍有 clang |
| GnuWin32 Make | 3.81 | `C:\Program Files (x86)\GnuWin32\bin\make.exe` | 2025-11-14 | 0.01 GiB | 旧式 make 命令；当前主门禁主要用 MSVC | 保留，体积很小 |
| WinLibs POSIX/UCRT | 16.2.0-14.0.0-r3 | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin` | **2026-10-28** | 0.90 GiB | 提供 `gcc`、`g++`、`ninja`、`nasm`；当前 `botw-iPhone` 主门禁未直接调用 | 可选；只维护当前仓库可卸载 |

补充：

- LLVM 不在 `PATH` 里。`scripts\syntax_check_metal.py` 会优先用绝对路径 `C:\Program Files\LLVM\bin\clang++.exe`，所以命令行直接敲 `clang++` 可能找不到。
- `build_abi_test.ps1` 在没有 clang 时会**跳过** Metal 语法检查，不会报失败；不要把“跳过”当成通过。
- Visual Studio 内的 `VC\Tools\Llvm\x64\bin` 只有 `clang-format.exe` / `clang-tidy.exe`，没有完整 `clang++.exe`；真正做语法检查的是独立 LLVM。
- WinLibs 提供 `gcc 16.2.0`、`ninja 1.13.2`、`nasm`。当前仓库构建 `spirv-cross` 用的是 CMake + VS2022 的 MSVC 生成器，不是 MinGW；所以这 0.90 GiB 不是当前主门禁的硬依赖。

### 2.2 C 盘：已存在但容易被误认为项目依赖

| 软件 | 版本 | 位置 | 说明 |
|---|---|---|---|
| .NET SDK / Runtime | SDK 9.0.317；Runtime 8.0.20 / 8.0.30 / 9.0.19 | `C:\Program Files\dotnet`（1.17 GiB） | 系统 `dotnet` 只有 9.x；`dmsh_xlate` 需要 .NET 10，直接运行会报缺少 `Microsoft.NETCore.App 10.0.0` |
| Python | 3.13.5 | `C:\Python313` | 另一个 Python；项目脚本默认用 `C:\Python314\python.exe`，不要混用 |
| Node.js | 24.15.0 | `C:\Program Files\nodejs`（0.10 GiB） | 与本项目无关 |
| Qt | 6.8.3（注册表另有 Qt 4.10.0 记录） | `C:\Qt` | 与本项目无关 |
| Zig | 0.16.0 | WinGet 包目录（约 0.34 GiB） | 与本项目无关 |
| WinRAR | 7.22 | `C:\Program Files\WinRAR` | 解压工具；当前 `7z` 命令不可用，必要时可用 WinRAR 或 Python |
| Sideloadly | 无可用版本 | `C:\Program Files\Sideloadly` | **目录为空**；只有 `%LOCALAPPDATA%\sideloadly\installations.db` 等残留；没有 exe、没有注册表项，当前不能用 |
| 夸克网盘 | 7.4.2.822 | `C:\Program Files\QuarkCloudDrive` | 上传/同步工具，不是 BOTW 项目依赖 |

### 2.3 C 盘：本次项目新增的安装

| 项目 | 日期 | 占用 | 性质 |
|---|---:|---:|---|
| LLVM 23.1.4 | 2026-10-30 | 2.98 GiB | 系统级安装，winget |
| WinLibs POSIX/UCRT 16.2.0 | 2026-10-28 | 0.90 GiB | 系统级安装，winget |
| `C:\botw-work\dotnet10` | 2026-10-30 | 0.75 GiB | 项目局部 .NET 10 SDK，非系统安装 |
| `C:\botw-work\nxsrc` | 2026-10-29 ~ 10-30 | 0.72 GiB | nxnative 源码快照 + 本地 shader 工具 |
| Python `capstone` + `unicorn` | 2026-10-30 | 约 0.07 GiB | 装进 `C:\Python314\Lib\site-packages` |
| Sideloadly 残留 | 2026-10-30 | 约 0 | 安装未完成/程序文件已不在 |

合计约 **5.4 GiB**（不含游戏数据，也不含此前已有的 VS/Python/CMake/Git）。

## 3. C 盘：项目局部工具，不是系统安装

### 3.1 `C:\botw-work\dotnet10`

| 项 | 值 |
|---|---|
| 路径 | `C:\botw-work\dotnet10\dotnet.exe` |
| SDK | 10.0.403 |
| Runtime | 10.0.14 |
| 占用 | 0.75 GiB |
| 用途 | `dmsh_xlate`（Maxwell SASS → SPIR-V）和 `scripts/shader_chain/build_tools.ps1` 需要 .NET 10 |
| 是否在 PATH | 否；系统 `dotnet` 仍是 9.0.317 |

直接运行 `dmsh_xlate.exe` 时，必须先让 .NET 10 可见：

```powershell
$env:DOTNET_ROOT = 'C:\botw-work\dotnet10'
$env:PATH = 'C:\botw-work\dotnet10;' + $env:PATH
$job = 'build\shader-chain\work\job.json'
$out = 'build\shader-chain\out'
& 'C:\botw-work\nxsrc\winchain\xlate\dmsh_xlate.exe' $job $out
```

或者构建时代码显式传入：

```powershell
powershell -File scripts\shader_chain\build_tools.ps1 `
  -Src C:\botw-work\nxsrc\nxnative-git `
  -Dotnet C:\botw-work\dotnet10\dotnet.exe
```

### 3.2 `C:\botw-work\nxsrc`

| 子目录 | 大小 | 内容 | 说明 |
|---|---:|---|---|
| `nxnative-git` | 329.93 MiB | nxnative Git 快照，HEAD `d1ba3e9` | shader 工具源码来源 |
| `nxnative-main` | 300.36 MiB | nxnative 源码快照，无 `.git` | 备用/对照 |
| `winchain` | 约 0.10 GiB | SPIRV-Cross 源码/构建 + Ryujinx 翻译器 | 本地实验构建，见下 |

`winchain` 里已经有两个可直接使用的本地工具：

| 工具 | 路径 | 版本/说明 |
|---|---|---|
| `dmsh_xlate.exe` | `C:\botw-work\nxsrc\winchain\xlate\dmsh_xlate.exe` | .NET 10，Ryujinx shader 翻译库，Maxwell → SPIR-V；162,304 字节 |
| `spirv-cross.exe` | `C:\botw-work\nxsrc\winchain\build\Release\spirv-cross.exe` | 固定 `vulkan-sdk-1.4.363.0`，commit `f11ba9f...`；2,271,744 字节 |

注意：

- 仓库里的 `build\shader-chain\tools\` 当前不存在；上面两个是 `C:\botw-work\nxsrc\winchain` 里的本地实验产物。
- 重新构建时，`build_tools.ps1` 会把工具输出到仓库的 `build\shader-chain\tools\`，不会覆盖 `winchain`。
- `dmsh_xlate.exe` 不是 `--help` 风格的工具，它的用法是 `dmsh_xlate.exe job.json out-dir`；传 `--help` 会被当成文件路径。

### 3.3 其他项目局部目录

| 路径 | 大小 | 内容 | 是否软件 |
|---|---:|---|---|
| `C:\botw-work\tools` | 0.05 GiB | Unicorn/M2/M3 分析脚本、日志、辅助 Python | 否，纯脚本 |
| `C:\botw-work\repo` | 很小 | 早期源码快照 | 否 |
| `C:\botw-work\port` | 很小 | 早期 iOS 骨架快照 | 否 |
| `C:\botw-work\m1` | 0.06 GiB | `main/sdk/subsdk0/rtld` 的 `.reloc.bin` | 否，产物 |
| `C:\botw-work\inventory` | 0.01 GiB | imports/symbols/shader 清单 | 否，产物 |
| `C:\botw-work\logs` | 0.05 GiB | 历史运行日志 | 否 |
| `C:\botw-work\runs` | 很小 | 运行目录 | 否 |
| `C:\botw-work\keys` | 很小 | `prod_hactool.keys` | **敏感材料**，不是软件 |
| `C:\botw-work\data` | 44.76 GiB | 游戏数据（secure/RomFS/DLC 等） | 否，游戏内容 |

游戏数据明细见同目录的 `HANDOFF_CURRENT.md` 第 2 节；这里只强调：`C:\botw-work\tools` 里没有 `.exe` / `.dll`，它是 Python 脚本集合，不是安装的程序。

## 4. Python 包

项目使用系统 Python `C:\Python314`，没有为这个仓库创建独立 venv。

| 包 | 版本 | 安装时间 | 用途 | 状态 |
|---|---|---|---:|---|---|
| `capstone` | 5.0.9 | 2026-10-30 | ARM64 反汇编、NSO/重定位分析 | 已安装 |
| `unicorn` | 2.1.4 | 2026-10-30 | Windows 侧 Unicorn 客体冒烟/指令跟踪 | 已安装 |
| `pycryptodomex` | 3.24.0 | 2026-10-09 | NCA/AES 解密相关 | 已安装 |
| `cryptography` | 50.0.1 | 2026-09-20 | 通用加密/证书 | 已安装 |
| `numpy` | 2.3.5 | 2026-03-27 | 数值/二进制分析 | 已安装 |
| `pillow` | 12.1.1 | 2026-03-27 | 图像处理/截图 | 已安装 |
| `PyYAML` | 6.0.3 | 2026-09-20 | 配置解析 | 已安装 |
| `requests` | 2.32.5 | 2026-03-27 | HTTP/下载 | 已安装 |
| `zstandard` | 0.25.0 | 2026-08-27 | zstd 解压 | 已安装 |
| `lz4` | — | — | `C:\botw-work\tools\inventory_modules.py` 需要 | **未安装** |

如果重新跑 M0 的 NSO/模块清点，先补：

```powershell
C:\Python314\python.exe -m pip install lz4
```

如果要从零恢复核心分析环境：

```powershell
C:\Python314\python.exe -m pip install capstone==5.0.9 unicorn==2.1.4 pycryptodomex==3.24.0
```

## 5. E 盘：没有系统安装，只有源码/密钥/便携工具

按注册表 `InstallLocation` 查询，E 盘安装项为 **0**。实际内容：

| 路径 | 大小 | 内容 | 与本项目关系 |
|---|---:|---|---|
| `E:\平台移植\botw-iPhone` | 37.15 MiB | 当前权威源码仓库 | 核心 |
| `E:\平台移植\d2r-mobile-analysis` | 41.61 MiB | D2R Mobile 参考源码/分析 | 参考 |
| `E:\平台移植\ProdKeys.NET-v22.5.0` | 0.02 MiB | **只有 `prod.keys` / `title.keys`，没有 exe** | 密钥，敏感 |
| `E:\tools` | 545.78 MiB | Android `platform-tools`、`scrcpy`、`lanhu-mcp`、`xgm_framework` | 与本项目无关 |
| `E:\Program Files` | — | `MobaXterm_Personal_24.3.exe` 等便携文件 | 与本项目无关 |
| `E:\平台移植` 根目录 | 很小 | `_patch_*.py`、`_analyze*.py` 等临时脚本 | 工作产物，不是软件 |

结论：

- **E 盘没有需要卸载的 BOTW 工具链软件。**
- `ProdKeys.NET-v22.5.0` 这个目录名容易误解：它不是已安装的 ProdKeys.NET 程序，只是密钥文件目录。
- E 盘剩余 28.03 GiB，适合放源码、文档、密钥的加密备份；不要放明文密钥的公开分享链接。

## 6. 当前缺失或不可用的工具

| 工具 | 状态 | 影响 |
|---|---|---|
| Xcode / `xcodebuild` / `xcrun` | 未安装 | Windows 不能本地构建 iOS App；只能靠 GitHub Actions macOS runner |
| `codesign` / `ldid` | 未安装 | 不能本地签名 IPA |
| iTunes / iCloud / Apple Mobile Device | 未安装 | Sideloadly 即使重装也可能缺少设备驱动 |
| `ideviceinstaller` / `iproxy` | 未安装 | 不能命令行安装/转发 iOS 设备 |
| Sideloadly 可执行文件 | 未找到 | 当前无法用 Sideloadly 安装 IPA |
| `hactool` | 常见目录未发现 | 已有 `secure`/`RomFS` 不受影响；重新解包前要另备工具 |
| `7z` | 未安装/不在 PATH | 可用 WinRAR 或 Python 解压 |
| `gh` | 未安装 | GitHub Actions 状态用 API/网页查看 |
| Docker | 未安装 | 本项目不需要 |
| Java | 未安装 | 本项目不需要 |
| Swift | 未安装 | iOS 编译在 macOS runner 上做 |
| WSL Linux 发行版 | 未安装（只有 `wsl.exe`） | 本项目不需要；portable 测试在 Windows/MSVC 跑 |

因此，**当前机器不能生成或安装签名 IPA**。即使补上 Sideloadly，也必须先有 macOS/Xcode 产出的未签名 `.app`/`.ipa` 和 Apple 签名材料；这与“游戏能否运行”是两件事。

## 7. 保留/清理建议

### 7.1 必须保留

| 路径 | 原因 |
|---|---|
| `C:\Program Files\Microsoft Visual Studio\2022\Community` | Windows 编译核心，删了 `build_abi_test` 直接失败 |
| `C:\Program Files (x86)\Windows Kits\10` | Windows SDK 头文件/库 |
| `C:\Python314` | 所有项目脚本的默认解释器 |
| `C:\Program Files\CMake` | shader 链构建 |
| `C:\Program Files\Git` | 仓库操作 |
| `C:\botw-work\data\switch\1.6.0\secure` | XCI 已删除后的主 NCA 副本，不能随意删 |
| `C:\botw-work\keys` | 重新解包/校验需要 |
| `E:\平台移植\ProdKeys.NET-v22.5.0` | 密钥副本，敏感但关键 |

### 7.2 继续做 shader 链时保留

| 路径 | 原因 | 如果删除 |
|---|---|---|
| `C:\Program Files\LLVM` | 本地 Metal 语法检查 | 可重装；CI 仍有 clang |
| `C:\botw-work\dotnet10` | `dmsh_xlate` 需要 .NET 10 | 可重下 .NET 10 SDK |
| `C:\botw-work\nxsrc\winchain` | 已有 `dmsh_xlate.exe` / `spirv-cross.exe` | 可重新 clone + 构建，约 0.10 GiB |
| `C:\botw-work\nxsrc\nxnative-git` | shader 工具源码 | 可重新 clone，约 330 MiB |

### 7.3 可以清理/重下

| 路径 | 大小 | 说明 |
|---|---:|---|
| `C:\Program Files\Sideloadly` + 三个 AppData 残留 | 约 0 | 当前无 exe，清理后可重装 |
| `C:\botw-work\repo` | 很小 | 早期源码快照；当前权威源码在 `E:\平台移植\botw-iPhone` |
| `C:\botw-work\port` | 很小 | 早期 iOS 骨架；当前权威源码在仓库里 |
| `C:\botw-work\logs` | 0.05 GiB | 历史日志；需要排查时再留 |
| `C:\botw-work\runs` | 很小 | 运行目录 |
| `C:\botw-work\build` / 仓库 `build` | 数百 MiB | 可重建；删前确认没有要保留的产物 |
| WinLibs | 0.90 GiB | 当前主门禁不直接用；只维护 `botw-iPhone` 可卸载 |
| `C:\botw-work\dotnet10` | 0.75 GiB | 不做 shader 链时可删，之后重下 |
| `C:\botw-work\nxsrc` | 0.72 GiB | 不做 shader 链时可删，之后重 clone/build |

### 7.4 不要删

- `C:\botw-work\data\switch\1.6.0\secure`（15.71 GiB，12 个文件）
- `C:\botw-work\keys`、`E:\平台移植\ProdKeys.NET-v22.5.0`
- `C:\botw-work\data\switch\1.6.0\ExeFS`、`Shaders`、`m1`、`inventory`、`tools`
- `RomFS` / `DLC` 在云端副本校验完成前也不要删；它们能从 `secure` 重建，但成本很高

### 7.5 关于 `C:\msys64`

2026-11-03 核查：**不存在**。旧 `HANDOFF_CURRENT.md` 中的“约 0.30 GiB，可用 uninstall.exe 卸载”已作废；不需要再执行任何 MSYS2 卸载操作。

## 8. 重装/恢复命令

### 8.1 winget 可重装的系统工具

```powershell
winget install --id LLVM.LLVM -e
winget install --id Kitware.CMake -e
winget install --id Git.Git -e
winget install --id BrechtSanders.WinLibs.POSIX.UCRT -e
winget install --id GnuWin32.Make -e
```

Visual Studio 2022 用 Visual Studio Installer 维护，至少保留：

- 工作负载：**使用 C++ 的桌面开发**
- 组件：MSVC v143、Windows 11 SDK `10.0.26100`、C++ CMake 工具（可选）

Python 3.14 当前来自 `C:\Python314` 的 pymanager 安装；重装时确保 `python --version` 指向 3.14，并重新安装第 4 节的包。

### 8.2 恢复 `.NET 10` 和 shader 工具

`.NET 10` 是项目局部 SDK，不在系统 PATH。重新获取后放到 `C:\botw-work\dotnet10`，确认：

```powershell
& 'C:\botw-work\dotnet10\dotnet.exe' --list-sdks
# 应看到 10.0.403
```

重新构建两个 shader 工具：

```powershell
cd E:\平台移植\botw-iPhone
powershell -File scripts\shader_chain\build_tools.ps1 `
  -Src C:\botw-work\nxsrc\nxnative-git `
  -Dotnet C:\botw-work\dotnet10\dotnet.exe
```

如果只想用现有的 `winchain` 产物：

```powershell
$env:DOTNET_ROOT = 'C:\botw-work\dotnet10'
$env:PATH = 'C:\botw-work\dotnet10;' + $env:PATH
$job = 'build\shader-chain\work\job.json'
$out = 'build\shader-chain\out'
& 'C:\botw-work\nxsrc\winchain\xlate\dmsh_xlate.exe' $job $out
& 'C:\botw-work\nxsrc\winchain\build\Release\spirv-cross.exe' --help
```

### 8.3 版本自检

```powershell
python --version
cmake --version
git --version
make --version
ninja --version
gcc --version

& 'C:\Program Files\LLVM\bin\clang.exe' --version
& 'C:\Program Files\dotnet\dotnet.exe' --list-sdks
& 'C:\botw-work\dotnet10\dotnet.exe' --list-sdks

Test-Path 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
Test-Path 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
```

预期：

- `python` = 3.14.x
- `cmake` = 4.1.1
- `git` = 2.54.x
- `make` = GNU Make 3.81
- `ninja` = 1.13.2
- `gcc` = 16.2.0
- LLVM clang = 23.1.4
- 系统 `dotnet` = 9.0.317
- 局部 `dotnet10` = 10.0.403
- `vcvars64.bat` = True
- Windows SDK include = True

### 8.4 重新生成这份清单

```powershell
# 系统安装项（按 E: 过滤可确认 E 盘没有安装软件）
$roots = @(
  'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
  'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
  'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*'
)
Get-ItemProperty $roots -ErrorAction SilentlyContinue |
  Where-Object { $_.DisplayName } |
  Select-Object DisplayName,DisplayVersion,InstallDate,InstallLocation |
  Sort-Object DisplayName -Unique

# winget 视角
winget list

# 项目局部工具
Get-ChildItem C:\botw-work\dotnet10\sdk
Get-ChildItem C:\botw-work\nxsrc\winchain\xlate
Get-ChildItem C:\botw-work\nxsrc\winchain\build\Release

# Python 包
C:\Python314\python.exe -m pip list
```

## 9. 与备份的关系

如果准备把 `C:\botw-work` 上传网盘：

- **必须传**：`data\switch\1.6.0\secure`、`ExeFS`、`Shaders`、`m1`、`inventory`、`tools`、`keys`（密钥要单独加密）。
- **建议传**：`data\switch\1.6.0\RomFS`、`DLC`（体积大，但重建成本高）。
- **可以排除**：`dotnet10`（0.75 GiB，可重下）、`nxsrc`（0.72 GiB，可重 clone/build）、`logs`、`runs`、`__pycache__`、构建产物。
- **不要传**：明文 `prod.keys` / `title.keys` / `prod_hactool.keys`；不要把密钥和公开分享链接放在一起。
- 源码仓库 `E:\平台移植\botw-iPhone` 已在 GitHub，通常不需要再放进网盘；但本地 `.git` 和未推送分支要保留。

### 9.1 `E:\平台移植` 的上传建议

`E:\平台移植` 当前约 **78.92 MiB / 3,470 个文件**，不是游戏数据，上传成本很低。建议按下表处理：

| 内容 | 是否上传 | 说明 |
|---|---|---|
| `botw-iPhone` | 建议上传一份 bundle | `main` 和 `m4/guest-run` 已在 GitHub；但本地分支 `m4/hle-registry` **没有推到远端** |
| `d2r-mobile-analysis` | 可不上传 | 是 `jhaizhou-ops/d2r-mobile` 的干净 clone，`git status` 干净，可重新 clone |
| `ProdKeys.NET-v22.5.0` | **必须上传，但只能加密私有** | 只有 `prod.keys` / `title.keys`，没有 exe；不能明文放公开链接 |
| 根目录 `_patch_*.py` / `_analyze*.py` | 建议上传 | 24 个脚本共约 88 KiB，成本可忽略 |
| 根目录 `README.md` | 可忽略 | 0 字节空文件 |
| `E:\tools`、`E:\Program Files` | 不要算进 BOTW 备份 | Android 工具/MobaXterm 等，与本项目无关 |

最省事的做法：

```powershell
cd E:\平台移植\botw-iPhone
git bundle create ..\botw-iPhone-all-branches.bundle --all
git bundle verify ..\botw-iPhone-all-branches.bundle
```

然后把 `botw-iPhone-all-branches.bundle`、根目录的 `_patch_*.py` / `_analyze*.py` 放到同一个“源码/脚本”压缩包；`ProdKeys.NET-v22.5.0` 单独用 WinRAR 加密压缩（勾选“加密文件名”），密码不要和压缩包放在一起，也不要写进文件名。当前机器没有 `7z` 命令，不要照搬 `7z` 命令。

如果不想处理本地分支，也可以直接：

```powershell
cd E:\平台移植\botw-iPhone
git push origin m4/hle-registry
```

推送后再把 GitHub 当作源码备份即可。

## 10. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-11-03 | 首次整理：核对 C/E 盘安装项、项目局部工具、Python 包、缺失工具、清理建议和 E 盘上传建议 |

> 维护规则：安装/卸载工具、改变 `.NET`/LLVM/Python 版本、移动 `C:\botw-work` 或更换机器后，更新本文件；不要只改 `HANDOFF_CURRENT.md` 而留下过时的软件清单。

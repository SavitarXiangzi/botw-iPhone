# BOTW iOS 移植项目 · 续作交接手册

> 快照日期：2026-11-03
> 适用仓库：`E:\平台移植\botw-iPhone`
> 远端：<https://github.com/SavitarXiangzi/botw-iPhone.git>
> 当前分支：`m4/guest-run`
> 代码验证快照：`3d896b8`（CI run 41559631519 success）


## 0. 先看结论

这是一个把《塞尔达传说：旷野之息》Switch 1.6.0 向 iOS 原生移植的研究工程，不是通用模拟器，也不是已经可玩的成品。

当前最重要的事实：

- iOS 壳、Metal 后端、客体装载器、重定位、线程/同步 HLE 已能编译和测试。
- 1.6.0 的 `main + sdk + subsdk0 + rtld` 已能在 Windows/portable 代码中完成交叉链接和重定位。
- iOS App 已经能执行内置 AArch64 探针，以及一个**合成** `patched.bin` 的映射/gate/DT_INIT 链。
- **真实标题还没有接进 iOS App**，iOS 上的真实 draw/present 仍为 0。
- **暂无签名和 IPA 导出流程**，不能直接生成可安装到普通 iPhone 的 `.ipa`。
- 当前最大的两堵墙是：
  1. 把四模块真实镜像映射进 App，并进入真实 `main` 的初始化；
  2. Maxwell SASS → Metal Shading Language 的着色器翻译器。

## 1. 固定坐标

| 项目 | 当前值 |
|---|---|
| 本地仓库 | `E:\平台移植\botw-iPhone` |
| GitHub | <https://github.com/SavitarXiangzi/botw-iPhone> |
| 当前分支 | `m4/guest-run` |
| 代码验证快照 | `3d896b8fa1cffaa4eebb187c3badd68347a774ff` |
| 上游跟踪 | `origin/m4/guest-run` |
| 默认分支 | `main`（当前停在旧提交，未合并 M4） |
| 标题 | BOTW 1.6.0，Title ID `01007EF00011E000`，version `v786432` |
| 本地平台 | Windows，无 Apple Silicon Mac / Xcode |
| iOS 构建验证 | GitHub Actions 的 macOS runner |

当前工作区应保持干净。新会话开始先执行：

```powershell
cd E:\平台移植\botw-iPhone
git status --short
git branch -vv
git pull --ff-only
```

如果 `git status` 有未提交改动，先确认是不是本轮工作，不要直接覆盖。

## 2. 数据资产与不可逆变化

### 2.1 XCI 已删除

原始 XCI 已从 `E:\平台移植` 删除，且用户已确认不需要再上传 XCI。

因此：

- `C:\botw-work\data\switch\1.6.0\secure` 现在是最接近原始 NCA 的主副本。
- **不要在没有云备份/其他副本的情况下删除 `secure`。**
- `RomFS` 和 `DLC` 可以从 `secure` + 密钥 + 解包工具重建，但重建成本高。
- 没有 `secure`、密钥或卡带，就无法从本机恢复完整游戏内容。

### 2.2 当前数据位置

| 路径 | 内容 | 体量/状态 |
|---|---|---:|
| `C:\botw-work\data\switch\1.6.0\secure` | 加密 NCA 原始内容 | 15.71 GiB，12 个文件 |
| `C:\botw-work\data\switch\1.6.0\RomFS` | 已解包 RomFS | 27.52 GiB，80,730 个文件 |
| `C:\botw-work\data\switch\1.6.0\DLC` | DLC1/DLC2 解包内容 | 1.29 GiB，7,095 个文件 |
| `C:\botw-work\data\switch\1.6.0\ExeFS` | 本体/更新 ExeFS，含四模块 NSO | 约 0.05 GiB |
| `C:\botw-work\data\switch\1.6.0\modules` | 转成分析用 `.bin` 的模块 | 约 0.11 GiB |
| `C:\botw-work\data\switch\1.6.0\Shaders` | `.sbfsha` + `.bin` | 约 70 MiB，18 个文件 |
| `C:\botw-work\m1` | `*.reloc.bin` 重定位产物 | 约 0.06 GiB |
| `C:\botw-work\inventory` | imports/symbols/shader 统计 | 约 11 MiB |
| `C:\botw-work\tools` | Unicorn/M2/M3 私有工具链 | 约 50 MiB |
| `C:\botw-work\keys\prod_hactool.keys` | 密钥材料 | 敏感，不要明文上传 |
| `E:\平台移植\ProdKeys.NET-v22.5.0` | `prod.keys` / `title.keys` | 敏感，仅本地使用 |

注意：`C:\botw-work\tmp` 当前为空，之前的 `tmp\rewrite160`、`rewrite160abs`、`rewrite160b` 已不存在。需要复跑真实模块时，从 `ExeFS\update` 重新生成。

### 2.3 密钥和云盘红线

- `prod.keys`、`title.keys`、`prod_hactool.keys` 绝不能提交到 Git。
- 上传网盘时不要把密钥明文放入公开分享链接；密钥应单独用强密码压缩。
- `secure`、`RomFS`、`DLC` 属于游戏内容，只应放在私有云盘。
- `.gitignore` 已忽略 `.xci`、`.nsp`、`.nca`、`.nso`、`.kdb`、`.keys`、`prod.keys`、`title.keys`、`RomFS/`。

### 2.4 磁盘空间

快照时：

- C 盘剩余约 6.96 GiB。
- E 盘剩余约 28.03 GiB。
- `C:\botw-work` 约 46.49 GiB。

不要在本地生成完整的 45 GB 级 ZIP；空间不足，而且 RomFS 已经有 8 万个小文件。优先用网盘客户端按目录上传。

`C:\msys64` 约 0.30 GiB，当前项目不使用它；可用其自带 `uninstall.exe` 卸载。释放空间有限，不能解决主要问题。

## 3. 仓库结构

```text
ios/BotW/          SwiftUI 壳、CAMetalLayer、CADisplayLink、自检
ios/nvn/           NVN host-HLE、分发表、Metal 后端、纹理/管线/着色器接口
core/guest/        客体装载、重定位、多模块链接、映射、线程、HLE
core/src/          Swift 可见的 C ABI 与自检实现
core/include/      botw_core.h
scripts/           NVN 生成、NSO 改写、重定位探测、门禁、shader 工具
tests/             portable、AArch64 探针、合成镜像、NVN/Metal 回归
docs/              路线图、移植手册、HLE 边界、NVN 覆盖、shader 链
.github/            Linux portable + macOS iOS CI
```

主要入口：

- `core/guest/guest_image.c`：单模块 ELF/patched.bin 解析、绑定、重定位。
- `core/guest/guest_modules.c`：四模块先绑定、建全局符号索引、再逐模块重定位。
- `core/guest/guest_map.c`：单模块可执行映射、按页保护、key page。
- `core/guest/guest_run.c`：客体栈、入口/退出哨兵、协作式线程。
- `core/guest/guest_hle_os.c`：`nn::os` 线程、TLS、时钟、同步对象。
- `ios/nvn/src/nvn_metal.mm`：Metal 后端主体。
- `ios/BotW/BotWApp.swift`：`BOTW_SELFTEST=1` / `--selftest` 入口。
- `tests/guest_modules_test.cpp`：多模块交叉链接的合成测试。

`C:\botw-work\repo` 和 `C:\botw-work\port` 是早期快照，不是当前源码；当前权威源码是 `E:\平台移植\botw-iPhone`。

## 4. 当前能力矩阵

| 层 | 状态 | 说明 |
|---|---|---|
| iOS 壳 | ✅ | SwiftUI + `CAMetalLayer` + `CADisplayLink` + HUD |
| C ABI | ✅ | `botw_core.h` 是 Swift 唯一入口 |
| NVN 分发 | ✅ | 当前门禁：514 个注册入口，484 个 Metal binding |
| Metal 合成渲染 | 🟡 | 纹理、管线、draw、compute、MRT、清屏、回读等合成测试通过 |
| NSO/patched 装载 | 🟡 | 单模块与四模块 portable 链接已通 |
| 四模块交叉链接 | ✅ | 真实 1.6.0 重定位已跑通，未执行真实指令 |
| iOS 客体执行 | 🟡 | 内置探针 + 合成镜像映射/DT_INIT 已验证 |
| 真实标题入口 | ❌ | `main.patched.bin` 入口尚未在 App 内调用 |
| 真实 iOS draw/present | ❌ | iOS 真实标题 draw/present 为 0 |
| 着色器翻译 | ❌ | SASS → MSL 还没有实现 |
| 输入/音频 | ❌ | M5 |
| 可安装 IPA | ❌ | 无签名、无 Archive/Export 流程 |
| Release 构建 | ❌（有意） | `kEnumsVerified = 0`，Release 门禁拒绝编译 |

## 5. 已经验证的事实

### 5.1 本地 Windows 门禁

最近一次本地结果：

```text
build_abi_test: all tests passed
guest_modules_test: ok (cross=2, hle=4, duplicate/missing refusals)
verify_ios: ok
generated artifacts up to date (514 procs, 484 implemented)
extract_nvn_signatures: ok (477 shapes, max arity 8)
test_gen_blobs: ok (ids, packed slots, bindless, c1, bundle, holes, refusals)
```

Windows x86 上会跳过 AArch64/mmap 探针；这些由 macOS arm64 CI 执行，不能把 “skipped” 误认为通过。

### 5.2 四模块链接数据

真实 1.6.0 的实测输出：

```text
module main    resolver=1414 cross=1402 hle=12
module sdk     resolver=16   cross=10   hle=6
module subsdk0 resolver=6545 cross=6541 hle=4
module rtld    resolver=19   cross=3    hle=16
linked modules=4 symbols=35739 cross=7956 hle=38
```

解释：

- `cross`：由另一个模块定义回答的引用；
- `hle`：没有模块定义、落到宿主 HLE 的引用；
- 交叉链接后只有 38 次 HLE 引用、25 个不同名字；
- 之前在单模块视角下得到的 “1,137 个 HLE 名字” 是把跨模块导入也算进去了，不能作为 HLE 工作量。

### 5.3 iOS 自检

`BotWApp` 在 `BOTW_SELFTEST=1` 或 `--selftest` 时依次执行：

1. `botw_core_guest_selftest()`：内置 AArch64 探针；
2. `botw_core_guest_image_selftest()`：合成 `patched.bin` 映射、slot、gate、DT_INIT；
3. `botw_core_selftest()`：Metal 合成渲染/回读；
4. 将结果写入 App 容器 `Documents/selftest.txt`，期望：
   `BOTW_SELFTEST rc=0 (guest=0 image=0 replay=0)`。

最新 CI 运行 `41559631519` 已成功，但它是合成镜像/合成验证，不等于真实标题可运行。

## 6. 明确未完成 / 不要误判

### 真实标题

- App 内没有真实 `main/sdk/subsdk0` 模块。
- 没有调用真实 `main` 的 `DT_INIT`。
- 没有执行真实游戏的第一条指令。
- 没有真实 `nn::init` / `nn::fs` / `vi` / `nv` / NVN bootstrap 的 App 内链路。

### 图形

- Windows Unicorn 捕获过真实标题的 draw/present；这是宿主捕获，不是 iOS 客体执行。
- iOS 的 `presents` 仍可能只是宿主清屏/合成画面。
- 真实 SASS shader 还没有翻译为 MSL。
- 没有真实标题的 pipeline/draw/present 闭环。

### 系统层

- `nn::os` 线程层是协作式，不是完整 Horizon 内核。
- 没有抢占式调度、步数预算、信号处理和用户异常派发。
- `SetUserExceptionHandler` 只保存配置。
- SystemEvent 的 inter-process/handle 模式未实现。
- `WaitAny` 只覆盖当前 Event holder 场景。
- 多队列、跨队列同步、present blit 仍不完整。

### 平台/发布

- `ios/project.yml` 没有签名 Team、证书、Provisioning Profile。
- CI 只产出模拟器 `.app` 和未签名真机 Debug `.app`。
- 没有 `xcodebuild archive`、`-exportArchive`、`ExportOptions.plist`。
- Release 构建故意失败；当前不能走 TestFlight/App Store。
- 真机可执行内存、JIT entitlement、arm64e PAC 未验证。
- `guest_map.h` 明确说明：匿名内存变成可执行在 iOS 上需要 JIT entitlement；真机不是模拟器。
- 若走 App Store，通常还要改为构建期静态嵌入/合法可执行段方案，不能依赖运行时 JIT。

## 7. 新会话开工清单

```powershell
cd E:\平台移植\botw-iPhone
git status --short
git pull --ff-only
git log -3 --oneline --decorate

# 数据存在性
Test-Path C:\botw-work\data\switch\1.6.0\secure
Test-Path C:\botw-work\data\switch\1.6.0\RomFS
Test-Path C:\botw-work\data\switch\1.6.0\ExeFS\update\main
Test-Path C:\botw-work\data\switch\1.6.0\ExeFS\update\sdk
Test-Path C:\botw-work\data\switch\1.6.0\ExeFS\update\subsdk0
Test-Path C:\botw-work\data\switch\1.6.0\ExeFS\update\rtld

# 本地门禁
powershell -ExecutionPolicy Bypass -File scripts\build_abi_test.ps1
python scripts\verify_ios.py
python scripts\gen_nvn.py --repo . --check
python scripts\extract_nvn_signatures.py --check
python scripts\shader_chain\test_gen_blobs.py
```

如果只是继续源码工作，先保证 `build_abi_test` 和 `verify_ios` 绿；不要在没有新证据时修改 README 中的“已通过”结论。

## 8. 本地构建与测试命令

### 8.1 Windows 全量 portable 测试

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_abi_test.ps1
```

要求：

- Visual Studio 2022 Community；
- `vcvars64.bat` 默认路径存在；
- Python 3.14 或 `python` 可用；
- LLVM clang 可选，用于 `nvn_metal.mm` 语法检查。

### 8.2 仓库不变量

```powershell
python scripts\verify_ios.py
python scripts\gen_nvn.py --repo . --check
python scripts\extract_nvn_signatures.py --check
python scripts\syntax_check_metal.py
```

### 8.3 客体改写/重定位自测

```powershell
python scripts\guest\verify_rewrite.py --self-test
python scripts\guest\probe_relocs.py --self-test
```

### 8.4 真实四模块复跑

当前 `C:\botw-work\tmp\rewrite160` 不存在，需要先重建。使用更新版 ExeFS：

```powershell
cd E:\平台移植\botw-iPhone
python scripts\guest\rewrite_botw.py `
  --nso-dir C:\botw-work\data\switch\1.6.0\ExeFS\update `
  --out build\guest

python scripts\guest\verify_rewrite.py build\guest main

# 先跑完整 Windows 门禁，确保 build\vn\reloc_run.exe 存在
build\vn\reloc_run.exe --multi build\guest 0x4000 --hle --names
```

期望核心数字（具体 load 时间可变）：

```text
linked modules=4 symbols=35739 cross=7956 hle=38
```

如果 `reloc_run` 报缺失文件，检查输出目录里是否同时有：

```text
main.patched.bin    main.fixups.tsv
sdk.patched.bin     sdk.fixups.tsv
subsdk0.patched.bin subsdk0.fixups.tsv
rtld.patched.bin    rtld.fixups.tsv
```

### 8.5 macOS / iOS CI

```text
.github/workflows/ios.yml
  portable job：Linux 编译 + 测试
  ios job：macOS 15 + XcodeGen + 模拟器自检 + 未签名真机 Debug 编译
```

CI 运行页面：

<https://github.com/SavitarXiangzi/botw-iPhone/actions>

当前没有 Mac 本机，所以 iOS 侧编译/模拟器执行必须靠 GitHub Actions；不要把 Windows 上的“语法检查通过”当成 iOS 真机通过。

## 9. 下一里程碑的实现顺序

### Step 1：把四模块映射接进 iOS

目标文件：

- `core/guest/guest_map.{h,c}`
- `core/guest/guest_modules.{h,c}`
- `core/guest/guest_image.{h,c}`
- `core/src/botw_guest_selftest.cpp`
- `ios/project.yml`

建议拆法：

1. 保留 `botw_guest_modules_add()` 的 `bind` 阶段；
2. 为每个模块单独 `mmap` 一个连续 span；
3. 把 patched 字节复制到各自映射；
4. 建立跨模块符号索引；
5. 在映射仍可写时完成全部重定位；
6. 按 `guest_segments.c` 的计划逐页 `mprotect`；
7. 让 `botw_guest_run` 以一个共享地址空间执行多个模块。

验收条件：

- `main/sdk/subsdk0/rtld` 四块地址互不重叠；
- 四模块全部 link 成功；
- 未解析导入返回错误，不是 NULL；
- `rtld` 的 16 KiB RWX 冲突被明确处理；
- 合成测试和真实镜像测试都能复跑；
- 没有跨模块 key page/slot 串用。

`rtld` 的两种选择：

- 优先：不运行 `rtld`，由宿主 loader 承担它的大部分职责；
- 备选：把 text/data 双映射到不同地址，避免同页 RWX。

### Step 2：每模块 key page / 线程槽

当前 `guest_slot` 和 `guest_map_publish_slot()` 的接口是以单模块/单线程为主。多模块需要确认：

- 每个模块的 key page 独立；
- KGATE/K18/KTLS/KTLR 索引来自各自 fixups；
- `SLOT_BASE=abs` 模式下线程切换要重新 publish；
- gate handler 能根据当前 PC 或当前模块找到正确的槽；
- 不允许把 main 的槽地址误写进 sdk/subsdk0 的 key page。

验收条件：每个模块都能读到自己的 gate handler，且不产生错误地址。

### Step 3：进入真实 `main` 的 `DT_INIT`

最低目标不是完整启动，而是：

1. 从 `main` 的 `DT_INIT`（当前记录为 `0x30`）进入；
2. 观察第一条真实标题指令；
3. 记录第一个 HLE 导入名字和参数；
4. 证明 gate/import 链在真实镜像上工作。

建议先打印：

```text
module=main entry=0x30 pc=... hle_call=...
```

然后再进入 `nn::init::Start` / `nnMain` 相关链路。不要在没有第一条真实指令证据时宣称“真实启动已打通”。

### Step 4：收口 25 个 HLE stub

用：

```powershell
build\vn\reloc_run.exe --multi build\guest 0x4000 --hle --names
```

得到剩余名字，按两类处理：

1. `glslc*`：明确实现，或明确拒绝并记录；
2. `__rel_dyn_*` / `__EX_*` / `__got_*` / `__rela_*` / `__tbss_*` / `__tdata_*`：按链接器语义提供真实值。

验收条件：没有“返回 0 但调用者误以为成功”的 stub。

### Step 5：真实 shader 翻译

当前 `ios/nvn/src/nvn_shader.c` 只是接口，`gen_blobs.py` 还没有 SASS→MSL 翻译器。需要：

- 从 `Shaders/*.sbfsha` / `.bin` 找到 program 和 stage；
- 解析 Maxwell SASS；
- 生成 MSL；
- 生成 `manifest.tsv` / shader bundle；
- 用 guest key 找到 pipeline；
- 先用一个最小真实 shader 做像素回读。

验收条件：一个真实标题 shader 能建立 `MTLRenderPipelineState`，不是合成 MSL。

### Step 6：真实 draw/present

在 Step 3–5 后才现实。验收不能只看 `frame` 计数：

- 必须有真实 draw 被编码；
- 必须有真实 present 到 `CAMetalLayer`；
- 最好有像素回读或帧截图；
- `presents > 0` 本身不代表画面正确。

### Step 7：输入、音频、性能

- `hid` 共享内存 → 触摸/手柄映射；
- `nn::audio` / audren → AVAudioEngine（当前还是替身/待办）；
- 6 GB RAM 机型起步，重点观察纹理池、命令 arena、内存峰值；
- 多队列、fence、present blit 仍未闭环。

## 10. IPA / 真机安装的独立路线

当前不能直接产出可安装 IPA。若要“只有壳的测试 IPA”，需要：

1. Apple Developer 账号（付费或免费个人团队）；
2. `Apple Development` 证书；
3. 包含目标设备 UDID 的 Provisioning Profile；
4. Apple Team ID；
5. `ExportOptions.plist`；
6. GitHub Actions 签名 secrets；
7. `xcodebuild archive` + `xcodebuild -exportArchive` 工作流。

同时注意：

- `Release` 当前被 `kEnumsVerified = 0` 故意拒绝；
- 可以先做 **Debug 签名**验证安装，但这不代表游戏可运行；
- 真机可执行内存/JIT entitlement 仍需单独验证；
- 即使签名成功，当前 App 也只能运行合成自检，不能游玩 BOTW。

不要把“安装成功”与“移植成功”混为一谈。

## 11. 备份与恢复策略

因为 XCI 已删除，推荐顺序：

1. 先备份 `C:\botw-work\data\switch\1.6.0\secure`；
2. 再备份 `ExeFS`、`modules`、`Shaders`、`m1`、`inventory`、`tools`；
3. 视空间备份 `RomFS` 和 `DLC`；
4. 密钥单独加密，绝不与公开链接放一起；
5. 云端校验文件数和大小后，才考虑删除本地派生数据；
6. `secure` 保留本地，直到至少有一个经过验证的云端副本。

如果必须释放 C 盘空间，优先级：

```text
botw-iPhone\build               约 733 MiB（可重建）
C:\botw-work\dotnet10           约 770 MiB（可重下）
C:\botw-work\nxsrc              约 733 MiB（可重下）
C:\msys64                       约 0.30 GiB（可卸载）
```

不要优先删除 `secure`。

## 12. 排错快速索引

| 现象 | 优先检查 |
|---|---|
| `verify_ios` 报生成物过期 | 是否改了 `registry.json`/生成器但没重跑 `gen_nvn.py` |
| `guest_modules_test` 失败 | 模块 bind 顺序、符号定义、HLE resolver、重复名 |
| `reloc_run --multi` 找不到文件 | 是否先跑 `rewrite_botw.py` 生成 8 个文件 |
| iOS 模拟器自检 `rc != 0` | `Documents/selftest.txt`、`BOTW_SELFTEST` 日志 |
| `BOTW_SELFTEST` 没文件 | App 是否启动、是否传了 `--selftest` |
| 真机 `mprotect`/`mmap` 失败 | 可执行内存/JIT entitlement、PAC、签名 |
| 屏幕黑但 `presents` 增加 | 可能只是宿主清屏，不等于真实游戏画面 |
| `noPipeline` | 还没有真实 MSL / shader key / pipeline |
| CI portable 绿、ios 红 | 先看 macOS 的 XcodeGen、Swift、Objective-C++ 语法 |

## 13. 术语

- **HLE**：High-Level Emulation，宿主实现 Switch SDK/NVN 调用，不模拟 CPU 指令。
- **客体 / guest**：被移植的标题 ARM64 代码。
- **host**：iOS App 和 Metal 后端。
- **patched.bin**：`rewrite_botw.py` 对零售 NSO 做静态改写后的镜像。
- **fixups**：改写器输出的 gate/TLS/x18/patch 元数据。
- **key page**：改写块读取 gate/TLS 槽地址的可写页。
- **gate**：把客体 `svc`/导入调用转交宿主的 trampoline。
- **NVN**：Switch 图形 API；本项目把它映射到 Metal。
- **SASS**：Maxwell GPU 指令；需要翻译成 MSL。
- **present**：宿主把 swapchain 画面提交到 `CAMetalLayer`。

## 14. 文档地图

| 文档 | 用途 |
|---|---|
| `README.md` | 项目总览和里程碑 |
| `docs/IOS_PORTING.md` | iOS 侧操作手册和缺口 |
| `docs/ROADMAP.md` | 详细 M1–M4 研究记录，约 430 KB |
| `docs/HLE_BOUNDARY.md` | HLE 边界原则 |
| `docs/NVN_COVERAGE.md` | NVN 入口覆盖表 |
| `docs/SHADER_CHAIN.md` | 着色器链路线 |
| `scripts/guest/README.md` | NSO 改写、重定位、真实模块复跑 |
| `docs/HANDOFF_CURRENT.md` | 本文件，下一次开工先读 |

## 15. 维护规则

每次改变以下任一事实，都要更新本文件：

- 当前分支/提交/CI 状态；
- 真实标题是否已进入 App；
- 四模块映射是否已接；
- NVN 入口/Metal binding 数量；
- 是否已有真实 draw/present；
- 是否已有签名/IPA；
- XCI/secure/RomFS 数据状态；
- 下一步优先级。

本文件只记录“当前可验证事实”和“下一步”，详细历史继续写进 `docs/ROADMAP.md`。

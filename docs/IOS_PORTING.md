# 把《旷野之息》接到 iOS：这份骨架怎么用、怎么验、还缺什么

本文是 `ios/` 目录的操作手册。它描述的是**已经写进仓库的东西**和
**机器能验证的事实**，不是你付钱就能玩到游戏的承诺。当前状态一句话：

> iOS 侧的壳、C ABI、513 个 NVN 入口点的分发表、32 个 Metal 后端入口，
> 都能在非 Apple 主机上编译并跑通自测；**真正的画面还没有**，因为
> 渲染目标绑定和着色器翻译都还没做。

---

## 1. 五分钟看懂目录

```
core/include/botw_core.h      Swift 能看到的全部 C 接口（只有这一个头）
core/src/botw_core.cpp        C ABI 实现：创建时按平台装 Metal 或 null 后端
ios/BotW/BotWApp.swift        @main，SwiftUI 入口
ios/BotW/GameView.swift       CAMetalLayer 视图 + 调试 HUD（nvn 覆盖数、presents）
ios/BotW/Renderer.swift       CADisplayLink 驱动 botw_core_frame，把视图的层交给核心
ios/BotW/BotWBridge.h         Swift/ObjC 桥接头，只 #import botw_core.h
ios/project.yml               XcodeGen 工程描述（不提交 .xcodeproj）
ios/nvn/registry.json         513 个入口点，从标题自己的 getproc 字符串提取
ios/nvn/signatures.json       470 个参数形状（公开 nxnative 原型表，MIT）
ios/nvn/inferred_signatures.json  4 个按命名对称性推断的形状，单独标注
ios/nvn/include/nvn_api.h     生成：名字表 + 函数指针 ABI
ios/nvn/src/nvn_dispatch.cpp  生成：二分查找 + 缺失上报
ios/nvn/include/nvn_backend.h 后端接口（null / metal 二选一）
ios/nvn/src/nvn_null.cpp      空后端，任何主机都能编译
ios/nvn/src/nvn_metal.mm      Metal 后端（32 个入口，含大量 TODO）
ios/nvn/implemented_metal.txt 32 行，和 BIND(...) 列表由门禁强制相等
ios/nvn/shaders/              空目录 + 路线说明（见该目录 README）
docs/NVN_COVERAGE.md          生成的逐域覆盖表
scripts/                      提取、生成、校验、编译脚本
tests/nvn_abi_test.cpp        513 个入口点的名字/顺序/查找一致性
tests/core_smoke_test.cpp     C ABI 冒烟：创建、查覆盖数、跑帧、销毁
```

`.xcodeproj` 不入库是刻意的：它是生成物，入库只会制造冲突，且在没有 Mac 的
机器上无法校验。

---

## 2. 没有 Mac 时怎么验（本仓库的默认姿势）

这套东西在 Windows 上就能跑，全部命令都在仓库根目录执行。

```powershell
# 1) 生成物是否和 registry.json 一致（防手改、防漏提交）
C:\Python314\python.exe scripts\gen_nvn.py --repo . --check

# 2) 仓库不变量：注册表 / BIND 列表 / 括号平衡 / Swift 结构 / 枚举诚实 / 无泄漏
C:\Python314\python.exe scripts\verify_ios.py

# 3) 用真编译器编真程序并运行（需要 MSVC 2022），内含下一行的语法检查
powershell -ExecutionPolicy Bypass -File scripts\build_abi_test.ps1

# 4) 单独跑 Objective-C++ 语法检查（需要 clang，无需 Apple SDK）
C:\Python314\python.exe scripts\syntax_check_metal.py
```

最近一次实测结果（Windows / MSVC 19.4x，`/W4 /WX`）：

```
nvn_abi_test: ok (513 procs)                 exit=0
core_smoke_test: ok (nvn 0/513 on null)      exit=0
syntax_check_metal: ok (clang++.exe)
verify_ios: ok
build_abi_test: all tests passed
```

### `syntax_check_metal.py` 到底检查什么

`nvn_metal.mm` 是 Objective-C++，直接 `#import <Metal/Metal.h>`，没有 Apple SDK
就编不动。仓库里放了 `tests/stubs/Metal/Metal.h` 与
`tests/stubs/QuartzCore/CAMetalLayer.h`，只声明这个文件用到的那些符号，然后：

```
clang++ -fsyntax-only -x objective-c++ -std=c++20 -fblocks -Wall -Wextra -Werror \
        -D__APPLE__=1 -I tests/stubs -I ios/nvn/include ios/nvn/src/nvn_metal.mm
```

**能查出来**：类型不匹配、消息签名写错、模板实例化失败、未使用变量。
第一版就是靠它抓到一个真错误（`Tex** list = obj<Tex>(x[1]);` —— 把句柄当成
`Tex*` 赋给 `Tex**`，Xcode 里会直接编译失败）。

**查不出来**：框架链接、`API_AVAILABLE` 可用性标注、ARC 语义、stub 比真实
SDK 宽松的接口。所以它只是把“CI 上第一次编译”提前到本机，不是替代
`xcodebuild`。

`verify_ios.py` 检查的 7 类东西：

| # | 检查 | 失败意味着 |
|---|---|---|
| 1 | `registry.json` 结构、去重、命名形状、`count` 字段 | 注册表被手改坏了 |
| 2 | 生成物与 `gen_nvn.py` 输出逐字节一致 | 改了注册表却没重新生成 |
| 3 | `BIND(...)` ∩ registry == `implemented_metal.txt` | 覆盖数字在撒谎 |
| 4 | 所有 C/C++/ObjC++ 文件括号平衡（能跳过字符串和注释） | 文件被截断 |
| 5 | Swift 有 `@main`、MetalView 接上了、括号平衡 | 壳被改坏 |
| 6 | `--release` 时 `kEnumsVerified` 必须非 0 | 见第 9 节 |
| 7 | 树里没有 `.xci/.nsp/.nca/.nso/.kdb/.keys`，源码里没有绝对 Windows 路径 | 泄露了标题数据或本机路径 |

第 7 条是硬性的：**游戏文件、`prod.keys`、dump 出来的 `.nso` 一律不进仓库**。
本仓库只保存“接口面的名字”这类从二进制里读出来的元信息，不保存任何可执行内容。

---

## 3. 有 Mac 时怎么编

需要 macOS + Xcode 16 或更新（`deploymentTarget: iOS 17.0`，
`xcodeVersion: 16.0` 是 XcodeGen 生成工程格式的提示）。

```bash
brew install xcodegen
cd ios
xcodegen generate --spec project.yml
open BotW.xcodeproj
```

然后选自己的 Team、选真机、Run。模拟器也能跑（CI 里就是模拟器构建），
但模拟器上没有真正的 Metal GPU 行为差异这件事对骨架无所谓——它只是把
`MTLCreateSystemDefaultDevice` 走一遍。

命令行构建：

```bash
cd ios
xcodebuild -project BotW.xcodeproj -scheme BotW \
  -configuration Debug -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  CODE_SIGNING_ALLOWED=NO build
```

> **Debug，不是 Release。** Release 现在编不过，是设计如此，见第 9 节。

工程里有三条路径必须在：`../core/src`（C ABI）、`nvn/src`（NVN 后端）、
`BotW`（Swift 壳）。头文件搜索路径是 `$(SRCROOT)/../core/include` 和
`$(SRCROOT)/nvn/include`。如果你挪动了 `core/`，改 `ios/project.yml` 里的
`HEADER_SEARCH_PATHS`，不要改源码里的 `#include`。

---

## 4. ABI 约定：一个函数指针吃掉 513 个入口

```c
typedef void (*nvngfx_proc)(nvngfx_ctx* ctx, uint64_t x[8]);
```

- `x[0..7]` 是 guest 调用点的 x0..x7。ARM64 整数参数寄存器正好 8 个，而
  `ios/nvn/signatures.json` 里 470 个原型中**最大 arity 就是 8**
  （`nvnCommandBufferDrawElementsInstanced`），所以"8 个字装得下所有参数"
  这句话是被量出来的，不是假设。
- **NVN 对象是 guest 内存**。`nvnWindowInitialize(NVNwindow*, builder)` 的
  `x[0]` 是 guest 自己的对象地址，宿主既不该拥有那块存储，也不该把自己的
  指针写回去。宿主状态一律以 guest 地址为键存在侧表里
  （`table<T>()` + `find/find_or_add/forget`）。
- **返回值写进 w0，也就是 `x[0]`**。`NVNboolean` 是 1/0；getter 直接用结果
  覆盖 `x[0]`。
- **出参是 guest 内存里的指针**，统一内存下直接写穿：
  `nvnWindowAcquireTexture(window, sync, &index)` 的 `x[2]` 就是那个 `int*`。
- **builder 是不透明 guest 结构**，字段偏移未验证。所以
  `*BuilderSetXxx` 把值记在以 builder 地址为键的侧表里，
  `*Initialize` 再从侧表取回来——只在一个地方需要改。

> 这一节是被事故改写的。最初的 32 个实现把 `x[0]` 当成"写回句柄"的出参，
> 把自己 new 出来的宿主指针交给 guest。它能编译、能通过冒烟测试，
> 但 guest 下一次调用就会把**它自己的对象地址**传进来，宿主却拿它当
> `Win*`/`Pool*` 解引用——第一次真机运行就会崩，而且是难以定位的那种崩。
> 现在 `verify_ios.py` 会拒绝"已实现但没有已知参数形状"的入口，
> 并把 arity > 8 的入口直接判失败。

参数形状从哪来：`scripts/extract_nvn_signatures.py` 解析公开项目
[nxnative](https://github.com/seedds/nxnative) 的 `dm/gfx/nvn/nvn_protos.h`
（MIT），只保留**名字 / 返回类型 / 参数类型**，头文件本身不入库。470 个入口有
源码可依；剩下 4 个（`nvnDeviceBuilderGetFlags` 等）在
`ios/nvn/inferred_signatures.json` 里单独标注为"按命名对称性推断"，
`verify_ios.py` 分开计数，不会被悄悄算进已验证的一类。

**浮点参数**（`float`/`double`）走 s0..s7/d0..d7，不在 `x[]` 里。
好消息是真正需要的颜色/深度参数多是**指针**（`nvnCommandBufferClearColor`
的 `const float*`），可以直接从 guest 内存读；暂时不需要 FP 寄存器搬运。
真遇到 FP 传值的入口，在实现前必须先扩展参数块，否则就属于"没有形状"，
门禁不会允许它被绑定。

---
## 5. 内存：不复制，但必须对齐

iOS 是统一内存，guest 堆本来就是宿主可读的，所以
`nvngfxMemoryPoolInitialize` 用
`newBufferWithBytesNoCopy:length:options:MTLResourceStorageModeShared` 直接
**包住** guest 存储，一个字节都不拷。

代价是 Metal 的要求必须由 guest 侧的分配器满足：

- `storage` 必须 **4 KiB 对齐**（`newBufferWithBytesNoCopy` 实际要求页对齐）。
  不满足时 `nvn_metal.mm` 会打 `WARNING pool storage ... is not page aligned`，
  然后 Metal 返回 nil，`nvnMemoryPoolInitialize` 返回 0。
- `length` 必须是页大小的整数倍，且和 guest 实际映射长度一致。
- 这块内存在 Metal buffer 生命周期内**不能被 guest 释放或移动**。

如果你看到 `pool wrap failed (N bytes)`，先查这三条，不要改后端去 memcpy。

---

## 6. 已经实现的 32 个入口点

按域分布（完整明细见 `docs/NVN_COVERAGE.md`）：

| 域 | 已实现/总数 | 说明 |
|---|---:|---|
| Window | 6/12 | 创建 CAMetalLayer、acquire、present interval、active textures |
| WindowBuilder | 5/12 | native window、textures、interval、active count |
| CommandBuffer | 7/144 | begin/end recording、drawArrays、viewport、scissor、clearColor(丢弃) |
| MemoryPool | 3/10 | builder storage/flags、initialize(零拷贝)、getSize |
| MemoryPoolBuilder | 3/8 | defaults、storage、flags |
| Device | 2/27 | initialize（建 queue）、getProcAddress |
| DeviceBuilder | 3/3 | defaults、set/get flags |
| Queue | 1/15 | presentTexture（blit 快路径） |
| Texture | 1/45 | initialize 只分配句柄 |
| TextureBuilder | 1/43 | defaults 清零 |

**必须说清楚的四件事**，否则这张表会误导人：

0. 这 32 个的参数解码在本轮按 `signatures.json` **全部重写过一遍**。
   `SetViewport`/`SetScissor` 是 4 个 `int`（原来当成 double 读），
   `DrawArrays` 是 `(primitive, first, count)`（原来顺序错），
   `DrawElements` 是 `(primitive, indexType, count, indexAddress)`
   （原来把 `x[6]` 当 indexType），`WindowBuilderSetTextures` 是
   `(builder, n, array)`（原来看反），`WindowAcquireTexture` 把索引写进
   `x[2]` 指向的 `int`（原来从 `x[0]` 返回句柄）。

1. `nvnTextureInitialize` 只 `new Tex()`，**不建 MTLTexture**。因为
   `NVNtextureBuilder` 的字段偏移还没验证过，猜偏移会得到一个“看起来能跑、
   实际采样全黑”的后端。这是有意留的洞。
2. `nvnCommandBufferClearColor` 记录参数后丢弃，因为还没有绑定渲染目标。
3. `MTLRenderCommandEncoder` 从来没有被创建过，所以 `drawArrays` 里的
   `if (!b->enc) return;` 会直接返回。**当前不会产生任何 draw call。**
   要让这个数字变成真的，需要先做 `nvnCommandBufferBindTexture` +
   `nvnCommandBufferSetRenderTargets` 之类的目标绑定，再建 encoder。

---

## 7. 渲染管线分阶段（建议的施工顺序）

按“能最早看到东西”的顺序排：

### 6b. 第一次上屏：宿主驱动的 clear + present（已完成一半）

原生层交给核心这件事已经做了，而且是**架构性**的：之前 `MTKView` 自己画一个三角形，
NVN 窗口另建一个**没人能看见**的 `CAMetalLayer`——客体的帧全部 present 到一块
不显示的 surface 上。现在：

* `GameView.swift` 的 `MetalView` 是普通 `UIView`，`layerClass = CAMetalLayer`，
  `layoutSubviews()` 里按 `displayScale` 设 `drawableSize`；
* `botw_core_desc.native_layer` 把这块层传给核心，`nvngfx_metal_set_layer()`
  用 `__bridge` 借存（层由视图持有，核心不 retain）；
* `nvnWindowInitialize` **只用**这块层；`nvnWindowBuilderSetNativeWindow` 传进来的
  是 guest 的 `nn::vi` 句柄，**不是** CAMetalLayer，直接当指针用会崩，所以刻意不采信；
* `CADisplayLink` 每帧调 `botw_core_frame()`；
* `botw_core_frame()` 在还没有标题代码可跑时，用
  `nvngfx_metal_present_clear()` 清屏并 present，`presents` 计数写进
  `botw_core_nvn_stats`，HUD 直接显示。

**它证明了什么**：层绑定 → `nextDrawable` → command buffer → present 这条路在
真机上能走通，且 HUD 的 `presents` 会涨。
**它没证明什么**：这不是游戏在渲染。没有 render target 绑定、没有 shader、
没有纹理，客体的 draw 仍然一条都不执行。屏幕上的颜色是宿主画的。

> 证据边界：这段代码通过了 CI 的模拟器 Debug 构建和真机 Debug 构建
> （`xcodebuild -sdk iphoneos`），**没有**真机运行记录——本机没有设备。

**阶段 A：清屏能出颜色**（不依赖任何 shader）

- 实现 `nvnCommandBufferSetRenderTargets`（拿到颜色纹理）
- `beginRecording` 时如果有目标就建 `MTLRenderCommandEncoder`，
  `loadAction = .clear`，`clearColor` 用 `nvnCommandBufferClearColor` 记下的值
- 结束时 `presentTexture` 走 blit（已有）

做完 A，**客体提交的** clear 才会真正被执行（现在上屏的是宿主自己的 clear，
见 6b）。区别在于：A 完成后 `nvnCommandBufferClearColor` 记下的颜色才有意义，
`presents` 才由客体命令驱动而不是由 `botw_core_frame` 代劳。

**阶段 B：几何能出来**（需要 program + vertex state）

- 实现 `nvnCommandBufferBindProgram`（拿到 shader 的 GPU 码地址）
- 实现 `nvnCommandBufferBindVertexBuffer(s)` / `BindVertexAttribState`
- 实现 `nvnCommandBufferDrawElements`（index type 现在是猜的，必须验证）

**阶段 C：纹理正确**

- `nvnTextureBuilder*` 的字段偏移验证
- 平铺（tiled/swizzled）布局 → Metal 的 linear 或自定义 `MTLTexture` 视图
- BCn（BC1/BC3/BC4/BC5）在 Apple GPU 上没有对应格式，必须**离线转码**
  成 ASTC/ETC2，或者运行时用 compute 解压。这是个体力活，不是难点。

**阶段 D：状态完整**

- Blend / DepthStencil / Multisample / Sampler / Polygon / ChannelMask 的
  默认值表（`*SetDefaults` 可以先给出全零/安全默认，让 guest 至少在
  不依赖具体值的情况下跑通）
- `nvnCommandBufferBarrier`、fence/sync、events

---

## 8. 纹理和着色器的现实约束

**纹理**：BCn 在 iOS 上没有等价格式。可行路线有两条，都要离线或预计算：

1. 打包时把 BCn 转 ASTC（画质和行为都稳定，代价是包体变大、需要额外工具）；
2. 运行第一个场景时用 compute shader 解成 RGBA8 缓存起来（省包体，首帧卡顿）。

**着色器**：见 `ios/nvn/shaders/README.md`。一句话——NVN 交出来的是
Maxwell SASS，Metal 不接受，必须 SASS → IR → SPIR-V → MSL 离线翻译。
这是整个移植里技术风险最高的一块，不要指望“先跑起来再说”。

---

## 9. 为什么 Release 现在编不过

`nvn_metal.mm` 里有两道闸：

```cpp
constexpr int kEnumsVerified = 0;

#if defined(NDEBUG)
#error "Release build refused: NVN enum values are unverified ..."
#endif
```

`NDEBUG` 由 `ios/project.yml` 的 Release 配置显式定义，不依赖 Xcode 默认值。

原因：NVN 的 `NVNformat`、`NVNdrawPrimitive`、`NVNindexType` 这些枚举的真实
数值写在任天堂的 `nvn.h` 里，本仓库没有（也不应该有）。现在的 `switch`
里那些 `case 0` 是占位。一个把 `RGBA8` 猜成 `BGRA8` 的后端会**跑得很正常、
颜色全错**——这种 bug 比崩溃难查一百倍。所以宁可不给 Release 产物。

放开条件（两条都要做）：

1. 从真实 `nvn.h` 抄出用到的枚举，填进 `switch`，把 `kEnumsVerified` 改成 1；
2. 删掉那段 `#error`。

`scripts/verify_ios.py --release` 会同时检查这两件事：值还是 0 就拒绝，
值变成 1 但 `#error` 还留着也拒绝。

---

## 10. 剩余 481 个入口点的优先级

不要按字母序做。按“挡住第一个 draw call 的”排序：

| 优先级 | 入口 | 理由 |
|---|---|---|
| P0 | `nvnCommandBufferSetRenderTargets` | 没有它就没有 encoder，A 阶段做不了 |
| P0 | `nvnCommandBufferBindProgram` / `nvnProgramInitialize` | B 阶段的前提 |
| P0 | `nvnCommandBufferBindVertexBuffer(s)` / `BindVertexAttribState` | 同上 |
| P1 | `nvnTextureBuilder*` 全套（43 个） | C 阶段，字段偏移要先验证 |
| P1 | `nvnSamplerBuilder*`（25）+ `nvnSamplerInitialize` | 采样状态 |
| P1 | `nvnCommandBufferDrawElements` 的真实 index type | 现在只计数 |
| P2 | 各 `*StateSetDefaults` / `*StateGet*` | 让 guest 的查询路径不崩 |
| P2 | `nvnCommandBufferBarrier` / `nvnSync*` / `nvnEvent*` | 正确性，不是画面 |
| P3 | `nvnQueue*` 的其余 14 个 | 只有在用多队列时才需要 |

顺便一提：`nvnBootstrapLoader` 在注册表里，但它不是给游戏用的入口，
不实现也没关系；`nvngfx_lookup` 对未实现的入口返回**日志桩**而不是 NULL，
所以不会有空指针跳转。

---

## 11. 已知缺口（照抄，别当成 bug 报）

| 缺口 | 现状 | 影响 |
|---|---|---|
| 渲染目标绑定 | 未实现 | 没有 encoder，draw 不执行 |
| 着色器翻译 | 未开始 | 无法出任何几何 |
| 纹理创建 | 只分配句柄 | 采样全黑 |
| 浮点参数 | 不在 `x[8]` 里 | 少数入口需要单独签名 |
| 同步/fence | 未实现 | 长时间跑可能撕裂 |
| compute | 未实现 | 后处理不可用 |
| 多线程 | 单队列 | 与真机行为有差异 |

`ios/project.yml` 的跨目录引用（`../core/src`）、`buildPhase: none`、
framework 依赖，都只有在 macOS runner 上跑 `xcodegen generate` +
`xcodebuild -list` 才能真正确认。本机（Windows）只能检查“这些路径存在”。
所以 CI 一旦红了，优先看 `ios` job 的 “Generate Xcode project” 步骤。

Swift 侧同理：`Renderer.swift` / `GameView.swift` 只做了括号与结构检查
（`verify_ios.py` 第 5 项），本机没有 Swift 编译器可跑。已知的一处真实修复
是 `MTKView.delegate` 是 **weak**，`Pump` 必须被 Coordinator 强引用，
否则首帧前就被释放、屏幕永远黑。

---

## 12. 排错

| 现象 | 先查 |
|---|---|
| 屏幕全黑，`presents` 一直是 0 | `native_layer` 没传（视图层不是 CAMetalLayer）、`drawableSize` 是 0（`layoutSubviews` 没跑）、或视图不在 window 上 |
| 屏幕有颜色但 `presents` 不涨 | `CADisplayLink` 没加进 run loop，或 `botw_core_create` 返回空 |
| `verify_ios` 说 generated artifacts are stale | 跑了 `gen_nvn.py --repo .` 但没提交生成物 |
| `verify_ios` 说 bound but not declared implemented | 加了 `BIND(...)` 但没加进 `implemented_metal.txt` |
| `pool wrap failed` | guest 存储没按 4 KiB 对齐，或长度不是页整数倍 |
| 屏幕全黑但 `frame` 在涨 | 正常：encoder 从没创建过（第 6 节第 3 条） |
| 屏幕全黑且 `frame` 不动 | `MTKView.delegate` 是 weak，Pump 必须被 Coordinator 强引用（已在 `GameView.swift` 修过一次） |
| `[nvn] UNVERIFIED xxx = 0x...` | 正常，那是诚实闸门在报未验证的枚举 |
| `[nvn] not implemented: nvnYyy` | 该入口不在 32 个里，按第 10 节排期 |
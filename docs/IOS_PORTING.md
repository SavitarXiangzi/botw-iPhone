# 把《旷野之息》接到 iOS：这份骨架怎么用、怎么验、还缺什么

本文是 `ios/` 目录的操作手册。它描述的是**已经写进仓库的东西**和
**机器能验证的事实**，不是你付钱就能玩到游戏的承诺。当前状态一句话：

> iOS 侧的壳、C ABI、513 个 NVN 入口点的分发表、201 个 Metal 后端入口，
> 都能在非 Apple 主机上编译并跑通自测；纹理能建、渲染目标能绑、encoder
> 能开，program -> pipeline -> draw -> 回读这条链已经在 **CI 的 iOS 模拟器**
> 里跑通了十六条合成 draw（模拟器实际执行十五条），其中包括从 guest 内存池
> 解块上传的纹理、从离线 MSL bundle 按 guest key 命中的 shader，以及
> `WriteTexels` / `FlushTexels` 触发重传的连续纹理更新。
> **但客体的 draw 依然一条都不执行**：首要缺口仍是 Maxwell SASS -> MSL 的
> 翻译器；通用 texture view、任意 mapped write 的精确脏页跟踪、BC6H/BC7 和
> 同步也还没有闭环。

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
ios/nvn/src/nvn_metal.mm      Metal 后端（201 个入口，含大量 TODO）
ios/nvn/src/nvn_texture.c     157 项 NVNformat -> MTLPixelFormat 表（附 lossy 标记）
ios/nvn/include/nvn_texture.h 纹理描述结构 + 表查询接口
ios/nvn/src/nvn_shader.c      着色器翻译的唯一接口：现在只会说"没有翻译器"
ios/nvn/implemented_metal.txt 201 行，和 BIND(...) 列表由门禁强制相等
ios/nvn/shaders/              空目录 + 路线说明（见该目录 README）
docs/NVN_COVERAGE.md          生成的逐域覆盖表
scripts/                      提取、生成、校验、编译脚本
tests/nvn_abi_test.cpp        513 个入口点的名字/顺序/查找一致性
tests/core_smoke_test.cpp     C ABI 冒烟：创建、查覆盖数、跑帧、销毁
tests/nvn_texture_test.cpp    157 项格式表的抽查：精确项、替代项、越界、重复
tests/nvn_shader_test.cpp     翻译接口的行为：拒绝 SASS、passthrough 需显式打开
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
nvn_texture_test: ok (157 formats in table)  exit=0
nvn_shader_test: ok (seam, blob table, packed slots, bindless, c1, SHA-256, shader key, layered/cube fixtures)  exit=0
nvn_vertex_test: ok (packing, 26 formats, layout id)  exit=0
nvn_texlayout_test: ok (block shapes, sizes, offsets, round trip, linear, write region, refusals)  exit=0
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 202/513)
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
typedef struct nvngfx_args {
    uint64_t x[8];   /* x0..x7：整数/指针参数和整数返回值 */
    uint64_t f[8];   /* s0..s7/d0..d7：FP 参数和 float 返回值 */
} nvngfx_args;
typedef void (*nvngfx_proc)(nvngfx_ctx* ctx, nvngfx_args* args);
```

- `x[0..7]` 是 guest 调用点的 x0..x7。ARM64 整数参数寄存器正好 8 个，而
  `ios/nvn/signatures.json` 里 470 个原型中**最大 arity 就是 8**
  （`nvnCommandBufferDrawElementsInstanced`），所以整数/指针参数装得下。
- `f[0..7]` 是另一组寄存器：float 存在低 32 位，double 用完整 64 位；返回
  float 的 getter 也把结果写进 `f[0]`。M3.82 加入这半块，是因为
  `nvnSamplerBuilderSetLodClamp` 等入口的值根本不在 x 里。
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
M3.82 已把参数块扩展为 `nvngfx_args { uint64_t x[8]; uint64_t f[8]; }`：
float 存在 `f[N]` 的低 32 位，double 使用完整 64 位，生成头文件提供 f32/f64
的位型安全读写 helper。颜色/深度参数仍然大量使用指针，因此 UBO 和 clear 路径
不依赖 FP 搬运。**但仓库外真正的 guest -> host 调用桥还没实现**：调用端必须把
s/d 寄存器捕获进 `f[]`，否则 `nvnSamplerBuilderSetLodBias` 一类入口拿不到值。

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

## 6. 已经实现的 201 个入口点

按域分布（完整明细见 `docs/NVN_COVERAGE.md`）：

| 域 | 已实现/总数 | 说明 |
|---|---:|---|
| CommandBuffer | 50/144 | recording/commit、draw、buffer/vertex/index、viewport/scissor、clear、render target、program、texture/sampler 绑定；M4 增加六个固定功能状态 bind 与 state object setter 转发 |
| Texture | 4/45 | initialize 建 `MTLTexture`；WriteTexels/WriteTexelsStrided 写回 guest 布局并标脏，FlushTexels 标脏，下次采样重传 |
| TextureBuilder | 14/43 | defaults + 13 个 setter：target/format/size1D/2D/3D/宽高深/levels/samples/storage/stride/flags |
| Device | 7/27 | initialize、getProcAddress、纹理/采样器句柄查询等 |
| SamplerBuilder | 24/25 | 状态 setter/getter 与 Metal 采样器创建 |
| Sampler | 14/15 | builder -> `MTLSamplerState` |
| SamplerPool / TexturePool | 7/8 + 7/8 | 注册、句柄和 pool 绑定 |
| Window / WindowBuilder | 6/12 + 5/12 | CAMetalLayer、acquire、present interval、active textures |
| MemoryPool / MemoryPoolBuilder | 3/10 + 3/8 | storage/flags、initialize(零拷贝)、getSize |
| Queue | 1/15 | presentTexture（blit 快路径） |
| Program | 3/7 | Initialize / SetShaders / Finalize；记录 shader 地址、control 指针、参数布局与 c1 |
| VertexAttribState / VertexStreamState | 5/5 + 5/5 | attrib/stream 解码为 `MTLVertexDescriptor`，stream -> pool buffer |
| DeviceBuilder | 3/3 | defaults、set/get flags |
| BlendState / ColorState | 4/15 + 4/7 | 默认值、blend enable/func/equation/target、logic op、alpha test |
| DepthStencilState / PolygonState | 7/13 + 5/9 | 默认值、深度/模板/剔除/偏移 setter |
| MultisampleState / ChannelMaskState | 12/23 + 2/3 | 默认值、采样数、coverage 相关 setter、写掩码 |

上表是主要域；完整 29 个域共 **202/513**，逐项状态见 `docs/NVN_COVERAGE.md`。

**必须说清楚的六件事**，否则这张表会误导人：

0. 所有入口的参数解码都按 `signatures.json` 的真实原型写，不是猜的。
   `SetViewport`/`SetScissor` 是 4 个 `int`（曾被当成 double 读），
   `DrawArrays` 是 `(primitive, first, count)`（原来顺序错），
   `DrawElements` 是 `(primitive, indexType, count, indexAddress)`
   （原来把 `x[6]` 当 indexType），`WindowBuilderSetTextures` 是
   `(builder, n, array)`（原来看反），`WindowAcquireTexture` 把索引写进
   `x[2]` 指向的 `int`（原来从 `x[0]` 返回句柄），
   `CommandBufferSetRenderTargets` 是
   `(cb, numColor, colorArray, depth, depthStencil, ...)`。
   本轮新增的 14 个 `nvnTextureBuilderSet*` 也来自同一份原型表。
1. `nvnTextureInitialize` 现在**真的建 `MTLTexture`**：尺寸、格式、级别、
   采样数全部来自 builder 的 setter 记录值，**不去猜 `NVNtextureBuilder`
   的字段偏移**。代价是像素由宿主分配（`MTLStorageModePrivate`），和 guest
   内存**没有 alias**。渲染目标由本后端独占；采样纹理在第一次被 draw 读取时，
   由 `tex_upload()` 从 guest pool 逐 mip 解 block-linear 并 blit 进来。M3.95 起，
   显式 `nvnTextureWriteTexels*` 会按 guest 布局做整级读改写并标脏，
   `nvnTextureFlushTexels` 也会标脏；下一次 draw 会重传整条 mip 链。仍没有
   通用脏页跟踪，guest 绕过这些入口直接改 mapped storage 时不会自动同步。
2. `nvnCommandBufferClearColor` 不再丢弃：它先关掉当前 encoder，把颜色记成
   `pendingClear`，下一次 draw 前的 `ensure_encoder()` 把它变成
   `loadAction = .clear`。mask 仍只记录（那是未验证的枚举）。
3. `MTLRenderCommandEncoder` 会在绑了渲染目标之后由 `ensure_encoder()` 创建，
   draw 也确实走到 encoder。`BindProgram` 之后 `ensure_pipeline()` 会调用
   `nvngfx_shader_to_msl()`；**它现在返回 NULL**（没有翻译器），所以
   `DrawArrays` 打一条 `noPipeline` 日志就返回。**客体的 draw 仍然是 0 条。**
   但这条链本身已经能跑：`nvngfx_metal_selftest()` 用内置 MSL 走完
   program -> pipeline -> draw -> 回读，并由 CI 用 `simctl` 在模拟器里执行 App 的自检钩子。
4. 只挂 `colorAttachments[0]`：`numColor > 1` 的 MRT、以及 `setRenderTargets`
   的 texture view 参数都只记日志。`EndRecording` 直接 `[cb commit]`，
   `nvnQueueSubmitCommands` 还没实现，所以“提交”这件事目前不等队列。

5. 顶点缓冲与顶点参数缓冲区已经接通：stream 从 pool 零拷贝绑定，顶点常量在
   `buffer(0)`，stream 从 `NVNGFX_VERTEX_STREAM_BASE`（16）开始。自检 draw 2/3
   使用真实顶点缓冲，draw 8 使用顶点阶段参数缓冲区并回读平移后的像素。客体尚未
   跑到这些路径。

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

**阶段 A：清屏能出颜色**（不依赖任何 shader）——代码已完成，未上真机

- `nvnCommandBufferSetRenderTargets` 已实现：记录 `numColor` / 颜色纹理数组
  / 深度纹理，关掉旧 encoder（一次 pass 换目标就必须重开）。
- `ensure_encoder()` 在**第一次** draw / viewport / scissor 之前懒开
  `MTLRenderCommandEncoder`，只挂 `colorAttachments[0]`；
  `pendingClear` 为真时 `loadAction = .clear` 并用
  `nvnCommandBufferClearColor` 记下的 RGBA。
- `EndRecording` 直接 `[cb commit]`；`presentTexture` 的 blit 路径是旧的。

没做的：真机上没跑过（本机无设备），深度目标只记录不挂载，
`clearColor` 的 mask 语义未验证。所以“客体 clear 现在会真的执行”这句话
只对“绑了 `color[0]` 且该纹理确实建成功”的路径成立——在真机上看到之前，
这只是**代码路径**，不是现象。

**阶段 B：几何能出来**（代码通路已建，缺翻译器）

- ✅ `nvnProgramInitialize` / `nvnProgramSetShaders` / `nvnProgramFinalize`
- ✅ `nvnCommandBufferBindProgram` -> `ensure_pipeline()` -> `MTLRenderPipelineState`
- ✅ `nvnCommandBufferBindVertexBuffer(s)`（真的读 `NVNbufferRange`，draw 真的用它）
- ✅ `nvnCommandBufferBindVertexAttribState` / `BindVertexStreamState`（M3.79，
  客体 4 / 8 字节记录 -> `MTLVertexDescriptor`）
- ✅ draw 真的编码：`setRenderPipelineState` + `drawPrimitives` / `drawIndexedPrimitives`
- ✅ `nvnCommandBufferDrawElements`（M3.80；索引值已按 nxnative 的 `nvn.h`
  核对为 0=u8 / 1=u16 / 2=u32；后端目前只接 1/2，u8 仍丢弃，且那是
  reimplementation 头文件而不是 Nintendo SDK）
- ❌ `nvngfx_shader_to_msl()` 没有翻译器 -> 客体的 program 永远建不出管线

**已经能证明的部分**：`nvngfx_metal_selftest()` 用内置 MSL 走完
texture -> render target -> clear -> program -> pipeline -> draw -> 回读，
并额外创建、比较和读回 `MTLSamplerState`。CI 在 iOS 模拟器里用 `simctl`
启动 App（`--selftest`），App 在 `init()` 里跑它、把
`BOTW_SELFTEST rc=<n>` 写进容器的 `Documents/selftest.txt` 并 `exit(rc)`；
现在跑**十六条** draw（模拟器实际执行十五条；`[[vertex_id]]` / `[[attribute(0)]]` / 16 位索引的
`DrawElements` / `BindUniformBuffer` 参数缓冲区 / 从 texture pool + sampler pool
取句柄并采样 2x2 纹理 / 只通过 `[[id(0)]]` 读 c1 常量段 / bindless 句柄表 / 顶点阶段
参数缓冲区 / 从 guest pool 解块上传的 2x2 纹理 / 从 `<app>/shaders` 的
`manifest.tsv` 装载 MSL 并按 guest key 建管线）加一条 sampler 状态路径，
全部通过才 rc=0。
M3.76 的 run 40997940031 证明了第一条；run 41008588834 证明当时的三条；
M3.82 的 run 41018975699 以 `BOTW_SELFTEST rc=0` 通过；M3.83 的 run
41034005513（ios job 123990428590）当时五条全过。M3.84 的 run 41037369546
（portable job 124001430324、ios job 124001563704）也以 `BOTW_SELFTEST rc=0`
通过，日志里多了侧车查找/拷贝的证据：

```
[selftest] blob layout: key=00000000000000a5 numArgs=3 ids=5/9/12
[selftest] textured draw sampled all four pool texels
```

M3.85 的 run 41042618062（portable job 124018857722、ios job 124019022555）也以
`BOTW_SELFTEST rc=0` 通过；第六条 draw 只读 c1：

```
[selftest] c1 sidecar: key=00000000000000a6 bytes=256 id=0
[selftest] c1 buffer: gpuAddress=0x15004b0000 bytes=256
[selftest] c1 draw read back its copied constants
```

第一次 `3d8ba8f` 的 run 41040694114 在 iOS 上以 `rc=16392` 失败（中心像素全
0）：自检把单个 c1 成员声明在 `[[id(2)]]`，而 Metal 参数缓冲区按成员声明顺序
紧凑排布，运行时按 `id*8` 写到了偏移 16。`ddde8f8` 改成 `[[id(0)]]` 后通过。
M3.86 已把声明顺序写进 blob 的 `packed` 字段，因此带空洞的真实布局也按紧凑
参数缓冲区排布；M3.87 接上顶点阶段参数缓冲区，M3.88 补 block-linear 尺寸/解块，
M3.89 把访存池里的纹理接进采样路径。细节与自检编号见 `docs/ROADMAP.md`。

M3.86–M3.89 的 run `41045568006`（packed slot 与 bindless）、`41046781000`
（顶点阶段参数缓冲区）、`41047317230`（block-linear 布局与解块）和 `41051685313`
（从 guest 池上传纹理；portable job `124049028971`、ios job `124049107710`）
都以 `BOTW_SELFTEST rc=0` 通过。第九条 draw 的日志：

```
[nvn] texture upload: pool=0x5400 offset=8192 levels=1 staging=16 bytes
[nvn] texture upload encoded: 1 level(s)
[selftest] guest texture: 1 level(s) deswizzled from the pool and sampled back
```

M3.90 把离线翻译产物接成文件 bundle：`<app>/shaders/manifest.tsv` 加
`<key>_<stage>.msl` / `.c1`，启动时由 `nvngfx_shader_load_bundle()` 取代或补充
生成的 C 表。自检第十条 draw 在运行时构造合成 control/code，算出 key
`e01e5955f6f39e29`，只通过 bundle 查 VS/PS，画一条全屏三角形并回读橙色。
run `41064580709` 的 iOS job `124092690787` 成功；第一次模拟器启动卡住，重试后
日志为：

```
[selftest] draw 10: MSL loaded from the bundle drew key e01e5955f6f39e29
BOTW_SELFTEST rc=0
```

CI 现在用 `SIMCTL_CHILD_BOTW_REQUIRE_SHADER_BUNDLE=1` 要求这条必须执行；
没有 bundle 时返回 `16397`，不会再把 draw 10 记成 skipped。
`gen_blobs.py --bundle-out <dir>` 可以直接从 `nx_shader.py translate` 的输出
生成同一种目录。这仍是我们自写的合成 MSL，不是标题 shader。

M3.91 的第十一条 draw 把一块 2×2 RGBA8 的两层 2D 数组放进 guest pool：
layer 0 全红、layer 1 全黄。运行时先检查 `MTLTexture.textureType == 2DArray`
和 `arrayLength == 2`，再让片段着色器同时采样两层，左半屏取 layer 0、右半屏
取 layer 1，最后回读左红右黄。这个布局能抓住两个很容易静默通过的 bug：
把数组建成普通 2D 纹理，以及所有 slice 复用同一块暂存区（两边都会变成最后一层）。
通过时的日志为
`[selftest] draw 11: 2D array layers 0/1 sampled from guest memory`；
这条已在 run 41070475247 的 iOS job（124113134061）上执行通过，
同一段日志里 `texture upload` 报 `layers=2 staging=32 bytes`。

M3.92 补上另外两种层形状：第十二条建 `MTLTextureTypeCube`（`arrayLength=1`、
6 个 slice），NVN 面 4（+Z）涂红、面 5（-Z）涂黄、其余四面涂绿，片段着色器
分别沿 `(0,0,1)` 与 `(0,0,-1)` 采样，左红右黄才算过；第十三条建
`MTLTextureTypeCubeArray`，`depth=2` 即 `arrayLength=2`、12 个 slice，
红放在 slice 4、黄放在 slice 11，着色器从 cube 0 取 +Z、从 cube 1 取 -Z。
后者同时验证“Metal 的数组长度”和“上传的总 slice 数”没有混用：把 12 写成 2
会让 slice 11 落空，写成 6 则越界。失败码 `16401/16402`（cube）、
`16403/16404`（cube array）。

第十二条（cube）已在 run 41080975529 的 iOS job 上跑通：
`draw 12: cube faces 4/5 sampled from guest memory`，上传日志是
`pool=0x7300 offset=0 levels=1 layers=6 staging=96 bytes`。第十三条在模拟器上
**跑不了**：建 `MTLTextureTypeCubeArray` 时进程直接消失，`@try/@catch` 抓不到
（不是 NSException），也没有崩溃报告，所以它改成 opt-in —— 只有设了
`BOTW_SELFTEST_CUBE_ARRAY=1` 才执行；模拟器上打印跳过原因后继续。
布局与 shader 由 `nvn_texlayout_test` / `nvn_shader_test` 覆盖，端到端要等真机。

第十四条把 M3.93 的 LINEAR 布局接到端到端：2×2 RGBA8、`flags = 0x10`、
`stride = 32`，guest 存储里每行只有 8 字节有效数据、其余是 padding。上排
红/绿、下排蓝/白，回读左上红、左下蓝；如果上传忽略了 stride（把 8 字节当行距），
左下会读到 padding 的 0 而变黑。失败码 `16405`（布局 / 建纹理 / 编码）、
`16406`（像素不对）。这条已在 run 41083158406 通过：
`draw 14: LINEAR texture rows sampled from guest memory at a 32-byte stride`，
同段日志里 `texture upload ... offset=12288 ... staging=16 bytes`。

M3.94 的第十五条 draw 把一块 4×4 BC1 放进 guest pool：`format = 0x42`，
左半红、右半蓝。iOS Metal 没有 BCn 像素格式，所以 `nvnTextureInitialize`
必须把目标格式改成 `RGBA8Unorm`；`tex_upload` 先把 block-linear 的 guest
字节解成紧凑 BC1 行，再逐块解码到 RGBA8 暂存区并 blit，最后回读左红右蓝。
sRGB 变体建 `RGBA8Unorm_sRGB`，不会丢 transfer function。失败码 `16407`
（布局 / 建纹理 / 编码）、`16408`（像素不对）。这条已在 run 41086648054
的 iOS job（124169655661）通过；portable job 是 124169430691。

M3.95 的第十六条 draw 专门验证“上传过的纹理还能再变”。它先把一张 2×2
RGBA8 guest 纹理上传并采样，初始四象限是红/绿/蓝/白；然后调用
`nvnTextureWriteTexels` 改成黄/青/品红/黑并重画。接着绕过写入口，直接改写
guest storage，再调用 `nvnTextureFlushTexels` 改成棕/钢蓝/橙/天蓝并第三次
回读。只有三次像素都正确、且每次上传后 `dirty` 都被清掉才算通过。失败码
`16409`（初始布局 / 建纹理 / 编码）、`16410`（WriteTexels 重传或像素）、
`16411`（FlushTexels 重传或像素）。run 41090664172 的 portable job
（124183374680）与 iOS job（124183554875）均通过；成功日志是
`draw 16: WriteTexels and FlushTexels both re-uploaded a sampled texture`。

第五条在模拟器里走的是**直连纹理/采样器绑定**：模拟器的
`MTLTexture.gpuResourceID` 是 0，参数缓冲区的纹理条目在那里没有意义
（见 `docs/ROADMAP.md` 的 M3.83 / M3.84 段）。所以“Metal 这半边是通的”
是已经执行过的结论；“客体的 shader 能跑”仍然完全没验证。模拟器覆盖到池、
句柄、上传、sampler 状态、直连绑定、侧车布局拷贝、c1 常量段与像素回读，
不覆盖 Metal 3 参数缓冲区的纹理条目，也不覆盖真实 shader 的 bindless /
顶点参数缓冲区路径。

**阶段 C：纹理正确**

- `nvnTextureBuilder*` 的字段偏移验证
- ✅ 平铺：M3.88 的 Tegra X1 block-linear 尺寸 + 解块，M3.89 接到首次采样上传；
  M3.91 接上 2D 数组的端到端上传，并修正 cube / cube-array 的层数与 Metal 描述；
  M3.93 接上 LINEAR 行距与逐行拷贝
- ✅ BCn：M3.94 在首次采样上传时把 BC1/BC2/BC3/BC4U/BC5U 解成 RGBA8，
  sRGB 变体用 sRGB 纹理；BC4S/BC5S、BC6H/BC7 仍拒绝。代价是 BC1 的
  显存变成 8 倍，后续仍应由 ASTC 离线转码或 compute 解压取代
- ✅ 显式更新：M3.95 接上 `nvnTextureWriteTexels*` 与 `nvnTextureFlushTexels`，
  下一次采样会重传；代价是 RMW 需要整级临时线性图像
- 仍缺：通用 texture view、任意 mapped write 的精确脏页跟踪、3D 线性纹理的真实场景

**阶段 D：状态完整**

- ✅ 六类固定功能状态：M4 续已把 Blend / Color / ChannelMask / Multisample /
  Polygon / DepthStencil 合成 `nvngfx_pipeline_state`，blend、write mask、深度模板格式、
  alpha-to-coverage 进入 `MTLRenderPipelineDescriptor`，cull / winding / fill、
  `MTLDepthStencilState` 与 polygon offset 在 encoder 逐 draw 应用；缓存键包含颜色/深度/
  模板格式、采样数、顶点布局与状态哈希。`SetStencilRef` / `SetStencilMask` /
  `SetStencilValueMask` 已接入：引用值放 encoder，前后脸读/写掩码进深度模板状态缓存键。
  `ClearDepthStencil` 已转成深度/模板附件的 load action，并在 EndRecording 或换 render
  target 前强制落地。`SetDepthClamp` 映射到 `setDepthClipMode:`；`SetDepthRange(s)` 无论标量还是数组形式
  都落到同一个单 viewport 状态；`SetDepthBounds`
  只记录状态，Metal 没有深度边界测试，非默认范围会明确上报。MRT 已支持 1–8 个颜色附件：
  管线缓存键含每个目标的格式，描述符按目标填混合/写掩码，render pass 按目标挂载并允许各自的
  清屏值。模拟器用两个颜色目标验证了独立清屏和双颜色输出。仍缺
  `SetStencilCullCriteria` 和纯深度 pass；部分模板清屏掩码无法用 Metal load
  action 表达，会明确上报而不是错误清除。
- ✅ Sampler：M3.82 完成 builder、object、getter 和真实的
  `MTLSamplerState`；M3.83 完成 sampler pool、`BindSeparateSampler(s)` 与
  参数缓冲区/直连绑定两条路径。M3.84 已把真实 shader 的 `[[id(k)]]` 顺序
  写成 `gen_blobs.py` 侧车并在 `prog_set_shaders()` 里应用；M3.85 接上
  c1 常量段；M3.86 接上 packed slot 与 bindless；M3.87 接上顶点阶段参数缓冲区
- `nvnCommandBufferBarrier`、fence/sync、events

---

## 8. 纹理和着色器的现实约束

**纹理路径现状（本轮）**：`nvnTextureBuilder*` 的 13 个 setter 只记录值，
`nvnTextureInitialize` 拿这些值建 `MTLTextureDescriptor` + `newTextureWithDescriptor:`。

| 项 | 现状 |
|---|---|
| 描述来源 | builder setter 的记录值；**不读 guest builder 内存**，所以没有字段偏移风险 |
| 像素归属 | 宿主所有，`MTLStorageModePrivate`，**不与 guest 内存 alias** |
| 能干的事 | render target；首次采样时从 guest pool 解块 + blit 上传；WriteTexels/FlushTexels 后标脏，下一次采样重传；BC1/2/3/4U/5U 先解成 RGBA8 |
| 上传粒度 | 逐 mip、逐层；所有层都成功才允许采样，否则丢 draw，不读半张纹理；FlushTexels 当前整条 mip 链重传 |
| 格式 | 157 项表（见 `nvn_texture.c`），exact / substitute 分开标记，替代项返回 `lossy=1` |
| 未支持 | 通用 texture view、任意 mapped write 的精确脏页跟踪、BC4S/BC5S、BC6H/BC7；BCn 解压和更新 RMW 的内存开销尚未优化 |

所以别把“`nvnTextureInitialize` 返回 1”理解成“纹理能用了”。它只证明
**描述符合法、Metal 分配成功**。

**BCn**：在 iOS 上没有等价格式。当前实现是路线 0：

0. ✅ 首次采样时用 `nvn_bcn.c` 在 CPU 上解成 RGBA8（M3.94）。范围是
   BC1/BC2/BC3/BC4U/BC5U；不认识的 signed/BPTC 格式明确返回失败，不会猜。
   这条路线实现最简单，但 BC1 的每纹素 0.5 字节会变成 4 字节，显存是 8 倍。
1. 打包时把 BCn 转 ASTC（画质和行为都稳定，代价是包体变大、需要额外工具）；
2. 运行第一个场景时用 compute shader 解成 RGBA8 缓存起来（省包体，首帧卡顿）。

**着色器**：见 `ios/nvn/shaders/README.md`。一句话——NVN 交出来的是
Maxwell SASS，Metal 不接受，必须 SASS → IR → SPIR-V → MSL 离线翻译。
这是整个移植里技术风险最高的一块，不要指望“先跑起来再说”。

**这一轮把“翻译器之外的一切”做完了**，并且给出唯一接口：

```
guest NVNshaderData { data, control }   (Maxwell SASS)
        |
        v
nvngfx_shader_to_msl(code, size, stage, &why)   <- 现在返回 NULL + 理由
        |
        v
MTLLibrary -> MTLRenderPipelineState -> draw     <- 已实现，自检已跑通
```

`nvn_shader.h` 里还有一个 `nvngfx_shader_set_msl_passthrough()`：打开后
`code` 被当作 MSL 源码原样返回。它**不翻译任何东西**，只让自检（以及将来
的离线 MSL 缓存）能在没有翻译器时走通后半条链。默认关闭。

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

## 10. 剩余 379 个入口点的优先级

不要按字母序做。按“挡住第一个 draw call 的”排序：

| 优先级 | 入口 | 理由 |
|---|---|---|
| P0 | ~~`nvnCommandBufferSetRenderTargets`~~ | ✅ 本轮已实现；只剩 MRT / 深度 / view |
| P0 | ~~`nvnCommandBufferBindProgram` / `nvnProgramInitialize`~~ | ✅ 本轮已实现（管线建到 `nvngfx_shader_to_msl` 为止） |
| P0 | ~~`nvnCommandBufferBindVertexAttribState`~~ | ✅ 本轮已实现（含 `nvnVertexAttribState*` / `nvnVertexStreamState*` 与 `BindVertexStreamState`） |
| P0 | 真实 shader 的第一次 draw | UBO/texture/sampler、`[[id(k)]]` 侧车、c1、packed slot、bindless、顶点参数缓冲区、分层/LINEAR/BC1–BC5 纹理上传都已接上；仍缺把标题 shader 真正翻成 MSL 的离线链路，以及 BC6H/BC7、texture view 和任意 mapped write 的精确脏页跟踪 |
| P1 | `nvnTextureBuilder*` 剩余 29 个 | 已做 14 个；剩余主要是 getter 与 pool/offset 细节 |
| P1 | ~~`nvnSamplerBuilder*` + `nvnSampler*` + sampler pool~~ | ✅ M3.82/M3.83：38 个 sampler 入口、8 个 pool/bind 入口，能建 `MTLSamplerState` 并从 pool 解析句柄；`SamplerBuilderGetDevice` 无已知形状、`SamplerGetDebugID` 的返回值语义未定 |
| P1 | `NVNindexType` 枚举值核对 | nxnative 的 `nvn.h` 给出 0=u8 / 1=u16 / 2=u32；后端只接 1/2，u8 未知值不画，且该头文件是 reimplementation 而非 Nintendo SDK |
| P0 | Maxwell SASS -> MSL 翻译器 | 不是入口点，是整条链的墙 |
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
| 渲染目标绑定 | 只挂 `color[0]`，深度/view 只记录 | MRT 与深度测试不可用 |
| 着色器翻译 | 只留了接口，没有实现 | 客体的 draw 全部停在 `noPipeline` |
| 纹理创建 | 建 host `MTLTexture`；首次采样时从 guest pool 解块/上传，WriteTexels/FlushTexels 后标脏重传 | 2D/数组/cube/LINEAR 和 BC1–BC5 可采样；BC6H/BC7、通用 texture view 与任意 mapped write 的脏页跟踪仍不完整 |
| draw 执行 | 有 pipeline 就真的编码 draw；没有就丢弃 | 客体仍是 0 条；自检十六条 draw（模拟器执行十五条）加 sampler 状态由 CI 执行 |
| 顶点输入 | attrib/stream 已接，缓冲从 pool 零拷贝绑到 `MTLBuffer` | 自检的第二条 draw 就是这条路；客体尚未跑到 |
| 浮点参数 | `nvngfx_args.f[8]` 已扩展，float 在低 32 位 | 本仓库外 guest 调用端仍需实现 s/d 寄存器桥 |
| 采样器资源 | builder/object、sampler pool 与 `MTLSamplerState` 已建，句柄可解析 | `[[id(k)]]`、c1、packed slot、bindless 与顶点参数缓冲区已接；真实标题 shader 仍缺离线翻译链 |
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
| 屏幕全黑但 `frame` 在涨 | 正常：没有离线 MSL（表为空）就没有 pipeline，draw 被丢掉 |
| `[nvn] nvnCommandBufferDraw.vertexBuffer: N` | 该 stream 的 guest 地址不在任何 pool 里，`pool_span` 拒绝绑定 |
| `[nvn] nvnPipeline.vertexFormat: 0x..` | 该 NVNformat 没有 Metal 顶点格式（如 RGB8 / RGB16 / `_UI2F`） |
| `[nvn] nvnCommandBufferDraw.pipeline: no translator...` | 正常，这条就是当前的墙；自检不经过它 |
| 屏幕全黑且 `frame` 不动 | `MTKView.delegate` 是 weak，Pump 必须被 Coordinator 强引用（已在 `GameView.swift` 修过一次） |
| `[nvn] UNVERIFIED xxx = 0x...` | 正常，那是诚实闸门在报未验证的枚举 |
| `[nvn] not implemented: nvnYyy` | 该入口不在 201 个里，按第 10 节排期 |

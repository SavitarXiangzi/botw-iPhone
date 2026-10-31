# 把《旷野之息》接到 iOS：这份骨架怎么用、怎么验、还缺什么

本文是 `ios/` 目录的操作手册。它描述的是**已经写进仓库的东西**和
**机器能验证的事实**，不是你付钱就能玩到游戏的承诺。当前状态一句话：

> iOS 侧的壳、C ABI、514 个 NVN 入口点的分发表、484 个 Metal 后端入口，
> 都能在非 Apple 主机上编译并跑通自测；纹理能建、渲染目标能绑、encoder
> 能开，program -> pipeline -> draw -> 回读这条链已经在 **CI 的 iOS 模拟器**
> 里跑通了十八条合成 draw（模拟器实际执行十七条）与七阶段 instanced / base-vertex / indirect draw 回归，其中包括从 guest 内存池
> 解块上传的纹理、从离线 MSL bundle 按 guest key 命中的 shader，以及
> `WriteTexels` / `FlushTexels` 触发重传的连续纹理更新。
> **但 iOS 侧还没有客体核心，draw 依然一条都不执行**：Windows Unicorn
> 捕获已证明 1.6.0 标题会发真实 draw/present（两次 700 s 长跑为
> 707 / 304 与 667 / 288 draws/presents），但那是宿主调用捕获，不是 iOS 上的客体执行；首要缺口
> 仍是把 1.6.0 的 ARM64 客体核心接进 app（AOT 改写已跑通，loader/HLE 未接），以及 Maxwell SASS -> MSL 的
> 翻译器；NVNtextureView 的 16 字节状态对象已可读写，但通用 view 的
> swizzle/depth-stencil mode 尚未应用到采样/渲染目标，多队列、跨队列同步与
> present blit 等待仍不完整；任意 mapped write 的精确脏页跟踪、BC6H/BC7 也还没有闭环。

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
ios/nvn/registry.json         514 个入口点，从标题自己的 getproc 字符串提取
ios/nvn/signatures.json       477 个参数形状（公开 nxnative 原型表，MIT）
ios/nvn/inferred_signatures.json  29 个按推断的形状，单独标注
ios/nvn/include/nvn_api.h     生成：名字表 + 函数指针 ABI
ios/nvn/src/nvn_dispatch.cpp  生成：二分查找 + 缺失上报
ios/nvn/include/nvn_backend.h 后端接口（null / metal 二选一）
ios/nvn/src/nvn_null.cpp      空后端，任何主机都能编译
ios/nvn/src/nvn_metal.mm      Metal 后端（484 个入口，含大量 TODO）
ios/nvn/src/nvn_texture.c     157 项 NVNformat -> MTLPixelFormat 表（附 lossy 标记）
ios/nvn/include/nvn_texture.h 纹理描述结构 + 表查询接口
ios/nvn/src/nvn_shader.c      着色器翻译的唯一接口：现在只会说"没有翻译器"
ios/nvn/implemented_metal.txt 484 行，和 BIND/BIND_CB 列表由门禁强制相等
ios/nvn/shaders/              空目录 + 路线说明（见该目录 README）
docs/NVN_COVERAGE.md          生成的逐域覆盖表
scripts/                      提取、生成、校验、编译脚本
tests/nvn_abi_test.cpp        514 个入口点的名字/顺序/查找一致性
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
nvn_abi_test: ok (514 procs)                 exit=0
core_smoke_test: ok (nvn 0/514 on null)      exit=0
nvn_texture_test: ok (157 formats in table)  exit=0
nvn_shader_test: ok (seam, blob table, packed slots, bindless, c1, SHA-256, shader key, layered/cube fixtures)  exit=0
nvn_vertex_test: ok (packing, 26 formats, layout id)  exit=0
nvn_texlayout_test: ok (block shapes, sizes, offsets, round trip, linear, write region, refusals)  exit=0
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 484/514)
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
  `ios/nvn/signatures.json` 里 477 个原型中**最大 arity 就是 8**
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
（MIT），只保留**名字 / 返回类型 / 参数类型**，头文件本身不入库。477 个入口有
源码可依；剩下 25 个（`nvnDeviceBuilderGetFlags`、10 个 QueueBuilder 入口、Window / Program / Sampler 的若干 getter 等）在
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

## 6. 已经实现的 481 个入口点

按域分布（完整明细见 `docs/NVN_COVERAGE.md`）：

| 域 | 已实现/总数 | 说明 |
|---|---:|---|
| CommandBuffer | 119/144 | recording/commit、普通与实例化 draw、base-vertex、两种 indirect draw、buffer/vertex/index、viewport/scissor、clear、render target、program、texture/sampler 绑定；M4 增加六个固定功能状态 bind、state object setter 转发、Barrier、UpdateUniformBuffer、CopyBufferToBuffer、Buffer↔Texture / Texture↔Texture copy 与 copy row/image stride，以及 initialize / finalize / is-recording、command / control memory 记账、memory callback、debug label / marker / group、13 个逐 draw 固定功能状态入口，以及纹理对象查询（14 个 getter + label / finalize / invalidate）、fence sync / signal event / wait event / wait sync；DiscardColor / DiscardDepthStencil 把旧附件内容声明的丢弃映射成下一次 pass 的 DontCare，GetMemoryCallbackData 回读 guest 指针 |
| Texture | 45/45 | initialize 建 `MTLTexture`；14 个对象 getter 加 storage class / raw storage class 与 sparse tile 布局；ReadTexels / ReadTexelsStrided 从 guest 存储按 view 的 level/layer/slice 读回；WriteTexels/WriteTexelsStrided 写回 guest 布局并标脏，FlushTexels 标脏，下次采样重传；14 个 texture view 状态入口（16 字节实测布局）；Compare / GetViewOffset / GetDepthStencilMode |
| TextureBuilder | 43/43 | packaged data / layout 读写、两个 storage class getter 与 sparse tile 布局已接；defaults + 17 个 setter（target/format/尺寸/levels/samples/storage/stride/flags/device/swizzle/GL 名称/depth-stencil mode）与 17 个查询入口：标量、storage size/alignment、device、swizzle 四路、GL 名称、depth-stencil mode |
| Device | 27/27 | initialize / getProcAddress / 纹理与采样器句柄查询；M4 补齐 finalize / deferred finalize / debug domain 与 label / debug callback / walk database / fast-clear 注册 / 深度与窗口原点模式 / 中间 shader cache / 时间戳 / `GetInteger` 属性表 |
| SamplerBuilder | 25/25 | 状态 setter/getter、device 回读与 Metal 采样器创建 |
| Sampler | 15/15 | builder -> `MTLSamplerState`；对象 getter 与 host debug ID |
| SamplerPool / TexturePool | 8/8 + 8/8 | 注册、句柄、pool 绑定与 `GetMemoryPool` |
| Window / WindowBuilder | 12/12 + 12/12 | CAMetalLayer、acquire、present interval、active textures；builder 的 device / native window / 纹理列表可回读，window 侧 native window / present interval / active count / crop / debug label 可读写 |
| MemoryPool / MemoryPoolBuilder | 10/10 + 8/8 | storage/flags/device、initialize(零拷贝)、地址/映射/尺寸查询、mapped-range coherency 空操作；virtual pool（flags `0x200`）可以没有存储就初始化，`MapVirtual` 在“单一物理池 + 同一偏移差 + 覆盖整个虚拟区间”时做别名 |
| Buffer / BufferBuilder | 11/11 + 7/7 | builder storage(pool, offset, size) -> 地址/映射/尺寸/调试 ID；buffer 是 pool 的视图，不另拷贝 |
| Queue | 15/15 | initialize / finalize、flush、finish、getError、submitCommands 记账、acquireTexture 轮转、fenceSync、debug label、memory usage reset；presentTexture 仍走 blit 快路径 |
| QueueBuilder | 19/19 | 9 个 setter + 10 个 getter；缺省 64 KiB command / 256 KiB compute / 16 KiB control / 4 KiB flush；`GetQueueMemorySize` 按 4 KiB 对齐 |
| Event | 6/7 | builder / initialize / value / signal / finalize；GPU signal 走 Metal completion handler |
| Sync | 4/6 | initialize / finalize / debug label / wait；fence 绑定最后提交的 `MTLCommandBuffer` |
| Program | 6/7 | Initialize / SetShaders / SetShadersExt / SetDebugLabel / SetSampleShading / Finalize；记录 shader 地址、control 指针、扩展字段、参数布局与 c1 |
| VertexAttribState / VertexStreamState | 5/5 + 5/5 | attrib/stream 解码为 `MTLVertexDescriptor`，stream -> pool buffer |
| DeviceBuilder | 3/3 | defaults、set/get flags |
| BlendState / ColorState | 13/15 + 7/7 | 默认值、blend enable/func/equation/target、logic op、alpha test；advanced mode / premultiplied / overlap 按实测位布局回读，NormalizedDst 两项因无语义依据保留 |
| DepthStencilState / PolygonState | 13/13 + 9/9 | 默认值、深度/模板/剔除/偏移 setter 与全部 getter；双面 stencil func/op 可分别回读 |
| MultisampleState / ChannelMaskState | 22/23 + 3/3 | 默认值、采样数、coverage 相关 setter、写掩码与 10 个单字节 getter；SampleLocationsGrid 因状态里没有网格尺寸保留 |

上表是主要域；完整 29 个域共 **484/514**，逐项状态见 `docs/NVN_COVERAGE.md`。

纹理侧最近两批：14 个 `NVNtextureView` 状态入口（16 字节实测布局，7 个 setter / 6 个 getter /
字段比较）；texture builder 的 device / swizzle / GL 名称 / depth-stencil mode 三组读写、
`nvnTextureGetDepthStencilMode`、`nvnTextureGetViewOffset`、`nvnTextureCompare`，以及 texture /
sampler descriptor pool 的 `GetMemoryPool`。自测码 `16432`（view）与 `16433`（texture state）。

Window / WindowBuilder 侧补齐 13 个入口：builder 的 device / native window / present interval /
纹理列表与 active count 可回读，`SetDefaults` 会清空纹理列表但保留 device；window 侧的
native window / present interval / active count / crop / debug label 可读写。`GetCrop` 未设置时
返回第一个交换链纹理的尺寸；`SetCrop` 目前只记录，present blit 还没有消费它。自测码 `16434`。

Program / Sampler 侧再补 5 个入口：`nvnProgramSetDebugLabel`、`nvnProgramSetShadersExt`（每个 stage 的 0x18 字节 `{data, control, ext}` 转成普通 `NVNshaderData`，ext 不消费）、`nvnProgramSetSampleShading`（只记录，Metal 没有独立开关）、`nvnSamplerBuilderGetDevice` 与 `nvnSamplerGetDebugID`。自测码 `16435`。

Device / 帧计数器侧再补 26 个入口：Device 域 20 个（`Finalize`、`ApplyDeferredFinalizes`、
`FinalizeCommandHandle`、`GenerateDebugDomainId`、`SetDebugLabel`、`Set/GetDepthMode`、
`Set/GetWindowOriginMode`、`InstallDebugCallback`、`IsExternalDebuggerAttached`、
`WalkDebugDatabase`、`RegisterFastClearColor(ui/i)`、`RegisterFastClearDepth`、
`SetIntermediateShaderCache`、`GetInteger`、`GetCurrentTimestampInNanoseconds`、
`GetTimestampInNanoseconds`），CommandBuffer 3 个（`ReportCounter` / `ReportValue` /
`ResetCounter`），Texture 1 个（`GetZCullStorageSize`），TextureBuilder 2 个
（`SetPackagedTextureLayout` / `GetZCullStorageSize`）。自测码 `16436`。

`GetInteger` 的属性表取自 nxnative 的 Tegra X1 profile（API 55.15），不是对 Metal 能力的
描述；本标题（1.6.0）是否也报 55.15 需实机/私有运行日志确认。`ReportCounter` 写 16 字节
`{counter, timestamp}`，类型 `0x10` 用 BeginRecording 时刻，其余用 EndRecording 时刻，两者都是
宿主 `steady_clock` 的提交时刻而不是 Metal GPU 时间，因此只能当作顺序/间隔的近似；samples-passed
等计数一律写 0，occlusion query 没有实现。ZCull storage 返回 0 是因为 Metal 不做 ZCull 驻留，
不是占位值。

memory pool 与纹理存储侧再补 8 个入口：`nvnMemoryPoolMapVirtual`（连同 virtual pool 的
初始化）、4 个 storage class getter（builder / 对象 × class / raw）与
`nvnTextureBuilderSetPackagedTextureData` / `GetPackagedTextureData` /
`GetPackagedTextureLayout`。自测码 `16437`。

`MapVirtual` 不建页表：只有“所有请求指向同一个物理池、`物理偏移 - 虚拟偏移` 一致、
且请求恰好铺满整个虚拟区间”时才把虚拟池的存储别名到物理池，其余（真正的稀疏映射）
明确返回失败并打日志。storage class 返回 0 / raw 返回 1，是 nxnative HLE 的答案
（`NVNstorageClass` 在公开头里只是 `typedef int`，没有可对照的枚举）；映射请求自带的
storage class 只进日志，不折进 getter。packaged texture data 在 `nvnTextureInitialize`
时按 layout 的 `size` 拷进池，与 nxnative 一致。

纹理读回侧再补 2 个入口：`nvnTextureReadTexels` / `ReadTexelsStrided`。它们和
`WriteTexels` 共用同一段区域/视图/步长解析（`tex_region_prepare`），只是拷贝方向相反，
所以两条路径不会对“该读哪些字节”产生分歧；`nvn_texlayout` 里新增的
`nvngfx_tex_read_region` 在 Windows 单测里覆盖了紧凑/带行距、LINEAR 行距、越界与
BCn 块对齐的拒绝。自测码 `16438`。已经渲染过的纹理其像素在私有 `MTLTexture` 里，
读回需要 blit + 格式转换，这一路还没接，所以那种情况只报一次“guest 副本已过期”。

**必须说清楚的七件事**，否则这张表会误导人：

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
2. ClearColor / ClearColori / ClearColorui 的完整清屏走 `loadAction = .clear`；
   部分通道 mask 走 Load + 全屏三角形按写掩码清除（RGBA8 的 R/G 合并与
   RGBA8Uint 的 R/G 掩码已在模拟器回读）。
3. `MTLRenderCommandEncoder` 会在绑了渲染目标之后由 `ensure_encoder()` 创建，
   draw 也确实走到 encoder。`BindProgram` 之后 `ensure_pipeline()` 会调用
   `nvngfx_shader_to_msl()`；**它现在返回 NULL**（没有翻译器），所以
   `DrawArrays` 打一条 `noPipeline` 日志就返回。**客体的 draw 仍然是 0 条。**
   但这条链本身已经能跑：`nvngfx_metal_selftest()` 用内置 MSL 走完
   program -> pipeline -> draw -> 回读，并由 CI 用 `simctl` 在模拟器里执行 App 的自检钩子。
4. MRT 已支持 1–8 个颜色目标（每个目标独立格式、混合、写掩码与清屏）；
   `setRenderTargets` 的 texture view 参数仍只记日志。命令缓冲经 `metal_resolve`
   分发表进入时，90 个状态/绘制入口由 `BIND_CB` thunk 记成宿主命令段；
   `EndRecording` 不再一律立即提交，而是返回唯一 `NVNcommandHandle`，
   `nvnCommandBufferCallCommands` / `CopyCommands` 记录 `OP_CALL` 等价项，
   `nvnQueueSubmitCommands` 按提交顺序展开并递归执行嵌套段（深度上限 12、
   有环检测），最后才在拥有者的 `MTLCommandBuffer` 上编码和提交。
   `nvnDeviceFinalizeCommandHandle` 释放段，上下文销毁时清空剩余段。
   这仍不是 guest 命令内存解析：记录的是宿主捕获的调用，不是客体写进
   `AddCommandMemory` 的字节。

5. 顶点缓冲与顶点参数缓冲区已经接通：stream 从 pool 零拷贝绑定，顶点常量在
   `buffer(0)`，stream 从 `NVNGFX_VERTEX_STREAM_BASE`（16）开始。自检 draw 2/3
   使用真实顶点缓冲，draw 8 使用顶点阶段参数缓冲区并回读平移后的像素。客体尚未
   跑到这些路径。

6. texture view 的 16 字节状态对象可读写，但通用 view 的 swizzle /
   depth-stencil mode 还没有应用到采样或渲染目标；GL 名称只是记录，没有 GL
   interop；`nvnTextureCompare` 的字段集按 nxnative 选择（描述符、存储身份、
   通道映射），Nintendo 的精确语义未核实。

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
- ✅ `DrawArraysInstanced` / `DrawElementsInstanced` / `DrawElementsBaseVertex`
  已编码；负参数明确拒绝，零 count / 零 instance 是合法 no-op；
- ✅ `DrawArraysIndirect` / `DrawElementsIndirect` 的参数 record 由 GPU 从
  guest pool 的 `MTLBuffer` 读取；数组 record 为 16 字节，索引 record 为
  20 字节，均要求 4 字节对齐，索引缓冲偏移按索引宽度对齐
- ❌ `nvngfx_shader_to_msl()` 没有翻译器 -> 客体的 program 永远建不出管线

**已经能证明的部分**：`nvngfx_metal_selftest()` 用内置 MSL 走完
texture -> render target -> clear -> program -> pipeline -> draw -> 回读，
并额外创建、比较和读回 `MTLSamplerState`。CI 在 iOS 模拟器里用 `simctl`
启动 App（`--selftest`），App 在 `init()` 里跑它、把
`BOTW_SELFTEST rc=<n>` 写进容器的 `Documents/selftest.txt` 并 `exit(rc)`；
现在跑**十八条** draw（模拟器实际执行十七条）以及七阶段实例化 / base-vertex / indirect 回归（其中六条真实编码、一条零实例 no-op；`[[vertex_id]]` / `[[attribute(0)]]` / 16 位索引的
`DrawElements` / `BindUniformBuffer` 参数缓冲区 / 从 texture pool + sampler pool
取句柄并采样 2x2 纹理 / 只通过 `[[id(0)]]` 读 c1 常量段 / bindless 句柄表 / 顶点阶段
参数缓冲区 / 从 guest pool 解块上传的 2x2 纹理 / 从 `<app>/shaders` 的
`manifest.tsv` 装载 MSL 并按 guest key 建管线）加一条 sampler 状态路径和一个 compute dispatch 回归，
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
  模板格式、采样数、顶点布局与状态哈希。每个 `Program` 的管线缓存是
  16 槽 MRU：命中项移到队首，淘汰最久未用项；模拟器用 16 个不同状态键填满并逐一回查。
  `SetStencilRef` / `SetStencilMask` /
  `SetStencilValueMask` 已接入：引用值放 encoder，前后脸读/写掩码进深度模板状态缓存键。
  `ClearDepthStencil` 的完整掩码转成深度/模板附件的 load action，并在 EndRecording 或换
  render target 前强制落地；8 位模板部分掩码走独立 depth/stencil-only pass，加载旧模板后以
  ALWAYS/REPLACE + 写掩码的全屏三角形修改选中的位。`SetDepthClamp` 映射到 `setDepthClipMode:`；`SetDepthRange(s)` 无论标量还是数组形式
  都落到同一个单 viewport 状态；`SetDepthBounds`
  只记录状态，Metal 没有深度边界测试，非默认范围会明确上报。MRT 已支持 1–8 个颜色附件：
  管线缓存键含每个目标的格式，描述符按目标填混合/写掩码，render pass 按目标挂载并允许各自的
  清屏值。模拟器用两个颜色目标验证了独立清屏和双颜色输出。0 颜色附件的纯深度 pass
  也已支持：`SetRenderTargets(0, depth)`、深度清屏和 draw 能打开只有深度附件的 pass；
  模拟器用“先以 LESS 写 z=0.5，再以 EQUAL 画进颜色目标”验证深度值真的写入。
  ClearColor / ClearColori / ClearColorui 共用 pending-clear 路径：整数分量先精确提升为
  double 再交给 MTLClearColor，模拟器在 R32Uint / R32Sint 目标上回读 0x12345678 与
  -123456789 的原始字节。`ClearBuffer` 使用一个很小的宿主 MSL compute kernel：Metal 的
  blit `fillBuffer` 只能重复单字节，而 NVN 的 value 是 32 位字。kernel 在同一个
  `MTLCommandBuffer` 内写入已注册 memory pool 的 span，支持任意 32 位模式，地址和长度
  要求 4 字节对齐；模拟器用 0xA1B2C3D4 回读 8 个 word。仍缺
  `SetStencilCullCriteria`；guest `DispatchCompute` / `DispatchComputeIndirect` 已支持 compute-only 程序的 direct-binding 路径（uniform 为 `[[buffer(slot)]]`、storage buffer 为 `[[buffer(16 + slot)]]`、image 为 `[[texture(slot)]]`、sampled texture 为 `[[texture(16 + slot)]]`、sampler 为 `[[sampler(slot)]]`；组合与分离纹理 / sampler 句柄都解析，sampled texture 在 compute encoder 打开前解块上传；受 Metal 每阶段 31 个 buffer / texture 参数限制，storage 与 sampled texture 各 15 槽，uniform / image / sampler 各 16 槽，越界绑定明确拒绝；线程组大小来自 MSL 侧车 `// nvngfx-threadgroup X Y Z`，indirect 参数由 GPU 从 guest pool 读取），但仍缺 compute image 写回 guest 存储 / CPU 回读、混合 graphics+compute 程序与真实标题 compute blob；颜色清屏的部分通道掩码已改为
  Load + 全屏三角形（管线写掩码选通道），模拟器验证 RGBA8 的 R/G 合并清屏与
  RGBA8Uint 的 R/G 掩码清屏保留 B/A。`ClearTexture` / `ClearTexturei` /
  `ClearTextureui` 已支持 2D / 2D array / cube / cube array / 2D MSAA 颜色纹理的
  完整级别、region、mask 与完整多 mip 清屏；模拟器回读完整、区域、mask、整数、
  cube 六面和 mip 1。显式 region 仅接受单 level 视图，1D / 3D / depth-stencil
  明确拒绝。模板部分掩码无法用 load action 表达，现由独立的 depth/stencil-only pass
  处理：模板附件 `Load` 保留旧值，全屏三角形用 `REPLACE` 和 8 位写掩码更新。模拟器先清
  `0xF0`，再以 mask `0x0F` 写入 `0x5A`，最后用 `EQUAL 0xFA` 的 draw 和新像素回读确认结果。
- ✅ Sampler：M3.82 完成 builder、object、getter 和真实的
  `MTLSamplerState`；M3.83 完成 sampler pool、`BindSeparateSampler(s)` 与
  参数缓冲区/直连绑定两条路径。M3.84 已把真实 shader 的 `[[id(k)]]` 顺序
  写成 `gen_blobs.py` 侧车并在 `prog_set_shaders()` 里应用；M3.85 接上
  c1 常量段；M3.86 接上 packed slot 与 bindless；M3.87 接上顶点阶段参数缓冲区
- ✅ Buffer / MemoryPool：builder、对象查询和映射都对齐到 pool 的零拷贝 storage；
  `nvnCommandBufferUpdateUniformBuffer` 在录制时快照 CPU 字节、提交时用 blit 写入，
  `CopyBufferToBuffer` 走 GPU blit，`Barrier` 由单 command buffer 顺序保证。
- ✅ Buffer ↔ texture copy：`CopyBufferToTexture` / `CopyTextureToBuffer` /
  `CopyTextureToTexture` 都走 `MTLBlitCommandEncoder`；buffer 一侧按命令缓冲的
  copy row/image stride（0 = tight）解释，region 与 `NVNtextureView` 逐层校验。
  只做同格式、同类型、非 BCn transcode、非多重采样的 copy，mirror flag 明确拒绝；
  目标纹理写入后标记为已上传，避免后续采样用旧 guest 字节覆盖。
- ✅ CommandBuffer 生命周期与调试：`Initialize` / `Finalize` / `IsRecording` 按记录状态回答；
  `AddCommandMemory` / `AddControlMemory` 只做记账——回放路径不写 guest 命令内存，所以
  `GetCommandMemoryUsed` 恒为 0、`Free` 等于 `Size`，空 pool 的补段调用也接受而不解引用；
  memory callback 只存取、从不回调；debug label / marker / group 转成 `MTLCommandBuffer` 的
  `label`、`insertDebugSignpost:` 与 `push/popDebugGroup:`，group 栈在每次录制开始时清空。
- ✅ 逐 draw 固定功能状态：point size / line width / sample mask / primitive restart index /
  conservative raster 及其 dilate / subpixel precision bias / tiled cache action 与 tile size /
  ZCull save、restore / shader scratch memory 共 13 个入口。前几类 Metal 没有对应 API，
  值被记录并各上报一次；primitive restart 在 index 为 0xFFFF / 0xFFFFFFFF 时与 Metal 的
  固定值一致；rasterizer discard 真正生效——draw 被消费、计入 `discardedDraws`，但不会
  进入 encoder，也不会被误报成缺 shader。
- ✅ 纹理对象查询：14 个 getter（宽、高、深、级别、采样数、format、target、flags、pool、
  offset、stride、debug id、纹理地址、storage size）与 swizzle 四路输出都按 guest 通过
  builder 设置的值回读；storage size 由 `nvngfx_tex_layout` 计算。另接 `SetDebugLabel`、
  `Finalize`（池槽位之后解析为 null）和 `InvalidateTexels`（丢弃一次性上传，下次采样重读
  guest 池；已渲染纹理保留宿主内容）。
- ✅ 纹理 builder 查询：11 个标量 getter（宽、高、深、级别、采样数、format、target、flags、
  pool、offset、stride）加 storage size / alignment 与 swizzle 四路输出，全部回答 setter
  记录的值；storage size / alignment 由 `nvngfx_tex_layout` 计算。未设置过的 builder 返回
  NVN 默认值（1×1、1 级、R/G/B/A），不是全零。
- `fence / sync / events` 和 `MapVirtual` 仍未接

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

## 10. 剩余 32 个入口点的优先级

不要按字母序做。按“挡住第一个 draw call 的”排序：

| 优先级 | 入口 | 理由 |
|---|---|---|
| P0 | ~~`nvnCommandBufferSetRenderTargets`~~ | ✅ 本轮已实现；只剩 MRT / 深度 / view |
| P0 | ~~`nvnCommandBufferBindProgram` / `nvnProgramInitialize`~~ | ✅ 本轮已实现（管线建到 `nvngfx_shader_to_msl` 为止） |
| P0 | ~~`nvnCommandBufferBindVertexAttribState`~~ | ✅ 本轮已实现（含 `nvnVertexAttribState*` / `nvnVertexStreamState*` 与 `BindVertexStreamState`） |
| P0 | 真实 shader 的第一次 draw | UBO/texture/sampler、`[[id(k)]]` 侧车、c1、packed slot、bindless、顶点参数缓冲区、分层/LINEAR/BC1–BC5 纹理上传都已接上；仍缺把标题 shader 真正翻成 MSL 的离线链路，以及 BC6H/BC7、texture view 和任意 mapped write 的精确脏页跟踪 |
| P2 | ~~`nvnTextureBuilder*` 剩余 1 个~~ | ✅ 已做 43/43：`GetSparseTileLayout` 报 64 KiB tile 的 tiled level 数与尺寸 |
| P1 | ~~`nvnSamplerBuilder*` + `nvnSampler*` + sampler pool~~ | ✅ M3.82/M3.83：38 个 sampler 入口、8 个 pool/bind 入口，能建 `MTLSamplerState` 并从 pool 解析句柄；`SamplerBuilderGetDevice` 无已知形状、`SamplerGetDebugID` 的返回值语义未定 |
| P1 | `NVNindexType` 枚举值核对 | nxnative 的 `nvn.h` 给出 0=u8 / 1=u16 / 2=u32；后端只接 1/2，u8 未知值不画，且该头文件是 reimplementation 而非 Nintendo SDK |
| P0 | Maxwell SASS -> MSL 翻译器 | 不是入口点，是整条链的墙 |
| P2 | 各 `*StateSetDefaults` / `*StateGet*` | 让 guest 的查询路径不崩 |
| P2 | ~~六个 state 的 getter（约 36 个）~~ | ✅ 已接 33 个；BlendState 的 `AdvancedNormalizedDst` 两项与 MultisampleState 的 `SampleLocationsGrid` 因无语义依据保留 |
| P2 | ~~`nvnCommandBufferBarrier`~~ / ~~`nvnSync*`~~ / ~~`nvnEvent*`~~ | ✅ Barrier、Sync、Event 已接；fence 等最后提交的 command buffer，event signal 在 GPU 完成回调里写回 |
| P3 | ~~`nvnQueue*` 的其余 14 个~~ | ✅ 已接；但仍是单 Metal 队列，多队列语义没有验证 |

顺便一提：`nvnBootstrapLoader` 在注册表里，但它不是给游戏用的入口，
不实现也没关系；`nvngfx_lookup` 对未实现的入口返回**日志桩**而不是 NULL，
所以不会有空指针跳转。

---

## 11. 已知缺口（照抄，别当成 bug 报）

| 缺口 | 现状 | 影响 |
|---|---|---|
| 客体核心 | 1.6.0 四模块的 AOT 改写链路已在 `scripts/guest` 跑通（vendored nxnative，MIT）；iOS 工程仍未链接运行时 loader / HLE | 真实标题的 draw/present 仍只存在于 Windows Unicorn 捕获，iPhone 上不会执行 |
| 渲染目标绑定 | 1–8 个颜色附件、深度/模板附件、纯深度 pass 与 MRT 已接；通用 texture view 仍只记录 | 通用 view 的绑定仍不完整 |
| 命令句柄回放 | `CallCommands` / `CopyCommands`、嵌套展开、句柄释放与录制时输入快照已接；实现是宿主调用捕获，不是 guest 命令字节解释器 | 不能观察或原地修补客体的命令记录；持久对象（纹理/程序/池/事件）仍按回放时状态解析；单段 arena 上限 256 MiB |
| 着色器翻译 | 只留了接口，没有实现 | 客体的 draw 全部停在 `noPipeline` |
| 纹理创建 | 建 host `MTLTexture`；首次采样时从 guest pool 解块/上传，WriteTexels/FlushTexels 后标脏重传 | 2D/数组/cube/LINEAR 和 BC1–BC5 可采样；BC6H/BC7、通用 texture view 与任意 mapped write 的脏页跟踪仍不完整 |
| draw 执行 | 有 pipeline 就真的编码 draw；没有就丢弃；实例化、base-vertex 与两种 indirect draw 已接 | iOS 客体仍是 0 条（未链接客体核心）；Windows 捕获侧已有 707 draws / 304 presents；自检十八条 draw（模拟器执行十七条）加七阶段 instanced / base-vertex / indirect 回归、sampler 状态与 compute dispatch 回归由 CI 执行 |
| 顶点输入 | attrib/stream 已接，缓冲从 pool 零拷贝绑到 `MTLBuffer` | 自检的第二条 draw 就是这条路；客体尚未跑到 |
| 浮点参数 | `nvngfx_args.f[8]` 已扩展，float 在低 32 位 | 本仓库外 guest 调用端仍需实现 s/d 寄存器桥 |
| 采样器资源 | builder/object、sampler pool 与 `MTLSamplerState` 已建，句柄可解析 | `[[id(k)]]`、c1、packed slot、bindless 与顶点参数缓冲区已接；真实标题 shader 仍缺离线翻译链 |
| 先进混合 | `adv` / `adv_npm` / `adv_ovl` 已按实测布局记录并可回读；`NormalizedDst` 未接 | Metal 管线只应用经典 blend 字段，advanced blend 还没有翻译成 `MTLBlendOperation` |
| 同步/fence | Sync / Event / Queue 已接：fence 等最后提交的 `MTLCommandBuffer`，事件值由 completion handler 写回；`WaitEvent` / `WaitSync` 按单队列顺序视为已满足 | 多队列、跨队列等待与 present blit 未覆盖；`QueueFinish` 不等待 present blit 的独立 command buffer |
| compute | compute-only 程序的 direct / indirect dispatch 已接（uniform / storage buffer / image / sampled texture / sampler 直连绑定，组合与分离句柄；sampled texture 先上传）；真实标题 blob 与 image 写回 guest 未接 | 后处理仍不可用 |
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
| `[nvn] not implemented: nvnYyy` | 该入口不在 483 个里，按第 10 节排期 |

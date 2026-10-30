# BOTW · iOS 移植工程

《塞尔达传说：旷野之息》(Switch 1.6.0) 到 iOS 的移植工程骨架。
架构参照 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)：**原生 C++/Metal 后端 + 把 Switch 二进制里的
SDK/NVN 调用整层接管（HLE）**，而不是跑一个通用模拟器。

> 本仓库**不包含**任何游戏文件。构建时需要你自己提供已购买的 1.6.0 更新数据，
> 解包工具在私有工作区（`C:\botw-work`）里，产物不进版本库。

## 目录

```
ios/BotW/       SwiftUI + Metal 壳（CAMetalLayer 视图、CADisplayLink 驱动、NVN 覆盖数 HUD）
ios/nvn/        NVN host-HLE：513 入口点注册表、生成的分发表、Metal 后端
  registry.json   从标题自己的 getproc 字符串提取，29 个域
  src/nvn_metal.mm 131 个入口点的 Metal 实现（其余是日志桩）
  src/nvn_shader.c 着色器翻译接口（现在只会返回 NULL + 理由）
  src/nvn_texture.c 157 项 NVNformat -> MTLPixelFormat 映射表（带 lossy 标记）
  src/nvn_null.cpp 空后端：任何主机都能编译、跑 ABI 测试
  shaders/        空目录，路线说明在里面的 README
core/           与 iOS 对接的 C ABI（Swift 只能看到 botw_core.h）
scripts/        注册表提取、代码生成、门禁校验、MSVC 编译脚本
tests/          513 入口点一致性 + C ABI 冒烟 + 157 项纹理格式表测试
docs/           路线图、HLE 边界、NVN 覆盖表、iOS 移植手册、着色器链
.github/        GitHub Actions：无 Mac 也能编译验证
```

**从 `docs/IOS_PORTING.md` 开始读**，那是 iOS 侧的操作手册与缺口清单。

## 当前状态（里程碑）

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | iOS 壳 + Metal 渲染 + CI 编译 | ✅ 本仓库 |
| M1 | 解包/装载 NSO，跑通 `.text` | ✅ 私有工作区 |
| M2 | 启动链 HLE（nn::oe / fs / vi / nv / NVN 引导） | ✅ nn::init::Start 跑完 |
| M2.5 | Horizon 线程 + IPC/CMIF 服务端（hid/sm/pctl） | 🟢 主循环已跑起来 |
| M3 | NVN 对象层 + 命令流录制（`m3_nvn.py`） | 🟢 对象/记录已就绪，draw=0（游戏还没提交几何） |
| M3.8 | LightEvent 同步（SVC 0x34/0x35），资源加载真正完成 | ✅ 53 MB FSHA 可完整落盘并重定位 |
| M3.10 | `mrs CNTPCT_EL0` 恒为 0 → `GetSystemTick` 改成 HLE（**宿主时钟那版已被 M3.18 取代**） | ✅ 定时/超时循环不再自旋 |
| M3.11 | `svcSleepThread` 从"空让出"改为真按 ns 挂起 | ✅ 空闲轮询不再吃满 CPU |
| M3.13 | `fs::ReadFile` 的 147 MB 缓冲区被逐 64 KiB 提交（2247 次 `mem_map`）→ HLE 时间 27.9 s 降到 0.27 s | ✅ |
| M3.14 | `UC_HOOK_BLOCK` 53 倍代价：热页采样下 2 MIPS，无钩子 19 MIPS；长跑一律无钩子 | ✅ |
| M3.15 | `nvnBufferMap` / `nvnMemoryPoolMap(MapVirtual)` 签名写错，往脏 `x1` 写 8 字节 | ✅ |
| M3.16 | 纹理存储尺寸：`BPP` 表漏 `DEPTH16`、RG8/RG16 记错，块压缩格式全缺 → 解码器越界覆盖空闲块头 | ✅ |
| M3.17 | `audren` 更新协议不可补，音频/账号客户端入口整体替身（`m2_audio.py` / `m2_account.py`） | ✅ |
| M3.18 | 时间源虚拟化：`GetSystemTick` 接宿主时钟导致同一份脚本跑出 21 帧 / 8 帧两种结果 | ✅ |
| M3.19 | 资源表是**新建的空表**而不是被覆盖；分支看 `A[1]`（结论已按 M3.20 更正） | ✅ |
| M3.20 | `nvnProgramSetShadersExt` 返回 0 = false，游戏判定着色器没装上 → `+0xD0/+0xD8` 空表 → 空指针崩溃 | ✅ 崩溃已消失 |
| M3.21 | `draws=0` 曾是假指标：`nvnDrawArrays` 一族未实现，全是静默 no-op | ✅ 已如实计数 |
| M3.22 | 存档挂载 → 线程别名栈（pc=0）→ `nn::time` → applet 启动参数，四道门连续推平 | ✅ 300 s 无 FAULT |
| M3.23 | 调度器只在 SVC 处让出 → 一个不含 SVC 的忙等会饿死全部工作线程；分块边界改成时片（2M 指令 ≈ 1.1 ms） | ✅ 178 → 199 帧 |
| M3.24 | mutex 的 `WAITERS_BIT`：解锁走了用户态快路径，等待者永远醒不过来（位算错，见 M3.26） | ✅ 帧泵活了 |
| M3.25 | 反汇编纠正 M3.24：等待位是 bit30；`DT_RELR` 在 1.6.0 不存在（loader 无需改） | ✅ |
| M3.26 | 互斥真正修好：主线程 tag=0 让 M3.24 的互斥形同虚设；现在线程真同步、资源真加载 | 🟢 20 帧 + 541 MiB，卡在吞吐 |
| M3.27 | `GetSystemTick` 的 HLE 往返 35 µs × 几十万次 = 30 s 死窗口；改成入口一条 `b` + 内联计数器 | ✅ 死窗口消失 |
| M3.28 | 178 帧后静止的全量快照：主线程无限等 LightEvent，32 线程 idle，唯一活动是 250 µs 轮询 worker | 🔴 缺完成通知 |
| M3.65–M3.71 | 12 MB 缓冲区是包尾 `0x49E000` 的逐字节拷贝；真正卡点是 58.2 MiB 分配返回 0；最小跳过让字节序转换器返回 | ✅ 结论已定，draws 仍为 0 |
| iOS 骨架 | 513 入口点注册表 + 生成分发表 + Metal 后端 131 项 + 三重门禁 | 🟡 十二条合成 draw 已在 CI 模拟器通过（M3.92 的 cube：run 41080975529；第十三条 cube array 需 `BOTW_SELFTEST_CUBE_ARRAY=1`，模拟器的 Metal 建 CubeArray 会崩），含 bindless、顶点参数缓冲区、c1 常量段、guest pool 解块上传和离线 MSL bundle；客体仍无画面 |
| M4 | NVN → Metal 回放（管线/纹理/命令缓冲） | ⬜ |
| M5 | 音频（nn::audio → AVAudioEngine）、输入 | ⬜ |
| M6 | 真机性能与内存（6 GB 机型起步） | ⬜ |

### 最新一段（M3.16 → M3.23）

无钩子长跑现在的极限是 **199 帧 / API 1087 / draws 0**，卡在资源系统：

* **M3.16** `nvnTextureBuilderGetStorageSize` 的 `BPP` 表错（RG8/RG16 记错、`DEPTH16`
  缺失、DXT/RGTC/BPTC/ASTC 全缺）→ 返回偏小 → 游戏按它开 pool → 法线图解码器写满就越界，
  覆盖了分配器的空闲块头。修完这个 FAULT 消失。
* **M3.17** 音频 `audren:u` 的更新协议等价于重写一遍 yuzu 的 audren 服务端，而它对"第一帧
  Draw"毫无贡献 → 把碰 IPC 的客户端入口整体替身（18 个），纯计算的 config/voice 仍让 guest
  真跑。顺带修掉 `GetMemoryPoolState` 的 5 ms 轮询死等，以及 `nn::account` 的 NULL 全局。
* **M3.18** `nn::os::GetSystemTick` 原来接宿主单调时钟 → guest 的超时按宿主时间算，宿主被
  抢占就"更慢"，同一份脚本 60 s 能到 21 帧、被占满时 200 s 只到 8 帧。改为每次调用前进固定
  tick（`BOTW_TICK_STEP`），换回确定性。
* **M3.19（结论已更正）** 崩溃点上 `mgr`（`0x41D40D30` 新建）的 `[mgr+0xD0]`/`[mgr+0xD8]`
  是空的，写入监视也证明这个对象不是被 Yaz0 覆盖。但原来说的"378 个表 / 注册点零命中"站不住：
  `378` 和 `151` 来自两次不同 `LIM` 的统计，而 `0x41D41038` 只是 `A[0]&2==0` 那条子路径的
  注册点，不能用来推断另一条分支。实测 `A` 的形状只有 `(A[0],A[1]) = (2,0)` 一种。
* **M3.20（本轮，已修）** 那句话里的"注册调用"其实是 `nvnProgramSetShadersExt(program, 2,
  shaders)`，它返回 `NVNboolean`。我们没有实现它 → 落进 `_nvn_miss` → **返回 0 = false**，
  游戏于是判定"program 没装起来"，`mov w22,#3` 立刻返回，`mgr+0xD0`（6 个 stage 指针的数组）
  与 `mgr+0xD8`（反射表）都不写；再走几十条资源，才在 `0x41D41A10` 以空指针崩掉——崩溃点
  离根因隔着一整个资源循环。补上 NVN 的 program 层（14 个入口，`SetShadersExt` 存 shader
  记录并返回 1）之后：150/150 次调用返回 1、`+0xD0` 写入 150 次、**空指针崩溃消失**；
  无钩子 150 s 跑到 175 帧无 FAULT，帧时间线显示 21 帧后进入资源加载（Yaz0 解压吃满 CPU）。
  同一轮还接上了 `nn::nfp` 的 20 个入口（`m2_nfp.py`，"已初始化、无设备"），并修掉
  `AttachActivateEvent` 的 mangled name 长度前缀（`21`→`19`）——差一个字符整条 HLE 就不生效。

* **M3.21** `m3_nvn.py` 里 `OP_DRAW`/`OP_DRAW_IDX`/`OP_DISPATCH` 只存在于名字表里，
  `nvnDrawArrays` 一族**一个都没实现**，全落进 `_nvn_miss` 静默丢——所以那之前的
  `draws=0` 是结构性假象，不能当成"游戏没画"。现在补了 8 个入口（含实例化/multi/dispatch）
  并如实计数，`DRAWS != 0` 才是可信的 M4 判据。
* **M3.22（本轮）** 资源系统通了之后连续四道门：① `nn::fs::MountSaveData` 缺失 →
  `Abort(0x1A80A)`（fs 模块），改成把 `save:` 映射到私有宿主目录，存档是真文件但不入库；
  ② 线程启动 `pc=0` FAULT —— `MapAliasStack` 的参数被解读错（`x0` 是 this、`x1` 才是别名、
  `x2` 源栈、`x3` size），别名区永远是零页，`*(stack_top-0x20)` 取到的入口是 0，改成按 4 KiB
  页镜像主栈（源栈懒提交，必须逐页容错）；③ `nn::time` 7 个入口要求 `time:u` 的共享内存 →
  改为固定时钟（2020-01-01T12:00:00Z）；④ `applet::TryPopFromApplicationParameterChannel`
  （SDK 内部调用，main 不导入）答"没有启动参数"。300 s 无钩子长跑：175 帧 → `time` 通过 →
  持续解包（chunks 2116→4945）、无 FAULT、`committed` 1151 MiB 持平。

* **M3.23（本轮）** M3.22 之后的长跑出现新的"假卡死"：帧停在 178，PC 采样有 1544 次
  落在 `0x41062388` 同一条指令上——那是资源管理器状态机里"在途请求计数 `[mgr+0x38]` 非零就
  原地重试"的忙等。根因不在 guest：我们的调度器**只在 SVC 处切线程**，于是一个不含任何 SVC
  的 `while` 就把所有 runnable 的工作线程永久饿死（真机是 1 ms 定时器抢占）。修法是把
  `CHUNK`=2M 指令的分块边界当成时片（2M/1.8 GHz ≈ 1.1 ms），到点保存上下文并轮转。
  效果：帧 178 → 199、API 895 → 1087、无 FAULT，FSLOG 里第一次看到完整的标题资源
  （`Title.pack` 4.9 MB、`TitleBG.pack` 147 MB 全部读入）。但 **t≈220 s 之后整机静止**
  （`frames`/`API`/`committed`/`chunks` 四个数 900 s 内一动不动），全线程栈显示任务系统的
  入口都空转在 `ReceiveMessageQueue`/`AcquireSemaphore` 上——这是下一道墙，不是变慢。
  顺带更正两个观测口径问题：
  `chunks` 不是指令计数器（SVC 密集时它在 250 s 里一动不动，而主线程调了 80 万次
  `GetSystemTick`），`0x4191F000` 那页也不是"任务树轮询"而是逐字节 LZ 解码器。

细节、地址与下一步实验都在 [`docs/ROADMAP.md`](docs/ROADMAP.md) 的 M3.16–M3.23。

## M2 进展

本轮把 **IPC/CMIF 服务端**做出来了，游戏于是从"启动即 abort"变成**真的在跑主循环**：

* 启动改走真实链路 `nn::init::Start(heap, arg, nninitStartup, nnMain)`，**完整返回**
* NVN 引导：`nvnBootstrapLoader` → `nvnDeviceGetProcAddress` → **512 个入口全部解析**
* 3.22 GiB 主堆（`AllocateMemoryBlock(0xC1000000)`）分配成功
* 4 个 guest 线程（含 x0-x28/Q0-Q31/SP/PC/NZPCR/TLS 的完整上下文切换；
  M3.23 起按 2M 指令时片抢占，不再只依赖 SVC 边界）
* **hid 服务打通**：`CreateAppletResource` → `GetSharedMemoryHandle` → `svcMapSharedMemory(0x40000)`
  → `SetSupportedNpadStyleSet` / `SetSupportedNpadIdType` 等命令全部返回成功
* 之后进入渲染主循环：每帧 `nvnCommandBufferBeginRecording … EndRecording` +
  `nvnWindowSetPresentInterval` + 等 vsync，回归跑 45 秒可录到 **13000+ 帧（≈295 fps）**

## M3 进展（本轮）

### NVN 对象层（`tools/m3_nvn.py`）

照 D2R `dm/gfx/nvn/nvn_obj.h` 的三条原则实现，**不建指针表**：

1. 对象就地存在**游戏自己分配**的 NVN 对象内存里（magic + 字段），
   预留大小取自 `nvn.h`（device 0x3000 / pool 0x100 / buffer 0x30 / texture 0xC0 /
   program 0xC0 / queue 0x2000 / window 0x180 / commandBuffer 0xA0 / sync 0x40）。
2. 命令写进**游戏自己给的**命令内存：`nvnCommandBufferAddCommandMemory(cb, pool, off, size)`
   的命令区 VA 是 `pool.storage + off`，句柄就是段首地址。记录格式 `{op,size}+payload`，8 字节对齐。
3. **每个 NVN 入口都记一条**（含还没实现语义的），所以 dump 出来的是完整调用序列，
   而不是"只有我们认识的调用"。

本轮实测：对象层建立成功（4 个 memory pool、2 张 texture、sampler/texture pool、
queue、window、command buffer、buffer 各 1），每帧写入 **10 条记录 / 416 字节**。

### 关键结论：现在还没有 draw call

```
$ BOTW_NVN_VERBOSE=0 python tools/_m3a.py
frame ops=10 bytes=416
1  BEGIN
2  nvnBufferGetAddress                     a=1080BE020 107EBD020 ...
2  nvnCommandBufferReportCounter           a=107E8B3E0 10 ...
2  nvnCommandBufferSetSamplerPool          a=107E8B3E0 106D8E0B8 ...
2  nvnCommandBufferSetTexturePool          a=107E8B3E0 106D8E098 ...
2  nvnCommandBufferSetShaderScratchMemory  a=107E8B3E0 107FBD040 ...
2  nvnBufferGetAddress                     a=1080BE020 41254F70 ...
2  nvnCommandBufferReportCounter           a=107E8B3E0 0 ...
2  nvnCommandBufferEndRecording            a=107E8B3E0 ...
0  END
```

也就是说：**渲染线程在提交"空帧"**（只设置 pool 与计数），没有 render target、
没有 clear、没有 draw、也没有 present。`nvnQueuePresentTexture` 一次都没被调用。
这不是 HLE 的 bug，是游戏还在资源加载阶段 —— 所以"移植到 iOS 能出画面"离现在仍有
M4（管线/纹理/回放）+ 资源加载的距离。不要把它当成"差一点就能玩"。

### 性能：把 1 MIPS 修回 ~100 MIPS

之前跑 45 秒只能录到 8000 帧，看起来像"卡住"。定位到两个真问题：

| 问题 | 症状 | 修法 |
|---|---|---|
| `emu_start(..., timeout=...)` | 一旦传 timeout，Unicorn 2.1.4 的 TCG 主循环退化，同一段 guest 代码慢 ~100 倍（present 循环 305 fps → 2 fps），而且 `count` 与块钩子几乎都不触发 | **永不传 timeout**，改成 `count=2M/20M` 分块，块之间看表 |
| `find_svc` 依赖逐指令 ring | 快路径下 ring 为空，`pc` 落在**下一个** stub 的 `svc` 上 → 派发错函数（表现为 GetProcAddress 循环里反复调用同一个 API，最后跳到 0） | stub 槽改成 16 字节步长（`svc` 后 12 字节 0），`find_svc` 先试 `pc-4` 再试 `pc`，不依赖 ring |

顺带：stub 区改为启动时**一次性预写**（原来每个 stub 现写 4 字节，写入可执行页会让
翻译缓存失效）。现在 `m2_verify.py` 45 秒能跑 **13000+ 帧（≈295 fps）** 且
`ipc missing` 为空。Unicorn 本身实测 ~100 MIPS（1M 条 guest 指令 7.6 ms）。
* 当前一轮 IPC 命令表**零缺失**：`hid` 0/0x64/0x66/0x6D、`IAppletResource` 0、
  `pctl` 0/1/3 全部返回成功（家长控制统一回"无限制"）

当前"跑起来"的含义要说明白：**游戏逻辑在跑，画面还什么都没有**。所有 `nvn*`
仍是记账式桩（只记参数、返回假缓冲），下一阶段才是把它们翻译成 Metal。

> 最新进展（M3.5）：修掉调度器的线程饿死后，启动链已经真正跑进资源加载——
> 游戏会读 `content:/Pack/Bootup*.pack`、`Title*.pack`，并建立 NVN memory pool。
> 当前卡在纹理格式初始化的空指针写，`present`/`draw` 仍为 0。细节见
> [`docs/ROADMAP.md`](docs/ROADMAP.md#m35-真实启动流程本轮突破进行中)。

## IPC/CMIF 服务端（本轮核心）

我们没有绕过 IPC，而是把 guest 自己的 `nn::sf` 序列化层留在原地，只实现**对端服务**：

```
guest 业务代码
  → nn::sf 代理（序列化/校验/句柄接收）   ← 原样运行
  → svcSendSyncRequest(handle)           ← 我们接管
  → m2_svc.py 命令表（按 (service, cmd) 分发）
```

这样一条通路同时服务所有命令，且校验逻辑与真机一致。协议细节见
[`docs/HLE_BOUNDARY.md`](docs/HLE_BOUNDARY.md#ipccmif-协议实测)。

## iOS 骨架现状（本轮新增，全部可机器验证）

这不是"能玩了"，是"接口面已经量出来、能编译、能被门禁卡住"。

| 项目 | 数字 | 怎么来的 |
|---|---:|---|
| NVN 入口点 | **513** | 扫 `main.reloc.bin` 里会被 `nvnDeviceGetProcAddress` 按名解析的字符串 |
| 域 | 29 | 按前缀归类（CommandBuffer 144、Texture 45、TextureBuilder 43…） |
| Metal 后端已实现 | **131**（25.5%） | 真实 `BIND(...)`，不是靠注释声称 |
| 未实现 | 382 | 按"挡住第一个 draw call"排序，见 `docs/IOS_PORTING.md` |
| 生成的代码 | 2 个文件 | `gen_nvn.py` 生成 `nvn_api.h` / `nvn_dispatch.cpp`，`--check` 防手改 |
| 参数形状 | 470 个 | `signatures.json`，解析公开 nxnative 原型表（MIT），最大 arity 8 |

四条诚实说明：

1. **屏幕上的颜色不是游戏画的**。`MetalView` 是 CAMetalLayer 视图，这块层通过
   `botw_core_desc.native_layer` 交给核心；还没有标题代码可跑，所以
   `botw_core_frame()` 用宿主 clear + present 驱动交换链，HUD 的 `presents`
   就是这条路的计数。它证明层/绘制/上屏通了，不代表渲染通了。
2. **客体的 draw 仍然一条都不执行**：program、管线、draw 编码都写好了，
   但 `nvngfx_shader_to_msl()` 没有翻译器（NVN 给的是 Maxwell SASS），
   所以管线建不出来，draw 停在一条 `noPipeline` 日志。
   **例外且刻意**：`nvngfx_metal_selftest()` 用内置 MSL 和离线 bundle 跑十二条合成 draw（cube array 是第十三条，模拟器跳过），
   由 CI 用 `simctl` 启动 App（`BOTW_SELFTEST=1`）执行并回读像素。它证明的是
   Metal 这半边，不是客体能跑。
3. **采样纹理是宿主分配的 `MTLTexture`，首次采样时才从 guest 池填充**：
   `tex_upload()` 按 Tegra X1 的 block-linear 布局逐 mip 解块，再用 blit 拷进
   私有纹理。M3.91 已接 2D 数组的端到端上传，并修正 cube / cube-array 的
   层数与 Metal 描述；目前仍是**一次性**上传，不跟踪 guest 后续修改，LINEAR
   存储、连续更新和 BCn 转码仍未接。渲染目标始终由本后端独占写入。
4. **Release 编不过**：`kEnumsVerified = 0`，`nvn_metal.mm` 用 `#error` 拒绝 NDEBUG 构建。
   把没验证的 NVN 枚举值带上船，等于发一个颜色全错的版本。

## 本地验证（不需要 Mac）

四条命令，全部在 Windows 上跑过（MSVC 2022，`/W4 /WX`）：

```powershell
C:\Python314\python.exe scripts\gen_nvn.py --repo . --check     # 生成物是否最新
C:\Python314\python.exe scripts\verify_ios.py                   # 7 类仓库不变量
C:\Python314\python.exe scripts\nvn_status.py                   # 覆盖进度 + 下一步
C:\Python314\python.exe scripts\syntax_check_metal.py           # Objective-C++ 语法检查（需 clang）
powershell -ExecutionPolicy Bypass -File scripts\build_abi_test.ps1   # 真编译真运行 + 上面那条
```

最近一次实测：

```
nvn_abi_test: ok (513 procs)                 exit=0
core_smoke_test: ok (nvn 0/513 on null)      exit=0
nvn_texture_test: ok (157 formats in table)  exit=0
nvn_shader_test: ok (seam, blob table, packed slots, bindless, c1, SHA-256, shader key, layered fixture)  exit=0
nvn_vertex_test: ok (packing, 26 formats, layout id)  exit=0
nvn_texlayout_test: ok (block shapes, sizes, offsets, round trip, refusals)  exit=0
nvn_shader_bundle_test: ok (manifest, merge, c1, refusals)  exit=0
test_gen_blobs: ok (ids, packed slots, bindless, c1, bundle, holes, refusals)
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 131/513)
build_abi_test: all tests passed
```

Mac 侧还有一条**真的执行**的测试：CI 用 `simctl` 在 iOS 模拟器里安装并启动 App，
`--selftest` 让 App 在 `init()` 里跑完
texture -> target -> clear -> pipeline -> draw -> 回读然后 `exit(rc)`，
结果写进 App 容器的 `Documents/selftest.txt` 由工作流断言。
现在执行十二条 draw；M3.85 的 run 41042618062、block-linear 布局的 run 41047317230、
从 guest 池解块上传纹理的 run 41051685313、离线 MSL bundle 的 run 41064580709、
分层纹理的 run 41070475247 均已通过。第十条真正从 `<app>/shaders/manifest.tsv` 取 MSL、按 guest key
建管线并回读橙色像素；第十一条从 guest pool 上传两层 2D 数组，片段着色器
分别采样 layer 0/1，回读左红右黄；第十二条上传一个 6 面 cube，取 +Z / -Z
两面回读左红右黄（已在 run 41080975529 通过）。第十三条的 cube array 要把
第 11 个 slice 送到 cube 1 的 -Z 面，但模拟器的 Metal 在
`newTextureWithDescriptor`（`MTLTextureTypeCubeArray`）里直接把进程带崩，
所以这条只有显式设置 `BOTW_SELFTEST_CUBE_ARRAY=1` 才跑，模拟器上跳过并打印原因。CI 现在设置
`BOTW_REQUIRE_SHADER_BUNDLE=1`，bundle 缺失会让自检返回 `rc=16397`，不能静默跳过。
它不替代真机，但比“编过了”强一档。

> 为什么不用 XCTest：本仓库先试过 unit-test bundle，`xcodebuild test` 在 runner 上
> 只报 “Executed 0 tests” 然后 TEST FAILED，日志里没有任何原因（拿不到 admin 日志，
> 注解里也只有 348 秒的等待）。`simctl` + 退出码是一条更短、能看见失败原因的路径。

`syntax_check_metal.py` 用 `tests/stubs/` 里的最小 Metal/QuartzCore 桩头文件，
让 clang 以 `-fsyntax-only -x objective-c++` 解析 `nvn_metal.mm`。它已经抓到过一个
真错误（`Tex**` 被当成 `Tex*` 赋值），这是没有 Mac 时唯一能验证 ObjC++ 的手段。
它查不出链接、可用性标注和 ARC 语义——那部分只能靠 macOS runner。

`verify_ios.py --release` **应该失败**（1 个问题：`kEnumsVerified = 0`）。CI 里有一条
步专门断言它失败，因为这正是门禁在工作。

CI 分两个 job：`portable`（Ubuntu，无需 Apple 工具链）负责上面这些；
`ios`（macOS 15）用 XcodeGen 生成工程，再 `xcodebuild` 编模拟器 Debug 版，
并断言 Release 版**必须**编译失败。

## 你需要准备

* 已购买的《旷野之息》1.6.0（含本体 + 更新）
* `prod.keys`（仅本地使用，**永远不要**提交）
* 一台 iPhone（建议 6 GB RAM 及以上），用于真机验证
* 若要出 `.app`：macOS + Xcode 16 或更新 + `brew install xcodegen`
  （XcodeGen 生成 `BotW.xcodeproj`，`.xcodeproj` 本身不入库）

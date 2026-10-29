# BOTW · iOS 移植工程

《塞尔达传说：旷野之息》(Switch 1.6.0) 到 iOS 的移植工程骨架。
架构参照 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)：**原生 C++/Metal 后端 + 把 Switch 二进制里的
SDK/NVN 调用整层接管（HLE）**，而不是跑一个通用模拟器。

> 本仓库**不包含**任何游戏文件。构建时需要你自己提供已购买的 1.6.0 更新数据，
> 解包工具在私有工作区（`C:\botw-work`）里，产物不进版本库。

## 目录

```
ios/            SwiftUI + Metal 壳（M0：可运行的渲染循环）
core/           与 iOS 对接的 C ABI；后续接 NVN→Metal 回放后端
docs/           路线图、HLE 边界说明
.github/        GitHub Actions：无 Mac 也能编译验证
```

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
| M4 | NVN → Metal 回放（管线/纹理/命令缓冲） | ⬜ |
| M5 | 音频（nn::audio → AVAudioEngine）、输入 | ⬜ |
| M6 | 真机性能与内存（6 GB 机型起步） | ⬜ |

### 最新一段（M3.16 → M3.19）

无钩子长跑现在的极限是 **175 帧 / API 895 / draws 0**，卡在资源系统：

* **M3.16** `nvnTextureBuilderGetStorageSize` 的 `BPP` 表错（RG8/RG16 记错、`DEPTH16`
  缺失、DXT/RGTC/BPTC/ASTC 全缺）→ 返回偏小 → 游戏按它开 pool → 法线图解码器写满就越界，
  覆盖了分配器的空闲块头。修完这个 FAULT 消失。
* **M3.17** 音频 `audren:u` 的更新协议等价于重写一遍 yuzu 的 audren 服务端，而它对"第一帧
  Draw"毫无贡献 → 把碰 IPC 的客户端入口整体替身（18 个），纯计算的 config/voice 仍让 guest
  真跑。顺带修掉 `GetMemoryPoolState` 的 5 ms 轮询死等，以及 `nn::account` 的 NULL 全局。
* **M3.18** `nn::os::GetSystemTick` 原来接宿主单调时钟 → guest 的超时按宿主时间算，宿主被
  抢占就"更慢"，同一份脚本 60 s 能到 21 帧、被占满时 200 s 只到 8 帧。改为每次调用前进固定
  tick（`BOTW_TICK_STEP`），换回确定性。
* **M3.19（当前卡点）** 走到"解析 `sysTextureSampler0`"这一步时，资源表
  `[mgr+0xD0]`/`[mgr+0xD8]` 是空的；`mgr` 是刚由 `0x41D40D30` 新建的（写入监视证明不是被
  Yaz0 覆盖）。跑到崩溃为止一共建了 **378** 个这样的表，而注册点 `0x41D41038` **一次都没命中**
  ——每个表的入口列表 `[A+0x20]` 都是空的。`fs` 侧 21 次 open 全部成功、没有 MISSING。

细节、地址与下一步实验都在 [`docs/ROADMAP.md`](docs/ROADMAP.md) 的 M3.16–M3.19。

## M2 进展

本轮把 **IPC/CMIF 服务端**做出来了，游戏于是从"启动即 abort"变成**真的在跑主循环**：

* 启动改走真实链路 `nn::init::Start(heap, arg, nninitStartup, nnMain)`，**完整返回**
* NVN 引导：`nvnBootstrapLoader` → `nvnDeviceGetProcAddress` → **512 个入口全部解析**
* 3.22 GiB 主堆（`AllocateMemoryBlock(0xC1000000)`）分配成功
* 4 个 guest 线程（协作式调度：SVC 边界换上下文，含 x0-x28/Q0-Q31/SP/PC/NZPCR/TLS）
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

## 本地验证（不需要 Mac）

CI 会做两件事：用 clang 编译 `core/`，再用 XcodeGen + xcodebuild 在 macOS runner 上
分别编译模拟器版和未签名真机版。推送即可，无需本地 Xcode。

```bash
git init && git add . && git commit -m "M0: ios shell + metal renderer"
git remote add origin <your-repo>
git push -u origin main
```

## 你需要准备

* 已购买的《旷野之息》1.6.0（含本体 + 更新）
* `prod.keys`（仅本地使用，**永远不要**提交）
* 一台 iPhone（建议 6 GB RAM 及以上），用于真机验证

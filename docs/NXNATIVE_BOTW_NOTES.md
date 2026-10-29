# nxnative 的 BOTW 移植经验（对标清单）

对标对象：<https://github.com/seedds/nxnative/tree/main/dm/games/01007EF00011E000>
（源码已下载到私有区 `C:\botw-work\nxsrc\nxnative-main`，只读参考，不入库）。

**一句话结论**：路线和我们完全一致（guest 代码直接跑 + 把 NVN 重建到 Metal，离线译着色器），
但他们已经走到"iPhone 上 60 fps 能玩"，我们还在 M3。真正值得抄的不是某个地址，而是
**语义纪律**：内核对象怎么记账、未实现入口怎么暴露、补丁和文档怎么不漂移。
其中一条（mutex 的 `WAITERS_BIT`）本轮已经在我们的内核里落地并验证。

## 0. 我们和他们不是一回事

| 项 | nxnative | 我们 |
|---|---|---|
| 目标版本 | 1.9.0（v1114112） | 1.6.0（v786432） |
| 执行方式 | ARM64 **原生执行** + AOT 改写 guest 指令 | Unicorn **解释执行** |
| 着色器 | 19,592 个，全部离线译成 `<key>_b.metallib` | 5,099 个程序 / 20.07 MiB 唯一字节码，未译 |
| 出画面 | 是（Mac + iPhone） | 否（`draws=0`） |
| 内存 | 4 GB 分区，首次进神庙约 2.8 GB | 主堆 `AllocateMemoryBlock(0xC1000000)` |
| 60 fps | 已做到（VFR interval 2→1） | 未开始 |
| CI | 本机 Xcode | 只有 GitHub Actions（无 Apple 芯片 Mac） |

**所有地址、偏移、结构体布局都不能照抄**：SDK 20 与 1.6.0 的 SDK 布局、sead 代码位置、
资源数量都不同。能抄的是方法、判据和坑。

## 1. 可直接借用（与方法/工具相关，不依赖版本）

### 1.1 NVN→Metal 逐条映射表

`dm/gfx/nvn_metal_mapping.md` 把 NVN 的每个对象和命令映射到 Metal 对象，可以直接当我们
M4 的设计对照表。要点：

* **GPU 地址 = CPU 地址**。内存池用游戏自己分配的内存建 `newBufferWithBytesNoCopy`；
  苹果页 16 KiB，pool 不对齐 → 区间向下/向上取整，**允许相邻 pool 的 buffer 重叠**。
* **先 record、后 replay**。`nvnCommandBuffer*` 只往游戏给的命令内存写记录，submit 时才
  翻译成 Metal 命令。我们的 `m3_nvn` 已经是这个形状，继续保持。
* 状态对象（Blend/DepthStencil/VertexAttrib…）**按值拷贝**，宿主侧就地打包成 bitfield；
  render pipeline 与 depth-stencil state 按 state hash 缓存。
* `nvnSync` = 最近一次提交的 fence 值（`MTLSharedEvent` 的 completed value）；
  从未 fence 过的 sync 直接返回 timeout。
* 边框颜色 Metal 只有 3 个预设 → 取最接近的。

### 1.2 脏页跟踪代替"每帧全传"

纹理是 block-linear，上传时 deswizzle；游戏改 pool 内存后靠**写保护脏页**重新上传
（`nvn_dirty.c`）。这是 M4 里最省事也最容易做错的一块：不做就每帧全量上传，
做了就要处理"page 里只有一部分变了"。

另外两个同族的坑，他们已经踩过并写进 README：

* 上传用的 staging block **要复用**。他们原来每次小纹理重传都新分配一个 32 MB 块，
  跑到约 3 GB/s。
* `mtl_addr` / `mtl_gpu_addr` 原来是线性扫 1,760 个 pool；改成"last hit + 二分"后
  submit 时间 21 ms → 7 ms。

### 1.3 管线 key 必须归一化

一条录制里有 4,829 个 pipeline key，其实只有 1,982 条不同管线、1,818 个 Metal base：
**2,845 个 key 的唯一差别是"混合被关闭的目标的 blend factor"**。
修法是 `pso_key_norm`：混合关闭时清掉 blend factor；普通 pass 里没有格式的目标，
连 blend-enable 位和 write-mask 一起清。合并过的 pass 要保留逻辑状态（用 `lmap` 把
物理 attachment 映回逻辑 target），否则会把"合并后暂时没有物理格式"误判成"目标没用到"。

我们 M4 一旦上 PSO 缓存，这条几乎是必踩的：NVN 的状态是**粘滞**的，跨 draw 保留，
包括当前 pass 根本不存在的颜色目标。

### 1.4 未实现入口一律"打红字"

他们对 523 个 NVN 名字逐个生成 thunk，实现了的走实现，没实现的走
`[RED] NVN unimplemented function called: <name>`，绝不静默 no-op。

我们的 M3.21 已经吃过一次同样的亏（`draws=0` 是个假指标）。这条纪律应当上升到
所有 HLE：**宁可红字刷屏，也不要静默成功**。想"先让它过去"的每一个 no-op，
最后都会变成一次找不到源头的状态错乱。

### 1.5 补丁注册表 + 文档自动生成

* `dm/tools/patch_nso.py` + 游戏目录下的 `patches.py`：补丁按 **NSO build ID** 注册，
  每条是 `(偏移, 原 4 字节, 新 4 字节, 说明)`；打完校验原字节，不匹配就报错。
  我们从一开始就该这么干——现在散在 `_doc_*.py` 里的"某地址某值"没有这个保护。
* `nso_asm.py` 提供 `ADRP/B/LDRX` 编码器，"在函数入口插一段 stub 再跳回"是
  脚本生成的，不是手搓机器码。
* `docs_patches.py` / `docs_symptoms.py` / `impact_map.tsv`：**文档由脚本生成**，
  避免文档和补丁表漂移。我们的 `ROADMAP.md` 目前是手写的，已经开始出现
  "上一轮的结论被下一轮推翻"的痕迹（M3.23 §8 就是）。
* `check_thread_cuts.py`：检查"被改写的自旋循环"名单是否和代码一致。

### 1.6 `DT_RELR`

SDK 20 把相对重定位打包成 `DT_RELR`，`main` 里有 **223,994 条**；不处理就
"`.data`/GOT 里每个指针都是模块偏移，到处崩"。

**这条对我们是个待查项**：1.6.0 的 `main` 是否也有 `DT_RELR`、我们的 loader 是否正确
应用了它，还没验证过。纯静态检查，不影响任何观测，应该尽早做。

### 1.7 内置探针与崩溃报告

`probe.c`（32 KB）+ `crash.c`（16 KB）+ `scripts/bug_report.py`：崩溃时自动收集
寄存器、栈、模块偏移、最近的 API 调用。我们的 `dump_fault` / `report_break` 是同一思路，
可以补上"自动转成一份可贴进 issue 的报告"。

## 2. 要按 1.6.0 重新定位才能用

### 2.1 60 fps：`ksys::VFR` 与 sead 的间隔控制器

他们的做法（1.9.0 地址）：

* BOTW 的间隔控制器在 `sead::GameFrameworkNx` 主循环（`main+0x112660c`）：
  候选 interval 被 clamp 到 [2,3]，连续 3 帧超时升档、每 6 帧降一档，
  结果经 `setWaitVsyncInterval`（vtable +0x130）传给 `nvnWindowSetPresentInterval`。
* `ksys::VFR` 的 base interval 固定 2，`VFR::updateInterval` 每帧取框架 interval，
  frame ratio = interval/2 → **把下界 2 改成 1 就是 60 fps**（5 处 patch）。
* 还要强制走 GPU-time 路径（2 处 patch），否则控制器只看 CPU span：
  GPU 受限时仍停在 interval 1，游戏整体变慢（iPhone 16 Pro Max 上 25 ms 一帧、2/3 速度）。
* 附带：按住按钮的重复计数按"帧"算（`sead::ControllerBase::updateDerivativeParams_`），
  60 fps 下菜单重复速度会翻倍，要把常量 1.0 换成 VFR 的 ratio。

我们 1.6.0 大概率是同一套 sead/VFR 逻辑，但地址全变。这是"能不能玩"之后的第一优先级，
不是能不能出画面的前提。

### 2.2 渲染分辨率注入点

1.9.0 的动态分辨率把 scale 存在 s8（0.8–1.0，步进 0.05，`+0x1218`），
在 `main+0x1266880` 处应用到每个 view 的 `+0x28a0/+0x28a4`，
再由 size 函数 `main+0x106340c` 定每帧的 scene buffer。

**关键教训**：不要在 size 函数里乘一个系数（TOTK 的做法），因为
shader 里的 `uking_dynamic_resolution_ratio` 和其它读者（LOD 判断）会停在旧 scale。
要改就改那个 s8，让所有读者一起变。

同时记一笔负面结果：`DM_RENDER_SCALE=0.5` 在 Mac 上**不改变 GPU 时间**
（一帧约 650 个 pass，多数很小）。它的意义是给 iPhone 省带宽（每个 pass 都要
load/store attachment），不是省算力。

### 2.3 内存分区

1.9.0 的 NPDM **没有声明 system resource**（META +0x14 = 0），游戏自己 `SetHeapSize`。
他们的 4 GB 分区：`mem_reserve 0x100000000`、`mem_alias 0x10000000`、
`mem_stack 0x10000000`、`app_pool 0xCD500000`。

对照我们：主堆 `AllocateMemoryBlock(0xC1000000)` + 私有 alias window。
值得复核一次"窗口够不够 / 会不会和 pool 重叠"，但不必照抄数值。

### 2.4 SDK 服务清单

他们为 BOTW 补的服务（可直接当 checklist 对照我们的 126 条 HLE）：
`applet::GetAppletResourceUserId`、`pl::RequestSharedFontLoad`、`pl::IsVrMode` /
`BeginVrMode` / `EndVrMode`、`pctl::CheckStereoVisionPermission`（答"不允许"）、
`oe::TryPopLaunchParameter`、`time::StandardSteadyClock::GetCurrentTimePoint`、
`hid::*SevenSixAxisSensor*`（零态）、`nfp::{AttachActivateEvent, AttachDeactivateEvent,
Mount(2 参), Unmount, GetCommonInfo, GetRegisterInfo, GetState, Finalize}`、
`ro::ProtectRelro`、`ScalableFontEngineHelper::Decode`（共享字体按 BFTTF 供给）、
focus handling mode 2。

注意他们明确写了：`nfp::AttachActivateEvent/AttachDeactivateEvent` **在 SDK 里会崩**
（`sdk+0x3f2a78`），因为参数个数不对——和我们 M3.20 的 mangled-name 坑同类。

## 3. 暂不适用（别抄）

| 项 | 为什么 |
|---|---|
| MTL4 编译器 / `pso_archive` 预热（910 MB、187 s） | 我们还没有可用的管线缓存；等 M4 出图再说 |
| MetalFX 上采样 | 同上；而且它救不回游戏自己做丢的细节 |
| `present_snap`、TBDR 预算分析、pass 合并 | 需要先有 pass |
| iPhone 17 Pro Max 专属调参、GPU 时间归因 | 我们连设备侧都还没跑起来 |
| 1.9.0 的一切地址/偏移/结构体布局 | 版本不同，必须按 1.6.0 重新定位 |

## 4. 他们踩过、我们极可能同样会踩

1. **Metal 在 guest 小栈上崩**（`SIGBUS in AGCDeserializedReply`）：结束 render pass 时
   驱动在渲染线程的小栈上编译 fast-clear blit。修法：除录制调用外，所有 NVN 实现都在
   512 KB 的宿主栈上跑（`nvn_big_entry`）。**M4 一开始就要这么设计**，不要等崩。
2. **SDK 布局随版本变**：`AudioRenderSystem` 的 getter 被内联、`GenerateCommand` 换了签名，
   宿主接管点必须按 SDK 版本重核。
3. **vtable 绕过**：他们接管了 `AudioRendererImpl` 的 `Impl` 函数，但游戏走的是 vtable，
   改的函数从没被调用 → 两个线程写同一命令缓冲。**接管必须连 PLT/vtable 目标一起接管**。
4. **重写器 bug**：`main` 里 1–6 张半字跳转表，`half_count` 只按最大项填充会改坏。
5. `nvnPrivate0`（sdk bss 的标志位）必须保持 0。
6. storage buffer 绑 size 0 被当成"写整个 buffer" → 误判 residency。
7. GPU 回读没有 autorelease pool → 临时对象被游戏线程的外层 pool 滞留，
   iPhone 上 0.2 MB/帧涨到被杀（他们的 issue #9）。
8. `finalized` 的 transient render target 又被 replay 解析一次 → 序列化表每帧漏约 10 项
   （25,000 帧后 276,000 个）。
9. compute dispatch 夹在 pass 的 draw 之间 → 要提前跑（`cs_hoist_try`）。
10. 同一 texture level 的其它 layer 的 pass 可以合并成一次 layered pass
    （cube map 过滤：284 → 173 pass）。
11. depth `EQUAL` 测试在长 pass 内不可逐位复现 → replay 校验要允许每像素 ≤46/255 的噪声。
12. **静音开关（Ring/Silent）会让 iOS 上没声音。**
13. 同一张纹理被挂到两个颜色槽（c1=c2，写掩码 0）：Apple TBDR 上后一个会覆盖前一个，
    按 32×32 tile 出"散落的方块"→ 重复槽位不要 attach。

## 5. 本轮落地的一条：mutex 的 `WAITERS_BIT`

### 5.1 从他们的代码里看到的

`dm/host/hle_thread.c` 的 `lock_handoff` / `lock_acquire_for` 把 Horizon 的互斥字
当成 `owner | (1<<31 waiters)` 来维护：加锁失败要**置 waiters 位**再阻塞；
解锁时按优先级/注册顺序挑**一个**等待者，直接把锁交给它（返回时已持有）。

我们的 `m1_kern.py` 里：`svcArbitrateLock(0x1A)` **从不置这个位**，
`svcArbitrateUnlock(0x1B)` 只从影子表里取人。后果是致命的：

* SDK 的 `nn::os::Mutex::Unlock()` 在**用户态**读这个位。位没置 → 它只写 0、
  根本不发 `svc 0x1B` → 我们的影子等待队列永远没人来取 → 死锁。

### 5.2 改法与效果

```python
# 0x1A：置位后再阻塞
self.lockwait.setdefault(addr, []).append((self.cur, me))
self.w32(addr, v | 0x80000000)
# 0x1B：交给队列里的第一个，后面还有人就把位留着
self.w32(addr, me | (0x80000000 if rest else 0))
```

| 观测 | 改前 | 改后 |
|---|---|---|
| `m2_verify` 45 s | 20 帧 | **11,374 帧（253 fps）** |
| `m3_verify` 30 s | 通过但几乎不动 | **7,089 帧（236 fps）** |
| 裸跑 `LIM=320` 帧数 | 199 | **115,686** |
| 裸跑 `API` 调用 | 1,087 | **925,488** |
| 裸跑 `committed` | 1,151 MiB | **335 MiB** |

最后两行要一起读：**帧泵活了，但资源加载退回去了**（`fs` 只读了 1 次，
`Title.pack` 没读过）。所以这不是"修好了"，而是**换了一堵墙**。

### 5.3 新的墙（已定位）

`t=45 s` 的全线程栈：

* 主线程 `0x101`：`run`，持有消息队列 mutex `0x1080BE140`（字面值 `0x40000108`）。
* `0x10B`、`0x10C`：`TryPeekMessageQueue` → `svcArbitrateLock` 上阻塞，
  `owner=0x108`（= 0x101），`nwait=0`。
* 其余 10 个线程：`ReceiveMessageQueue` 的 `WaitProcessWideKeyAtomic`。

也就是：**主线程拿着队列锁在忙等，两个 worker 在等这把锁**。
这和 M3.23 的"忙等饿死 worker"是同一族，只是这次锁在忙等区间里没释放。

## 6. 两个被数据否定的假设（省下后面的时间）

1. **"`*key` 没记账导致丢唤醒"——不是。** 给 wait/signal 的 cvlog 加上 key 字面值后，
   2035 次 signal / 677 次 wait 的 key **全是 0**：1.6.0 的 `Signal` 并不读这个字
   （否则一次都不会发 SVC）。我们确实少了内核那套"有等待者置 1、清空归 0"的记账，
   但它不是这堵墙的原因。补上之后帧数没有变化（3,815 vs 4,363，同一量级）。
2. **"一次唤醒多个却给每个都写 mutex"——方向对，但不能直接照做。** 按内核语义改成
   "只有第一个拿到锁、其余退回 lockwait"后，游戏 **3 帧后从 `nn::init::Start` 正常返回**
   （= 主动退出）。说明还有别的假设不成立（很可能是线程退出/持锁退出的路径）。
   这一条先保持现状，等 §5.3 的墙拆掉再看。

## 7. 下一步（按优先级）

1. 查 1.6.0 `main` 是否有 `DT_RELR`（纯静态，零风险）。
2. 拆 §5.3 的墙：主线程持 `0x1080BE140` 忙等。先找它忙等的位置
   （`0x41062388` 那一族？）以及为什么不解锁。
3. 把 `dbg_cv` 的 cvlog 扩展成 `(cond, mutex, tag, key)` 完整配对，长期保留。
4. 只有以上都清了，才考虑 §5.2 的"严格单人交接"和 60 fps 路线。

# 路线图

## 为什么不是“模拟器”

通用 Switch 模拟器需要 JIT + 完整内核 + GPU 翻译，iOS 上既不允许 JIT，也扛不住开销。
D2R 的做法是：把游戏二进制里对 SDK / 图形 API 的调用**在符号边界整层替换**成宿主实现，
只保留游戏自己的逻辑代码在本机执行。我们沿用这条路。

## 里程碑拆解

### M1 装载（已完成）
* NSO 解压、段映射、重定位
* 符号表重建（`inventory/symbols.json`，注意两套地址表）
* 从 `nnMain` 起跑，指令级 trace

### M2 启动链 HLE（进行中）
* `nn::oe` 性能/运行模式：置空
* `nn::fs`：RomFS 挂到宿主目录（挂载名 `content`）
* `nn::vi`：Display / Layer / NativeWindow 占位
* `nv::InitializeGraphics` + `nvnBootstrapLoader` + 512 个 NVN 入口
* `nn::os`：内存块、线程创建、地址空间分配器、别名栈
* **出口条件**：跑到 NVN `DeviceBuilder → DeviceInitialize → WindowBuilder → Window → Queue`
  并稳定采集 N 帧的 NVN 调用流

### M3 线程与设备
* 协作式调度器（CreateThread/StartThread/WaitSynchronization/futex）
* NVN 对象布局照 `nvn_obj.h` 落地
* 出口条件：创建出 swapchain 并提交第一帧（哪怕是空帧）

### M3.5 真实启动流程（本轮突破，进行中）

上一轮为止游戏只是"每帧提交一个空 pass"，`fs::OpenFile` 调用数为 0。本轮在
协作式调度器里修掉一个**线程饿死**的 bug（`_pick_next()` 把已被标记 blocked 的
当前线程当成轮转起点，永远回落到 `live[0]`），主线程之外的线程从此才真正被调度。

修完之后启动链第一次跑通到：

```
nnMain → application case0 → app init
  → account::Initialize / OpenPreselectedUser（HLE 直通）
  → 全局 Application 指针 0x42CCFB50 被赋值
  → 任务线程开始加载 RomFS：
       RegionLangMask / ResourceSizeTable / Font_JP.sbfarc
       Bootup*.pack / Title*.pack / Version.txt
  → NVN 建立 4 个 memory pool（含 2×120 MiB）
```

（这一段的卡点已在 M3.6 解决，见下节。）

### M3.6 资源初始化打通（本轮）

上一轮停在 `main+0x12AFF34`：拿空指针当基址写"纹理格式掩码表"。
本轮把它彻底查清并修掉，启动链又往前走了一大截。

**根因链（全部实测，不是推测）**

```
nvnTextureBuilderSetDefaults/SetTarget/SetFormat/SetLevels
nvnTextureBuilderSetWidth / SetHeight      <- 我们没有实现，静默 no-op
nvnTextureBuilderGetStorageSize            -> 0
main+0x12AFE5C: cbz w25 -> 跳过分配
main+0x12AFE90: [x23+0x18] == NULL
main+0x12AFF34: str w12,[x9,w10,uxtw #2]   x9 = 0 + 偏移 -> 写地址 0
```

配套发现：`nvnMemoryPoolInitialize` 的 builder 可以是 `storage==0`
（真机上表示"驱动分配"），宿主必须自己补一块 guest 存储，否则
`nvnMemoryPoolGetStorage` 返回 0，游戏会把 120 MiB 的 pool 当成地址 0。

**本轮修掉的 3 个宿主层 bug**

1. NVN 空实现静默失效：新增 `NVN_MISS` 计数（谁被调用过、几次），
   并补 `nvnTextureBuilderSetWidth/SetHeight/SetDepth/SetSize1D/SetSize3D/SetStride`
   与对应 getter、`nvnMemoryPoolGetFlags/GetBufferAddress/Map/MapVirtual`。
2. pool 无 storage 时由宿主分配（2 MiB 对齐），`GetStorage` 返回真实地址。
3. 动态 stub 区被当数据写：实测 `nvnBufferMap` 把 pool 地址写到 `0xA0008A0`
   （正是某个 stub 桩的位置），下一次函数指针调用跳进数据 →
   `UC_ERR_EXCEPTION pc=stub+4`。现在 `wr64/wr32/wmem` 统一拦截并打印肇事 HLE；
   `handle_svc` 对未登记 stub 槽单独记账，不再掉进 `udf`。

**现在的进度（长跑实测）**

```
nnMain -> app init -> 任务线程解包 RomFS -> NVN 建 30+ 个 memory pool
  （含 0x211FE000 = 553 MiB 一块）-> 走到主循环 scene 状态机
```

新卡点：`main+0x1D3A90C` 读 `[x19+0x20]` 得到 `0x38`（应该是对象指针），
`main+0x1D3A910` 解引用 `x21+0x38` → 读地址 0x70 崩溃。
调用链 `0x4124EE48(呈现) -> 0x41251718(主循环) -> 0x40E3AA00(scene case1)
-> 0x41835740 -> 0x41D3A8E0`，即标题场景的第一帧初始化。

门禁：`m2_verify` / `m3_verify` 均为 PASS（17 帧、15 线程、`ipc missing={}`）。
注意 `present=0 / draw=0` 仍未变——现在还在"能跑通初始化"阶段，不是"能出画面"。

### M3.7 资源重定位异常（进行中）

卡点从 `main+0x1D3A90C` 继续下钻，先把调用链上的 thunk 全部翻译成符号：

```
0x421298E0 -> nn::util::BinaryFileHeader::IsRelocated() const
0x421298F0 -> nn::util::BinaryFileHeader::SetRelocated(bool)
0x42129900 -> nn::util::RelocationTable::Relocate()
0x421286A0 -> nn::util::BinaryFileHeader::GetRelocationTable()
```

被测对象 `0x19B739000` 是**货真价实的 FSHA 资源**（头 16 字节
`46 53 48 41 20 20 20 20 01 00 03 00 ff fe 0c 00`），53 MB 数据已全部写入
（最后一个非零字节在 `obj+0x32B7FF4`），`IsRelocated()` 返回 false，
所以 `Relocate()` 确实被调用了——问题是**它拿到的表是全零**。

三个数对不上，这是本轮最关键的发现：

| 来源 | 值 |
|---|---|
| 头部 `+0x18`（`GetRelocationTable()` 用的偏移） | `0x032B7000` |
| 实际找到 `_RLT` 魔数的位置 | `obj + 0x317D398` |
| 表自己 `+0x04` 记录的自身偏移 | `0x248B398` |

而 `0x248B398 + 0xCF0000 == 0x317D398`，其中 `0xCF0000` 正好是头部
`+0x10`（`0xCEEE1A`）按 64 KiB 向上对齐的值。也就是说：**这份资源的数据在
运行期被整体搬动过约 13.5 MB**，表跟着搬了，但表内的自述偏移和头部偏移都没有跟着改。

排除项（都实测过）：
* 不是文件没读全：`fs::ReadFile` 的 4 次大读（Bootup/Graphics/Title/TitleBG）
  `got == size`，只是请求比文件多 2~6 字节（对齐补齐）。
* 不是 RomFS 缺文件：base 39768 / update 41052，只有 4 个文件只在 base 里；
  已改成 **base 打底 + update 覆盖** 的分层挂载。
* 不是 4 个 boot pack 的明文成员：在 `Bootup/Bootup_Graphics/Title/TitleBG` 里
  搜 FSHA 魔数 0 命中 → 该资源来自压缩成员或别的 pack。

下一步：盯住"谁把资源搬到 `obj+0xCF0000` 之后"——在 `obj` 与 `obj+0xCF0000`
两段分别下写断点，抓解压/搬运代码；顺带确认 Yaz0 解压是否用了我们 HLE 掉的
`memcpy/memset`。

配套修复（都属于"宿主替身"层，不含任何游戏数据）：
* `nn::fs::ReadFile` 补 `ReadOption` 重载，否则绕进真 SDK 把句柄当对象指针
* HLE 写 guest 缓冲区前按需提交 lazy 页（BOTW 的 `.pack` 单次读上百 MB）
* lazy 映射窗口改为互不重叠（堆窗口原来把线程栈窗口整个包住）
* `nn::oe` 查询类 API 直接给值；`nn::account` 整体直通（只影响存档/UI）
* `nn::settings` 的 `MakeLanguageCode` / `GetAvailableLanguageCodes2` 等按命令号实现

### M3.8 加载同步打通（LightEvent）

M3.7 的“重定位表全是零”不是文件问题，也不是重定位算法问题，而是**时序问题**：

* 磁盘上的 `uking_mat.sbfsha` 尾部 `+0x32B7000` 就是一张合法的 `_RLT`（2 项）；
  小资源（`+0x18=0x45000`）在同一套代码里本来就能正常重定位。
* 崩溃时 `obj+0x32B7000` 的 16 字节**全为零** —— 53 MB 的解压只差最后约 0xE8631 字节。
* 解压是异步分块任务：多个 worker（0x101..0x10B）从队列取块执行，主线程 0x100 通过
  `nn::os::LightEvent` 等“加载完成”。
* 宿主里 **SVC 0x34/0x35（WaitForAddress / SignalToAddress）没有实现**，落到默认分支
  直接返回 0 —— `WaitLightEvent` 于是立即“成功”，主线程在数据还没写完时就去解析 FSHA。

补上真正的阻塞语义后，同一次运行里：

```
SCENE  obj=0x19B739000  tblHead=5f 52 4c 54 00 70 2b 03 02 00 00 00   <- _RLT 在
AT90C  obj=0x19B739000  p20=0x19B739038                             <- +0x20 已重定位
```

原来的 `main+0x1D3A910` 崩溃消失，启动链继续往下建立 NVN pool，直到指令预算用尽。

实测语义（按 SDK 反汇编核对）：

* `svcWaitForAddress(addr, tag, ..., timeout)`：`*addr != tag` 时阻塞；等于 tag 返回 0；
  值已变化时返回 `0xFA01` 让调用方重试；超时返回 `0xEA01`。
* `svcSignalToAddress(addr, tag, count)`：`*addr = tag` 并唤醒至多 count 个等待者；
  **x0 是 Result（0=成功），唤醒数在 x1**（返回唤醒数会被 SDK 当错误并 abort）。

门禁：`m2_verify` PASS（52.1s / 15 帧 / `ipc missing={}`）、`m3_verify` PASS
（`draw=0 overflow=0`）。`draw=0` 仍未变：现在只是“资源能完整加载并解析”。

### M3.9 当前卡点：条件变量死锁（待办）

900 秒 / 3e9 指令长跑：不再崩，`DRAWS=0`，`OPSTATS={BEGIN:21, API:168, END:21}`。
结束时 11 个 worker 的 PC 都停在 `sdk+0x37EB40`（`svc #0x1C` 的返回地址，
即 `WaitProcessWideKeyAtomic`），只有 0x10B 可运行 —— 典型的**丢唤醒**。

下一步：给 `pwkey/pwsig` 加按 condvar 地址的计数与等待者快照，找出“谁该 signal、
为什么落空”；同时复核 `svcSignalProcessWideKey` 的 count 语义（`<=0` / 全唤醒）。

**本轮复查结果**（已做的两处修正）：

* `pwsig`（“落空信号记账”）是错的：真实内核在没有等待者时**丢弃** signal。
  实测它堆出了 67 / 28 / 21 个幽灵令牌，制造伪唤醒。已删除，改为内核语义。
* `svcWaitProcessWideKeyAtomic` 的 `timeout` 之前被忽略；现已支持
  `timeout==0` → 立即 `0xEA01`，有限超时在全阻塞时按 `0xEA01` 放行。
* `svcSignalProcessWideKey` 的 count：`-1` 全唤醒、`0` 不唤醒、其余按个数。

修正后门禁仍 PASS，但 21 帧后依然停住。现场：

```
MAIN state=blocked pc=0x2037ec38 lr=0x20324ce8     <- 卡在 WaitLightEvent
worker 0x101..0x10E: pc=0x2037eb40 (WaitProcessWideKeyAtomic)
WAITERS: 11 个 condvar 各 1 个等待者，timeout 全是 -1
```

主线程等的 LightEvent `0x111bc37a0` 与 worker 的 condvar `0x111bc36a4/6a8`
在同一块对象里，SVC 日志尾部是二者高频 ping-pong（每轮都成功 wake，但
`BEGIN/END` 帧数不涨）。下一步应从 `0x4126CF74`（任务队列入口）和
`0x4192xxxx`（主线程 LEvent 等待函数）两头对，确认是“队列永远有假任务”
还是“主线程在等一个不会再来的完成事件”。

### M3.10 `mrs CNTPCT_EL0` 恒为 0：guest 里所有"按时间等待"都永不超时

追这一轮卡点时先做的是**量化**：每 10 秒采一次 `BEGIN/API/END` 和块级热页直方图。
结果是第 21 帧之后 **99% 的块采样都落在 `main+0x191F000` 一个页里**，其它线程寸步难行。

那个页里的循环（`main+0x1F0F0` 起）：

```
TryPeekMessageQueue(&msg, &queue)   // 没消息就退
now = GetSystemTick()
if (now - t0 > 1s)  -> 处理/清理分支
else                -> SleepThread(ConvertToTimeSpan(timeout/N)); 再看一次队列
```

`x25`（超时阈值）在运行期读出来是 `0x124F800` = 19,200,000，正好是 1 秒。
也就是说这是一个**带 1 秒超时的空闲轮询**。它之所以不退出，是因为时间不走：

* `nn::os::GetSystemTick`（sdk+0x327FD0）就是两条指令：`mrs x0, cntpct_el0; ret`
* `nn::os::GetSystemTickFrequency`（sdk+0x327FE0）返回 `0x124F800`
* Unicorn 2.1.4 不实现 / 不推进 `CNTPCT_EL0`，读出来恒为 0 → `now - t0` 永远是 0

修复：用 HLE 接管这两个函数入口，把 tick 映射到宿主 `time.monotonic()`（19.2 MHz，
起点 `0x100000000`，避开"0 表示未初始化"的 SDK 判断）。

**注意**：`svc #0x1E (GetSystemTick)` 的调用计数一直是 **0** —— SDK 是直接读系统寄存器的，
不经过 SVC，所以只有改**函数入口**（HLE 打桩）才有效，改 SVC 表无效。

### M3.11 `svcSleepThread` 之前是"立即让出"

M3.12 的同一份采样显示：45 秒里 `svc #0x0B` 只有 13 次，可那个轮询却吃掉了 99% 的 CPU。
原因是最初的 `SleepThread` 实现直接 `_yield()`，等于"睡 0 纳秒"，于是
`while (!timeout) { SleepThread(1ms); }` 退化成 100% 空转。

现在按真实语义处理：`svcSleepThread(ns)` 把线程挂起到宿主墙钟上的
`monotonic() + min(ns/1e9, 0.25s)`，`run()` 的分块边界统一唤醒到点的线程；
如果全体都在睡，宿主 `sleep` ≤ 5 ms 再继续。

### M3.12 卡点复测（240 s）：停在 21 帧，不是丢消息

两处修复后门禁仍是 `m2_verify PASS`（48.5 s / 15 帧 / `ipc missing={}`）、
`m3_verify PASS`（`draw=0 overflow=0`）。240 秒长跑：

```
[t= 15s] BEGIN=9   [t=30s]  BEGIN=16  [t=60s] BEGIN=21
[t=240s] BEGIN=21  API=168  END=21     块采样 main+0x191F000 = 2,081,534
```

在 `nn::os::SendMessageQueue / TrySendMessageQueue / ReceiveMessageQueue /
TryReceiveMessageQueue` 四个入口打点（每次记 tid / queue / msg），得到的事实：

* 主线程**每帧**往队列 `0x111BC3668` 发 1 条消息（21 帧 = 21 条），消费者是 worker `0x109`
* 停住前的最后两条：`tid=0x100 Send(q=0x111BC3668, msg=1)` → `tid=0x109 Recv(...)` →
  `tid=0x100 Send(q=0x111BC3668, msg=2)`，之后 `0x109` 再没取过
* 结束时所有队列都是 `count=0`（全部取空）→ **不是"队列里堆了消息没人取"**

`MessageQueueType` 布局（反汇编 sdk+0x25DC0 `SendMessageQueue` / sdk+0x26340
`ReceiveMessageQueue` 得到，对以后接其它 SDK 版本也有用）：

| 偏移 | 字段 |
|---|---|
| +0x00 / +0x08 / +0x10 | 三个链表头（等待者节点） |
| +0x20 | `u64 *buffer` |
| +0x28 | `u32 capacity` |
| +0x2c | `u32 count` |
| +0x30 | `u32 read_pos` |
| +0x38 | `u32 mutex`（InternalCriticalSection） |
| +0x3c | `u32 condvar`（InternalConditionVariable） |
| +0x40 | light event |

配套的 SDK 侧语义（同样是这一轮反汇编核对的）：

* `InternalConditionVariableImpl::Wait`（sdk+0x32F1A0）：先断言
  `(*mutex & 0xBFFFFFFF) == 当前线程 tid`，再
  `svcWaitProcessWideKeyAtomic(mutex, cv, tag=tid, timeout=-1)`
* `InternalCriticalSectionImpl::Enter`（sdk+0x32F7A0）：快路径 CAS `0 -> tid`；
  否则把 mutex 的 bit30（"有竞争者"）置上，再 `svcArbitrateLock(owner, &mutex, me)`
* `LightEvent`（sdk+0x324A80/AC0/C20/C50）：`state` 0=closed、1=有等待者、2=已 signal；
  `Wait` = `svcWaitForAddress(&state, 2, 1, -1)`，`Signal` = CAS `0->2` 或
  `svcSignalToAddress(&state, 2, 1)`

结论：主线程在队列 `0x111BC3668` 上等"任务完成"，worker `0x109` 能收到消息，
但**任务体没有把完成标志写回去**，于是主线程的信号量状态机原地打转
（每轮都 wake，但 `BEGIN/END` 不涨）。下一步要对读两处游戏代码：

1. `main+0x1F0F0`：`TryPeekMessageQueue` 成功后的处理分支（它才是真正"跑任务"的地方）
2. `main+0x126DB10`：任务组的完成计数循环（`[obj+0xc8]` vs 读到的值，`[obj+0xc0]` 判断继续）

顺带记一条性能债：`m2_verify` 里 `hle_calls=1949 / hle_time=26.32s`（整轮 48.5s），
时间几乎都花在把 140 MB 的 `TitleBG.pack` 拷进 guest 内存上，后面可用
`mmap + 一次 memcpy`（或直接让 guest 读宿主映射）替掉逐块 `mem_write`。

### M4 Metal 回放
* NVN 命令流 → MTLCommandBuffer
* 着色器：已有 5,099 个程序 / 20.07 MiB 唯一字节码，需要 NVN 的 GPR/SH 描述 → MSL 翻译
* 纹理格式映射（BC1-7 / ASTC）、采样器、渲染目标
* 出口条件：标题画面出图

### M5 输入与音频
* `nn::hid` → GameController / 触屏虚拟摇杆
* `nn::audio` → AVAudioEngine

### M6 性能
* 主堆 3.22 GiB（`AllocateMemoryBlock(0xC1000000)`）是硬指标
* 目标是 iPhone 6 GB 机型 30 fps 起步

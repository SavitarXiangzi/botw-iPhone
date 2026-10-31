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

> 更正（M3.14）：上面那段「停在 21 帧」的观测全部是在 `UC_HOOK_BLOCK` 采样下做的（约 2 MIPS）。
> 关掉钩子后有 19 MIPS，同一份资产预载 70 秒就跑完并进入标题画面（175 帧 / API 895），
> 所以这里既不是死锁，也不只是「模拟器慢」，而是**采样本身把速度压了 50 倍**。

### M3.13 一次 `fs::ReadFile` 里的 2247 次 `uc.mem_map`（25.9 s）

上一节结尾记的"性能债"是错的：那 26 s **不是**在拷 140 MB 数据。

用 `BOTW_HLE_SLOW=0.3`（`m1_kern.py` 里新加的 HLE 打点）逐次打印慢调用：

| 调用 | 缓冲区 | 大小 | 墙钟 |
|---|---|---|---|
| `fs::ReadFile(option)` | `0x15FD33280` | 31.5 MB (`Bootup.pack`) | 0.956 s |
| `fs::ReadFile(option)` | `0x15E986B00` | 20.6 MB (`Bootup_Graphics.pack`) | 0.275 s |
| `fs::ReadFile(option)` | `0x1621C1F00` | **147.2 MB** (`TitleBG.pack`) | **25.912 s** |

耗时与大小完全不成比例，而独立基准（同一文件）是：`open().read()` 0.09 s +
`uc.mem_write(147 MB)` 0.07 s（2.1 GB/s）。所以瓶颈既不在磁盘也不在 Unicorn 的
批量写，而在**我们自己提交页的那段代码**：

* `commit_range` 是按"匹配窗口的 chunk"步进提交的
* `0x1621C1F00` 落在**线程栈窗口** `[0x100000000,0x180000000)`（`chunk=0x10000`），
  因为 BOTW 的大块资源缓冲区是从 `ThreadManager` 的那个 allocator 里要的
* 于是 147 MB / 64 KiB = **2247 次 `uc.mem_map`**；更早的失败路径还会退化成逐
  4 KiB 重试（3.5 万次），这也是 M2 阶段就开始记的那笔"26 s 性能债"的真身

修复（`m1_kern.py`，备份 `m1_kern.py.bak_map`）：

1. `commit_range` 改成按 64 MiB 步进，不再跟着窗口 chunk 走
2. `_map_safe` 先**减掉已映射区间**，只补真正的空洞
3. `_map_block` 整块 `uc.mem_map`，与已映射页冲突时**折半重试**（最细 4 KiB），
   代价从 O(pages) 降到 O(log)

效果（同一台机器、同样的门禁脚本）：

```
修复前: boot 45 s  blocks=17,749,963  frames=15  hle_time=27.93 s  (hle_calls=1949)
修复后: boot 45 s  blocks=37,553,920  frames=20  hle_time= 0.27 s  (hle_calls=2072)
```

`hle_time` 降了两个数量级，同样墙钟下 guest 多跑 2.1 倍的块。`m2_verify` / `m3_verify`
仍然 PASS（`ipc missing={}`、`draw=0 overflow=0`）。

### M3.14 块钩子要 53 倍代价：所谓"21 帧卡死"主要是采样本身

先把"模拟器到底多快"量出来（同机、同一个 20 M 条指令的循环）：

```
无钩子                       107 MIPS
装 UC_HOOK_BLOCK              2 MIPS   (10,000,001 次 Python 回调)
uc.ctl_set_tcg_buffer_size   113 MIPS
```

`UC_HOOK_BLOCK` 是**每个翻译块**一次 Python 回调，于是热页直方图把整台模拟器按到
~2 MIPS。之前所有"长跑 240 s / 300 s 仍停在 21 帧"的结论，都是在 2 MIPS 下测的：
那段 210 MB 的 pack 预载 + Yaz0 解码并没有死循环，只是被采样拖慢了 50 倍。

改成 `BOTW_BARE=1`（不装任何 Python 钩子，靠 `emu_start(count=...)` 分块，块边界回
Python 看表）后，真实吞吐约 **19 MIPS**，同一台机器的同一份门禁：

```
钩子模式   90 s  → BEGIN=21  API=168  draws=0   （91% 的块在 main+0x191F000）
无钩子     70 s  → BEGIN=175 API=895  draws=0   （已经越过资源预载、进到标题画面）
```

结论：**热页采样必须只用于短诊断跑**，长跑一律无钩子。为了还保留"卡在哪"的能力，
`_on_block` 里新增 `pcx`：在 1/64 采样中对**非 `YAZ0_PAGE`（main+0x191F000）**的块
记录**精确地址**并带上 tid，代价可以忽略，却足够定位卡点循环。

### M3.15 三个 NVN 入口签名写错：往野指针里写 8 字节

权威签名在 d2r 那边的 `dm/gfx/nvn/include/nvn.h`：

| 入口 | 真实签名 | 旧实现 |
|---|---|---|
| `nvnBufferMap` | `void* (const NVNbuffer*)` | 往 `x1` 写 out、`return 1` |
| `nvnMemoryPoolMap` | `void* (const NVNmemoryPool*)` | 往 `x1` 写 out、`return mem` |
| `nvnMemoryPoolMapVirtual` | `NVNboolean (NVNmemoryPool*, int, const NVNmappingRequest*)` | 往 `x1` 写 out、`return mem` |

三个都没有 out 参数，`x1` 是**上一次调用残留的脏值**。两个后果：

* `nvnBufferMap` 的情况被日志抓了个正着：`wmem` 的守卫打出
  `HLE out-param write into stub arena 0xA0008A0 dropped`，而游戏拿到返回值 `1`
  当指针用，下一条就是 `memcpy(dst=1, src=0x19EB81DD8, n=0x1B0)`
  → `UC_ERR_WRITE_UNMAPPED @ sdk+0x4CB9D0 (memcpy)`
* 另外两个更危险：脏 `x1` 恰好是个可写地址时，8 字节会**静默**写进 guest 内存

修完这三处，同一无钩子跑法从 160 帧推进到 **175 帧 / API 895**。

### M3.16 纹素解码越界：`BPP` 表漏了 DEPTH16、RG8/RG16 记错（已修）

上一轮把 `0x1A026D000` 的写入时间线拍下来以后，结论是"一块还在空闲链表里的内存被法线
图解码器整块覆盖"。顺着"缓冲区比游戏以为的小"这条线查 `nvnTextureBuilderGetStorageSize`，
根因在 `tools/m3_nvn.py` 的 `BPP` 表：

| 格式 | 旧值 | 真值 |
|---|---|---|
| RG8 | 4 | 2 |
| RG16 | 4 | 4 |
| DEPTH16 | 缺项（退回 4） | 2 |
| DXT1/3/5、RGTC1/2、BPTC、ASTC | 未列，按 4 B/像素 | 按 4x4 块计 |

返回值偏小 → 游戏按这个值开 NVN pool → 法线图解码器按真实纹素尺寸写满，越界覆盖
`0x1A026D000` 的空闲块头 → 分配器摘链时拿到野指针（`main+0x125B1D4`）。

修法（`m3_nvn.py`，备份 `m3_nvn.py.bak_texsize`）：

* `BPP` 按 `nvn.h` 的 `NVNformat` 枚举重写，补上 `DEPTH16`
* 新增 `BLOCK`：DXT1/3/5、RGTC1/2、BPTC、ASTC（含 SRGB 变体）
* `_surface_size(fmt,w,h,depth)` / `_tex_size(fmt,w,h,levels,layers,depth)`，按 mip/array/depth 累加
* 所有调用点补 `b.get('d',1)`（3D 纹理的 depth）

验证：`fmt=0x1 w=32 h=32 levels=6` 的存储需求从 `0x1000` 变成 `0x16000`（6 级 mip 累加），
分配器那个 FAULT 不再出现。

### M3.17 audren 更新协议不可补：音频/账号的客户端入口整体替身

修掉纹素池后，无钩子跑到 175 帧进入音频初始化，停在 `svcBreak`：

```
[ipc] audren:u cmd=1 args=0000BB80 000000F0 0000001E 00000001 00000060 00000001   ×4
[ipc] audren:u cmd=0 ...（带 transfer memory 0xA0E75000）
[ipc] ?        cmd=7     ← 未知句柄
[ipc] ?        cmd=4
===== svcBreak =====  sdk+0x1801E8 diag::detail::Abort
   Abort(Result*): 0x00000000      ← 纯断言，不是 Result 码
```

`OpenAudioRenderer` 没有返回会话句柄，后面的 `UpdateAudioRenderer` /
`RequestUpdateAudioRenderer` 落在未知句柄上，我们回空 payload，客户端读栈上垃圾 →
`NN_ASSERT` → `Abort(0)`。

补齐 audren 的更新协议（voice / mix / sink / memory-pool 的 in/out 参数块）等于把 yuzu 的
audren 服务端重写一遍，而这一层对"第一帧 NVN Draw"毫无贡献。按 M2 的边界原则：**碰 IPC /
设备的入口整体替身，纯计算的留在 guest 真跑**。

新增 `tools/m2_audio.py`（18 个入口）：

| 类别 | 入口 | 行为 |
|---|---|---|
| 工作区 | `GetAudioRendererWorkBufferSize` / `...ConfigWorkBufferSize` | 4 MiB |
| 生命周期 | `Open`/`Start`/`Stop`/`Close`AudioRenderer | 写假句柄 `0x41444F0000000001`，不开服务 |
| 每帧 | `RequestUpdateAudioRenderer` | 直接 0（先不出声，M5 补 mixer） |
| 内存池 | `GetMemoryPoolState` / `IsMemoryPoolAttached` | **恒 1（Attached）** |
| 设备 | `GetActiveChannelCount` / `ListAudioDeviceName` / `SetAudioDeviceOutputVolume` | 2 声道 / 1 个 `DeviceOut` |

保留原生：`InitializeAudioRendererParameter` / `InitializeAudioRendererConfig` /
`AcquireVoiceSlot` / `SetVoiceXxx` / `AddDeviceSink` —— 它们只改 guest 内存里的 config。

**`GetMemoryPoolState` 必须直接返回 1。** 渲染器整个替身掉以后，真机那条
`RequestAttach → 下一帧服务端把状态翻成 Attached` 永远不会发生，音频线程就卡在
`main+0x1CF9698` 的 5 ms 轮询里（实测 16370 次/10 s，主线程 61% 的采样落在
`nn::os::GetSystemTick`）。直接回答"已挂载"后恢复推进。

修完 audren 之后撞上 `nn::account::ListAllUsers`（`pc=0x2003B710 ldr x8,[x0]`，`x0=0`）：
`nn::account::Initialize` 走的是真实 IPC，我们伪造的空服务让它静默失败、SDK 全局保持
NULL，`ListAllUsers` 于是解引用 0。真机这一步是"选用户"，是系统层的事。新增
`tools/m2_account.py`（4 个入口）造一个固定 UID（`0x0000B07A00000001`）的已登录用户：
`Initialize` / `ListAllUsers`（回 1 个用户）/ `OpenPreselectedUser` / `GetUserId`。

### M3.18 时间源必须虚拟化：宿主时钟让同一份脚本跑出两种结果（已修）

这一轮最值钱的一条观测，是在改任何东西之前先做对照：

| 条件 | 同一份 `_probe_obj.py`，60 s |
|---|---|
| 机器空闲 | 21 帧 / 797 MiB / 1416 块 |
| 机器被别的 python 占满 | 200 s 只到 **8 帧 / 340 MiB** |

根因是 M3.10 那个修法本身：`nn::os::GetSystemTick` 被接到了宿主 `time.monotonic()`。
guest 里所有 `while (now - t0 < T)` 的超时于是**按宿主时间算**——宿主被抢占，guest 就
"更慢"，同一份二进制在不同负载下走不同分支。此前"175 帧 vs 21 帧"的分歧有一部分就是它。

改成**每次读 tick 前进固定量**（`BOTW_TICK_STEP`，默认 4096 tick ≈ 213 µs）：

```python
_TICK_CALLS[0] += 1
return 0x100000000 + _TICK_CALLS[0] * TICK_STEP
```

* 确定性：与宿主负载无关
* 任何忙等最多 STEP 次调用就超时
* 代价：guest 侧"每帧多少毫秒"不再真实。帧节流 / 物理 dt 要等 M4 有真 present 环再重做
  ——Unicorn 在无钩子模式下不给逐条指令计数（`self.instr` 只在带 trace 钩子时累加），
  暂时拿不到"按指令数走"的时钟。

改完复测：无钩子仍然 175 帧、仍然撞同一个资源 FAULT —— 它没有掩盖问题。

### M3.19 当前卡点：资源类型表全是空的（`sysTextureSampler0` 查不到）

> **M3.20 更正**：本节"注册点一次都没命中 / 378 个表"的结论作废一半。
> `0x41D41038` 只属于 `A[0]&2==0` 那条子路径，而本作数据里 `A[0]` 恒为 2；
> `378` 与 `151` 也来自两次不同 `LIM` 的统计。真正的根因见下一节 M3.20。
> 本节其余部分（`mgr` 是新建的、分支看 `A[1]`、入口枚举在 `[A+0x20]==0` 处跳过）仍然成立。

```
[mem] UNMAPPED access=19 addr=0x0 size=8 pc=0x41D41A10 main+0x1D41A10
   pc=0x41D41A10  insn=ldr x21, [x9, x8]      x8=0 x9=0
   x3=0x426C25B0 "sysTextureSampler0"
```

调用链（下面这几层都是精确 PC，不是栈扫描猜的）：

| 地址 | 作用 |
|---|---|
| `0x41D6A89C` | 遍历 `*(ctx+0x20)` 里的 0x40 字节条目：条目 `+0x00` = `B`，`+0x10` = `A` |
| `0x41D6A8CC` | `0x41D40D30([A+0x68], ..., A)` —— 每条目建一个"资源表"`mgr` |
| `0x41D6A924` | `0x41D6AFF0(slot, entry, state)` → `slot+8 = [[entry+0x10]+0x68]` |
| `0x41D6B020` | `LoadResult` 构造：依次解析 `sysEmtMat2Buff` / `sysTextureSampler0/1` / `sysFrameBufferTexture` |
| `0x41D41920` | 按名字查表：`x22=[mgr+0xD8]` 为 0 时走 `0x41D41A00` 兼容路径，`[mgr+0xD0]` 也是 0 → 空指针 |

对 `0x143239880..0x143239B00`（含 `mgr` 上下 0x280 字节）挂写入监视，645 次写里
**639 次是 Yaz0 按字节解压**（这块内存原来是解压缓冲），紧接着 4 次是 `0x41D40D30`
自己在建对象：

```
#643 W pc=0x41D40D58 addr=0x143239920 val=0x143239928   ; mgr+0x08 = mgr+0x10（内联缓冲）
#644 W pc=0x41D40D60 addr=0x143239919 val=0x02          ; mgr+0x01 = A[0] = 2
```

**对象是新建的，不是被覆盖**：建完之后 `mgr+0xD0`/`mgr+0xD8`（6 类 × 6 模式的类型表）
仍然是 0。

`A` 长这样（378 次调用，形状完全一致）：

```
A[0..0x1F] = 02 00 00 00 00 00 00 00 <ptr> 01 00 00 00 00 00 00 00 00 ...
             ^^ A[0]=2（拷到 mgr+1）   ^^ A+8
             ^^ A[1]=0（决定走哪条初始化分支）
```

`0x41D40D30` 里真正的分支看的是 **`A[1]`**（`0x41D40D80 ldrb w8,[x20+1]`，x20 = A）：

* `A[1]==0` → `0x41D40F50`：再看 `A[0]` 的 bit1（`A[0]=2` 命中）→ `0x41D41058`，
  用 `0x41D41E30(A, i)` 逐条枚举条目、拷到栈上，最后在 `0x41D41038`/`0x41D41040`
  调注册函数
* `A[1]==2` → 另一条（读设备上限）
* 其他 → `0x41D41050`：`mov w22,#1` 直接返回成功

实测在 `0x41D41038` 下钩子，**整个跑到崩溃为止一次都没命中**（`reg calls 0`）。也就是说
378 个表全部建出来了，但没有一个走到注册点——入口列表 `[A+0x20]` 是空的。

同一时刻 `fs` 侧没有异常：`BOTW_FSLOG=1` 打出的 21 次 open 全部成功，包括
`System/Resource/ResourceSizeTable.product.srsizetable`、`Actor/ActorInfo.product.sbyml`、
四个 `Pack/*.pack`、`Sound/DummySound.bfsar`，没有一次 MISSING。

下一步：在第一次进 `0x41D40D30` 时 dump `A+0x20`（条目链表头）、`A+8` 指向的对象，以及
`0x41D41E30` 的返回值；弄清"条目列表为空"是因为解析器（`0x41D67xxx` 那一段按 4 字节 tag
遍历的代码）读到的数据里就没有条目，还是这些类型本来应该由别处注册。同时在
`0x41D67F24` 那层把 `x19`/`x20` 指到的缓冲区 dump 出来，确认它到底在解析哪块数据。

在拿到这张表之前，不要用 `0x41D41A00 → 0x41D41DFC`（"当作资源不存在、返回 -1"）那种
诊断补丁往下走：它能过这一步，但资源是空的，后面只会以更远的方式再崩。

### M3.20 `nvnProgramSetShadersExt` 返回 0，等于告诉游戏"着色器没装上"

```
[mem] UNMAPPED access=19 addr=0x0 size=8 pc=0x41D41A10 main+0x1D41A10
   pc=0x41D41A10  insn=ldr x21, [x9, x8]      x8=0 x9=0
   x3=0x426C25B0 "sysTextureSampler0"
```

#### 怎么定位到 NVN 的

崩溃点本身看不到 NVN，得从**写 `mgr+0xD0` 的那条路径**往回找。`0x41D40D30` 的
`A[1]==0` 分支里，收集完 6 个 stage 之后有一句 `blr`：

```
0x41D41058  mov w22, wzr                      ; 记录条数
0x41D41068  mov x0, x20 / mov w1, w21
0x41D41070  bl  #0x41D41E30                   ; e = entry(i) = *(A + 8 + 8i)
0x41D41074  cbz x0, #0x41D4112C               ; 该 stage 为空就跳过（不中断循环）
0x41D41078  ldr x8, [x0]      ; rec[0]  = e[0]      （stage 枚举）
0x41D41084  ldr x8, [x0, #8]  ; rec[8]  = e[8]      （shader 二进制）
0x41D4108C  ldr x8, [x0,#0x30]; rec[0x10] = e[0x30]（大小）
0x41D41138  adrp x8,#0x42c80000 ; ldr x8,[x8,#0x310]  ← GOT 0x42C80310
0x41D41150  blr x8                             ; (**)
0x41D41154  tst w0, #0xff
0x41D41158  b.eq #0x41D41178                   ; 低字节 0 → mov w22,#3 → 直接返回
0x41D41168  ldr x8,[x20,#0x78] / str x8,[x19,#0xd0]   ; 只有成功才写 mgr+0xD0
```

`(*)` 那个 `blr` 的目标不是游戏代码，也不是真实 SDK：

```
0x42C80310 -> 0x42D2F120 -> 0x0A0005C0
0x0A0005C0 = 动态 stub 槽 #0x105C  ->  'nvnapi:nvnProgramSetShadersExt'
```

即 `nvnProgramSetShadersExt(program, 2, NVNshaderData[2])`。这条 `NVNboolean` 返回值的
语义是"着色器装上没有"。`m3_nvn.py` 里当时**一个 program 入口都没有**，于是 `_nvn_getproc`
把它登记成 `_nvn_miss` → 一律 `return 0` → 游戏判定失败 → 提前返回 → 两张表都不建
→ 几十条资源之后在 `0x41D41A10` 空指针崩掉。

实测（`LIM=150`，无钩子）：

```
0x41D41150 命中 150   w22=2（每条都是 2 个 stage）
0x41D41154 命中 150   返回字节直方图 {0: 150}     ← 修复前：全失败
0x41D41168 命中 0                              ← 修复前：mgr+0xD0 从未写入
```

#### 关于 `+0xD8`：它不是这条路建的

`str [x19,#0xd8]` 在本函数里只有 `0x41D41474` 一处，位于 `A[1]==2` 分支建反射表的地方
（`w0=0x190` 分配 → `+0x110` 拷反射 blob → 按 `blob+0x4c` 的条目数逐项解析）。
本作数据里 `A` 的形状**只有 `(A[0],A[1]) = (2,0)` 一种**（150 次调用全同），所以那条分支
从不进入；`0x41D41920` 会走 `0x41D41A00` 的兼容路径，用 `[mgr+0xD0]` 这张
"6 个 stage 指针的数组"（表 `0x4271B848 = {0,8,0x10,0x18,0x20,0x28}`）做名字查找。
换句话说：**`+0xD0` 一填上，兼容路径就能用**，`+0xD8` 不是必要条件。

#### 修复

`m3_nvn.py` 新增 program 层（14 个入口）：`Initialize/Finalize/SetDebugLabel/
SetShadersExt/SetShaders/GetStageCount/GetUniformLocation(+ByName)/
GetUniformBlockIndex(+ByName)/GetTextureIndex/GetSamplerIndex/AddShader/AddSeparateShader`。
`SetShadersExt` 把 0x18 字节一条的 `NVNshaderData` 记进对象镜像并**返回 1**；
`GetUniform*` 一律 `-1`（反射表整体替身）。

修复后同一脚本：

```
0x41D41154 ret w0=0x1     × 150
0x41D41168 mgr+0xD0 <= …  × 150
STATUS budget frames 175 draws 0     （150 s，无 FAULT）
```

帧时间线（20 s 采样）：`21 帧` 之后 frames 不再增长，`chunks` 一路涨、热页 100% 在
`main+0x191F000`（Yaz0 解压）——**游戏进资源加载了**，这正是要的结果。

#### 同轮附带

* `m2_nfp.py`：`nn::nfp`（amiibo）20 个入口整体替身成"已初始化、无设备"，
  接进 `botw_hle.py`（原来 `Initialize` 走真 SDK → `sf::SharedPointer<ISystem>` 保持 NULL
  → `ldr x8,[x0]` 在 `0x203003C4` 崩）。
* `AttachActivateEvent` 的 mangled name 长度前缀写错（`_ZN2nn3nfp21…` 应为 `…19…`），
  这种错**不会报错**，只会让那一条 HLE 静默失效。接线时拿 `inventory/imports.tsv`
  逐条比对，别靠眼睛。

#### 下一步

资源加载期是纯 guest 的 Yaz0 解压，慢是预期的（无钩子约 2 MIPS）。要看的是
**加载完之后能不能走到第一个 `nvnCommandBuffer` 里有 draw 的帧**。门禁仍要求
`m2_verify` / `m3_verify` PASS（已复跑），目标仍是 `DRAWS != 0` 才进 M4。

### M3.21 `draws=0` 之前是个假指标：`nvnDrawArrays` 一族根本没实现

`m3_nvn.py` 里 `OP_DRAW / OP_DRAW_IDX / OP_DISPATCH` 只出现在 `OPNAME` 字典里，
**没有任何入口会 emit 它们**：`nvnDrawArrays` / `nvnDrawElements` /
`...Instanced` / `nvnMultiDraw*` / `nvnDispatchCompute*` 一个都没登记，全落进
`_nvn_miss` 变成静默 no-op。于是 `DRAWS[0]` 是**结构性**的 0——"游戏还没提交几何"
这个结论当时并没有证据支撑。

补上 8 个入口（含实例化/multi/dispatch），如实计数并把参数记进命令流。从现在起
`DRAWS != 0` 才可以作为进 M4 的判据。

### M3.22 一口气过四道门：存档挂载 → 线程别名栈 → nn::time → applet 启动参数

M3.20 之后资源系统不再崩，长跑停在 175 帧。这一节是"顺着 Abort/FAULT 一路往下推"
的四步，每一步都有确定的现场。

#### 1. `nn::fs::MountSaveData`（Abort 0x1A80A）

```
svcBreak pc=0x2037EBA4  Abort(Result*): 0x0001A80A  module=10 description=212
stack: nn::fs::MountSaveData(char const*, nn::account::Uid)
```

module 10 = `fs`。BOTW 在进标题之前要挂存档，我们没这个入口 → 真 SDK 去连
`fsp-srv` 的存档会话 → 失败断言。移植版把 `save:` 映射到私有宿主目录
（`BOTW_SAVE_ROOT`，默认 `tools/save`），补 `CommitSaveData` / `CreateFile` / `DeleteFile`。
存档是**真文件**：可复现，也能直接删掉重来，但不进版本库。

#### 2. 线程别名栈：`MapAliasStack` 的参数全解读错了（pc=0 FAULT）

```
[mem] UNMAPPED addr=0x0 size=4 pc=0x0
   lr=0x2032E294 ThreadManagerImplByHorizon::CreateThread   sp=0x815D9FC0
```

`pc=0` 是新线程启动时**跳到空函数指针**。SDK 的 trampoline（`0x2032E240`）这样取入口：

```
0x2032E280  ldp x8, x9, [x19, #0x50]     ; ThreadType+0x50 = 栈基址字段, +0x58 = size
0x2032E284  add x8, x9, x8
0x2032E288  ldur x8, [x8, #-0x20]        ; *(stack_top - 0x20) = 入口
0x2032E290  blr x8
```

创建时 SDK 确实把入口写到了 `*(stack_top-0x20)`（`0x2032E0B4`），但随后
`MapAliasStack` 把 `ThreadType+0x50` **改写成了别名地址**，而线程跑在别名那份上：

```
[CT] tt=0x1AB168020 +50=0x9F0C8000 base=0x9F1C8000  entry written @0x9F1C7FE0
[TR] tt=0x1AB168020 +50=0x1AB068000 read @0x1AB167FE0 = 0x0     <- 别名是全新零页
```

原实现的三个参数全错了：`x0` 是 `this`（`ThreadManagerImplByHorizon` 单例
`0x20CA5BB8`）、`x1` 才是别名地址、`x2` 是源栈、`x3` 是 size——被当成
(src, dst, size) 用，于是"接受映射但什么都不做"。Unicorn 不能真让两段虚拟地址
指向同一组物理页，所以改成**按 4 KiB 页把主栈镜像到别名区**（源栈是懒提交的，
整块读会 `UC_ERR_READ_UNMAPPED`，必须逐页容错），卸载时再拷回去。

#### 3. `nn::time`（`Abort(Result*=0)`，纯断言）

`nn::time::Initialize` 向 `time:u` 要标准用户时钟/稳态时钟的共享内存，我们回空
payload → `g_SharedMemoryClient` 保持 NULL → 断言。移植版给**固定时间**
（2020-01-01 12:00:00Z），7 个入口全部替身：`Initialize/Finalize/IsInitialized/
StandardUserSystemClock::GetCurrentTime/StandardSteadyClock::GetCurrentTimePoint/
GetSpanBetween/ToCalendarTime`。原则同 M3.18：guest 看到的时间不能依赖宿主。

#### 4. `nn::applet::TryPopFromApplicationParameterChannel`

`nn::time` 之后解引用 NULL 的是它。注意这个入口 **main 不导入**，是 SDK 自己调的
（导出符号在 sdk 里），所以按符号名接管；答案固定为"没有启动参数"（返回 false，出参清零）。

顺带踩到本轮的**第二个** mangled name 坑：符号真实名字末尾还有一个 `E`
（`…LaunchParameterKindE`），少写就静默不生效。为此 `m1_kern._install_hle` 现在装完
会打一行汇总：

```
[hle] 126 entries installed, 4 missing  <- os::FreeMemoryBlock, account::CloseUser, ...
```

（那 4 条是历史遗留的防御性重载，本 SDK 里没有对应符号，无害但会一直列出来。）

#### 当前状态

`LIM=300`、无钩子：175 帧在 ~80 s 到；`[time] Initialize` 在 ~190 s 通过；
之后到 300 s **没有 FAULT**，`chunks` 从 2116 稳定涨到 4945（资源解包），
`committed` 持平 1151 MiB。`draws` 仍为 0——现在这个 0 是**可信的**（见 M3.21），
说明游戏还在加载，没有提交几何。

### M3.23 抢占式调度：一个不含 SVC 的忙等会饿死所有工作线程（已修）

M3.22 之后的长跑出现一种新的"假卡死"：`chunks` 继续涨到 2118，帧却停在 178，
之后 100 s 里 `chunks` 一动不动。200 s 的全线程快照只有主线程活着。

#### 1. 主线程在等"资源管理器在忙"

采样窗口里 **1544 次**落在**同一条指令**上：

```
0x41062258  ldr   w8, [x26]          ; 状态机状态（0..0x1b，跳表 @0x42611E70）
0x4106225C  cmp   w8, #0x1b
0x41062260  b.hi  0x41062388
0x41062388  ldr   w8, [x20, #0x38]   ; 管理器"在途请求"计数
0x4106238C  tbz   w23, #0, 0x4106239C
0x41062394  cbnz  w8, 0x41062258     ; 非零 → 回到分发，等于原地打转
```

`[mgr+0x38]` 就是"有请求在飞"。置 1 的两处：

* `0x4165F898`：按名字查表（`[mgr+0xe00]` 的 count@+0x28 / array@+0x30，命中后逐字节比
  `"caption.sav"` 之类），然后调 `[mgr+0x30]` 的 `vtable+0x18` 提交加载；
* `0x4165FA24`：另一条入口，路径用 `"%s://%s"`（`0x426572F1`）拼出来再提交。

清零的三处：`0x4165ADF8` / `0x4165B2EC` / `0x4165EA98`——都在**别的线程**上跑的完成回调里。
运行时探针（`_probe_peek.py`，在分块边界读 guest 寄存器）拿到的现场：

```
pc=0x41062388 tid=0x100   state[x26]=6
[x20+0x38]=0x400000001   [x20+0x3C]=0x400000004   线程数=38
```

同一时段的 `_probe_stall2.py` 现场：这个窗口里除主线程之外几乎没有线程被采样到——
队列里明明有 runnable 的 worker，却一次都没拿到 CPU。

#### 2. 根因：调度器只在 SVC 处让出

`Kernel.run()` 的分块边界原来只是"回 Python 看一眼表，然后**继续跑同一个线程**"：

```python
p = mu.reg_read(UC_ARM64_REG_PC)
if p != END:
    self.chunks += 1
    pc = p; continue          # ← 没有调度
```

真机是**定时器抢占**：Horizon 的时片 1 ms ≈ 180 万条指令。我们没有这一层，于是任何
`while (!flag) {}` 都等于"进程停摆"——不是死锁，是其它线程**永远拿不到 CPU**，
而那个唯一能把 `[mgr+0x38]` 清零的解压/IO 线程就在 runnable 队列里躺着。

#### 3. 修法：分块边界就是时片

```python
def _tick(self, pc):
    cur = self.cur
    if cur is None or cur.state != 'run': return pc
    if len([c for c in self.runnable if c.state == 'run']) < 2: return pc
    self._save_ctx(cur, pc)
    nxt = self._pick_next()
    if nxt is None or nxt is cur: return pc
    self._load_ctx(nxt)
    return nxt.regs['pc']
```

`CHUNK`=2M 条指令，按 1.8 GHz 折算 ≈ **1.1 ms**，与真机 1 ms 时片同量级，所以不用改
`CHUNK`。代价：每 2M 条指令一次上下文切换（20 次/秒量级），可忽略。

#### 4. 效果（`LIM=300`，`BOTW_BARE=1`，无钩子）

| | M3.22 | M3.23 |
|---|---|---|
| 帧 | 178 | **199** |
| API 调用 | 895 | **1087** |
| FAULT | 无 | 无 |
| 标题资源 | 读不到 | `Title.pack` 4.9 MB、`TitleBG.pack` 147 MB 全部读入 |

`m2_verify.py` / `m3_verify.py` 都仍然 PASS。首段帧时间线也变了：M3.22 是
"21 帧 → 60 s 静默 → 175 帧"，现在是 "42 帧 → 立刻进资源加载 → 190 帧"。

#### 5. 第二个假指标：`chunks` 不是指令计数器

`chunks` 统计的是"`emu_start` 因 count 用完而返回"的次数。**SVC 密集的阶段永远到不了
边界**：`LIM=250` 那次 `chunks` 从 2199 一动不动跑了 80 s，而这 80 s 里主线程调了
**804,025 次 `os::GetSystemTick`**，线程 `0x121` 每 2.31 ms 一次 `svcSleepThread` 共
16,074 次。所以"chunks 不涨"和 M3.21 的 "draws=0" 一样是**观测口径**问题。

现在看进度至少三个数一起看：`k.chunks`（纯计算段）/ `sum(k.svcs.values())`（内核调用）/
`k.hle_calls`（HLE 入口分布）。按 tid 统计的办法是先包一层 `Kernel.handle_svc`。

#### 6. 顺带更正：`0x4191F000` 不是"任务树轮询"

M3.22 的交接把它读成"加载任务树轮询"，反汇编 `0x4191F830` 起是一段**逐字节 LZ 解码器**：

```
[x19+0x100] 目标指针      [x19+0x108] 标志位缓冲    [x19+0x114] 模式（1/2 都是回引）
[x19+0x118] 16 位距离     0x4191F948..0x4191F998  回引拷贝（8 字节一组展开）
```

同一 4 KiB 页里确实还有 `0x41C4B630`（"子任务是否全部完成"：遍历 `[obj+0x78]` 个条目，
检查 `[entry+0xfc] & 8`），4 KiB 页粒度把两者混成了一页。

#### 7. 新的墙：t≈220 s 之后整机静止（下一个目标）

`LIM=900` 的裸跑证实这不是"加载慢"：从 t≈220 s 起 `frames`(199) / `API`(1087) /
`committed`(1152 MiB) / `chunks`(2199) **四个数一起冻住**，一直冻到 900 s。
"能跑但很慢"和"停了"必须分开判。

在 SVC 边界（安全点）遍历各线程保存的 `x29` 帧链（`_probe_allstacks.py`）看到的现场：

| 停在哪 | 线程 |
|---|---|
| `nn::os::ReceiveMessageQueue`（`sdk+0x32637C` ← `main+0x126DB3C` ← `main+0x126CF74`） | 约 20 个 |
| `nn::os::AcquireSemaphore`（`sdk+0x327698` ← `main+0x1EE4E18`） | 0x114–0x119 共 6 个 |
| `nn::os::TimedWaitEvent` | 0x110 |
| `svcSleepThread` | 0x113、0x121（0x121 每 2.31 ms 醒一次，是唯一 runnable） |
| `svcWaitProcessWideKeyAtomic` | 主线程 0x100 |

也就是"任务系统的入口都在排队等活，而主线程在等一个永远不会来的完成信号"。
条件变量侧（`_probe_cv.py`，`k.dbg_cv=True`）：2035 次 `SignalProcessWideKey` /
677 次 `WaitProcessWideKeyAtomic`，其中 **1391 次 signal 到达时一个等待者都没有**。

#### 8. 顺带修掉的两处（都不改语义，只改"能不能跑完/跑多快"）

* **别名栈镜像改成按 `mem_regions()` 整段拷贝。** 原来是按 4 KiB 页逐页
  `mem_read` + `wmem`，`os::MapAliasStack` 在 300 s 裸跑里占 **35.4 s（12%）**；
  现在先枚举已映射区间、每段一次读写，降到 **0.22 s**（同一份脚本、同样 199 帧）。
  记录实际镜像过的区间，`UnmapAliasStack` 只回拷这些区间。
* **lazy 映射账本缺失导致的 `UC_ERR_MAP`。** 加速后长跑第一次撞上 Yaz0 往
  `0x146244000` 写时的 `Invalid memory mapping`：这一页在 Unicorn 里其实**已经映射**
  （`svcMapSharedMemory` 直接调了 `mem_map`，没写进 `self.ranges`），`_map_block`
  折半到 4 KiB 仍报"已存在"于是返回失败。现在最后一层失败会先用 `mem_read` 探测，
  已映射就补账本。修完 `LIM=320` 无 FAULT，指标回到 199 帧 / API 1087。

#### 下一步（按顺序）

1. 按 tid 记录 `(cond, mutex, tag)` 的完整配对，把"有 waiter、却永远没等到 signal"
   的那一条挑出来——`SignalProcessWideKey` 在没有等待者时丢弃是内核语义，所以要么
   signal 早于 waiter 到达，要么 signal 根本没人发。
2. 重点怀疑 `nn::os::MessageQueue` / `Semaphore` 的唤醒计数：我们唤醒时把 mutex 字
   直接写成 waiter 的 tid（等价于"内核替它拿到锁"），若一次唤醒多个，互斥就被破坏，
   对象状态可能被写坏。
3. 顺带确认 `os::MapAliasStack` 的逐页镜像已经换成按 `mem_regions()` 整段拷贝
   （300 s 裸跑里原来占 **35 s = 12%**）。
4. `0x4191F830`（本作 LZ）与 `0x421209C4`（Yaz0）的宿主侧解码 HLE **放到最后**：
   它会改变观测，必须先确认墙不是"等信号"再动。

### M3.24 mutex 的 `WAITERS_BIT`：M3.23 的墙其实是"解锁走了用户态快路径"（已修）

对标 `nxnative` 的 `dm/host/hle_thread.c`（详见 `docs/NXNATIVE_BOTW_NOTES.md`）时发现，
我们把 Horizon 的互斥字只当"owner 句柄"用，忽略了 **bit 31 = 有等待者**。

* `svcArbitrateLock`（0x1A）加锁失败时**从不置这个位**；
* SDK 的 `nn::os::Mutex::Unlock()` 在**用户态**读这个位：没置位就只写 0、
  **不发 `svc 0x1B`**；
* 于是我们影子表里的等待者永远等不到人交接。

改法（`m1_kern.py`）：0x1A 置位后再 `_block`；0x1B 交给队列第一个，
后面还有人就把位留着（`me | (0x80000000 if rest else 0)`）。

| 观测 | 改前 | 改后 |
|---|---|---|
| `m2_verify` 45 s | 20 帧 | 11,374 帧（253 fps） |
| `m3_verify` 30 s | 通过 | 7,089 帧（236 fps） |
| 裸跑 `LIM=320` `frames` | 199 | 115,686 |
| 裸跑 `API` | 1,087 | 925,488 |
| 裸跑 `committed` | 1,151 MiB | 335 MiB |
| 裸跑 `fs` 读取次数 | 多次（含 `TitleBG.pack` 147 MB） | 1 |

**最后两行说明这不是"修好了"，是换了一堵墙**：帧泵活了，资源加载退回去了。

#### 新墙（t=45 s 全线程栈，已定位）

* 主线程 `0x101`：`run`，持有消息队列 mutex `0x1080BE140`（字面值 `0x40000108`）；
* `0x10B` / `0x10C`：`TryPeekMessageQueue` → `svcArbitrateLock`，
  `owner=0x108`（即 0x101），`nwait=0`；
* 其余 10 个线程：`ReceiveMessageQueue` 的 `WaitProcessWideKeyAtomic`。

即"主线程拿着队列锁忙等，两个 worker 在等这把锁"，与 M3.23 同族。

#### 两个被数据否定的假设

1. `*cv_key` 没记账**不是**这堵墙的原因：给 cvlog 加上 key 字面值后发现
   2035 次 signal / 677 次 wait 的 key **全是 0**（1.6.0 的 `Signal` 不读它）。
   补上记账后帧数量级不变（3,815 vs 4,363）。
2. 把 0x1D 改成"只有第一个等待者拿到锁、其余退回 lockwait"（内核语义）后，
   游戏 **3 帧后从 `nn::init::Start` 正常返回**（主动退出）。先不改，等新墙拆完再看。

#### 下一步

1. 静态确认 1.6.0 `main` 是否有 `DT_RELR` 且我们的 loader 已正确应用。
2. 拆新墙：主线程为什么持 `0x1080BE140` 忙等、在哪忙等、为什么不解锁。
3. `dbg_cv` 的 cvlog 长期保留 `(cond, mutex, tag, key)` 四元组。

### M3.25 反汇编纠正 M3.24：等待位是 bit30，而且主线程的句柄是 0

M3.24 只看了 nxnative 的 `hle_thread.c`，把"等待位 = bit31"当成了结论。这一轮把
1.6.0 的 SDK 反汇编出来，两处都要改。

#### 1. `DT_RELR`：1.6.0 没有（待办划掉）

| 模块 | RELR 标签 | `DT_RELACOUNT` | 解析出的 `R_RELATIVE` |
|---|---|---:|---:|
| rtld | 无 | 15 | 15 |
| sdk | 无 | 9,376 | 9,376 |
| subsdk0 | 无 | 1,677 | 1,677 |
| main | 无 | 210,547 | 210,547 |

四个模块都只有经典 `DT_RELA`，条数与 `DT_RELACOUNT` 逐一对上，loader 没问题。

#### 2. `InternalCriticalSection` 的真实编码

`sdk+0x32F780` = `Lock`，`sdk+0x32F870` = `Unlock`；全 sdk 里 `svc #0x1a`
**只有 1 个调用点**（`0x2032F7DC`）。

```
*cs == 0                        空闲
*cs == 我的句柄                  我持有（无等待者）
*cs & 0x3FFFFFFF == 我的句柄     我持有（bit30 = 有等待者）
```

`Lock` 里 `w20 = [[tpidrro_el0+0x1F8]+0x1B0]`；等到 SVC 返回后，还要
`(*cs & 0xBFFFFFFF) == w20` 才退出重试循环。所以**交接必须写纯 tag**：
带上 bit30 的话，被唤醒的一方永远退不出这个循环。

#### 3. 主线程的句柄是 0

运行期 `[tpidrro_el0+0x1F8]` 指向 `0x20CA5BC0`（SDK 自己建的那个 ThreadType），
`+0x1B0` 是 **0**。我们 boot 写的 `THREAD+0x1B0=0x100` 是另一个对象，早被换掉了。

于是主线程传进 `svcArbitrateLock` 的 tag 是 0；而老代码判的是
`(*cs & 0xBFFFFFFF) == me`，低 30 位为 0 时**恒真**——每次争用都"静默成功"。
M3.23 之后的 11k 帧，有相当一部分建立在这个假互斥上。

#### 4. 四种"修正"都试过

| 变体 | 结果 | 原因 |
|---|---|---|
| owner 掩码改 `0x3FFFFFFF` | 4 帧 | tag 还是 0，递归分支永不进 |
| `ArbitrateUnlock` 交接带 bit30 | 0.8 s 退出 | 被唤醒方退不出重试循环 |
| 主线程分配句柄 + 对齐 `+0x1B0` | 2 帧后死锁 | 主线程真的开始等锁 |
| 上述全做 + 递归清 bit30 + 纯 tag 交接 | 1 帧 | 主线程持 CS 自旋：12 s 内 `ArbitrateLock` **181,461** 次 |

#### 5. 当前状态

保留 M3.24 那一版（`m1_kern.py.vA`）：`m2` 11,759 帧 / `m3` 7,353 帧 / 裸跑 115,686 帧。
**它的位是错的（bit31），能跑是因为 tag=0 把互斥让掉了**，只作为"能继续观测"的基线。

下一步：在 `sdk+0x32F780/0x32F870/0x32F840` 三点上逐次打印 `(word, w20, 分支)`，
先只对齐主线程的句柄，把自旋的原因钉死。

### M3.26 互斥真的修好了：帧数掉下来，是因为 guest 开始干真活

M3.25 的结论是"主线程持锁自旋"，修法是"给主线程分配句柄再对齐 `+0x1B0`"。这一轮
按 M3.25 的待办插桩，发现**修法是错的**：问题不在交接，在于 guest 自己读到的 tag 是 0。

#### 1. 三点插桩：先把"谁在转"钉死

在 `sdk+0x32F79C`（`Lock` 刚把 tag 读进 w20）、`0x32F7D0`（准备 `svcArbitrateLock`）、
`0x32F7E8`（SVC 之后的比较）三点各记一次 `(tid, cs, word, w20)`：

```
lock   406 次
slow   89,908 次      <- 全在同一个 Lock 调用内部的 retry 循环里
retry  89,906 次
svc 1A 105,798 次     svc 1B 1 次
最热 (tid=0x100, cs=0x1080BE140, word=0x40000100, w20=0x0)
```

`word` 里的 owner `0x100` 是**我们内核写的**（`tid_handle`），而 guest 的 `w20` 是
**0**。`Lock` 的退出条件是 `(*cs & 0xBFFFFFFF) == w20`，于是它永远不相等，
在 12 s 里空转 18 万次 SVC——这就是 M3.25 看到的"持锁方自己打转"。

#### 2. 根因：`ThreadType+0x1B0` 一直是 0，而且我们补得太晚

`nn::os::detail::ThreadManager` 的构造函数（`sdk+0x32A2F0`）在 `0x2032A340` 执行
`str x20, [x8, #0x1f8]`，把 `[tpidrro_el0+0x1F8]` 从我们 boot 写的 `0x61000000`
换成自己的成员 `0x20CA5BC0`；那个对象的 `+0x1B0` 没有任何代码去填（扫描 sdk/main/
subsdk0 里全部 357 处 `#0x1b0` 访问，没有一处命中它）。

关键细节：`Lock` 在**函数入口**（`0x32F798`）就把 `+0x1B0` 读进 `w20`，整个重试
循环只用寄存器里的值。所以 M3.25 那种"在 SVC 处理里补写内存"对**当前这次调用无效**，
补完照样自旋——那版"1 帧"不是死锁，是补丁生效得太晚。

#### 3. 三处修正

| # | 改动 | 为什么 |
|---|---|---|
| a | 在 `[tpidrro+0x1F8]` 被换掉的那一刻（`UC_HOOK_MEM_WRITE`）把 `+0x1B0` 填成本线程句柄 | 必须早于任何一次 `Lock`；晚一步就白补 |
| b | `svcArbitrateLock` 的身份改用 **guest 传进来的 `x2`**，不再用内核的 `tid_handle` | 内核与 guest 各有一套编号时，任何一边"自以为是"都会让退出条件不成立 |
| c | 交接时队列里还有人，就保留 bit30 | M3.25 说"带 bit30 会退不出"是错的：退出条件掩的是 `0xBFFFFFFF`，正好清掉 bit30 |

#### 4. 数据：帧数暴跌不是退步

| 观测（`BOTW_BARE=1`，无钩子） | vA（假互斥） | M3.26（真互斥） |
|---|---:|---:|
| `m2_verify` 45 s | 11,759 帧 | 13 帧 |
| `m3_verify` 30 s | 7,353 帧 | 13 帧 |
| 裸跑 120 s | — | 175 帧 / 870 MiB |
| 裸跑 300 s | 115,686 帧 / 335 MiB | 20 帧 / 541 MiB |
| `svcArbitrateLock` 累计 | 105,798 | 95 |

vA 的 11 万帧是假互斥喂出来的：主线程 tag=0，`(word & 0xBFFFFFFF) == 0` 在低 30 位为 0
时恒真，每次争用都"静默成功"，工作线程全被饿死，present 循环空转。现在主线程真的等锁、
工作线程真的跑，guest 开始构建资源表。

#### 5. 凭据：这次是"真在干活"

* 40 s 内 `memcmp` 677,521 次、**43,309 个不同键**
* 调用方 `main+0x14E01D0`（48 字节记录）占 649,875 次；`main+0x13ABEF4` 比的是
  `gys_skeleton` / `gys_shape` / `gys_material` / `gys_shader_options` 这类名字
* 堆 335 MiB → 541～870 MiB，`fs` 读与 `nvnBufferInitialize` 都在推进
* 没有 `DEADLOCK`，没有 CS 自旋

#### 6. 新墙：吞吐，不是正确性

300 s 只推进 4,151 个 chunk ≈ 83 亿条指令 ≈ **28 M instr/s**，且几乎全部花在主线程的
`memcmp` 扫描上（无 SVC、无 I/O、无分配）。这是 x86 主机上 Unicorn TCG 的量级。
要继续往下走，只有两条路：把 sdk 的热叶子函数（`memcmp`/`memcpy`/`memset`）HLE 掉，
或者换执行后端（TCG JIT / AOT）。前者当天就能验证收益。

#### 7. 下一步

1. HLE `memcmp`/`memcpy`/`memset`（先只 HLE 被调用最多的那几个入口），量 `chunks/s` 与
   `MiB` 的变化；同时确认键数继续增长（不增长就是退化成了死循环）。
2. 把 `+0x1B0` 的补齐收敛成一条通用规则，并加断言：交给 `svcArbitrateLock` 的 tag
   必须等于 `[[tpidrro_el0+0x1F8]+0x1B0]`，不等就打印一次。
3. 采样器记录 `(frames, MiB, 当前热点函数)` 三元组——单看帧数会再次被假互斥误导。

### M3.27 热函数 `GetSystemTick`：一次 HLE 往返 35 µs，ShaderArchives 阶段要几十万次

M3.26 之后仍有 30 s 的死窗口（`chunks` 不动、帧不动）。这一轮把"死窗口里到底在干什么"测出来了。

#### 1. 死窗口的指纹

| 观测 | 值 |
|---|---|
| `chunks` | **+0**（30 s 内一次都没完成） |
| SVC 计数 | 不涨 |
| `handle_svc` 调用 | **26.5 万次 / 10 s** |
| HLE 归属 | 100% `os::GetSystemTick`，LR=`main+0x182A224` |
| 线程 | 12 个池 worker 全部 idle 在各自 CV 上，只有主线程在跑 |

栈：`4182A0BC ← 4181D964 ← 4180C49C ← 418357F4`，其中 `418357F4` 附近有
`'Graphics'`、`'ShaderArchives'` 两个字符串——**主线程卡在 ShaderArchives 初始化**。

结论：HLE 的 SVC 往返 ≈ **35 µs/次**（异常 + `find_svc` + Python 调度，且**不计入**
`hle_time`，所以以前看 HLE 时间永远是 0.2 s，完全看不出来）。这条函数在这一阶段被
调用几十万到上百万次，于是 30 s 全烧在往返上。

#### 2. 第一次改法为什么失败（值得记住）

把 `0x20327FD0` 就地换成 5 条指令（`adrp / ldr / add / str / ret`）。跑起来**一点没变快**，
而且第 5 个字是 `D4002721`（`svc #0x139`）而不是 `ret`：

* `0x20327FD0` 与 `0x20327FE0` 是**两个相邻的导出函数**（后者是 `GetSystemTickFrequency`）；
* 我们写满 20 字节，压到了邻居的头 4 字节；
* 之后 HLE 安装继续往 `0x20327FE0` 写 SVC，正好把我们的 `ret` 换成 SVC。

#### 3. 最终做法

* 入口只留**一条 `b`**（`0x20327FD0` → `0x20E00100`，4 字节，不碰邻居）；
* 跳板放在 sdk 镜像（`0x20000000..0x20CD4000`）后面新映射的一页：`adrp / ldr / add / str / ret`；
* 计数器 `0x20E00000`，每次读 +4095 tick（≈213 µs，保持 M3.18 的确定性）。

`b` 的 ±128 MB 范围够不着 TLS 窗口（`0x600xxxxx`），所以跳板必须放在 sdk 附近——
这是第一次尝试跳飞（`b` 到 `sdk+0xF108`、直接 FAULT）的原因。

#### 4. 效果（`BOTW_BARE=1`，180 s）

| 观测 | 旧（HLE SVC） | 新（内联 tick） |
|---|---:|---:|
| `t=90 s` 的 chunks | 1,545 | 1,606 |
| `t=90..120 s` | **+0（30 s 死窗口）** | +250 |
| 堆到 1151 MiB 的时刻 | `t≈180 s` | `t≈160 s` |
| 帧 | 178 | 178 |

#### 5. 诚实的限制

虚拟时钟是**调用驱动**的（每读一次 +4095 tick），所以"把调用做便宜"等价于"让超时过得更快"。
真实时钟要等 M4 有了 present 环，改成按帧/按指令推进。现在这套只保证**确定性**和
"超时不会把整机拖死"。

#### 6. 下一步

178 帧之后堆再次走平（t=160..180 只涨 0）。继续用同一套指纹（`chunks` 冻结 + 
`handle_svc` / `emu_start` 记账）找下一个死窗口；`draws` 仍然是 0，M4 的前提没变。

### M3.28 终局指纹：178 帧之后，主线程等一个永远不会来的 LightEvent

M3.27 把 tick 往返修好之后，30 s 死窗口没了，但 178 帧 / 1151 MiB 之后整机还是静止。
这一轮把"静止"拍成了全量快照。

#### 1. 静止时的指纹

| 观测（`BOTW_BARE=1`，300 s） | t=165 s | t=180 s … t=300 s |
|---|---|---|
| 帧 / 堆 | 178 / 1151 MiB | 178 / 1151 MiB（不动） |
| `chunks` | +128 | **+0** |
| 最热 `emu_start` 入口 | — | **`0x2037EA70`，2.4 万次 / 15 s** |

`0x2037EA6C` 是 `svc #0x0b`（`svcSleepThread`）的 trampoline，所以那段"热"就是
**一个 250 µs 的轮询睡眠循环**（`x0=0x3D090`）。

#### 2. 全量快照（t=200 s）

```
MAIN   pc=0x2037EC38  svcWaitForAddress  (svc 0x34，LightEvent，timeout = -1 无限等)
       #0 main+0x13A0440  #1 main+0x1506C58  #2 main+0x1251718
       #3..#5 main+0x1251730（递归三帧）  #6 main+0x124EE48
       #7 main+0x1614C60  #8 main+0x1614950  #9 main+0x1250B2C
tid=0x121  run      pc 在 main+0x126CF74：`while(...) SleepThread(250us);` 的轮询循环
其余 32 个线程        全部 blocked，各自 idle 在自己那条 condvar 上
```

也就是说：**主线程在等一个事件，唯一的"活人"是个每 250 µs 醒一次的空转 worker，
没有任何人会 signal 那个 LightEvent。** 这不是死锁（没有互相持有），是**缺一个完成通知**。

#### 3. 顺手修的一处：`svcSleepThread` 也要推进虚拟时钟

虚拟 tick 是"读一次才走一步"的（M3.18/M3.27），于是任何
`while (!done) SleepThread(1ms);` 形态的等待在虚拟时间里永远不前进。现在
`svcSleepThread(ns)` 会按 `ns×19.2 MHz` 补进计数器（`_bump_tick`）。

效果：终局形态不变（178 帧 / 1151 MiB），但这属于**语义补齐**——它让"只靠睡眠等待"
的循环至少能在超时语义上正常工作；真正的完成通知仍然缺。

#### 4. 下一步（精确）

1. 在 `main+0x1506C58` / `main+0x13A0440` 处取出 `svcWaitForAddress` 的 `x0`（LightEvent
   地址），并在 `nn::os::LightEventInitialize` 的调用点反查它属于哪个对象。
2. 顺着该对象的创建点（大概率在 1151 MiB 那批 pack 的加载路径上，紧邻
   `ShaderArchives` / `Graphics`）找出"谁应该 signal 它"。
3. 三种常见结局要先区分：① 该 signal 在我们 HLE 掉的服务里（fs/nvn/ssl…）被吞了；
   ② 该 signal 由一个工作线程发，而那个线程卡在 CV 上（本轮已经能确认 32 条 CV 的位置）；
   ③ 该事件本来就该由 GPU fence 回读触发，M4 之前不可能来。

## M3.29–M3.32：从"等一个没人 signal 的事件"到 14952 帧仍在加载

这四轮的形状是一样的：上一轮的结论被证据推翻，换成更底层的解释，然后往前走一层。
每一轮都记录"怎么证伪的"，因为下一次遇到同类现象时，最快的就是复用这条证据链。

### M3.29 先证伪 M3.28：`svc 0x34/0x35` 的参数签名读错了

M3.28 的结论是"主线程在无限等一个 LightEvent"。`_probe_stall_m3.py` 的
`addrwait` 表把它推翻了：

```
ev=0x10B628500 val=1 n=1 [('0x100', 2, 976927)]
```

第三个字段是**超时值 976927 ns ≈ 1 ms**，不是 -1。真正的死锁是这样产生的：

1. 主线程用 1 ms 超时等 `0x10B628500`；
2. 旧内核只在"没有可运行线程"时才让带超时的等待者超时返回；
3. 而 tid `0x121` 是一条 `SleepThread(250 µs)` 的轮询线程，永远可运行 ——
   于是**那个 1 ms 永远不到期**。

两处修复（`m1_kern.py`）：

* `_vtick_now()/_deadline_ticks()/_expire_waiters()`：等待项改成
  `[ctx, tag, timeout, deadline_tick]`，按 19.2 MHz 虚拟 tick 到期
  （和 `GetSystemTick` 同一时钟），在 `_bump_tick` / `_pick_next` 里检查。
* `svc 0x34/0x35` 按 Atmosphere 语义整体重写（之前把 `w1` 当 tag、`w2` 当超时，
  等于**把 type 字段直接写进了事件字**）：

```
0x34 WaitForAddress(x0 addr, w1 TYPE, x2 VALUE, x3 timeout)
     type 0: *addr <  value 时等       type 1: *addr <  value 时先减一再等
     type 2: *addr == value 时等       type 3: 64 位相等时等
     条件不成立 → 0xFA01（SDK 当"重试"）  超时 → 0xEA01   成功 → 0
0x35 SignalToAddress(x0 addr, w1 TYPE, w2 VALUE, w3 count)
     type 0: 只唤醒                   type 1: *addr == value 时 +1（manual-reset）
     type 2: autoClear，按等待者数改字；count<=0 唤醒全部
```

证据：`_ev.txt` 里 `0x10B628500` 全程只有 5 次 guest 写
（`InitializeLightEvent` ×2、`SignalLightEventImpl 0→2`、`ClearLightEvent 2→0`、
`TimedWaitLightEventImpl 0→1`），而 `_sig.txt` 里 `SVC35 exit` 连续 300+ 次
读回 `val=1/2`、`state==1` 命中 0 次 —— 事件字被我们自己写回了"等待中"。

效果：`wait_to['addr']` 归零，帧 178 → **192**，NVN API 919 → **998**。

### M3.30 三堵"空服务指针"墙：pl VR / oe / applet

死锁消失后，init 一路往前，连续撞到三处同一类崩溃：**服务对象指针是 NULL**，
`ldr x8,[x0]` 直接读 0 号地址。

| 顺序 | 崩溃点 | 为什么是 NULL | 处理 |
|---|---|---|---|
| 1 | `sdk+0x337DB0 nn::pl::EndVrMode` | `pl::detail::GetService()` 拿缓存对象，pl 从没初始化 | `pl::BeginVrMode/EndVrMode/IsVrMode → 0`（iOS 没有 VR，语义就是恒 false） |
| 2 | `sdk+0x23650 nn::oe::EnableRecording` | `oe::Initialize` 是空实现 | 把 main 导入的 20 条 `oe::` 全量替身 |
| 3 | `sdk+0x7BD94 applet::StartLibraryAppletImmediately` | 游戏调 `hid::ShowControllerSupport` 弹"请握持手柄"的库小程序 | 见下 |

`oe` 这一组里带出参/返回值的不能一律给 0：
`GetOperationMode → 0`（Handheld）、`GetPerformanceMode → 0`、`GetDesiredLanguage → 1`
（AmericanEnglish）、`GetCurrentFocusState → 1`、`GetExpectedVolumeBalance(float*,float*)`
写入 `0.5f`、`TryPopNotificationMessage → false`。

第三堵墙的调用链是 main 自己的状态机：

```
main+0x15D2E0C  bl 0x42128CE0 (hid::ShowControllerSupport)
main+0x15D2C28  if (++counter < 3) return;          ← 连续 3 帧"没有手柄"才弹
```

也就是说：**我们的 hid 一直报"没有连接的手柄"**，游戏就弹 applet。先按 M5 的口径
在 `m2_hid.py` 里做了虚拟 Pro Controller（布局取自 nxnative `dm/host/hle_hid.c`）：

```
NpadFullKeyState / Handheld / JoyDual / JoyLeft / JoyRight 共用 0x28
    int64 sampling; uint64 key; int32 lx, ly, rx, ry; uint32 attributes, pad
    attributes bit0 = 已连接（Pro Controller 0x15，Handheld 0x3F）
GetNpadStates 写满 n 条（新→旧，sampling 连续递减）并返回 n
SixAxisSensorState 0x60：静止平放 acc=(0,0,-1)、方向=单位矩阵、attributes=1
SevenSixAxisSensor（机身陀螺仪）：初始化成功、GetStates 返回 0 条（和 nxnative 一致）
```

实测游戏确实在用它：`GetNpadStyleSet` 555 次、`GetNpadStates<FullKey>` 412 次、
`GetNpadStates<Handheld>` 143 次、`GetSixAxisSensorStates` 698 次，查询直方图里
`('states', 1, 0, 16, 16)` / `('states', 2, 32, 16, 16)` 全是"连着的 16 条"。

这一层修完：192 → **2722** → **2998** 帧，NVN API **10024**。

### M3.31 互斥失效：`ThreadType+0x1B0` 的 tag 是**死线程的句柄**

2998 帧之后整机静止。全量快照显示两把 CriticalSection 互等：

```
cs=0x1AAFB6540 word=0x40000220 n=3 [0x127, 0x123, 0x124]   ← owner = 0x125
cs=0x1AB066CD8 word=0x40000228 n=2 [0x100(main), 0x125]    ← owner = 0x127
```

看似 guest 真的 ABBA。但 SDK 的 `InternalCriticalSectionImplByHorizon::Enter`
（`sdk+0x32F780`）是这样写的：

```
w20 = [[tpidrro_el0+0x1F8]+0x1B0]        ; 本线程的 tag
loop: w8 = ldaxr [mutex]
      w8 == 0 → stlxr w20（拿到）
      否则 → 置 bit30、ArbitrateLock(w8&0xBFFFFFFF, mutex, w20)
             返回后重新读：(word & 0xBFFFFFFF) == w20 才算拿到
Leave: 读 tag；word == tag → 写 0；word 带 bit30 → ArbitrateUnlock
```

**tag 就是 ThreadType 里的 +0x1B0。** 打开 `BOTW_LOCKLOG=1` 把每把锁的事件史打出来：

```
== cs=0x1AAFB6540 (owner 0x220 = tid 0x125)
   tid=0x127  L  a=0x40000220  b=0x0        ← 0x127 传给内核的 tag 是 0
== cs=0x1AB066CD8 (owner 0x228 = tid 0x127)
   tid=0x127  H0 ...                        ← 0x127 在 CondVar::Wait 里释放过
```

`b=0x0` 说明 0x127 的 tag 是 0 —— 而 tag=0 在 guest 的判据里等于"空闲"：
交接后 `(word & 0xBFFFFFFF) == 0` 成立，它**自以为拿到了锁**，互斥就此失效，
后面看到的 ABBA 是失效之后的产物。

tag 日志给出了根因：

```
[tag] tid=0x127 tt=0x1A1EE7020 +0x1B0=0x248 handle=0x228     ← 0x248 是已退出的 0x12F
[tag] tid=0x128 tt=0x1A1EE7020 +0x1B0=0x0   handle=0x22C
[tag] tid=0x12F tt=0x1A1EE7020 +0x1B0=0x0   handle=0x248
```

`0x127..0x12F` **共用同一个 ThreadType**（BOTW 的线程池复用线程对象），
而我们的 `_new_thread` 用 `idx = len(self.ctxs)` 分配 TLS 槽位 —— `ctxs` 以
**线程对象地址**为键，复用同一个对象时 `len()` 不增长：

```python
idx = len(self.ctxs)                 # 旧：0x127 和 0x128 拿到同一个 idx
tls = TLS_WIN + idx * TLS_STRIDE     #  → 同一个 TLS 块
self.ctxs[arg] = c                   #  → 后创建的覆盖前一个
```

于是两条活着的线程共用一个 TLS 块，后启动的那条在启动代码里把
`[tpidrro+0x1F8]` 写成**自己的** ThreadType —— 0x127 的 TLS 从此指向别人的
（很快退出的）ThreadType：锁 tag 变成死线程的句柄，`GetTlsValue` 读回 0。

修复：

* `self.tls_next` 单调递增分配 TLS 槽位（TLS 窗口同时从 0x40000 扩到 0x100000，
  即 255 条线程）；
* 线程切换时把 `[tpidrro+0x1F8]+0x1B0` 校正成本线程句柄（`_tag_fix`），
  防止复用同一个 ThreadType 的**顺序**线程沿用上一条的 tag。

效果：2998 帧的静止彻底消失，同一份镜像跑到 **14952 帧 / 1265 MiB**。

另有一处同源崩溃，正好可以当对照：`main+0x1C80F9C ldr w8,[x0,#0xd0]`，x0=0，
调用点是 `nn::os::GetTlsValue(nn::os::TlsSlot)`：

```
sdk+0x327F80  mrs x8, tpidrro_el0
              ldr x8, [x8, #0x1f8]
              add x8, x8, w0, uxtw #3
              ldr x0, [x8, #0x80]        ← TLS 值挂在 ThreadType+0x80+slot*8
sdk+0x327FA0  ... str x1, [x8, #0x80]    ← SetTlsValue 写同一处
```

也就是说 **TLS 值和锁 tag 住在同一个对象（ThreadType）里**：TLS 块一复用，
两样一起坏。修 TLS 分配之后再没出现。

### M3.32 共享字体：BOTW 的 UI 文字来自固件，不在 RomFS 里

（本轮先补上，尚未被调用）真机上 `fontll::ScalableFontEngine::LoadFont` 的入参来自
`nn::pl` 的共享字体。BOTW 导入的四条：

```
pl::RequestSharedFontLoad(type)     pl::GetSharedFontLoadState(type)
pl::GetSharedFontAddress(type)      pl::GetSharedFontSize(type)
```

`m2_font.py` 按 nxnative `dm/host/hle_sdk16.c` 的做法给一份 BFTTF：

```
u32 magic = 0x18029A7F                       ; "已解码"标记
u32 len   = bswap32(size ^ 0x49621806)
后面紧跟原始 TTF
```

`ScalableFontEngineHelper::Decode` 看到 magic 就直接返回 `addr+8`（不改数据），
所以 BFTTF 头 + 宿主系统 TTF 就能让 SDK 自己的 FreeType 解析通过；
解析与错误处理仍在 SDK 里，我们只负责"地址和长度是真的"。字体文件不进仓库。

### 当前状态（M3.32）

```
BOTW_BARE=1, LIM=340 s：frames 14952  API 55211  draws 0  presents 565  MiB 1265
```

`op 统计 {'BEGIN': 14952, 'API': 55211, 'END': 14952, 'PRESENT': 565}`，
**一条 DRAW / CLEAR_COLOR / SET_RT 都没有**。把最近一帧的命令序列导出来看：

```
BEGIN
  nvnBufferGetAddress              caller=0x4124EBDC
  nvnCommandBufferReportCounter    caller=0x4124EBF4
  nvnCommandBufferSetSamplerPool   caller=0x4124EC0C
  nvnCommandBufferSetTexturePool   caller=0x4124EC24
  nvnCommandBufferSetShaderScratchMemory  caller=0x4124EC40
  nvnCommandBufferInitialize / AddControlMemory(2 MiB) / AddCommandMemory(4 MiB)
  nvnCommandBufferEndRecording
END
```

每帧都是"新开一个 command buffer → 设置池和 scratch → 结束"，**没有任何渲染目标**。
同期堆还在长（1151 → 1265 MiB），Yaz0 解压页（`main+0x191F000`）在采样里仍占大头，
说明它还在**首启加载**：BOTW 这一段是黑屏（所以 0 draw 是正常的），
不是卡死。

### 下一步

1. 继续把加载跑完（单次 15–30 min），盯 **draws 是否从 0 变成非 0**；
   同时看 `chunks` 是否还在涨（涨 = 加载；长时间不涨且 frames 涨 = 新的空转）。
2. 若加载完成后仍 0 draw，就按 `caller` 反查 0x4124EBDC / 0x412BE964 这两条
   路径到底属于哪个子系统（更像"GPU 作业队列"而不是主渲染）。
3. M3 的出口条件不变：**创建出 swapchain 并提交第一帧带几何的画面**。

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


## M3.33–M3.60：把「首启加载」从 15 分钟压到 30 秒，然后撞上字节序墙

这一段的形状和前面几轮不同：前几轮是「卡死 → 找到缺失的 HLE → 打通」，
这一段是「**太慢，慢到看起来像卡死**」。判据只有一个：`draws` 是否从 0 变成非 0。

### M3.33–M3.52 Yaz0 / 填充循环的宿主快路径

首启加载的热点从块采样里可以直接读出来：

| 地址 | 是什么 | 代价 | 处理 |
|---|---|---|---|
| `main+0x191F000` | 游戏内**增量** Yaz0 解压状态机（`main+0x191F770`） | 长期占 90% 采样 | `m2_yaz0.py`：按同样的调用契约在宿主解压 |
| `main+0x150F4E0` | 编译器内联的 `0xBEEF2929BEEF2929` 16 字节填充（清 24 MiB 图形缓冲） | 单次约 40 s | `m2_fastfill.py`：宿主 memset |
| `main+0x12E52C0` | 16 字节节点链的**逐字翻转**（`rev` × 4，步长 = 节点首字） | — | `m2_fastparse.py`（见 M3.58 的坑） |

Yaz0 的调用契约（实测）：

```
x0 = 解压器/作业对象      x1 = 1..N 当前块序号
[x0+0x180] 压缩流基址     [x0+0x188] 压缩总长
[x0+0x18c] 块步长 0x80000 [x0+0x100] 输出游标
```

宿主按「已提交的压缩字节」解析完整的 Huffman/回溯组，把读不全的组回滚到下一块；
流结束时写回整块输出并跳进游戏自己的收尾代码（两条 LightEvent signal 仍由 guest 发）。

### M3.53 `m2_fastparse` 的坑：寄存器是对的，数据是错的

第一版 `m2_fastparse` 报：

```
[parse] bswap loop error at n=148897792 p=0x15AB80958: UC_ERR_READ_UNMAPPED
```

看着像钩子读错了寄存器。**不是。** 现场寄存器（`_probe_walk.py`）：

```
pc=0x412E52DC x10=0x18EB80958 x11=0x18EB80950 x12=0x8CB13AF x13=0x15AB80958
```

`x10 == x11 + 8`、`x13` 已经从基址**倒走 0x3400000（54 MiB）**，所以
`x12/x13` 正是这段循环的 count/pointer。钩子没错，是**它忠实地执行了一条
本不该存在的 1.5 亿次遍历**。教训：先看循环体，再怀疑钩子。

### M3.54 Yaz0 的产物是对的（先证伪自己）

`m2_yaz0` 写出的两块输出（8.1 MiB / 11.0 MiB）落在 `C:\botw-work\tmp`，按
BOTW 自己的约定（**LE 字段 + `BOM = 0xFEFF`**）解析：

```
yaz0_7B91C4.out  hl=14 bom=FEFF fsz=7B91C4(real 7B91C4) doff=1000 ver=100
                 SFAT hl=C nodes=6 hash=00000065  node0 attr=01000000 start=0 end=61A8
                 SFNT 同一节点数
```

`fileSize` 与实际长度逐字节吻合、`SFAT/SFNT` 名字表可读。
对照磁盘上的 `Pack/TitleBG.pack`（`53415243 1400 fffe fa11c6 08 34530000`）：
`fileSize` 也只有按 LE 读才是 147,198,458。**BOTW 的 SARC 包就是 LE 的，
`FF FE` 只是标记，不是「大端」的意思。** 所以 HLE 解码不是污染源。

### M3.55 裸跑对照：这堵墙只在加载变快之后才摸得到

```
BARE 150 s：hits(0x412E52C0)=0   MiB 541 → 733 → 797（120 s 后不再涨）   frames 14
+ Yaz0 HLE ：hits 从 ~40 s 起持续增长，主线程 pc 停在 0x412E52DC
```

也就是说：裸跑那 15 分钟全部花在 guest 自己的 Yaz0 状态机上，
`main+0x412E52C0` **一次都没进**。Yaz0 HLE 把加载压到 30 s 内，
才第一次露出后面这堵墙。这不是新引入的回归，是**下一层**。

### M3.56 墙的结构：`0x412E5000` 决定「要不要翻字节序」

调用者 `main+0x12C33D0`（返回地址 `0x412C33D4`，与 fp 链一致）以
`x0=作业对象 x1=资源数据 x2=0 w3=4 x4=…` 调 `0x412E5000`：

```
0x412E5020  ldrh w8,[x0,#0x7a]          作业标志
0x412E502C  tst  w3,#4
0x412E5038  str  x1,[x0,#0x28]          作业->资源
0x412E5050  ldr  w19,[x1,#0xc]          ← 关键字段
0x412E5054  tbnz w19,#0,skip            bit0 = 1 → 已翻过，跳过
0x412E5058  （否则把 x1 起 0x150 字节按 dword 逐个 rev）
0x412E51A8  tbz  w19,#0,0x412E5234      同一个判据，决定节点链要不要翻
```

实测唯一一次调用：

```
CALL #1 x0=0x1901E2838 x1=0x18EB7D000 x2=0x0 w3=0x4 x4=0x18F7DC308 lr=0x412C33D4
  [blk+0xc]=0x270  [blk+0x14]=0x3800  [blk+0x18]=0x27D  [blk+0x1c]=0
```

`0x270 & 1 == 0` → 游戏认为「这份数据是**大端**，要翻」。翻完之后
`0x412E5234` 走到 `x9 = x1 + 0x150 + [x1+0x14] = 0x18EB80950`，也就是
M3.53 里那条链的基址，于是：

```
节点链基址处的 8 字节（guest 已经把前两个 dword 翻过一遍）：
  [x11]   = 0x00000000         next = 0
  [x11+4] = 0x08E00000         ← 循环用的 count = 149,946,368
翻之前它的原始 LE 值是 0x0000E008 = 57,352，才是一个合理的节点数。
```

**同一个 4 字节，LE 读 57,352（合理），按 BE 翻完是 1.5 亿（荒谬）。**
所以「要不要翻」这个判据在这份数据上给出了错误答案 —— 要么数据本该是 BE
却以 LE 到达，要么 `[blk+0xc]` 的 bit0 本来就该是 1（已经翻过）而某处把它清了。

### 现在的状态（M3.60）

```
m2_verify  PASS   13 帧 / 15 线程 / ipc missing={} / hid 查询 18 类
m3_verify  PASS   draw=0 overflow=0 api_entries=56
+ Yaz0 快路径：14 帧后停在 main+0x412E52DC（0x412E5000 内的节点链）
裸跑        ：14 帧后停在加载，797 MiB 后堆不再增长
```

**M3 出口条件（建出 swapchain 并提交第一帧带几何的画面）仍未达成**，
但卡点已经从「加载太慢看不清」变成「一个可定位的判据」。

### 下一步（按优先级）

1. **确认 `[blk+0xc]` bit0 的来源**：按 `blk` 头 32 字节在 RomFS 里反查文件，
   对磁盘原文件读同一个字段；若原文件本来就是 LE，则判据被我们自己写坏，
   去查 `0x412E5000` 之前谁改了 `[blk+0xc]` / `[blk+0x28]`。
2. 若确认原文件真是 BE（判据没错），就把 `0x412E52C0` 那条链做成**语义等价**的
   宿主快路径（`m2_fastparse`），并加保护：`count > 4M` 时不接管、只记账。
3. `m2_verify` 的 hid 检查已从「hid IPC 日志」改成「`m2_hid` 查询直方图」——
   M3.30 起 hid 是进程内 HLE，IPC 日志永远不会再出现，旧判据是过期而非回归。

### 环境备注

`unicorn` / `capstone` 曾从 `C:\Python314` 里消失，重新
`pip install unicorn capstone`（走本地 wheel 缓存，2.1.4 / 5.0.9）即可；
门禁与探针都必须用 `C:\Python314\python.exe` 跑。

### M3.61 【已作废，见 M3.62】一次算错 `dataOffset` 得出的错误结论

本小节原先写「那块 32 KiB 不是 node3 的原样拷贝」。**这个结论是错的**，
原因是我把 SARC 的 `dataOffset=0x1000` 漏加了一次：
node3 的数据从 `0x49D000+0x1000` 开始，不是 `0x49D000`。
按正确基址重比：`blk[0:0x8000] == node3[0:0x8000]`，32768/32768 字节全等，
最长公共前缀就是整个 32 KiB。

结论回到 M3.60 的分叉点：数据没有「拷短」，链基址处的
`00 00 e0 08` 就是文件里的原样内容。

### M3.64 写入追踪：那 4 个字节被写了两遍，时序完全确定

对 `[blk+0xC, blk+0x10)` 和 `[blk+0x3950, blk+0x3958)` 挂写监视（`_probe_flag.py`），
40 s 内**一共只有 8 次写**，全部发生在 `t=14.6~14.7`：

```
CALL x1=0x18EB7D000
t=14.6  pc=0x412E507C  [blk+0x00C] = 0x00007002     <- 28 词头交换，bit0 清掉
t=14.6  pc=0x412E523C  [blk+0x3950] = 0x00000000    | 第一遍：正常
t=14.6  pc=0x412E5244  [blk+0x3954] = 0x0000E008    | count = 57,352
t=14.6  pc=0x412E5258  [blk+0x3958] = 0x000000CC    | 步长 = 0xCC
t=14.7  pc=0x412E52B0  [blk+0x3950] = 0x00000000    | 第二遍：同一个地址
t=14.7  pc=0x412E52B0  [blk+0x3954] = 0x08E00000    | count = 149,946,368  <- 事故点
t=14.7  pc=0x412E52C8  [blk+0x3958] = 0xCC000000    | 步长被翻成巨大的负数
```

三条结论：

1. **第一遍完全正确**（0xE008 = 57,352，步长 0xCC，和 BE 数据吻合）。
2. **第二遍是 `0x412E52B0` 这条 `stp w10,w12,[x11]` 直接覆盖的**，不是别处改的。
   它之所以落回同一个 `x11`，是因为「下一张表」的偏移字 `[blk+0x3950]` 恰好是 0，
   于是 `blk + 0x150 + 0x3800 + 0` 还是 `blk+0x3950`。
3. 第一遍的循环跑完后是**顺序落下**到 `0x412E5288`（`0x412E5284 b.ne` 不成立即进入
   第二张表的代码），中间没有保护，所以「表偏移为 0」就必然导致自翻两次。

### 下一步（收敛为一个问题）

`[blk+0x3950]` 该不该是 0：

* 若不该是 0，那是上游（`0x412E5000` 之前）把 `[blk+0x14]` 或段头写坏了，
  要继续往 `0x412C33D0` 之前的资源装载路径查；
* 若是 0 合法，那么第二张表的基址公式就少加了一项（多半该是 `+4`：头在 `0x3954`，
  里面第一个字正好是 0xE008，可以当「下一张表偏移」用，指向 0x3954+0xE008，落在块内）。

两条都能用同一个实验区分：在 `0x412E5248` 处读 `w11`（段头的第二个字）并与
BE 原值对照；若 `w11` 本应非 0 而这里读到 0，就是上游写坏；若本来就 0，
就是这段代码对「偏移 0」缺少保护，需要在宿主侧把第二次遍历挡掉。

---

## M3.65 硬证据：那块 12 MB 缓冲区是**文件尾部的逐字节拷贝**，我们没有写坏它

M3.64 留下的二选一（`[blk+0x3950]==0` 是上游写坏，还是本来就 0）今天有答案了。

做法：在 `0x412E5000` 处把 `x1` 起 12 MB 整个 dump 成 `C:\botw-work\tmp\buf.bin`
（`_probe_convmem.py`，逐 4 KiB 页 `mem_read`，未映射页记 0），再和解包产物
`yaz1_A83000.out`（11,022,336 B）做全量比对。

结果：

```
buf 与 yaz1_A83000.out[0x49E000:] 最长公共前缀 = 0x5E5000 字节
0x5E5000 == 文件长度 - 0x49E000          （正好是文件剩下的全部）
逐页抽查 3072 页，前缀不一致 = 0
```

三条结论：

1. **块基址在文件里的位置是 0x49E000**，缓冲区 = `file[0x49E000:]` 再补 0 到 12 MB。
   不是「拷短」，也不是「拼接」，就是原样。
2. 因此 `blk+0x3950 = 00 00 00 00 00 00 E0 08 00 00 00 CC ...` 是**文件原样**。
   M3.61/M3.62 里「上游把段头写坏」这条分支被彻底排除。
3. 我们之前用的对照基址 `0x48936C`（12 字节头 `00005e02 00000000 00003000 00007002`）
   只是文件中**另一个同族记录**的起点：前 0x32 字节相同，之后分叉。该模式在文件里
   出现 12 次，是一组并列记录，不是这一块的真正起点。

### 顺带确认：57352 确实是 BE 计数，且缓冲区尺寸与它自洽

`buf.bin` 里 `blk+0x3950` 起的 BE dword：

```
0x3950  0x00000000        <- 「下一张表」偏移
0x3954  0x0000E008  57352 <- 条目数
0x3958  0x000000CC    204 <- 条目步长
0x3960  0x00004808  18440
0x3964  0x00006000  24576
0x396C  0x00000100    256
0x3970  0x0000A808  43016   ( = 18440 + 24576 )
0x3988  0x0000B808  47112   ( = 43016 + 4096  )
0x3994  0x0000C408  50184   ( = 47112 + 3072  )
```

`57352 × 0xCC = 11,699,808`，加上 `0x3950` 的头部正好落在 12 MB 缓冲区内
（`blk` 的池尺寸打印就是 `size=0x8000` 之外的整块 0xC00000）。所以
**计数和步长都是对的**，M3.62 里「57352 不是节点数」这条推断作废。

真正的矛盾是：`blk+0x3958` 只走一跳（`+0xCC` → `0x3A24`）就进了**零区**，
而零区是文件里真实存在的洞（`0x3998..0x40B0` 基本为 0）。

## M3.66 上一轮「跳过第二张表」失败的原因找到了：跳错了落点

`_probe_skiptable.py` 在 `0x412E5288` 强制 `PC=0x412E56CC` 后 fault，
现场是**空指针**而不是数据错：

```
pc=0x412E56D0  insn=ldr w10,[x9,#0xc]
x9=0  x11=0   lr=0x412E5194  sp=0x701F3F30
[mem] UNMAPPED access=19 addr=0xC size=4
```

`0x412E56CC` 的第一条就是 `ldr x9,[x11,#0x28]`，而 `x11` 在 `0x412E5288`
处**还没有被赋值**。真正的三条合法入口都会先做 `ldr x11,[sp,#0x10]`：

| 入口 | 前置 | 写入的标志 |
|---|---|---|
| `0x412E5200` | `mov x11,x21`（x21=[sp+0x10]） | `w8=2`（只置 bit1） |
| `0x412E5228` | `ldr x11,[sp,#0x10]` | `w8=2` |
| `0x412E5658` | `ldr x11,[sp,#0x10]` | `w8=1`（置 bit0） |

所以「跳过第二张表」本身没错，错在直接跳到函数尾声。

## M3.67 最小侵入版跳过：只省掉那次重复交换，函数**跑通了**

改成在 `0x412E5288` 判断 `[x9]==0` 时，**只把 PC 移到 `0x412E5628`**
（节点登记/合并点），保留 `x9`、不碰 `x11`：

```
CALL#1 t=15.7 x1=0x18EB7D000
  @5288 x9=0x18EB80950 [x9]=0 w8=0x7002
  @5628 enter merge x9=0x18EB80950 [x9+4]=57352 w8=0x7002
  @56CC epilogue x11=0x1901E2838 w8=0x1      <- x11 合法，标志落成 0x7003
```

**函数返回了**（frame0 `ra=0x412E5928`，frame1 = 调用者 `0x412C33D4`）。
字节序转换这堵墙从「无限循环」变成「能走过去」。

注意 `w8=0x7002` ⇒ `0x412E5630 tbz w8,#1` 不成立，走的是 `0x412E5634`
的空转延迟分支（`(w19&-8)` 取负后 `adds #8` 直到回零，7169 次），
**没有**执行 `0x412E5668` 那轮「逐条登记」。这条分支的语义待定，
但它不是本次 fault 的原因（原因见 M3.68）。

## M3.68 新墙：游戏自己的堆要 58.2 MB，返回 0

函数走到 `0x412E58F0`：

```
412E58F0  ldr x8,[x19,#0x40]     ; 数组基址 -> 0
412E58F4  ldr w10,[x19,#0x38]    ; 数组长度
412E58FC  cmp x24,x10
412E5900  madd x11,x24,#0x428,x8
412E5910  csel x22,x11,x8,lo     ; x22 = 0
412E5924  bl  0x412855A0         ; 0x412855A0 第一句 mov x20,x0
```

⇒ `0x4128566C str x0,[x20,#8]` 写地址 `0x8`，`UC_ERR_WRITE_UNMAPPED`。

`[obj+0x38]/[obj+0x40]` 唯一的写入点在 `0x412E58A4/0x412E58A8`，
而要走到那里必须先成功分配：

```
412E577C  blr x8        ; vt[0x30]  allocate
412E5780  cbnz x0,0x412E5794     ; 成功才继续初始化 0x428 步长的数组
412E5784  b  0x412E58AC          ; 失败直接跳过
```

实测（`_probe_vcall.py`）：

```
ALLOC enter x4=[sp+0x48]=0x18F7DC308  x19=57352
  vcall obj=0x18F7DC308 vt=0x42BB3CF8 fn=0x4125AC00
        x1(size)=0x3A32148  x2=0x8
  vcall ret x0=0x0        committed=733 MiB
```

`0x3A32148 = 57352*0x428 + 8 = 58.2 MiB`，尺寸和我们推的完全自洽；
**是游戏自己那层堆（`0x4125AC00`，`sead` 风格 vtable 分配器）返回了 0**，
不是我们的 `nn::os` 层拒绝。

### 这里的判断

58.2 MiB 一次性拿走，说明这张表确实是「57352 个 0x428 字节的节点描述」。
放在真实机器上这是能成功的（日志里游戏自己要过 `AllocateMemoryBlock(0xC1000000)`），
所以下一步是查 `0x4125AC00` 为什么说「不够」——大概率是它内部的
free-size 记账依赖某个我们没实现/实现错的 `nn::os` 查询，
或者这段代码本应在 `m2_fastparse` 覆盖的范围内。

### 下一步（按优先级）

1. 在 `0x4125AC00` 入口和它判空返回的分支上挂钩，抓它读的 free/limit 字段；
   对照 `k.committed` 判断是「池真的小」还是「记账读错」。
2. 若确认是记账问题，优先修根因；若确认池子小，就在宿主侧把这张 0x428
   表按需虚拟化（`m2_fastparse` 同款思路：只保留节点数 + 按访问懒生成）。
3. `w8 bit1`（`0x412E5630` 走空转分支、跳过登记）这条语义单独确认一次：
   若本应登记，`[obj+0x38]/[obj+0x40]` 也可能由登记过程建立，那 M3.68 就自然消失。

## M3.69 堆取证：小堆（约 5.75 MB）本身没空间，而**向上回退的全局槽是空的**

在 `0x4125B52C`（"空闲链走完"的收口点）挂钩，拿到本次失败的全部状态：

```
OOM  需要 = 0x3A32148  (58.2 MiB = 57352 * 0x428 + 8)
     x24 = [0x42CBF018] = 0x42CBEF38        <- 非空（静态根堆单例，自指）
     [0x42CBEF48]      = 0x0                <- 空！向上回退的目标不存在
     堆对象 = 0x18F7DC308
        +0x090 = 1                (尺寸倍率，0x4125ACF0 用它算对齐后长度)
        +0x0DC = 0                (堆类型 0)
        +0x0E0 = 0x18F7DC418      (空闲链头)
        +0x0E8 = 0x18F7DC418      (== 链头 ⇒ 空链，哨兵自环)
        +0x0D8 = 0x005C0004       (≈5.75 MiB，这个堆的 arena 量级)
```

堆对象完整 dump（`_probe_heapobj.py`）显示它是一个虚表在 `0x42BB3CF8`
的 sead 风格堆，`+0x50/+0x58` 和 `+0xE0/+0xE8` 是**两条自环空链**。

### 失败路径（PC 轨迹，`_probe_heap.py`）

```
4125ACDC..4125AD00  取尺寸、检查溢出
4125AD08  cmp [h+0xe8], h+0xe0        （非空，不跳）
4125AD24  cbz w8,[h+0xdc] -> 4125B110 （类型 0 走这条）
4125B110  x8 = x13
4125B114  ldr x13,[x8,#0x18]          （下一个空闲块）
4125B118  cmp x13,x23                 （够不够）
4125B120  x8 += x9; ldr x8,[x8,#8]; x8 += x11; cmp x8,x12; b.ne 4125B114
4125B134  b 4125B52C                  （链走回起点 = 没有够大的块）
4125B52C  cbz x24,...                 （x24 非空）
4125B534  ldr x0,[0x42CBEF48]         （= 0）
4125B538  cbz x0,4125B55C
4125B55C  mov x19,xzr                 （返回 0）
```

### 结论

1. 这个 `0x18F7DC308` 是**调用者传进来的小工作堆**（arena 量级 ~5.75 MiB，
   而且空闲链已经空），
   让它在里面拿 58.2 MiB 本来就该失败 —— 这一层行为是**对的**。
2. 真机上失败后会走 `0x4125B534` 的"向上/扩容"虚调用，但那个目标指针
   `[0x42CBEF48]` 在我们这里**从来没被写过**。
3. 所以 M3.68 的根因不是"堆太小"，而是**根堆单例 `0x42CBEF38` 的 `+0x10`
   槽（`0x42CBEF48`）没初始化**。这个槽是从 `nn::os` 侧
   （`SetMemoryAllocator` / 根堆注册）填的，属于我们 HLE 的缺口。

### 下一步（收敛）

1. 在启动早期对这 8 个字节挂写监视（`0x42CBEF48`），确认真机路径里它就是被
   `nn::os::` 某个注册函数写的；对照我们 `m1_kern` 里同名 HLE 的返回值。
2. 若确认是注册函数被 HLE 成空实现，补上即可；补上后 58.2 MiB 会由根堆
   直接满足，M3.68 自然消失。
3. 备选：若真机上这条路也不走（即 `[0x42CBEF48]` 本来就可能为空），那就说明
   58.2 MiB 的请求本身不该发生，要回去重新确认 `57352` 的语义 —— 但
   58.2 MiB 与 `57352 * 0x428 + 8` 的精确吻合支持"计数是对的"。

## M3.70 那个空的回退槽是**设计如此**，不是 HLE 缺口

对 `[0x42CBEF38, 0x42CBEF68)` 全程挂写监视（`_probe_rootheap.py`），
整个启动只有 10 次写：

```
t=0.2  pc=0x400252BC  [0x42CBEF40] = 0
t=0.2  pc=0x400252BC  [0x42CBEF48] = 0        <- 静态初始化就写 0
t=0.2  pc=0x400252C8  [0x42CBEF38] = 0x42BB4028   (vtable)
t=0.2  pc=0x400252E4  [0x42CBEF68] = 0x400000000
t=0.6  pc=0x40E53FBC  [0x42CBEF48] = 0        <- nninit 再写一次 0
t=0.6  pc=0x40E53FF0  [0x42CBEF58] = 0xC1000000   <- 根堆 size = 3.0 GiB
t=0.6  pc=0x40E5403C  [0x42CBEF68] = 1
```

终态：`[0x42CBEF50] = 0x105D88000`（根堆 base，对应
`AllocateMemoryBlock(0xC1000000)` 那条日志）、`[0x42CBEF58] = 0xC1000000`。

**结论：`[0x42CBEF48]` 在真机上也是 0。** M3.69 的"补上这个槽"这条分支作废，
根堆是通过别的接口暴露的，`0x4125AC00` 里那个 `cbz x0,→0x4125B55C` 就是
"没有可选的扩容回调"的正常表达。

## M3.71 现状收敛：唯一未解 = 58.2 MiB 该由谁满足

被清零/初始化的小堆 `0x18F7DC308` 在 t=11.6 被一段 `memset`（`0x204CB958`/
`0x204CB95C`，subsdk 里的 libc）整片刷过，说明它在装载早期还只是一块裸内存，
真正的构造函数在其后。因此"arena 只有 ~5.75 MiB"这个读数可能只是
**构造完成前的中间态**，不能直接当作真机也这么小。

剩下三个候选（写进 `HANDOFF_M3_12.md`，按此顺序验）：

* **C**：`0x412E5630 tbz w8,#1` 走了空转延迟分支（7170 次 `adds`），
  跳过了 `0x412E5668` 的逐条登记；若真机该走登记，`[obj+0x38]/[obj+0x40]`
  可能由登记过程建立，58.2 MiB 这个请求就不该出现。**一次 hook 即可证伪。**
* **A**：`x4`（`0x18F7DC308`）在真机上指向一个大堆，我们这边给它 arena 的
  那一步缩水了；顺着 `x4` 反查 arena 赋值点。
* **B**：`[blk+0x3954]` 仍不是元素个数 —— 但 `57352*0x428+8 == 0x3A32148`
  与游戏实参精确相等，反对这条。

## 本轮净进展

从「卡死在字节序转换、永不返回」推进到：

```
转换函数返回 ✓  →  停在 [obj+0x40]==0 的数组分配  →  已精确归因到 58.2 MiB 请求
```

M3 出口（swapchain + draws>0）**仍未达成**。

## M3.72 转向：先交 iOS 侧可验证的骨架

M3.71 把唯一未解问题收敛成「58.2 MiB 该由谁满足」，三个候选都要靠 hook 继续挖。
继续挖的边际收益在下降——**即使挖通了，iOS 侧仍然是空的**。所以本轮换轨道：
把 iOS 侧做成"别人拿 Mac 就能编、没 Mac 也能验、验不过就红"的骨架。

### 做了什么

| 交付 | 数字 | 证据 |
|---|---:|---|
| NVN 入口点注册表 | 513（29 域） | 扫 `main.reloc.bin` 里 `nvnDeviceGetProcAddress` 按名解析的字符串，运行时按名取，名字即接口面 |
| 生成的分发表 | 2 个文件 | `nvn_api.h`（名字表+ABI）、`nvn_dispatch.cpp`（二分查找+缺失上报）；`gen_nvn.py --check` 防手改 |
| Metal 后端入口 | 32 / 513（6.2%） | `BIND(...)` 与 `implemented_metal.txt` 由门禁强制相等 |
| 可移植 null 后端 | 0 / 513 | 让 ABI 测试在 Windows/Linux 上真跑 |
| ABI 测试 | 513 procs | MSVC 2022 `/W4 /WX`，`nvn_abi_test` + `core_smoke_test` 均 exit=0 |
| ObjC++ 语法检查 | 1 个 TU | clang + `tests/stubs/` 桩头文件，`-Wall -Wextra -Werror` |

### 两个真 bug（不是设计缺口）

1. `Tex** list = obj<Tex>(x[1]);` —— 把句柄当 `Tex*` 赋给 `Tex**`，Xcode 里必然
   编译失败。**是 `syntax_check_metal.py` 抓到的**，不是靠读代码。
2. `MTKView.delegate` 是 weak，`Pump` 建完就释放 → 首帧永远不会来，屏幕全黑
   且帧号不动。改成由 Coordinator 强引用。

### 三道门禁

* 编译期：`nvn_metal.mm` 里 `#if defined(NDEBUG) #error`，Release 直接编不过；
  `NDEBUG` 由 `ios/project.yml` 的 Release 配置显式给出，不依赖 Xcode 默认值。
* 仓库级：`verify_ios.py --release` 在 `kEnumsVerified = 0` 时报错；
  CI 有一条专门断言它**必须失败**。
* 一致性：`BIND` 列表 ∩ registry == `implemented_metal.txt`，且 `project.yml`
  里每个 `- path:` 必须真实存在（否则 `xcodegen generate` 只在 mac runner 上炸）。

### 明确没做到

* **没有 draw call**：`MTLRenderCommandEncoder` 从未创建（渲染目标绑定未实现），
  `drawArrays` 走 `if (!b->enc) return;`。
* **没有纹理**：`nvnTextureInitialize` 只 `new Tex()`，builder 字段偏移未验证。
* **没有着色器**：NVN 给的是 Maxwell SASS，Metal 不接受，需要
  SASS → IR → SPIR-V → MSL 离线翻译（`ios/nvn/shaders/README.md`）。
* **M3 出口仍未达成**：58.2 MiB 的三个候选（C/A/B）原地挂起，没有新证据。

### 下一步（按序）

1. `nvnCommandBufferSetRenderTargets` → 让 `enc` 真的建起来，先做 clear-only 的
   "屏幕出纯色、帧号增长"。
2. 验证 `NVNtextureBuilder` 字段偏移，`nvnTextureInitialize` 建出真 `MTLTexture`。
3. `nvnCommandBufferBindProgram` + `nvnProgramInitialize`，开始 SASS 翻译。
4. 剩下的 481 个按 `scripts/nvn_status.py` 的 P0/P1/P2 顺序推。
## M3.73 第一次上屏：把 NVN 窗口接到 App 真正的 CAMetalLayer

### 先说清楚这不是什么

屏幕上的颜色**不是游戏画的**。现在没有任何标题代码在跑，`botw_core_frame()` 用
宿主的 clear + present 驱动交换链。它证明的是"层 → drawable → command buffer →
present"这条路在 iOS 上能走通，不证明渲染管线通了。

### 修掉的架构错误

之前 `Renderer.swift` 用 `MTKView` 画一个自娱自乐的三角形，而
`nvnWindowInitialize` 另建了一个 `CAMetalLayer`——**没有任何视图持有它**。
换句话说：即使客体的 NVN 命令全部正确，画面也会 present 到一个不显示的
surface 上。这不是"还没做"，是"做了也看不见"。

现在：

* `MetalView: UIView`，`override class var layerClass: AnyClass { CAMetalLayer.self }`，
  `layoutSubviews()` 里按 `displayScale` 设 `contentsScale` / `drawableSize`。
* `botw_core_desc.native_layer` 把这块层交给核心；`nvngfx_metal_set_layer()`
  用 `__bridge` 借存（视图持所有权，核心不 retain）。
* `nvnWindowInitialize` **只用**这块层。`nvnWindowBuilderSetNativeWindow` 传来的
  是 guest 的 `nn::vi` 句柄，不是 CAMetalLayer——直接 `__bridge` 成
  CAMetalLayer* 会向随机指针发消息，所以刻意不采信，只用 App 给的那一块。
* `CADisplayLink` 驱动 `botw_core_frame()`；`presents` 通过
  `botw_core_nvn_stats` 暴露给 HUD。
* `nvnQueuePresentTexture` 在客体纹理还没有 `MTLTexture` 时不再丢帧，而是
  清屏 present，并 `log_once` 一次——丢帧会让 App 看起来像卡死。

### 顺手修掉的

* `CADisplayLink` 的 target 由 `CoreDriver` 自己强引用并 `invalidate()`；
  上一版的 `MTKView.delegate` 是 weak，`Pump` 建完就释放（屏幕全黑、帧号不动）。
* `syntax_check_metal.py` 增加 `-fobjc-arc` 与 `-fobjc-runtime=ios-17.0`。
  没有后者，clang 走 GNUstep 运行时，连 `rpd.colorAttachments[0]` 都解析不了。
* `tests/stubs` 补上 `MTLRenderPassDescriptor` / `MTLClearColor` / `CGSize` /
  `CAMetalLayer.drawableSize`。

### 证据

```
nvn_abi_test: ok (513 procs)                 exit=0
core_smoke_test: ok (nvn 0/513 on null)      exit=0     (新增 presents == 0 断言)
syntax_check_metal: ok (clang++.exe)                   (-fobjc-arc -Werror)
verify_ios: ok
```

CI（本机无 Mac，全靠 runner）：`portable` 全绿；`ios` job 的
`xcodegen generate` / 模拟器 Debug 构建 / 真机 Debug 构建 / **Release 必须失败**
全部通过。

**没有真机运行记录**——本机没有 iPhone。`presents` 会涨这件事是设计意图，
不是实测结论。

### 下一步

`nvnCommandBufferSetRenderTargets`：拿到客体的颜色目标后，`beginRecording`
时就能建 `MTLRenderCommandEncoder`，`nvnCommandBufferClearColor` 才会真的
生效，draw 才有机会执行。这是阶段 A 的剩余部分。
## M3.74 参数形状：把 32 个实现按真实签名重写

上一轮那 32 个"已实现"的入口，参数解码全是猜的。这一轮拿到公开项目的
原型表一比，**几乎每一个都错**：

| 入口 | 原来的读法 | 真实签名 |
|---|---|---|
| `nvnCommandBufferSetViewport` | `x[1..4]` 当 double | `int x, y, w, h` |
| `nvnCommandBufferSetScissor` | 同上 | `int x, y, w, h` |
| `nvnCommandBufferDrawArrays` | `(first, count, primitive)` | `(primitive, first, count)` |
| `nvnCommandBufferDrawElements` | 把 `x[6]` 当 indexType | `(primitive, indexType, count, indexAddress)` |
| `nvnWindowBuilderSetTextures` | `(builder, array, count)` | `(builder, count, array)` |
| `nvnWindowAcquireTexture` | 从 `x[0]` 返回句柄 | 索引写进 `x[2]` 指向的 `int`，返回 result 枚举 |
| `nvnWindowInitialize` 等 | 把宿主指针写回 `x[0]` | `x[0]` 是 guest 的对象地址，返回 boolean |

最后一条是结构性的：**NVN 对象住在 guest 内存里**，宿主只能以 guest 地址
为键存侧表。原来那版会把宿主 `new` 出来的 `Win*`/`Pool*` 交给 guest，
guest 下次调用传回自己的地址，宿主再当指针解引用——能编译、能过冒烟测试，
真机第一次调用就崩。

### 做了什么

* `scripts/extract_nvn_signatures.py`：解析公开项目 nxnative 的
  `dm/gfx/nvn/nvn_protos.h`（MIT），只保留名字/返回类型/参数类型，
  产出 `ios/nvn/signatures.json`（470 条）。头文件本身不入库。
* `ios/nvn/inferred_signatures.json`：4 个原型表里没有的入口，按命名对称性
  推断，单独标注、单独计数。
* `nvn_metal.mm` 整体重写：以 guest 地址为键的对象表、返回值写 `w0`、
  出参写穿 guest 指针、builder 侧表。
* `verify_ios.py` 新增：已实现入口必须有已知形状；arity > 8 直接判失败
  （`x[8]` 装不下栈参数）；形状里的名字必须在注册表里。

### 量化结论

* 470 个原型里最大 arity = **8**，正好等于 `x0..x7` 的容量。
  之前"8 个字够用"是假设，现在是测量。
* 覆盖表仍然是 **32/513**，数字没变——变的是这 32 个现在真的对。

### 证据

```
extract_nvn_signatures: ok (470 shapes, max arity 8)
verify_ios: ok    argument shapes: 470 measured, 4 inferred, max arity 8
syntax_check_metal: ok
nvn_abi_test: ok (513 procs)
core_smoke_test: ok (nvn 0/513 on null)
```

仍然**没有** `nvnCommandBufferSetRenderTargets`：它的签名已经有了
（6 个参数），但目标纹理必须先由 `nvnTextureInitialize` 造出 `MTLTexture`，
而那需要 `NVNtextureBuilder` 的字段偏移。下一步是 `nvn_obj.h` 里的 `tex_t`。

## M3.75 纹理与渲染目标：从“只有句柄”到“能开 pass”

上一轮结尾的判断是：`nvnCommandBufferSetRenderTargets` 的签名已经有了，
但目标纹理必须先由 `nvnTextureInitialize` 造出 `MTLTexture`，而那需要
`NVNtextureBuilder` 的字段偏移。这一轮没有去猜偏移，换了一条路。

### 不猜偏移：让 setter 自己交底

`NVNtextureBuilder` 是不透明结构。与其从公开的 `nv_obj.h` 抄 `tex_builder_t`
的字段偏移（版本一变就错，且错了是静默错），不如把 13 个 setter 的实参
自己记下来：

| setter | 记录的字段 |
|---|---|
| `nvnTextureBuilderSetTarget` | `NVNtextureTarget`（1D/2D/3D/cube/array） |
| `nvnTextureBuilderSetFormat` | `NVNformat` |
| `nvnTextureBuilderSetSize1D/2D/3D` | 宽 / 高 / 深 |
| `nvnTextureBuilderSetWidth/Height/Depth` | 同上（两种建法都接） |
| `nvnTextureBuilderSetLevels` | mip 级数 |
| `nvnTextureBuilderSetSamples` | MSAA 采样数 |
| `nvnTextureBuilderSetStorage` | `NVNstorage`，先只映射 Private |
| `nvnTextureBuilderSetStride` | 行跨距（只记录） |
| `nvnTextureBuilderSetFlags` | 只记录 |

`nvnTextureInitialize` 用这份记录建 `MTLTextureDescriptor`。**没有任何一次
读取 guest builder 内部**，所以本轮不存在“偏移猜错”的失败模式。

代价写在明处：像素由宿主分配（`MTLStorageModePrivate`），和 guest 内存
**不 alias**。当渲染目标够用；要采样 guest 上传的像素，还得另做一条
上传或别名路径。

### 157 项格式表

`ios/nvn/src/nvn_texture.c` 里有 `NVNformat -> MTLPixelFormat` 的全表，
派生自公开 nxnative（MIT）的 `dm/gfx/tex/nvn_fmt_mtl.c`。每项带一个
exact/substitute 标记；映射到替代格式时 `lossy` 出参置 1，后端只打一次
`nvnTextureInitialize.substitutedFormat` 日志。

`tests/nvn_texture_test.cpp` 抽查三类：已知精确项、替代项、越界值。
**表里有 157 个非零项。**

> 这里踩过一个坑，值得记：测试第一次失败是**测试期望错**，不是表错。
> `0x49` 不是 BC7 而是 BC4_U(140)，BC7 是 `0x4D`；`0x30` 也不是空洞，
> 是 RGBA32Sint(124)。

### 渲染目标与 encoder

- `nvnCommandBufferSetRenderTargets(cb, numColor, colorArray, depth, depthStencil, ...)`：
  记录 `numColor`（上限 8）、`color[8]`、`depth`；换目标前先 `endEncoding`。
- `ensure_encoder()`：第一次 draw / viewport / scissor 时懒开 pass，
  只挂 `colorAttachments[0]`，`loadAction` 由 `pendingClear` 决定。
- `nvnCommandBufferClearColor`：不再丢弃；关掉当前 pass，记下 RGBA 与 mask，
  下一次开 pass 时变成 clear。mask 语义属于未验证枚举，只记日志。
- `nvnCommandBufferEndRecording`：`[cb commit]`。

### 量化结论

- 覆盖：32 -> **46 / 513（9.0%）**；TextureBuilder 1 -> 14 / 43，
  CommandBuffer 7 -> 8 / 144。
- `P0 blockers open: 5/6`，`nvnCommandBufferSetRenderTargets` 已 done。

### 证据

```
generated artifacts up to date (513 procs, 46 implemented)
verify_ios: ok   metal bindings: 46/513
nvn_abi_test: ok (513 procs)
core_smoke_test: ok (nvn 0/513 on null)
nvn_texture_test: ok (157 formats in table)
syntax_check_metal: ok (clang++.exe)
build_abi_test: all tests passed
```

CI：`portable` 与 `ios`（xcodegen / 模拟器 Debug / 真机 Debug / Release 必须失败）
全部通过（run 40989294645，commit `2967a41`）。**没有真机运行记录**，本机无设备。

### 仍然没有的东西（别把 46 读成“快能玩了”）

| 缺口 | 后果 |
|---|---|
| program / pipeline | draw 只打一次 `noPipeline` 就返回，**0 条 draw** |
| 顶点缓冲 / attrib state | 同上 |
| MRT / 深度 attachment | 只挂 `color[0]`，深度测试不可用 |
| texture view | 只记日志，实际挂 base texture |
| guest 像素 alias / 上传 | 纹理只适合当 render target |
| `nvnQueueSubmitCommands` | 命令在 `EndRecording` 就 commit，不等队列 |
| Maxwell SASS -> MSL | 整个移植最大的墙，一行没动 |

### 下一步

`nvnProgramInitialize` + `nvnCommandBufferBindProgram` + 顶点状态，让
`ensure_encoder()` 开出的 pass 里第一次出现**真正的** pipeline。但在着色器
翻译有结论之前，这一步只能验证“Metal 侧把管线建起来了”，不能验证
“客体的 shader 跑起来了”。

## M3.76 第一条真 draw：把翻译器之外的一切做完，并让它被 CI 执行

上一轮的结论是“着色器翻译是墙”。这一轮没有去撞墙，而是**把墙两侧都建好**：
墙的右边（MSL -> Metal 管线 -> draw -> 回读）现在能跑，墙的左边（SASS -> MSL）
只剩一个接口，返回 NULL 和理由。

### 为什么先做右边

如果没有一条能执行的 draw，之后任何翻译器都无法验证：分不清“翻译错了”和
“管线建错了 / 顶点没绑对 / 目标格式不对”。所以先造一个**已知正确**的最小
程序，把 Metal 侧的每一环串起来，再让翻译器往里填。

### 接口

```c
const char* nvngfx_shader_to_msl(const void* code, unsigned long long size,
                                 nvngfx_stage stage, const char** why);
```

* 现在返回 NULL，`why` = “no translator: Maxwell SASS -> MSL is not implemented”；
* 约定翻译器要产出 `nvngfx_main_vs` / `nvngfx_main_fs` 两个入口；
* `nvngfx_shader_set_msl_passthrough(1)` 是给自检和将来的离线 MSL 缓存用的：
  它把 `code` 原样当 MSL 返回，**不做任何翻译**，默认关闭。

### 新增的 6 个入口（46 -> 52）

| 入口 | 做了什么 |
|---|---|
| `nvnProgramInitialize` | 建侧表项，返回 1 |
| `nvnProgramSetShaders` | 记录每个 stage 的 `NVNshaderData{data, control}`；数组顺序即 stage 顺序 |
| `nvnProgramFinalize` | 释放缓存管线并删除侧表项 |
| `nvnCommandBufferBindProgram` | 设当前 program，尝试建管线（目标格式已知时） |
| `nvnCommandBufferBindVertexBuffer` | 记录 guest 地址与长度（**没有 draw 用它**） |
| `nvnCommandBufferBindVertexBuffers` | 只记录范围；`NVNbufferRange` 的内容故意不读 |

`DrawArrays` 现在：有管线就 `setRenderPipelineState` + `drawPrimitives`，
没有就打一次 `noPipeline` 并返回。`SetRenderTargets` 会清掉缓存的管线，
因为 Metal 把颜色格式烘进 pipeline。

### 自检：一条真的 draw，回读像素

`nvngfx_metal_selftest()` 走的是**客体入口点**，不是直接调 Metal：

```
TextureBuilder 5 个 setter -> nvnTextureInitialize (64x64 RGBA8)
BeginRecording -> SetRenderTargets -> ClearColor(红)
BindProgram(内置 MSL) -> DrawArrays(3) -> EndRecording
blit 到 shared buffer -> 检查中心像素 == (64,128,191,255)
```

顶点用 `[[vertex_id]]` 生成全屏三角形，所以**不需要顶点缓冲**——这正是
“先把管线跑通、顶点输入留到后面”的意思。

CI 在 iOS 模拟器里用 `simctl` 安装并启动 App（启动参数 `--selftest`），
`BotWApp.init()` 先写 `BOTW_SELFTEST start`、跑自检、再写 `BOTW_SELFTEST rc=0`
并 `exit(0)`；工作流读 App 容器里的 `Documents/selftest.txt` 断言。

**已跑通**（run 40997940031，commit `6f99850`，`ios` job 全绿）。
这条路线上踩过两个真 bug，都值得记：

| 现象 | 原因 |
|---|---|
| `xcodebuild test` 只报 “Executed 0 tests” + TEST FAILED，没有原因 | XCTest bundle 在该 runner 的宿主问题；日志拿不到（非 admin），注解只有 348 秒等待。改用 simctl |
| 写出的 `selftest.txt` 只有 `start`，没有 rc | 自检给 texture builder 传了假地址 `0x5101`，`nvnTextureBuilderSetDefaults` 会 `memset` 0x80 字节 → 段错误 |
| 第一次连 `start` 都没有 | `--console-pty` 在 App 秒退时接不上；改成写 App 容器文件 |

### 证据

```
nvn_abi_test: ok (513 procs)
core_smoke_test: ok (nvn 0/513 on null)
nvn_texture_test: ok (157 formats in table)
nvn_shader_test: ok (seam refuses SASS, passthrough opt-in)
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 52/513)
build_abi_test: all tests passed
```

覆盖：**52 / 513（10.1%）**；CommandBuffer 8 -> 11 / 144，Program 0 -> 3 / 7。
`P0 blockers open: 1/6`（只剩 `BindVertexAttribState`）。

### 没有做到的事

| 缺口 | 后果 |
|---|---|
| Maxwell SASS -> MSL | 客体的 draw 仍然全部停在 `noPipeline` |
| `BindVertexAttribState` | 顶点布局没有落地，任何真实几何都画不出来 |
| `DrawElements` | index type 未验证，仍然只计数 |
| 模拟器 Metal 的实测 | 本轮推送前没跑过，CI 结果见下 |

**没有真机运行记录**，本机无设备。

## M3.77 离线着色器链在 Windows 上跑通（本轮）

不再是"没有翻译器"的状态：`Bootup_Graphics.pack` 里取出真实 shader，
`dmsh_xlate`（Ryujinx）→ SPIR-V，`spirv-cross` → MSL，**都在本机从源码构建并执行**。

```
extract: 1 file(s), 5102 shader(s): 2033 vs, 3069 ps
translate: 2/2 shader(s)
  d756ff62a57fc508_vs → 3,411 行 MSL, vertex, notes = 0
  029de06ee5b5d21b_ps → 4,573 行 MSL, fragment, notes = 0
```

新增：`scripts/shader_chain/`（`build_tools.ps1`、`nx_shader.py`）、
`docs/SHADER_CHAIN.md`、`nvngfx_shader_lookup()` / `nvngfx_shader_set_blobs()`。

仍然没有的：`xcrun metal` → `.metallib`（只有 macOS，CI 上做）；
运行时按 key 取 MSL 的接线（M3.78 已补上，见下）；
顶点描述符（`BindVertexAttribState`）；**没有真机验证**，自检用的仍是内置 MSL。

## M3.78 真实 shader 的 key 在运行时算得出来，而且对得上（本轮）

M3.77 结束时，链的两端各自能跑：离线能产出 `<key>_<stage>.metal`，运行时有个
能按 key 查的表——**但没有任何东西在运行时算 key**。本轮把这根线接上，并且
用真实数据证明接对了。

### 做了什么

| 位置 | 内容 |
|---|---|
| `ios/nvn/src/nvn_shader_key.c` | 纯 C SHA-256 + `nvngfx_shader_key_from_guest(control, code, out)`；规则与 nxnative `hle_nvn.c` 的 `keys_hash` 逐字一致 |
| `nvn_metal.mm` · `Program::key[3][17]` | `nvnProgramSetShaders` 当场算 key（客体允许在这次调用之后释放 control 段） |
| `nvn_metal.mm` · `program_pipeline()` | 先按 key 查离线表，查不到才回落到 `nvngfx_shader_to_msl()`（自检的 passthrough 走后者） |
| `nvn_metal.mm` · `nvngfx_metal_create()` | 装 `nvngfx_shader_install_offline_blobs()` |
| `ios/nvn/src/nvn_shader_blobs.gen.c` | 入库版本是**空表**；`gen_blobs.py` 才会填真东西 |
| `scripts/shader_chain/gen_blobs.py` | `<key>_<stage>.metal` 目录 → C 表（分块转义） |
| `scripts/shader_chain/keycheck.cpp` | 用**运行时那个 C 函数**重算真实 shader 的 key，和文件名逐个对账 |

### 证据

```
build_abi_test: all tests passed
  nvn_shader_test: ok (seam, blob table, SHA-256, shader key)
keycheck build\shader-chain\work\code
  keycheck: 5102 shader(s), 5102 match, 0 bad, 0 skipped
gen_blobs --msl build\shader-chain\msl  -> 2 shader(s), 378.8 KiB
  blobs installed: 2
  vs=found (202798 bytes)  ps=found (175562 bytes)
  wrong stage must miss: miss(ok)
```

SHA-256 钉了两个标准向量（空串、`abc`）；key 规则钉了两个 Python 独立算出的
值（`2c8239fb360e36c1` / `a4a67a417698a5e1`）。

### 踩到的坑（会静默毁掉整条链）

key 哈希的 code area **含**开头 0x30 字节的头（`78 56 34 12 ...`），而交给
翻译器的输入是从 SPH（+0x30）开始的。第一版 keycheck 拿已经去掉头的 `.bin`
去算，**5102 个全部对不上**。运行时不会报任何错：`nvngfx_shader_lookup()`
只是返回 NULL，然后静默回落到"没有翻译器"。所以：

* `nx_shader.py extract --keep-full-code` 额外写 `<key>_<stage>.code`（带头）；
* `verify_ios.py` 新增不变量：入库的 `nvn_shader_blobs.gen.c` 必须是空表，
  超过 4 KiB 或出现 `kBlobs` 数组就 FAIL（防止有人把本体数据提交上来）。

另外把 `nx_shader.py` 的 key 规则也统一成 `ctl_len()`（`0x760 <= u32@0x20 < 0x880`
时取自身长度，否则 0x880），与 C 和 nxnative 三处同一个算法；BOTW 的 control 段
都是 0x7D1 字节，所以这一改不影响已经产出的文件名。

### 覆盖与仍然没有的

覆盖仍是 **52/513**（本轮没有新增 NVN 入口；改的是已有入口的行为），P0 仍只剩
`nvnCommandBufferBindVertexAttribState`。

| 缺口 | 后果 |
|---|---|
| 顶点描述符 / argument buffer | 真实 shader 找得到、编得出管线，但顶点数据没接上，**没有一条真实 shader 跑过 draw** |
| `xcrun metal` → `.metallib` | 未做；MSL 以 C 字符串进包，5,102 个约几十 MB，真正打包要换 metallib |
| 全部 19,592 个 shader | 未做；只跑了一个 pack 的 5,102 个 |
| 真机 | **没有真机运行记录**，本机无设备；CI 只在模拟器里验证自检 |

## M3.79 顶点输入接通：最后一个 P0 落地（本轮）

`nvnCommandBufferBindVertexAttribState` 一直是唯一开着的 P0。本轮把它连同
配套的 5 个 `NVNvertexAttribState*`、5 个 `NVNvertexStreamState*`、
`nvnCommandBufferBindVertexStreamState` 一起做完（**52 -> 64 / 513**，
`P0 blockers open: 0/6`），并且让 CI 的模拟器自检**真的画了第二条 draw**。

### 客体结构体的真实布局

这两个结构体很小（4 / 8 字节），引擎通常把它们放在栈上反复 memcpy，所以
**不能用"按地址查侧表"**——地址一复用就是静默错读。本轮直接读写客体自己的
字节，布局来自 nxnative 里带编译期断言的声明
（`dm/gfx/vn/nvn_obj.h`，`FITS(va_st, NVNvertexAttribState)`）：

```
NVNvertexAttribState: uint32 { format:8, offset:19, stream:5 }   4 字节
NVNvertexStreamState: uint32 stride, uint32 divisor              8 字节
```

19 位的 offset 上限是 0x7FFFF（512 KiB - 1），nxnative 超过就报错；我们照做：
越界不打洞、不截断，直接记一条日志并丢弃该字段。位域用手工移位实现
（`nvngfx_vattr_pack/unpack`），不依赖编译器的位域分配顺序。

### 格式表：宁可说"不支持"

NVNformat 是纹理和顶点共用的一套枚举。`nvn_vertex.c` 只接受 Metal 真的有
对应顶点格式的那些：

| 情况 | 处理 | 原因 |
|---|---|---|
| 1/2/4 组件的 8/16/32 位标量 | 全支持 | 与 `MTLVertexFormat` 一一对应 |
| RGB32F / RGB32UI / RGB32I | 支持 | 3×4 = 12 字节，正好等于 Metal 的 `Float3`/`UInt3`/`Int3` |
| **RGB8 / RGB16 家族** | **不支持** | Metal 的 `UChar3`/`UShort3` 占 4/8 字节，NVN 是紧凑的 3/6 字节，读出来会整体错位 |
| RGB10A2 / RGB10A2SN / R11G11B10F / RGB9E5F | 支持 | 单 32 位打包，Metal 有对应格式 |
| BGRA8 | 支持 | `MTLVertexFormatUChar4Normalized_BGRA` |
| RGBX8/16/32 | 按 4 组件处理 | 存储布局与 RGBA 相同，第 4 通道本来就没有定义 |
| 深度/模板/sRGB/压缩/**`_UI2F`/`_I2F`** | 不支持 | 没有 Metal 顶点格式；nxnative 在 CPU 上转换，我们还没做 |

遇到不支持的格式时**不画**：管线干脆不建，日志打一次格式值。悄悄画出来的
错误顶点比不画更糟。

### 管线与绑定

* `MTLVertexDescriptor` 由「绑定的 attrib 数组 + stream 数组」现建；
  属性下标就是着色器的 `[[attribute(i)]]` —— `BindVertexAttribState` 的第 i 个
  元素就是属性 i，spirv-cross 也按这个顺序发射，中间不需要重映射。
* 顶点缓冲不拷贝：`pool_initialize()` 用 `newBufferWithBytesNoCopy` 把客体的
  pool 存储直接包成 `MTLBuffer`，`pool_span()` 按地址找到所属 pool 和页内偏移，
  `setVertexBuffer:offset:atIndex:` 直接指过去。地址不在任何 pool 里就不画，
  日志写明是哪个 stream。
* 管线缓存的键从「program + 颜色格式」扩成「program + 颜色格式 + 顶点布局」
  （布局 id 是 `nvngfx_vertex_layout_id()`，FNV-1a 过打包后的 guest 字）。
  `Program` 留 4 个槽：标题会在几种布局之间来回切，单槽会每次切换重生管线。
  （该轮当时为 4 槽；后续 M4 续七已扩到 16 槽。）
* `nvnCommandBufferBindVertexBuffers` 现在**真的读** `NVNbufferRange{address,size}`
  （之前故意只记个数）。

### 自检：第二条 draw 走完整条顶点路径

`nvngfx_metal_selftest()` 现在画两条：

| # | 顶点来源 | 验证的东西 | 期望像素 |
|---|---|---|---|
| 1 | `[[vertex_id]]` | 管线、编码、回读（原有） | (64,128,191,255) |
| 2 | `[[attribute(0)]] float2` | **pool → MTLBuffer、attrib/stream 状态、顶点描述符、属性读回** | (0,255,0,255) |

第二条走的是客体入口点：`nvnMemoryPoolBuilderSetStorage` → `nvnMemoryPoolInitialize`
→ `nvnVertexAttribStateSetFormat/SetStreamIndex` → `nvnVertexStreamStateSetStride`
→ `BindVertexAttribState` / `BindVertexStreamState` / `BindVertexBuffer`
→ `DrawArrays`。中途还用 `nvnVertexAttribStateGetFormat` 把状态读回来核对
（证明 setter 写出的 4 字节记录能被 getter 原样读回）。

### 证据

```
build_abi_test: all tests passed
  nvn_vertex_test: ok (packing, 26 formats, layout id)
  syntax_check_metal: ok
verify_ios: ok (metal bindings: 64/513)
NVN host-HLE 64/513 (12.5%)   P0 blockers open: 0/6
```

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| argument buffer（UBO / 纹理 / sampler 绑定） | 真实 shader 现在能建管线、能绑顶点，但常量与纹理还没接，**真实场景仍画不出正确画面** |
| `DrawElements` | index type 与 index buffer 仍未接 |
| 真实 shader 的 draw | 没有一条；自检用内置 MSL |
| 模拟器执行第二条 draw | 本机无 Metal，**只有 CI 能证明**；本轮推送后看 `ios` job |
| 真机 | **没有真机运行记录** |

## M3.80 DrawElements 接通：索引缓冲、16/32 位索引与第三条自检 draw（本轮）

Unity（《旷野之息》用的引擎）几乎每一次几何提交都是
`nvnCommandBufferDrawElements`，而之前它只会"计数 + 打一行日志"。本轮把它接到
`drawIndexedPrimitives`，并让模拟器自检用**第三条 draw** 走完索引路径。
覆盖数不变（**64/513**）：这个入口本来就在 64 个里，本轮改的是它的行为。

### 签名与 index type

原型来自 `ios/nvn/signatures.json`（由真实调用形状量出来）：

```
nvnCommandBufferDrawElements(NVNcommandBuffer*, NVNdrawPrimitive,
                             NVNindexType, int count, NVNbufferAddress)
```

`NVNindexType` 的数值**没有 Nintendo 头文件可对**。本轮沿用 nxnative 的读法：
`1 = uint16`、`2 = uint32`（它的录制器对 1..2 之外的值直接拒绝）。于是：

| index type | 处理 |
|---|---|
| 1 | `MTLIndexTypeUInt16`，每索引 2 字节 |
| 2 | `MTLIndexTypeUInt32`，每索引 4 字节 |
| 其它 | **不画**，`log_once` 打出数值 |

这条"推断值"的标注同时写进代码注释、`docs/IOS_PORTING.md` 与本节；等拿到
Nintendo 的 `NVNindexType` 枚举再改成实测值。

### 索引缓冲：和顶点一样零拷贝

`NVNbufferAddress` 是客体虚拟地址，不是 `NVNbufferRange`。和顶点路径一致，
用 `pool_span()` 在内存池里找到 `MTLBuffer` 与页内偏移：

```
pool_span(indexAddress, count * indexSize) -> (MTLBuffer*, offset)
drawIndexedPrimitives:... indexBuffer:buf indexBufferOffset:offset
```

`count * indexSize` 用 64 位算，`pool_span` 里 `len > size - o` 的判断保证不越界；
地址不在任何 pool 里、或长度越过 pool 尾部，就不画并记一条
`nvnCommandBufferDrawElements.indexBuffer`。`count` 是 `int`，负值转成
unsigned 会变成很大的数，但结果是 `pool_span` 失败 -> 不画，不会踩内存。

### 前置条件与状态

索引路径和 `DrawArrays` 共用同一套闸门：`ensure_pipeline()` 建管线、
`bind_vertex_streams()` 绑顶点流，然后才编码。任一步失败都**不编码**，
所以"没有管线却画了索引"不会发生。

NVN 的状态挂在 command buffer 上，不是挂在 render pass 上；自检第三条 draw
正是靠这一点：同一个 `CmdBuf`（0x5700）先 `EndRecording` 提交第二条、再
`BeginRecording` 开第三条，`program` / `attrib` / `stream` / `vb` 都还在。
`SetRenderTargets` 会让缓存的 `pso` 失效（颜色格式可能变），
`ensure_pipeline()` 用原来的顶点布局键重建 —— 键没变，命中 `Program` 的
4 槽缓存。（该轮当时为 4 槽；后续已扩容到 16 槽。）

### 自检：三条 draw

| # | 入口 | 验证的东西 | 期望像素 |
|---|---|---|---|
| 1 | `DrawArrays` + `[[vertex_id]]` | 管线、编码、回读 | (64,128,191,255) |
| 2 | `DrawArrays` + `[[attribute(0)]]` | pool、顶点描述符、属性读回 | (0,255,0,255) |
| 3 | **`DrawElements`，16 位索引 `0,1,2`** | **索引地址寻址、index type 映射、`drawIndexedPrimitives` 编码** | (0,255,0,255) |

第三条复用第二条的 program / 顶点缓冲，只在 pool 的 +8192 偏移处写三个
`uint16_t`，然后走客体入口 `nvnCommandBufferDrawElements`。任一环节失败
都返回 `4096`。

### 证据

```
build_abi_test: all tests passed
  nvn_vertex_test: ok (packing, 26 formats, layout id)
  syntax_check_metal: ok
verify_ios: ok (metal bindings: 64/513)
NVN host-HLE 64/513 (12.5%)   P0 blockers open: 0/6
```

模拟器上的三条 draw 由 run **41008588834** 证明：`portable` success、
`ios` success。自检写出的 `BOTW_SELFTEST rc=0` 让 CI 继续跑完了
设备构建与「Release 必须被拒」两步，两步也都 success。

### 两次 CI 才看清的自检缺陷

本轮的自检不是一次写对的，两个 run 的反馈都记在这里：

| run | 结果 | 原因 |
|---|---|---|
| 41006846112 (`09fb5f7`) | `rc=2048` | 池大小写成 `sizeof(vertexPool)`；`vertexPool` 是 `unsigned char*`，所以池只有 **8 字节**，`pool_span()` 拒掉 24 字节的顶点跨度，draw 2 被静默丢弃。修复 `d33f9ba`：尺寸改用一个常量，分配 / memset / builder 三处共用 |
| 41007654642 (`d33f9ba`) | 仍 `rc=2048` | 池大小不是唯一原因；而此时"被丢弃"和"画错了"在同一张像素上无法区分 |

第二个 run 之后，可观测性补上了三件事：

* `CmdBuf::encoded`：**真正交给 Metal 的 draw 条数**（`draws` 是"被请求的条数"）。
  自检失败时同时打印两者，一眼能分出"上游丢弃"还是"编码了但画错"。
* CI 用 `simctl launch --console` 启动 App，把 `[nvn]` / `[selftest]` 的
  stderr 抓进日志；失败时逐行变成 check-run annotation（读原始日志需要 token，
  annotation 不需要）。
* draw 2 / 3 的顶点着色器改成 spirv-cross 的形状（一个 `[[stage_in]]` 结构体，
  成员带 `[[attribute(0)]]`），而不是裸的 `[[attribute(0)]]` 参数 —— 自检要
  测的正是翻译器实际产出的形式。

第三轮 run 41008588834 通过。那两轮之间**只有 shader 形状这一处改动会影响
绘制**（`encoded` 计数器只加数、`--console` 只改 CI），所以结论是：Metal 不接受
裸的 `[[attribute(n)]]` 顶点参数 —— 库或管线创建失败，没有管线，draw 被丢弃。
严格说这是排除法，不是从日志里读到的：成功那一轮的原始日志要 token 才能读，
`--console` 抓到的 `[nvn]` 行没能取回。好在 spirv-cross 产出的正是结构体形式，
真实翻译链不受这条限制影响。

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| argument buffer（UBO / 纹理 / sampler） | 真实场景仍画不出正确画面，**这是下一块墙** |
| `NVNindexType` 的枚举值 | 1/2 来自 nxnative，不是 Nintendo 头文件 |
| `DrawElementsInstanced` / `DrawArraysInstanced` | 未做（原型已在 `signatures.json` 里） |
| 真实 shader 的 draw | 仍然是 0 条；三条自检都用内置 MSL |
| 真机 | **没有真机运行记录** |

## M3.81 参数缓冲区接通：BindUniformBuffer -> `[[buffer(0)]]`

spirv-cross 的 MSL 不读 `setFragmentBytes`，它读一个**参数缓冲区**：

```
fragment main0_out nvngfx_main_fs(main0_in in [[stage_in]],
                                  constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
struct spvDescriptorSetBuffer0 { constant _16* fp_c3 [[id(0)]]; ... };
```

本轮把这条通路接上，覆盖 **64 -> 66 / 513**，并用**第四条自检 draw** 证明。

### 客体的入口

| 入口 | 行为 |
|---|---|
| `nvnCommandBufferBindUniformBuffer(cb, stage, index, address, size)` | stage 1（像素）记录 address / size；其它 stage 只记一次日志（原因见下） |
| `nvnCommandBufferBindUniformBuffers(cb, stage, first, n, ranges)` | 逐个调用上面的 |

`NVNshaderStage` 的数值来自 nxnative 的观察（0 vertex / 1 pixel / 2 geometry /
5 compute），不是 Nintendo 头文件。

### 为什么只有像素阶段

参数缓冲区绑在 **buffer(0)**，而顶点流的 `setVertexBuffer:atIndex:` 0 号槽
正是顶点数据；spirv-cross 的顶点着色器同样把参数缓冲区放在 buffer(0)。所以
真实顶点常量需要一个**索引偏移**约定（顶点流从高槽位开始，或顶点参数缓冲区
换一个 buffer index）。这个约定没验证之前，顶点阶段的 UBO 只记录并打一条
`nvnCommandBufferBindUniformBuffer.stage`，不猜位置。

### 编码

参数缓冲区是一段 16 项的 `MTLBuffer`（`MTLResourceStorageModeShared`），
每项 8 字节，内容是 **`MTLBuffer.gpuAddress + 池内偏移`** —— 也就是 MSL 里
`constant _16*` 直接解引用的那个地址。绑定：

```
[b->enc setFragmentBuffer:abuf offset:0 atIndex:0]
```

* 每次 `BeginRecording` 丢掉旧的参数缓冲区（上一次提交可能还在飞），
  绑定变化时也丢掉，下一次 draw 重建。真正跑起来要换成环形分配器。
* UBO 地址不在任何内存池里 -> 该项留 0（不是留一个错地址），记一条
  `nvnCommandBufferDraw.uniformBuffer`；参数缓冲区分配不出来 -> **不画**。

### 自检：第四条 draw

| # | 入口 | 验证的东西 | 期望像素 |
|---|---|---|---|
| 4 | `BindUniformBuffer` + `[[buffer(0)]]` | **条目编码（8 字节 GPU 地址）、`setFragmentBuffer`、MSL 从 `[[id(0)]]` 读常量** | (0,0,255,255) |

用的 MSL 就是 spirv-cross 的形状：

```
struct spvDescriptorSetBuffer0 { constant float4* tint [[id(0)]]; };
fragment float4 nvngfx_main_fs(constant spvDescriptorSetBuffer0& s0 [[buffer(0)]]) {
    return *s0.tint;
}
```

一个蓝色 float4 写在内存池 +1024 处，走客体入口
`nvnCommandBufferBindUniformBuffer(cb, 1, 0, addr, 16)`。失败码 `8192`。

### 证据（本地）

```
build_abi_test: all tests passed
  nvn_vertex_test: ok (packing, 26 formats, layout id)
  syntax_check_metal: ok
verify_ios: ok (metal bindings: 66/513)
NVN host-HLE 66/513 (12.9%)   P0 blockers open: 0/6
```

模拟器上的第四条 draw 只有 CI 能证明（本机无 Metal），结论看本轮推送后的
`ios` job。

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| 纹理 / sampler 条目 | `[[id(k)]]` 的 texture/sampler 还没填；需要纹理池 + 采样器池给出句柄 |
| `[[id(k)]]` 与 NVN 槽位的映射 | 已翻译的两份 MSL 里，id 是**按 binding 排序后的连续编号**（cbuf 在前、纹理随后各带一个 sampler）；运行时还没读 sidecar JSON 去建这张表 |
| 顶点阶段的参数缓冲区 | 与顶点流 0 号槽冲突，索引偏移约定未定 |
| 真实 shader 的 draw | 仍然是 0 条 |
| 真机 | **没有真机运行记录** |

## M3.82 采样器状态对象与 FP 参数块（本轮）

覆盖从 **66 -> 104 / 513（20.3%）**。新增的 38 个已绑定入口里，
`nvnSamplerBuilder*` 有 24 个，`nvnSampler*` 对象有 14 个。
`nvnSamplerBuilderGetDevice` 仍没有可依赖的参数形状；`nvnSamplerGetDebugID`
虽有签名，但返回值应当是什么 debug ID 尚未确定。两者都没有为了凑数硬绑。

### 默认值：照 nxnative 的真实实现，而不是猜 GL

`nvnSamplerBuilderSetDefaults` 的默认值直接对齐
`nxnative-git/dm/gfx/nvn/hle_nvn.c:546`：

```
min = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR   mag = NVN_MAG_FILTER_LINEAR
wrap s/t/r = NVN_WRAP_MODE_REPEAT
compare mode = NONE, compare func = LESS
lod_min = -1000, lod_max = 1000, max aniso = 1
border = (0,0,0,0) 透明黑
```

`SetDefaults` 会清零 builder 的 `0x60` 字节，但保留已设置的 device，
与 nxnative 的行为一致。

### 便携状态与 Metal 映射

新增 `ios/nvn/include/nvn_sampler.h` / `ios/nvn/src/nvn_sampler.c`，它不包含
任何 Apple 头文件，所以 Windows 上的 MSVC 测试和 CI 的 clang C 编译都能跑。

| NVN 输入 | 处理 |
|---|---|
| `NVNminFilter` 0..5 | 拆成 min filter + mip filter；6 以上拒绝 |
| `NVNmagFilter` 0..1 | nearest / linear |
| `NVNwrapMode` 0..7 | 映到 Metal 的四种地址模式；4（mirror-clamp-to-border）没有精确对应，标记 approximate |
| `NVNcompareMode` / `Func` | mode 1 时把 func 1..8 转成 Metal 0..7；mode NONE 仍保留原 func |
| `NVNsamplerReduction` 0..2 | 保存 average/min/max；Metal 26 起才设置 `reductionMode` |
| LOD clamp / bias / anisotropy | 保存 guest 原值；只在 `MTLSamplerDescriptor` 上夹紧并记录 approximate |
| border RGBA | 保存四个 float；Metal 只有透明黑/不透明黑/不透明白三个常量 |

关键语义是 **getter 返回客体原值**：NVN 的 `CLAMP`（0）和 `CLAMP_TO_EDGE`（7）
都会映到 Metal 的 clamp-to-edge，但 `nvnSamplerGetWrapMode` 必须分别返回 0 和 7。
LOD 的 `-1000` 也原样返回，不能把 Metal 描述符夹紧后的 0 写回状态。
`nvnSamplerCompare` 用原始 NVN 状态比较；border 按 `memcmp` 比字节，连 `-0.0`
和 `+0.0` 都区分，和 nxnative 一致。

`create_sampler_state()` 真正创建 `MTLSamplerState`，包含 filter、mip、wrap、
LOD clamp、anisotropy、compare、border 和 `supportArgumentBuffers = YES`。
`MTLSamplerReductionMode` 是 **iOS 26** 才有的 API，因此有两个门：编译期的
`__IPHONE_OS_VERSION_MAX_ALLOWED >= 260000`，以及运行期的
`@available(iOS 26.0, *)`。旧 SDK/旧系统不会引用这个符号，但 guest 的 reduction
值仍保存在对象里，getter 不受影响。

### 参数块：整数寄存器之外的 s/d 寄存器

`nvngfx_proc` 的参数从 `uint64_t x[8]` 扩展为：

```c
typedef struct nvngfx_args {
    uint64_t x[8];   /* x0..x7 / w0 返回值 */
    uint64_t f[8];   /* s0..s7：float 在低 32 位；d0..d7：完整 64 位 */
} nvngfx_args;
```

`nvnSamplerBuilderSetLodClamp` / `SetLodBias` / `SetMaxAnisotropy` 等入口接收
FP 寄存器，这半块是必需的。生成的头文件提供
`nvngfx_arg_f32()` / `nvngfx_arg_set_f32()` / `nvngfx_arg_f64()` /
`nvngfx_arg_set_f64()`；ABI 测试用 `static_assert` 固定了大小、偏移和 float 位型。
**缺口仍在仓库外**：本仓库没有真正的 guest -> host 调用桥，因此调用端必须自己把
s/d 寄存器捕获到 `f[]`。不能把“类型和 handler 已支持”说成“端到端 FP 调用已完成”。

### 自检与失败码

除原来的四条 draw 外，`nvngfx_metal_selftest()` 再跑一条 sampler 路径：

1. 用真实入口 `SetDefaults` / filter / wrap / LOD / bias / compare / border /
   anisotropy / reduction 建 builder；
2. 逐个读回，确认原始 NVN 值没有在映射中被改写；
3. `nvnSamplerInitialize()` 创建真实的 `MTLSamplerState`；
4. 从 sampler 对象读回状态，再建第二个 sampler，`nvnSamplerCompare` 必须相等。

失败码统一是 **16384**。成功时 stderr 出现：

```
[selftest] sampler state created, compared and read back
```

### 证据（本地）

```
nvn_abi_test: ok (513 procs)
core_smoke_test: ok
nvn_texture_test: ok (157 formats in table)
nvn_shader_test: ok
nvn_vertex_test: ok (packing, 26 formats, layout id)
nvn_sampler_test: ok (filters, wrap, compare, reduction, clamps)
syntax_check_metal: ok (clang++.exe)
build_abi_test: all tests passed

gen_nvn.py --check: generated artifacts up to date (513 procs, 104 implemented)
extract_nvn_signatures.py --check: ok (470 shapes, max arity 8)
verify_ios.py: ok
  metal bindings: 104/513
```

这些只能证明 Windows 上的状态逻辑、C ABI 和 Metal 源码通过类型检查。
`MTLSamplerState` 是否真的创建成功、自检 rc 是否为 0，仍要等推送后的
GitHub Actions `macos-15` 模拟器结论；**CI 尚未确认**。

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| sampler pool / texture pool | 还没有资源句柄，无法把 sampler 注册进 NVN 池 |
| `nvnCommandBufferBindSeparateSampler` / `SetSamplerPool` | 客体的采样器绑定调用仍不会影响 draw |
| argument buffer 的 texture/sampler 条目 | `[[id(k)]]` 仍只有 UBO；采样器状态对象存在，但 shader 引用不到 |
| 真实 shader 的 draw | 仍是 0 条；四条自检 draw 都用内置 MSL |
| 外部 guest 浮点寄存器桥 | `nvngfx_args.f[]` 已定义，仓库外调用端还没填 |
| 真机 | **没有真机运行记录** |

## M3.83 描述符池、句柄与采样纹理的 draw（模拟器已通过）

覆盖从 **104 -> 131 / 513（25.5%）**。新增 27 个入口：5 个句柄 getter、
7 个 texture pool、7 个 sampler pool、8 个 command buffer 绑定。

### 句柄是池索引，不是宿主指针

nxnative 的 `hle_nvn.c` 把 `NVNtextureHandle` 定义成一个打包整数：
`nvnDeviceGetTextureHandle(d, tex, samp)` 返回 `tex | samp << 20`；
`GetTexelFetchHandle` 返回 `tex`；`GetSeparateTextureHandle` / `GetImageHandle`
返回 `id`；`GetSeparateSamplerHandle` 返回 `id << 20`。低 20 位是纹理池 ID，
高 20 位是采样器池 ID，image 与 texture 共用索引空间。

**ID 是客体池内编号**，不是宿主槽位：两个池可以各自注册 id 5 而指向不同纹理。
查表时才把绑定池的 base 加上去，这也是 `SetTexturePool` / `SetSamplerPool`
必须发生在 draw 之前的原因。

### 池编号：照 nxnative 的 dpool_add

`ios/nvn/include/nvn_pool.h` / `src/nvn_pool.c` 是不含 Apple 头文件的纯算术，
Windows 上由 `tests/nvn_pool_test.cpp` 直接跑：

- 每种池最多 **16 个**（nxnative 的 `NDPOOL`）；第 17 个返回 0 并记日志，
  不会悄悄和 0 号池共用编号。
- base 按创建顺序分配，大小向上取整到 **256 项**。一个 4 项的池也占 256，
  所以下一个池的 id 0 不会撞上它。
- 同一个池再次 `Initialize` 返回原 base；`Finalize` **不回收**编号，
  因为旧 command buffer 的句柄可能还在飞。
- `nvngfx_pool_slot()` 检查 id 是否落在该池的 `size` 内，越界返回 -1。

### 注册：纹理存地址，采样器存快照

`nvnTexturePoolRegisterTexture` / `RegisterImage` 只保存客体 `NVNtexture*`
（和 view）地址，槽位键是 `base + id`。draw 解析句柄时再 `find<Tex>()`，
所以 `nvnTextureFinalize` 之后槽位自然解析失败，draw 被丢弃并记日志，
而不是悬空解引用或绑定 0 号槽。

`nvnSamplerPoolRegisterSampler` 从 sampler 对象复制状态并复用它的
`MTLSamplerState`；`RegisterSamplerBuilder` 没有 sampler 对象可查，
所以在注册时快照 builder 的状态并**当场**建 `MTLSamplerState`，
避免每帧 draw 重新分配。

### 参数缓冲区：固定 8 字节 ABI

M3.83 一度改成从 fragment function 取 `newArgumentEncoderWithBufferIndex:0`，
让 Metal 自己算布局。模拟器把这条路否掉了：

- 编码器对象**非 nil**，但 `encodedLength == 0`，offset 无从取得；
- x86_64 模拟器 `MTLDevice` 报告 `apple7=0 metal3=0 unifiedMemory=0`，
  且 `MTLTexture.gpuResourceID` **恒为 0**。参数缓冲区的纹理条目要写的正是
  这个 ID，写 0 等于绑空纹理，采样结果是透明黑，而且**不报错**。

所以现在写回的是一致的固定 ABI：`[[id(k)]]` 就是参数缓冲区第 k 个 64 位字，
buffer 写 `gpuAddress + offset`，texture / sampler 写 `gpuResourceID`；表长按
最高 id 分配（`(highestId + 1) * 8` 字节）。同一份 ABI 也是 d2r-mobile 的
翻译产物用的。

`Program::args` 仍是 `[[id(k)]]` ↔ NVN 槽位的映射表。翻译产物还没带侧车，
所以只有内置 MSL 声明了 `id(0)=常量 / id(1)=纹理 / id(2)=采样器`；
`numArgs == 0` 时保留旧规则（buffer i 就是 id(i)），这时如果有纹理被绑，
draw 会被丢弃并记 `nvnCommandBufferDraw.textureLayout`，不会拿未填的槽位去采样。

### 没有 Metal 3 时走直连绑定

`gpuResourceID == 0` 说明这台设备给不出参数缓冲区需要的资源句柄。因此
`program_pipeline()` 按 **fragment MSL 文本**选路（Apple 模拟器运行时的行为
不能作为判据）：

| fragment MSL 里出现 | 路径 | draw 前做的事 |
|---|---|---|
| `[[buffer(0)]]` | 参数缓冲区（固定 ABI） | `setFragmentBuffer:atIndex:0` |
| 只有 `[[texture(k)]]` / `[[sampler(k)]]` | `directBindings` | UBO 走 `setFragmentBuffer`，纹理走 `setFragmentTexture:atIndex:`，采样器走 `setFragmentSamplerState:atIndex:` |

直连路径不碰 `gpuResourceID`，所以模拟器也能验证池、句柄、上传和 sampler
状态；参数缓冲区路径原样保留给 Metal 3 真机。自检的 fragment MSL 因此有两份：
variant 3（`[[id(k)]]`）与 variant 4（`[[texture(0)]]` / `[[sampler(0)]]`），
自检按两个资源的真实 ID 选。

### 自检：第五条 draw

`nvngfx_metal_selftest()` 现在跑五条 draw。第五条走完整绑定路径：

1. 2x2 RGBA8 纹理经 `nvnTextureBuilder*` + `nvnTextureInitialize` 建出；
2. `upload_rgba8()` 用 staging buffer + blit 填红/绿/蓝/白四格
   （**宿主上传**，不是 guest 内存），随后回读源纹理四角，确认上传与坐标方向；
3. sampler builder 设 nearest + clamp，`RegisterSamplerBuilder` 进池 id 9；
   再从 sampler 对象 `RegisterSampler` 进 id 10，两条注册路径都跑；
4. `nvnDeviceGetTextureHandle(5, 9)` -> `BindTexture(cb, PIXEL, 0, h)`；
5. 读 `MTLTexture.gpuResourceID` / `MTLSamplerState.gpuResourceID`：两个都非 0
   才用 variant 3 的参数缓冲区，否则用 variant 4 的直连绑定；
6. 回读目标四角，必须分别等于红/绿/蓝/白（容差 2）。

UV 方向不是默认就对的：顶点着色器把 NDC 的 `-1..1` 映射成 `0..1` 之后还要
`v.uv.y = 1.0 - v.uv.y`。少了这一行，run 41033107396 的回读是
`top-left pixel is (0,0,255,255), want (255,0,0,255)`——整幅上下镜像。
Metal 的纹理原点在左上，而 NDC 的 +y 朝上。

失败码：池/纹理 16385，draw 未编码或命令缓冲错误 16387，四角逐像素不符 16388。
成功时 stderr 出现 `[selftest] textured draw sampled all four pool texels`。

### 证据（本地）

```
nvn_abi_test: ok (513 procs)
core_smoke_test: ok (nvn 0/513 on null)
nvn_texture_test: ok (157 formats in table)
nvn_shader_test: ok (seam, blob table, SHA-256, shader key)
nvn_vertex_test: ok (packing, 26 formats, layout id)
nvn_sampler_test: ok (filters, wrap, compare, reduction, clamps)
nvn_pool_test: ok (bases, slots, handles)
syntax_check_metal: ok (clang++.exe)
build_abi_test: all tests passed

gen_nvn.py --check: generated artifacts up to date (513 procs, 131 implemented)
extract_nvn_signatures.py --check: ok (470 shapes, max arity 8)
verify_ios: ok
  metal bindings: 131/513
```

这些只证明 Windows 上的池算术、C ABI 和 Metal 源码通过类型检查。

### 证据（CI，iOS 模拟器）

run `41034005513`（HEAD `67b6057`）两个 job 都 success：portable
`123990267681`、ios `123990428590`。

```
[selftest] metal device: apple7=0 metal3=0 unifiedMemory=0
[selftest] source texture readback: top-left=(255,0,0,255) top-right=(0,255,0,255) bottom-left=(0,0,255,255) bottom-right=(255,255,255,255)
[selftest] textured draw path: direct bindings (textureResourceId=0 samplerResourceId=1)
[selftest] textured draw state: pso=1 argbuf=0 hasArgs=1 encoded=1 draws=1 hasArgBuffer=0 direct=1 numArgs=0 ...
[selftest] textured draw sampled all four pool texels
BOTW_SELFTEST rc=0
release gate correctly refuses to compile
```

`textureResourceId=0 / samplerResourceId=1` 是"有没有"的布尔值，不是句柄：
模拟器给 sampler 分配了 ID，但纹理没有，所以整条路径必须走直连。
`argbuf=0` 是直连路径的正常结果，不是失败。

这证明的是：池、句柄、上传、sampler 状态、直连绑定、UV 方向与像素回读在
**模拟器**里全部正确。**没有**证明的是 Metal 3 真机上参数缓冲区的
texture/sampler 条目——那需要 `gpuResourceID != 0` 的硬件，GitHub 的 iOS
模拟器给不了。要补这块，应该加一个原生 macOS Metal 离屏测试
（`macos-15` runner 有 Apple GPU），而不是继续在模拟器里绕。

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| guest 纹理上传 | 自检的像素是宿主 blit；BOTW 放进池里的像素还读不到 |
| tiled / block-linear 布局 | 即使上传，未解块的纹理仍是花屏 |
| 真实 shader 的 `[[id(k)]]` 侧车 | M3.84 已补：真实 MSL 的参数布局已进 blob；M3.85 补 c1；真实 shader 仍不能采样，因为 bindless / guest 纹理上传还没接 |
| Metal 3 的 argument-buffer 纹理条目 | 只有 UBO 条目在 CI 里真正走过；纹理/采样器条目只在直连路径上验证过 |
| MRT / depth attachment | 只有单 color attachment，深度/模板未接 |
| 非 pixel stage 的绑定 | 顶点/几何/计算的纹理绑定只记日志 |
| `nvnTexturePoolGetMemoryPool` / `SamplerPoolGetMemoryPool` | 不在 470 个已测形状里，未绑 |
| `nvnCommandBufferBindImage(s)` | compute 图像路径未接 |
| 真实 shader 的 draw | 仍是 0 条；六条自检 draw 都用内置 MSL |
| 真机 | **没有真机运行记录** |

## M3.84 真实 shader 的 `[[id(k)]]` 侧车（本轮）

覆盖仍是 **131 / 513（25.5%）**：本轮没有新增 NVN 入口，改的是"参数从哪来"。

### 8 项上限不够：真实 shader 有 42 项

本地 `Bootup_Graphics` 的两个真实 shader（数据不入库）：

| stage | `[[id(k)]]` 成员 | 最高 id | 分类 |
|---|---|---|---|
| `d756ff62a57fc508_vs` | 7 | 6 | 3 UBO + 2 纹理 + 2 采样器 |
| `029de06ee5b5d21b_ps` | 42 | 41 | 5 UBO + 18 纹理 + 18 采样器 + 1 c1 |

`Program::args` 因此从 8 项改成 `NVNGFX_MAX_ARGS`（64）；参数表按最高 id
分配（42 项 = 336 字节），不再固定 16 个字。纹理槽位扩到 NVN 的 32 个
（c2 `0x20..0x120`），采样器 32 个。

### 名字就是映射：`gen_blobs.py` 解 `spvDescriptorSetBuffer0`

| MSL 成员名 | 含义 | 侧车 kind / slot |
|---|---|---|
| `fp_c3` / `vp_c5` | 常量段 N -> 统一缓冲槽 N−3 | `UBO` 0 / 2 |
| `fp_t_tcb_1C` | 合并纹理（c2 句柄表） | `TEXTURE` 10 |
| `fp_t_tcb_1CSmplr` | 同一个句柄里的采样器 | `SAMPLER_OF_TEXTURE` 10 |
| `fp_t_tcb_5A` | c2 `0x168` 起的独立纹理 | `SEPARATE_TEXTURE` 0 |
| `fp_t_cb2_1C` | c2 驱动表，按普通纹理解 | `TEXTURE` |
| `fp_t_cb7_2A` | 句柄写在统一缓冲里（bindless） | `UNSUPPORTED` |
| `fp_c1` | c1 常量段 | M3.84 当时记作 `UNSUPPORTED`；M3.85 改为 `C1` |
| `fp_i_*` / `fp_s3` | image / storage buffer | `UNSUPPORTED` |

`decode_tex` 与 nxnative 的 `dmsh_build.py` 同源（MIT），数字来自 §6 的驱动段
布局。生成器把布局写进 `kArgs_<key>_<stage>[]`，和 MSL 一起下发；id ≥ 64、
槽位越界或 id 重复时**构建失败**，不会产出一张运行时会拒绝的表。

两个真实 shader 一共解出 49 项：8 UBO、20 纹理、20 采样器、1 个 c1
（M3.84 当时把它记成 unsupported，M3.85 补上）。

### 运行时：MSL 与布局来自同一个 blob

`prog_set_shaders()` 算完 key 后用 `nvngfx_shader_lookup_blob()` 取回 blob，
`nvngfx_shader_copy_args()` 拷进 `Program::args`。`argument_buffer()` 按 kind 填：

- `UBO` -> `gpuAddress + off`；没绑就丢 draw（零指针会 fault，不是"可选"）
- `TEXTURE` / `SEPARATE_TEXTURE` -> `gpuResourceID`
- `SAMPLER_OF_TEXTURE` -> 同一合并句柄里的采样器
- `SEPARATE_SAMPLER` -> 独立采样器槽
- `UNSUPPORTED` -> 丢 draw，记 `nvnCommandBufferDraw.argUnsupported`

顶点阶段的布局**记录但不消费**：本后端 `buffer(0)` 是第一条顶点流，顶点参数
缓冲区要先有槽位偏移约定（§6），这是下一块工作。

### 自检：侧车路径每台设备都跑

第五条 draw 现在装一张临时 blob（key `00000000000000a5`，id **5 / 9 / 12**），
`lookup_blob` + `copy_args` 校验后打印，再恢复真正的表：

```
[selftest] blob layout: key=00000000000000a5 numArgs=3 ids=5/9/12
```

非直连设备把布局应用到程序；模拟器走直连时 `numArgs=0` 是预期的（直连不看
id）。Metal 3 真机的 fragment 原本打算用 variant 5（`[[id(5)]]`/`[[id(9)]]`/
`[[id(12)]]`）回读四角顺带证明表的步长与空洞；M3.85 发现 Metal 参数缓冲区按
声明顺序紧凑排布，`id*8` 只在 id 连续时成立，所以这个空洞布局在真机上仍未
验证（见 M3.85）。

### 证据（CI）

run `41037369546`（HEAD `09b0327`）：portable `124001430324`、ios
`124001563704`，都 success；`BOTW_SELFTEST rc=0`，release gate 仍拒绝编译。

本地 `test_gen_blobs.py`（合成 MSL，无游戏内容）钉住：合并纹理 10、独立纹理
128→0、bindless/c1/image/sbuf 的 unsupported、空洞，以及 `[[id(64)]]` 必须让
生成器失败。第一次推送（`c1b6500`）在 portable 上被 clang 的
`-Wmissing-field-initializers` 拦下——旧的三字段 blob 初始化在 MSVC 下不报，
CI 用 `-Wextra` 就报；`09b0327` 补全。

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| ~~c1 常量段~~ | M3.85 已补：侧车产出真实字节，运行时按 Program 复制并绑定；真实 shader 仍未在 Metal 里跑过 |
| bindless（句柄在统一缓冲里） | `fp_t_cb7_*` 要运行时读统一缓冲再解句柄 |
| image / typed buffer | compute 与部分像素路径 |
| 顶点阶段的参数缓冲区 | 与顶点流 0 号槽冲突，偏移约定未定 |
| guest 纹理上传 / tiled 布局 | 侧车只解决"填哪一格"，不解决"格子里是什么" |
| 真实 shader 的 draw | 仍是 0 条；c1 已解（M3.85），还缺 bindless / 顶点参数缓冲区，CI 也不含标题数据 |
| blob 表查找 | 线性表；19,592 条时每次 SetShaders 是 O(n)，量起来要换哈希 |
| 真机 | **没有真机运行记录** |

## M3.85 内嵌 c1 常量段（本轮）

覆盖仍是 **131 / 513（25.5%）**：本轮没有新增 NVN 入口。M3.84 的参数布局里
只剩 `fp_c1` 没有数据来源；这一轮把它从“unsupported”变成真正可绑定的段。

### c1 是什么：code area 尾部的一段常量

每个 stage 的 control 段给出两个数：

| 偏移 | 含义 |
|---|---|
| `control+0x700` | c1 相对 code area 起点的偏移 |
| `control+0x704` | code area 总长 |

`c1 = code[c1o:c1t]`，按 guest 原样保存。它不是命令缓冲区的绑定，而是编译进
shader 的常量段；间接跳转（BRX）的跳转表也在这里，所以不能填零。nxnative 侧
实测 BOTW 的 c1 用到 0 / 256 / 512 / 768 字节；后端把 64 KiB 作为硬上限，
防止损坏的侧车让手机分配无界缓冲。

### blob 与运行时

`nvngfx_shader_blob` 增加 `c1` / `c1_size` 两个字段，`NVNGFX_ARG_C1` 是新的
kind（值 6；`UNSUPPORTED` 顺延到 7，生成器和运行时必须一起重建）：

- `c1 == NULL && c1_size == 0`：这个 shader 没有 c1（没有布局项时必须如此）
- `c1 != NULL && c1_size == 0`：显式空 c1（MSL 声明了成员，但段长为 0）
- `c1 != NULL && c1_size > 0`：真实字节，最多 64 KiB
- 布局里有 `NVNGFX_ARG_C1` 时必须有非空 `c1` 指针；C1 的 slot 必须是 -1

`prog_apply_arg_layout()` 把字节**复制**进 `Program::c1`，所以临时 blob 的
生命周期结束后仍然有效。第一次需要 c1 的 draw 才分配 `MTLBuffer`：

- 数据向上取整到 256 字节，再加 256 字节零间隙（nxnative 的共享 bank 规则）
- 显式空 c1 也分配 512 字节零缓冲，不创建零长度 `MTLBuffer`
- 参数表写 `c1Buffer.gpuAddress`，并对 fragment 阶段 `useResource:usage:`
- 只有 c1、没有 UBO/texture/sampler 的 fragment shader 也会绑定 `buffer(0)`

### 生成器：sidecar 缺失必须是构建错误

`nx_shader.py translate` 在写 `<key>_<stage>.metal` 时检查该 stage 是否真的
声明了 `*_c1`：只有声明了才写同名 `.c1`，并删除上一轮遗留的 sidecar。于是：

| 情况 | 产物 |
|---|---|
| MSL 没有 c1 | 没有 `.c1`，blob 写 `0, 0` |
| 声明了 c1，段长为 0 | 空 `.c1`，blob 写 `kEmptyC1, 0` |
| 声明了 c1，有字节 | `.c1` 保存原始字节，blob 指向 `kC1_<key>_<stage>` |

`gen_blobs.py` 对“MSL 声明 c1 但 sidecar 缺失”、重复 blob、超过 64 KiB
都直接让构建失败；不会产出一张运行时才拒绝的表。

### 本地真 shader 验证

`Bootup_Graphics` 的两个真实 shader（数据不入库）重新生成：

```
gen_blobs: 49 argument(s): c1=1, sampler_of_texture=20, texture=20, ubo=8;
c1=1 shader(s), 256 bytes
```

真实 fragment 里的布局项是 `{2, NVNGFX_ARG_C1, -1}`，c1 共 256 字节；顶点
shader 不声明 c1，因此不再产出空的 `.c1`。

### 自检：第六条 draw

内置 variant 6 是一个**只有 c1** 的 fragment shader：
`constant float4* c1 [[id(0)]]`。第六条 draw 用临时 key
`00000000000000a6`，把 256 字节写成 `(0.25, 0.5, 0.75, 1.0)`，应用布局后
立刻把源数据填成 `0xAA`——回读仍是 `(64,128,191,255)`，证明 `Program`
拥有的是副本而不是外部指针。失败码：

- `16390`：SetShaders / blob 查找 / SetBlobs 失败
- `16391`：draw 没有编码，或命令缓冲报错
- `16392`：中心像素不是期望颜色

预期日志：

```
[selftest] c1 sidecar: key=00000000000000a6 bytes=256 id=0
[selftest] c1 buffer: gpuAddress=0x... bytes=256
[selftest] c1 draw read back its copied constants
BOTW_SELFTEST rc=0
```

### 额外发现：参数缓冲区按声明顺序排布

第一次把 c1 放在 `[[id(2)]]` 时，draw 编码成功、命令缓冲也完成，但中心像素是
`(0,0,0,0)`。原因不是 c1 没复制，而是 Metal **按成员声明顺序紧凑排布**参数
缓冲区：单个 `[[id(2)]]` 成员仍然在字节 0，`id` 只是 encoder 的绑定号，不是
字节偏移。运行时按 `id*8` 写表，所以在“只有一个 id=2 成员”的合成 shader 上写
到了偏移 16，shader 读偏移 0，得到零。

真实 BOTW fragment shader 的 id 是 0..41 连续，声明顺序与 id 顺序一致，所以
UBO/c1 路径不受影响；M3.84 的 5/9/12 空洞布局只在模拟器直连路径跑过，Metal 3
上的 raw table 仍未验证。M3.86 做 bindless 时会把 packed slot（声明顺序）也写进
blob，一并修掉带空洞的布局。

### 证据

本地门禁：`build_abi_test.ps1` 全过、`test_gen_blobs.py` 覆盖 c1 的
正常/缺失/空/超限四种情况、`verify_ios.py` 与 `syntax_check_metal.py` 通过。

第一次推送 `3d8ba8f` 的 run `41040694114`（portable job `124012453451` success）
在 iOS 自检里失败：`BOTW_SELFTEST rc=16392`，c1 draw 中心像素 `(0,0,0,0)`。
原因就是上一节的声明顺序。`ddde8f8` 把自检成员改成 `[[id(0)]]` 后，run
`41042618062`（portable job `124018857722`、ios job `124019022555`）两个 job
都 success，`BOTW_SELFTEST rc=0`：

```
[selftest] c1 sidecar: key=00000000000000a6 bytes=256 id=0
[selftest] c1 buffer: gpuAddress=0x15004b0000 bytes=256
[selftest] c1 draw read back its copied constants
```

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| bindless（句柄在统一缓冲里） | `fp_t_cb7_*` 要运行时读统一缓冲再解句柄 |
| image / typed buffer | compute 与部分像素路径 |
| 顶点阶段的参数缓冲区 | 与顶点流 0 号槽冲突，偏移约定未定 |
| guest 纹理上传 / tiled 布局 | c1 只解决常量，不解决“纹理里是什么” |
| 真实 shader 的 draw | 仍是 0 条；c1、UBO、纹理、采样器布局都已解，但还没有标题 shader 的离线 MSL 全链路 |
| blob 表查找 | 线性表；19,592 条时每次 SetShaders 是 O(n) |
| 真机 | **没有真机运行记录** |

## M3.86 packed slot 与 bindless 句柄

覆盖仍是 **131 / 513（25.5%）**：这一轮没有新增 NVN 入口，改的是参数缓冲区里
“每一项写到哪个字节”的规则，以及句柄不在命令缓冲上时的解析路径。

### id 是绑定号，packed 才是字节偏移

M3.85 的自检把单个 `[[id(2)]]` 成员写到偏移 16，shader 读到的是偏移 0。结论：
Metal 参数缓冲区按**成员声明顺序**紧凑排布，`[[id(k)]]` 只决定 encoder 的绑定
号，不是字节偏移。于是 blob 的每个参数多一个 `packed`（0..numArgs-1，来自
`spvDescriptorSetBuffer0` 里的声明顺序），运行时写 `slots[packed]`，不再写
`slots[id]`。带空洞的真实布局（自检里的 5 / 9 / 12）现在也能落到正确位置。

生成器对重复 `packed`、`id >= 64`、非法 kind 和越界槽位直接让构建失败，不会
产出运行时才被拒绝的表。

### bindless：句柄在统一缓冲里

BOTW 的 `fp_t_cb7_*` 不在命令缓冲上绑纹理：它把一个 32 位 NVN 句柄写进统一
缓冲，shader 自己读出来再解。新增三个 kind：

| kind | 读什么 | 怎么解 |
|---|---|---|
| `TEXTURE_IN_UBO` | UBO 里 `offset` 处的 u32 | 低 20 位是纹理池 ID，高 12 位是采样器池 ID |
| `SAMPLER_OF_UBO_TEXTURE` | 同一个 u32 | 高 12 位 -> 采样器池 |
| `SAMPLER_IN_UBO` | UBO 里的 u32 | 低 20 位是采样器池 ID |

`read_ubo_u32()` 直接按 pool 的字节偏移读（iOS 统一内存，不需要
`gpuAddress`）。解析不出资源就丢 draw，并记
`nvnCommandBufferDraw.bindless*`，绝不填零：填零会让 GPU 静默读到别处。

### 自检：第七条 draw

临时 blob（key `00000000000000a7`）声明 id 9 是纹理、id 12 是采样器，中间有
空洞。draw 把打包后的句柄写进 UBO，编码后回读 `b->argbuf[1]` 的前两个 8 字节
字：`word0/word1` 必须分别来自 id 9 与 12，即 `packed` 是 0/1 而不是 9/12。
失败码 `16393`。

### 证据

run `41045568006`（commit `02d47ca`，portable job `124032097565`、ios job
`124032250486`）两个 job 都 success，`BOTW_SELFTEST rc=0`：

```
[selftest] bindless table: packed 0/1 from ids 9/12, ubo=+0xA8 tex=... samp=...
```

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| Metal 3 硬件上的 raw table | 模拟器 `gpuResourceID == 0`，只验证了直连路径 |
| image / storage buffer 的 bindless | compute 与部分像素路径 |
| 顶点阶段参数缓冲区 | 见 M3.87 |

## M3.87 顶点阶段参数缓冲区

覆盖仍是 **131 / 513**。顶点和像素可以给同一个 `buffer(0)` 绑不同资源，所以
`Program` 保留两套布局（`vargs` / `args`）和两张表（`b->argbuf[0]` / `[1]`）：

- 顶点表 -> `setVertexBuffer:atIndex:0`
- 像素表 -> `setFragmentBuffer:atIndex:0`
- 顶点流从 **16** 开始（`NVNGFX_VERTEX_STREAM_BASE`），把 0 号槽让给参数缓冲区

这是 d2r-mobile 的约定，也是让两者共存的唯一办法：`buffer(0)` 只有一个。

### 自检：第八条 draw

variant 8 的顶点 shader 从 `buffer(0)` 读平移量，顶点流 0 落在 `buffer(16)`。
回读中心像素必须是平移后的颜色，同时检查 `b->argbuf[0]` 的第一个字等于
stream 的 `gpuAddress + 16`。失败码 `16394`（建表/编码）与 `16395`（像素）。

### 证据

run `41046781000`（commit `7fbf820`）成功：

```
[selftest] vertex table: buffer(0) word=0x1500430c00, stream at 16, shifted pixels read back
```

## M3.88 Tegra X1 block-linear 尺寸与解块

覆盖仍是 **131 / 513**。新增 `ios/nvn/src/nvn_texlayout.c`（纯 C，可在任何主机
上跑，portable job 会编译并执行它的测试）。

### 为什么不能按行拷

Switch 的纹理在显存里是 **block-linear**：GOB 是 64 字节 × 8 行的块，按
block height 分级，mip 起点还要按级别对齐。按 `width * 4` 逐行 memcpy 得到的
是一张斜纹图。

规则来自 nxnative（MIT）的 `dm/gfx/nvn/nvn_tex.c` 与
`dm/gfx/tex/nvn_deswizzle.c`，后者又移植自 deko3d（zlib）。接口分四层：

| 函数 | 给出什么 |
|---|---|
| `nvngfx_tex_layout()` | 整体 size、alignment、block 宽高、layers |
| `nvngfx_tex_level_gobs()` | 某一级的 GOB 数 |
| `nvngfx_tex_level_geometry()` | 一级一层的行字节、块行数、切片数 |
| `nvngfx_tex_deswizzle()` / `nvngfx_tex_swizzle()` | 互为逆运算的搬运 |

### 测试

`tests/nvn_texlayout_test.cpp`：

- 157 种格式的存储大小与 deko3d 的参考值逐个比对
- mip 偏移单调且对齐
- 随机像素 deswizzle -> swizzle 往返一致
- 明确拒绝：multisample、`LINEAR` / `LINEAR_RENDER_TARGET`、`BUFFER`

### 证据

run `41047317230`（commit `dd3035d`）成功，portable job 里
`nvn_texlayout_test: ok (block shapes, sizes, offsets, round trip, refusals)`。

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| LINEAR 存储 | 本来线性，但仍要一条单独的拷贝分支 |
| 分层 / cubemap | `MTLTextureType` 只按 rank 选 1D/2D/3D |
| BCn | Apple GPU 没有对应格式，必须离线转码或 compute 解压 |

## M3.89 从 guest 池采样纹理（本轮）

覆盖仍是 **131 / 513**。这一轮把“纹理里是什么”接上了：某个纹理第一次被 draw
采样时，后端从客体自己的内存池解块并上传。

### 上传发生在哪一刻

`prepare_textures()` 在 `ensure_encoder()` **之前**跑，遍历两个 stage 已绑定的
纹理槽（含 `BindTextures` / `BindSeparateTexture`）和布局里的 bindless 句柄，
对每个还没上传过的纹理调 `tex_upload()`。上传本身是一次 blit：

1. `nvngfx_tex_layout()` 校验布局；`offset + size` 不得越过 pool（先比较再相加，
   避免整数回绕）
2. 每个 mip 一层 shared staging buffer，`nvngfx_tex_deswizzle()` 按层填
3. blit 到私有 `MTLTexture` 的对应 level / slice
4. **所有 level 都成功**才置 `uploaded`；半个 mip 链不算可用纹理

blit 必须编码在这个 draw 自己的命令缓冲里，而且不能在 render pass 打开时编码
（Metal 拒绝嵌套编码器）。所以 `tex_upload()` 先 `end_encoder()`，上传完由
`ensure_encoder()` 重开 pass，并用记录下来的 viewport / scissor 恢复状态
（`argbuf` 表也要重建，因为 `useResource:` 是 encoder 状态）。

渲染目标永不上传：`SetRenderTargets` 把目标标记成 `rendered`，那块像素归本后端
所有，客体内存里的旧内容不能覆盖它。

### 三次失败换来的规则

| 版本 | 现象 | 结论 |
|---|---|---|
| `638a9a5` | 自检卡住，120 s 看门狗杀掉 | 在 encoder 打开时等命令缓冲 = 死锁 |
| `90a1ac3` | 仍然卡 | 光去掉 wait 不够，blit 和 render pass 抢同一个编码器 |
| `d968d14` | 仍然卡 | 只有“先上传、再开 pass”才是对的 |
| `9d8276e` | 不卡了，但 `selftest.txt` 只有 start | 进程在 draw 9 里**静默死掉** |

最后一条的根因不在 Metal：自检把 `0x6450` 当作纹理 builder 的地址，而
`nvnTextureBuilderSetDefaults()` 会 `memset` 0x80 字节**通过这个指针**。
`0x6450` 是表键，不是内存，第一条 builder 调用就写到未映射页。`simctl` 对
SIGSEGV 仍然报 `exit status: 0`，唯一的线索是“没有 rc 行”。

修复（`3178261`）：

- 自检改成真的分配一块 0x80 的 builder 后备存储
- 所有 builder `SetDefaults` 走 `guest_write()`：地址 < 64 KiB 或长度回绕时记
  一条 `nvn.guestPointer.write` 并丢掉这次调用。一个坏句柄应该是一行日志，
  不是一次崩溃
- CI 失败时打印模拟器最新的 `.ips` 崩溃报告，`launch exit status` 标明不可信

### 自检：第九条 draw

在 pool 的 offset 8192 处按 block-linear 写一张 2x2 全青 RGBA8 纹理，注册到
纹理池 6 号槽，用句柄 `(6, 9)` 绑定后绘制。中心像素必须是青色——只有“从 pool
解块并上传”这条路通了才可能。

```
[nvn] texture upload: pool=0x5400 offset=8192 levels=1 staging=16 bytes
[nvn] texture upload encoded: 1 level(s)
[selftest] guest texture: 1 level(s) deswizzled from the pool and sampled back
BOTW_SELFTEST rc=0
```

失败码 `16396`。

### 证据

run `41051685313`（commit `3178261`）：portable job `124049028971`、ios job
`124049107710` 都 success，`BOTW_SELFTEST rc=0`。

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| 一次性上传 | 客体之后改写同一块纹理不会同步；需要帧边界 / 脏页跟踪 |
| 分层 / cubemap | `MTLTextureType` 只按 rank 选，layer/slice 的复制还没接 |
| LINEAR 存储 | 还没有按行拷贝的分支 |
| BCn | Apple GPU 无对应格式，必须转码 |
| 真实 shader 的 draw | 仍是 0 条 |
| 真机 | 没有真机运行记录 |

## M3.90 离线 MSL bundle

覆盖仍是 **131 / 513**。这一轮把“翻译产物怎样在不重编 App 的前提下到达运行时”
接上了。此前只有编译进二进制的 C 表；一个标题有约 19,000 个 shader，把全部
MSL 变成 C 字面量既大又无法在设备上替换。

### 文件格式

```
<dir>/manifest.tsv
<dir>/<key>_<stage>.msl
<dir>/<key>_<stage>.c1       optional
```

`manifest.tsv` 是 tab 分隔的五列：

```
key    stage    msl-file    c1-file    args
```

- `key`：16 位十六进制 guest shader key，大小写均可。
- `stage`：`vs` / `ps`（也接受 `vertex` / `fs` / `fragment`）。
- `msl`：bundle 内的 MSL 文件名，加载时读入。
- `c1`：`-` 表示没有 c1；**空文件**表示显式空 c1，二者不能混为一谈。
- `args`：`-` 或逗号分隔的 `id:slot:packed:kind[:offset]`，与 C 表的
  `nvngfx_shader_arg` 一一对应；`kind` 是 `nvngfx_arg_kind` 的数值（1..10），
  不是枚举名。
- `#` 开头是注释。

所有条目先经过 `nvngfx_shader_set_blobs()` 的完整校验。任何一行失败都会保留
旧表不变；加载成功后安装为“生成的 C 表 + bundle”，重复加载同一目录是替换而
不是累积。API 在 `ios/nvn/include/nvn_shader.h`：

```c
int nvngfx_shader_load_bundle(const char* dir, char* why, size_t why_size);
size_t nvngfx_shader_bundle_count(void);
void nvngfx_shader_unload_bundle(void);
```

运行时默认读 `<app>/shaders`，也可以用环境变量 `NVNGFX_SHADER_DIR` 覆盖。
没有 bundle 是正常状态，只打印一行日志，不阻止 App 启动。实现在
`ios/nvn/src/nvn_shader_bundle.c`，不依赖 Metal，portable job 的
`tests/nvn_shader_bundle_test.cpp` 覆盖 manifest、合并、c1、空 c1、坏 manifest
拒绝、重复加载和 unload。

### 生成器

`scripts/shader_chain/gen_blobs.py` 新增 `--bundle-out`：

```powershell
python scripts/shader_chain/gen_blobs.py --msl build\shader-chain\msl `
  --bundle-out build\shader-chain\shaders
```

它直接生成 `manifest.tsv` + `.msl` + `.c1`，不再只有 C 表；`--out` 现在是可选的，
两者可以在同一次运行中同时生成。`test_gen_blobs.py` 验证 bundle 的文件内容、
manifest 五列、参数编码、显式空 c1，以及“必须指定至少一种输出”。

### 自检：第十条 draw

自检在运行时构造合成 control/code，令 control 长 0x880、code 长 0x40，
control+0x20 = 0x880、control+0x704 = 0x40，code 为 0..0x3F。运行时
`nvngfx_shader_key_from_guest()` 算出的 key 是
`e01e5955f6f39e29`。该 key 的两条 MSL 只存在于 `ios/nvn/shaders` 的合成
fixture 中：

```
e01e5955f6f39e29<TAB>vs<TAB>bundle-vs.msl<TAB>-<TAB>-
e01e5955f6f39e29<TAB>ps<TAB>bundle-fs.msl<TAB>-<TAB>-
```

顶点 shader 画全屏三角形，像素 shader 返回橙色 `(1.0, 0.5, 0.0, 1.0)`。
draw 10 从 bundle 查 VS/PS、检查 `Program::key` 等于 guest key、清黑、绘制并
回读中心像素。这个橙色没有任何其他自检 draw 会产生，所以通过就说明文件里的
MSL 真的进入了 GPU 管线。

失败码：`16397`（key / bundle 查找 / SetShaders 失败），`16398`
（命令缓冲或像素不对）。CI 传入 `SIMCTL_CHILD_BOTW_REQUIRE_SHADER_BUNDLE=1`；
bundle 缺失时直接 `rc=16397`，不再允许 draw 10 静默 skipped。

### 证据

run `41064580709`（commit `ae139bb`，ios job `124092690787`）成功。第一次
启动被模拟器卡住并由 150 秒看门狗重试，第二次启动的日志：

```
[selftest] draw 10: MSL loaded from the bundle drew key e01e5955f6f39e29
BOTW_SELFTEST rc=0
```

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| 真实标题 MSL | fixture 是自写合成 shader，没有用游戏数据 |
| `.metallib` | 仍是运行时 `newLibraryWithSource`，首次启动会编译 MSL |
| 真实 shader 的第一次 draw | 还缺标题资源、真实 key 与完整绑定状态的组合验证 |
| 分层 / cubemap / LINEAR / BCn | 纹理路径仍未闭环 |
| 连续纹理更新、blend/depth、fence | 未接 |
| 真机 | 没有真机运行记录 |

## M3.91 分层纹理：2D 数组端到端，cube / cube-array 层数修正

覆盖仍是 **131 / 513（25.5%）**。这一轮没有新增 NVN 入口，补的是
`nvnTextureInitialize` 与首次采样上传之间的分层语义。

### 三个会静默出错的点

1. **cube array 的层数少乘了 6。** `nvngfx_tex_layout()` 的 `shape()` 把
   `NVN_TEXTURE_TARGET_CUBEMAP_ARRAY` 当成普通的 `depth` 层数组；正确的
   存储层数是 `depth * 6`（每 cube 六面）。结果是上传只覆盖第一个 cube，
   `L.size` 也偏小。
2. **Metal 描述只看 rank。** 之前 `to_metal_texture_type(rank)` 只知道
   1D/2D/3D；2D array、cube、cube array 全部被建成普通 2D。更隐蔽的是
   `arrayLength`：Metal 的 cube 是 1 个数组项（六面隐式），cube array 是
   `depth` 个 cube，而不是总面数。上传循环又必须按总面数逐 slice 寻址，
   两个计数不能混用。
3. **暂存区被所有 slice / mip 复用。** 命令缓冲提交后 GPU 才读暂存区；
   如果每个 `destinationSlice` 都指向同一块 CPU 缓冲，所有 slice 会看到
   最后一次写入的数据。单层纹理看不出这个问题，数组纹理一定错。

### 修复

- `shape()` 对 target 9 计算 `z = depth * 6`；1D array 仍把 `height` 当层数。
- `to_metal_texture_type()` 按 target 选出 `1DArray / 2DArray / 2DMS /
  2DMSArray / Cube / CubeArray / TextureBuffer`，rank 只作 1D/2D/3D 的回退。
- 新增 `metal_array_length()`：1D array 用 `height`，2D array / MS array /
  cube array 用 `depth`，cube 固定 1。`nvn_texlayout::layers` 继续表示
  “上传要写的总 slice 数”，cube 为 6，cube array 为 `depth * 6`。
- 上传尺寸不再从原始 `width/height` 移位推算，而是由
  `nvngfx_tex_level_geometry()` 的 block row 反推：这样 1D array 的
  `height` 不会被误当图片高度，BCn/ASTC 的宽高按 block 对齐，cube array
  的 slice 数也正确。
- 暂存缓冲改成每个 `(level, layer)` 独立区间。代价是上传命令缓冲存活期间
  需要一份完整的 CPU 侧纹理副本；这是正确性优先的临时实现，后续应由
  GPU deswizzle 取代，而不是在这里省内存。

### 可移植测试

`tests/nvn_texlayout_test.cpp` 新增：

- 1D array `height=4` → 4 层，每层 2048 字节；
- cube → 6 层；
- cube array `depth=2` → 12 层；
- layer 1 / cube face 5 / cube array face 11 / 1D array layer 3 的
  swizzle-deswizzle 往返，确认层偏移不是永远落在 layer 0；
- 超出层数的 layer 被拒绝。

`tests/nvn_shader_test.cpp` 固定第十一条自检的 MSL 必须声明
`texture2d_array<float>`，并同时出现 `in.uv, 0` 与 `in.uv, 1`。

### 自检：第十一条 draw

自检在 guest pool 的 offset 12288 构造一个 2×2 RGBA8 的 2D array：

- layer 0：红色；
- layer 1：黄色；
- target 4，`size2d(2,2)`，`setDepth(2)`，1 mip，1 sample。

`tex_initialize()` 必须给出 `MTLTextureType2DArray`、`arrayLength=2`、
内部总 slice 数 2。片段着色器同时采样两层，`uv.x < 0.5` 取 layer 0，否则取
layer 1；自检回读左侧 `(255,0,0,255)`、右侧 `(255,255,0,255)`。如果描述仍是
普通 2D，或两个 slice 复用了暂存区，这个检查都会失败。

失败码：`16399`（布局 / 建纹理 / 命令缓冲 / 上传），`16400`（像素不对）。

### 本地证据

```text
nvn_texlayout_test: ok (block shapes, sizes, offsets, round trip, refusals)
nvn_shader_test: ok (seam, blob table, packed slots, bindless, c1, SHA-256, shader key, layered fixture)
syntax_check_metal: ok (clang++.exe)
build_abi_test: all tests passed
```

### CI 证据

run [41070475247](https://github.com/SavitarXiangzi/botw-iPhone/actions/runs/41070475247)，
portable job 124112907657、iOS job 124113134061 均通过。模拟器日志：

```text
[nvn] texture upload: pool=0x5400 offset=12288 levels=1 layers=2 staging=32 bytes (largest slice=16)
[selftest] draw 11: 2D array layers 0/1 sampled from guest memory
BOTW_SELFTEST rc=0
```

`layers=2` 说明上传确实按两个 slice 走，`staging=32` 是两个 16 字节 slice 的和
（旧实现共用一块 16 字节暂存区时这里是 16，且回读两边同色）。

## M3.92 cube / cube-array 的真实 draw 自检

覆盖仍是 **131 / 513（25.5%）**。M3.91 只把 cube 与 cube-array 的**布局和
Metal 描述**修对了，端到端只有 2D array 跑过；这一轮补上最后两种层形状的
真实上传 → 采样 → 回读。

### 第十二条 draw：cube

- guest pool offset 13312 放一个 2×2 RGBA8 cube，6 个 slice 共 96 字节；
- 面序按 NVN：+X, -X, +Y, -Y, +Z, -Z。面 4 红、面 5 黄、其余绿；
- 描述必须是 `MTLTextureTypeCube`、`arrayLength = 1`，而
  `nvn_texlayout::layers = 6`——两个计数不同正是上一轮修的点；
- 片段着色器沿 `(0,0,1)` 与 `(0,0,-1)` 各采一次，左半屏放前者、右半屏放后者；
- 回读左红右黄。若 face 5 没被上传，右边是绿色，检查立刻失败。

### 第十三条 draw：cube array

- guest pool offset 13824，`depth = 2` 的两层 cube，12 个 slice 共 192 字节；
- 红放在 slice 4（cube 0 的 +Z），黄放在 slice 11（cube 1 的 -Z），其余绿；
- Metal 侧必须是 `MTLTextureTypeCubeArray`、`arrayLength = 2`、
  `layers = 12`；
- 着色器 `texturecube_array` 从 layer 0 沿 +Z、从 layer 1 沿 -Z 采样，
  回读左红右黄。

`destinationSlice:11` 是这里最关键的一条：如果实现把 `arrayLength` 当成
“要上传的 slice 数”，循环只走到 1 或 5，slice 11 永远不会被写；反过来把
12 传给 Metal 的 `arrayLength` 会直接建纹理失败。

### 可移植测试

`tests/nvn_shader_test.cpp` 新增两个 fixture：variant 10 必须声明
`texturecube<float>` 并出现两个 ±Z 方向；variant 11 必须声明
`texturecube_array<float>`，且采样语句带 layer `0` / `1`。两者都要求
顶点阶段与 variant 4 同一个 `kVsUv`。

### CI 上连翻四次车，每次都被自检指出来

run 41072154244 的 iOS job 报：

```text
[selftest] draw 12: guest cube layout failed (layers=6 size=3072)
BOTW_SELFTEST rc=16401
```

`layers` 是对的，错在我给自检写的 `size <= 1024` 预算。`nvngfx_tex_layout()`
把每层对齐到 block 边界，2×2 RGBA8 的一层仍是 **512 字节**，cube 6 层 =
3072，cube array 12 层 = 6144；原来的 16 KiB guest pool 也装不下
12288 + 3072 与 13824 + 6144 两组纹理。于是：

- 预算检查改成 `size == 512 * layers`；
- 两个 cube 自检改用**第二块 16 KiB 页**和独立的 NVN memory pool（`0x7300`），
  cube 在 offset 0、cube array 在 offset 4096；
- `tests/nvn_texlayout_test.cpp` 加一条 2×2 的可移植断言
  （`layer_size = 512`、cube `size = 3072`、cube array `size = 6144`），
  免得再靠模拟器才发现。

第一版改法是把原来的 pool 直接扩到 64 KiB。下一次 run 41075224028 里 App
在 draw 1 之后、建完这块 pool 的下一帧就退出了（两次 attempt 都一样，
没有崩溃报告，`selftest.txt` 只剩 `BOTW_SELFTEST start`），原因没查清。
`newBufferWithBytesNoCopy` 对 64 KiB 应该没有意见，但既有的 16 KiB 页是
**跑通过**的尺寸，于是退回“多一块小页”，而不是在同一块页上赌。

换成第二块页之后，run 41077027673 死在 draw 11 与 12 之间：

```text
[selftest] draw 11: 2D array layers 0/1 sampled from guest memory
（然后进程消失，没有 rc 行）
```

原因很土：`unsigned char* vpool12` 在 `std::malloc` **之前**就从
`poolRaw2` 算出来了，那时它还是 `nullptr`，于是 `memset(nullptr, 0, 16384)`
直接写空指针。把指针计算挪到 malloc 之后就好。这类错误在 Windows 上
一条都测不出来——自检才是唯一的执行环境。

接着 run 41079561654：draw 12 一次通过（`cube faces 4/5 sampled from guest
memory`），draw 13 在建 `MTLTextureTypeCubeArray` 时把模拟器带崩：
`@try/@catch` 没抓到（说明不是 NSException），也没有崩溃报告，最后一条 trace
停在 `draw 13 guest bytes ready`。因此第十三条改成 opt-in：

```text
[selftest] draw 13 skipped: MTLTextureTypeCubeArray aborts the iOS Simulator's
Metal (layout and shader are covered by the portable tests); set
BOTW_SELFTEST_CUBE_ARRAY=1 on a device to run it
```

### 本地证据

```text
nvn_texlayout_test: ok (block shapes, sizes, offsets, round trip, refusals)
nvn_shader_test: ok (seam, blob table, packed slots, bindless, c1, SHA-256, shader key, layered/cube fixtures)
syntax_check_metal: ok (clang++.exe)
build_abi_test: all tests passed
verify_ios: ok
```

### CI 证据

run [41080975529](https://github.com/SavitarXiangzi/botw-iPhone/actions/runs/41080975529)，
portable job 124149684772、iOS job 124149867094 均通过。模拟器日志：

```text
[nvn] texture upload: pool=0x7300 offset=0 levels=1 layers=6 staging=96 bytes (largest slice=16)
[selftest] draw 12: cube faces 4/5 sampled from guest memory
[selftest] draw 13 skipped: MTLTextureTypeCubeArray aborts the iOS Simulator's Metal ...
BOTW_SELFTEST rc=0
```

`layers=6` 是 cube 的六个面，`staging=96` 是 6 × 16 字节；`pool=0x7300` 是这一轮
新加的第二块 16 KiB 页。第十三条在真机（或支持 CubeArray 的宿主）上设
`BOTW_SELFTEST_CUBE_ARRAY=1` 才会执行，届时期望的日志是
`draw 13: cube array layer 0 face 4 and layer 1 face 5 sampled from guest memory`。

### 仍然没有的

| 缺口 | 后果 |
|---|---|
| 连续更新 | 首次采样后 guest 再改内存不会重传 |
| BCn 转码 | Apple GPU 无对应格式时仍无法建立可用纹理 |
| 暂存内存 | 分层纹理需要完整 CPU 副本，大纹理可能超出 iOS 内存预算 |
| 真机 | 没有真机运行记录 |

## M3.93 LINEAR 存储：布局、mip 偏移与逐行拷贝

覆盖仍是 **131 / 513（25.5%）**。这一轮补的是纹理路径里被直接拒绝的那一类：
`NVNtextureFlags_LINEAR` / `LINEAR_RENDER_TARGET`（`flags & 0x50`）。

### 三个错

1. **尺寸只算了一层一级。** 旧代码 `size = stride * height`：mip 链的其余级
   和数组的其余层都没有算进去，`nvnTextureGetStorageSize` 会偏小，按它开池
   的客体就会越界（和 M3.16 是同一类错误，只是换了个 flag）。
2. **两级 pitch 混用。** 线性纹理的 guest 存储行距来自
   `nvnTextureBuilderSetStride`，而调用方的缓冲区行距是另一回事。拷贝必须
   同时知道两者，否则“去 padding”变成“读错行”。
3. **`nvngfx_tex_level_geometry()` 直接 `return -1`。** 上传路径因此整条
   跳过线性纹理，`tex_upload()` 只留下一条 `deswizzle` 的日志。

### 实现

- `linear_pitch()`：`stride > 0` 用 guest 给的值，否则按 32 字节（线性渲染
  目标 128 字节）对齐一行；`stride` 比一行还窄时 `nvngfx_tex_layout()` 返回 -1。
- 布局：一级之后按对齐接下一级，`layer_size` 是整条 mip 链，
  `size = layer_size * layers`（1D array 的 `height` 仍是层数）。
- `nvngfx_tex_level_offset()` 对线性也给出真实偏移（0 / 128 / 192 …），
  不再恒为 0。
- `nvngfx_tex_level_geometry()` 接受线性纹理，`tile_h/tile_d` 报 0。
- 拷贝（`nvngfx_tex_swizzle` / `deswizzle`）走一条逐行 `memcpy`：源行距是
  存储 pitch，目标行距是调用方 pitch，padding 既不读也不写。

### 可移植测试

`tests/nvn_texlayout_test.cpp` 新增 `test_linear()`：

- 4×4 RGBA8、`stride = 32`：`layer_size = 128`；
- 三级 mip：`layer_size = 224`，级偏移 0 / 128 / 192；
- 2D array 两层：`size = 256`（旧代码会报 128）；
- `stride = 16`（正好一行）可用，`stride = 8` 被拒；
- 往返：把带 `0xEE` padding 的存储解成紧凑图像、再写回去，逐行比对，
  并确认 padding 一个字节都没被写过。

```
nvn_texlayout_test: ok (block shapes, sizes, offsets, round trip, linear, refusals)
```

### 第十四条 draw：LINEAR 端到端

自检在第二块 pool 页的 offset 12288 放一张 2×2 RGBA8 的 LINEAR 纹理：

- `flags = 0x10`（LINEAR），`stride = 32`，一行只有 8 字节有效数据；
- 上排红 / 绿，下排蓝 / 白，其余 24 字节是 0；
- 片段着色器复用 variant 4（`texture2d` + `sampler`），回读左上、左下两个角；
- 期望左上红、左下蓝。

这条抓的是“把 stride 当行宽”：若拷贝按 8 字节连续读，第二行会读到 padding，
左下变黑；若把 padding 也上传成一行，右下会串色。失败码 `16405`（布局 / 建纹理 /
编码）、`16406`（像素不对）。

### CI 证据

run [41083158406](https://github.com/SavitarXiangzi/botw-iPhone/actions/runs/41083158406)，
portable job 124157289034、iOS job 124157532695 均通过：

```text
[nvn] texture upload: pool=0x7300 offset=12288 levels=1 layers=1 staging=16 bytes (largest slice=16)
[selftest] draw 14: LINEAR texture rows sampled from guest memory at a 32-byte stride
BOTW_SELFTEST rc=0
```

`staging=16` 是两行 × 8 字节——padding 没有进暂存区，正是 stride 被正确跨过。

### 还没有的
- 每级独立 pitch（有的标题会给 mip 小级换更小的 pitch）没有建模，现在一条
  链共用一个 pitch。
- 3D 线性纹理、线性渲染目标（`0x40`）都只有布局算术，没有真实使用场景。

## M3.94 BCn：首次采样时解成 RGBA8

覆盖仍是 **131 / 513（25.5%）**。这一轮补的是纹理格式表里已经映射、但 iOS
Metal 根本建不出来的 BCn 家族。它不是入口点，所以覆盖率不会变；变的是
`nvnTextureInitialize` 不会再向 iOS 设备要一个不存在的 `MTLPixelFormatBC*`。

### 范围

解码器支持 **BC1 / BC2(DXT3) / BC3(DXT5) / BC4U / BC5U** 的无符号变体：
NVNformat `0x41/0x42/0x43/0x44/0x45/0x46/0x47/0x48/0x49/0x4B`。
其中 `0x45–0x48` 是 sRGB；解码后的像素保持 sRGB 码值，目标纹理用
`RGBA8Unorm_sRGB`，不会在 CPU 端先做一次错误的线性化。

明确拒绝而不是猜的：BC4S/BC5S（`0x4A/0x4C`）和 BPTC（BC6H/BC7，`0x4D–0x50`）。
这些格式返回“不支持”，纹理上传失败，draw 被丢弃；不会把有符号或 HDR 数据当成
无符号 RGB 画出一张颜色错误的图。

### 实现

- `ios/nvn/src/nvn_bcn.c` 是纯 C、无 Metal，负责单块解码和整级/整 Z slice 解码。
  `nvngfx_bcn_decode_image()` 接受带 padding 的源/目标 pitch，并把边缘 4×4
  block 裁到真实的 `width × height`；这正是 BCn 纹理尺寸不必是 4 的倍数时
  会走到的路径。
- `nvnTextureInitialize` 在查 `nvngfx_format_to_mtl()` 之前拦截 BCn：
  `MTLTexture` 建成 `RGBA8Unorm` 或 `RGBA8Unorm_sRGB`，尺寸仍是 guest 的
  真实宽高，mip 数不变。
- `tex_upload` 不再把压缩字节直接 blit 到目标。它先按原布局把压缩行
  deswizzle 到一小块 CPU scratch，再用 `nvngfx_bcn_decode_image()` 解成
  RGBA8 暂存区；暂存区按 `4 × width × height × slices` 计费，然后才 blit。
  分层/cube 仍逐层走同一条路径，3D 按 Z slice 解码。
- 这不是免费的：BC1 原本每纹素 0.5 字节，RGBA8 是 4 字节，显存放大 8 倍；
  BC4 放大 4 倍，BC5 放大 4 倍。当前优先正确性，后续应由 ASTC 离线转码或
  compute 解压取代。

### 可移植测试

`tests/nvn_bcn_test.cpp` 用手算的 S3TC/RGTC 期望值覆盖：

- BC1 四色模式、三色+透明模式，以及 RGB_DXT1 强制不透明；
- BC2 的 4-bit alpha nibble 顺序；
- BC3 的 3-bit alpha 索引跨 32 位边界；
- BC4U 的六值/八值模式，BC5U 的红绿两平面；
- 整级解码：2 个 block、6×3 裁边、源/目标 pitch 都带 padding，确认 padding
  不被写；窄 pitch、空指针、不支持格式全部拒绝。

```text
nvn_bcn_test: ok (BC1 4/3-colour, BC2 alpha, BC3 alpha, BC4/BC5, image clipping, refusals)
syntax_check_metal: ok (clang++.exe)
build_abi_test: all tests passed
verify_ios: ok
```

MSVC 的 `/W4 /WX`、Linux clang 的 `-Wall -Wextra -Werror` 都跑过这条测试；
CI 的 portable job 也把 `nvn_bcn.c` 编译进每个测试的公共链接集合。

### 第十五条 draw：BC1 端到端

自检在第二块 pool 页的 offset 14336 放一个 4×4 BC1 块：

- `format = 0x42`（RGBA_DXT1）；
- 端点 `c0 = 0xF800`（红）、`c1 = 0x001F`（蓝），每行索引 `0,0,1,1`；
- 因此左半红、右半蓝。

`tex_initialize` 必须报 `textureType == 2D`、`pixelFormat == RGBA8Unorm`，
并且 `transcoded == true`；片段着色器仍用 variant 4 直接采样，回读左侧红、
右侧蓝。若把 BC1 的 8 字节块当 RGBA8 直接拷，或把 `MTLTexture` 建成 BC1，
这条会失败。失败码：`16407`（布局 / 建纹理 / 编码），`16408`（像素不对）。

### CI 证据

run [41086648054](https://github.com/SavitarXiangzi/botw-iPhone/actions/runs/41086648054)，
portable job 124169430691、iOS job 124169655661 均通过。下面的日志是当前源码的
预期输出：该 run 的二进制还会把 sRGB 标志拼在格式名后面（打印成 `RGBA80`），
收尾时只清理了格式字符串，没有改变解码或回读逻辑。

```text
[nvn] texture upload: pool=0x7300 offset=14336 levels=1 layers=1 staging=64 bytes (largest slice=64)
[nvn] texture upload transcode: NVNformat 0x42 BCn -> RGBA8
[selftest] draw 15: BC1 texture decoded to RGBA8 and sampled from guest memory
BOTW_SELFTEST rc=0
```

`staging=64` 是 4×4×4 字节的 RGBA8，而不是 BC1 的 8 字节 guest 块；
这就是解块确实发生在 blit 之前的一个可观察证据。

### 还没有的

- BC6H/BC7 和 signed BC4/BC5；
- guest 在首次上传后继续修改纹理内存时不会重传；
- CPU scratch + 完整 RGBA8 staging 的内存开销还很高，尤其是大 BC1 纹理；
- 没有真机记录。

## M3.95 显式纹理更新：WriteTexels / FlushTexels 触发重传

覆盖从 **131/513** 提升到 **134/513（26.1%）**，Texture 域从 **1/45** 到
**4/45**。新增的是 `nvnTextureWriteTexels`、`nvnTextureWriteTexelsStrided`
和 `nvnTextureFlushTexels`。这一轮没有碰 SASS 翻译器；它解决的是“纹理第一次
上传后，guest 明确要求改写或刷新时，Metal 副本如何再次同步”。

### 单区域读改写

`ios/nvn/src/nvn_texlayout.c` 新增：

```c
int nvngfx_tex_write_region(
    const nvngfx_texdesc* d, unsigned level, unsigned layer,
    int x, int y, int z, int width, int height,
    const void* src, size_t src_row, size_t src_image,
    void* storage);
```

它处理一个 mip / layer / slice 内的子区域：

- 先把整级从 guest 的 block-linear 或 LINEAR 布局 deswizzle 到临时行主序图像；
- 只覆盖目标矩形/体素块，区域外字节原样保留；
- 再按 guest 布局 swizzle 回去；LINEAR 的行尾 padding 不读也不写；
- 2D 的 region depth 映射到数组层，3D 的 `z` 映射到 slice；
- BCn 的起点和尺寸必须按 4×4 块对齐，否则拒绝；
- 拒绝负坐标、越界、窄源行距、未知格式或空 geometry。

这是正确性优先的实现：每个区域都要分配一份整级临时图像，**不是最终性能
方案**。大 mip 的 CPU 内存峰值和拷贝量都会明显高于只改几十字节。

### Metal 后端

`Tex` 增加 `dirty` 标志：

- `tex_upload()` 对已上传但标脏的纹理重新执行整条 mip 链上传；
- `WriteTexels` 支持 `NVNtextureView` 的 base level / min layer；
- view 若改变 format 或 target 则拒绝；swizzle 与 depth-stencil 模式目前忽略；
- `FlushTexels` 没有脏页信息，保守地把整张纹理标脏，下次采样重传；
- host-rendered 渲染目标不会从 guest 重新上传，避免宿主渲染结果被覆盖。

`Texture` 域现在是 4/45：initialize、两个 WriteTexels 入口和 FlushTexels。
`NVNcopyRegion` / `NVNtextureView` 的结构布局来自 nxnative 的实测声明，不是
Nintendo SDK 头文件；因此当前只使用已验证的 level/layer 和一致性检查。

### 可移植测试

`tests/nvn_texlayout_test.cpp` 新增 `test_write_region()`：

- block-linear RGBA8 只改内部 2×2，四周边界逐像素不变；
- LINEAR `stride = 32` 时只改有效字节，行尾 `0xEE` padding 保持；
- BC1 只允许整块写入，块内起点或越界尾端被拒；
- 3D 的 `z=1` 命中第二 slice，slice 0 不变；3D 不接受数组层 1；
- 2D 不接受 z slice 1；空指针、非法 mip、窄源行距被拒。

```text
nvn_texlayout_test: ok (block shapes, sizes, offsets, round trip, linear, write region, refusals)
build_abi_test: all tests passed
verify_ios: ok (metal bindings: 134/513)
```

### 第十六条 draw：连续更新端到端

自检创建一张 2×2 RGBA8 guest 纹理，初始四象限为红/绿/蓝/白，先完成一次
采样回读；随后：

1. 调用 `nvnTextureWriteTexels` 改成黄/青/品红/黑，重画并回读；
2. 直接改写 guest storage，再调用 `nvnTextureFlushTexels` 改成
   棕/钢蓝/橙/天蓝，第三次重画并回读。

每轮还检查纹理已重新上传、`dirty` 已清掉。失败码：

- `16409`：初始布局、建纹理或编码失败；
- `16410`：WriteTexels 后的重传或像素错误；
- `16411`：FlushTexels 后的重传或像素错误。

成功日志：

```text
[selftest] draw 16: WriteTexels and FlushTexels both re-uploaded a sampled texture
```

自检现在列出 **16 条** draw，模拟器实际执行 **15 条**；第十三条 cube array
仍需 `BOTW_SELFTEST_CUBE_ARRAY=1`，默认跳过。

### CI 证据

run [41090664172](https://github.com/SavitarXiangzi/botw-iPhone/actions/runs/41090664172)：

- portable job `124183374680`：success；
- iOS job `124183554875`：success；
- iOS 的 “Run the replay-layer self-test in the simulator” 步骤 success。

工作流要求 `BOTW_SELFTEST rc=0` 才会通过。当前账号没有仓库 admin 权限，
REST 下载原始 job 日志返回 **403**，所以这里只记录 job/step 状态，不伪造
逐行日志。源码中的成功字符串和失败码如上。

### 还没有的

| 缺口 | 后果 |
|---|---|
| 通用 texture view | 采样时仍没有完整应用 view 的格式/目标/层级语义；WriteTexels 只用 level/layer |
| 任意 mapped write 的脏页跟踪 | guest 绕过 WriteTexels/FlushTexels 直接改 pool 字节时不会自动重传 |
| 精确 FlushTexels | 没有脏页表，整条 mip 链重传，更新一个 texel 也可能付出整张纹理的拷贝 |
| RMW 内存 | WriteTexels 需要整级临时线性图像，大纹理会抬高 CPU 内存峰值 |
| 真机 | 仍只有 CI 的 iOS 模拟器记录，没有 iPhone/iPad 真机帧率与内存数据 |

### M3.96 私有 harness：真实 guest draw 打通（并撤回 M3.65–M3.71 的一个结论）

本轮工作全部在 Windows 侧的发现装置（`C:\botw-work\tools`，Unicorn + Python）
里完成，不进本仓库；iOS 侧的 NVN/Metal 实现不受影响。写在这里是因为它**推翻了
文档里的一条结论**，而且给出了真实 guest 的 draw/present 证据。

#### 1. `draws=0` 的第二个结构性原因：入口命名

M3.21 修的是"独立式 `nvnDrawArrays` 一族没实现"。这次发现 1.6.0 真正解析到的是
**command-buffer 风格**入口（cb 作为第一个参数）：

```text
nvnCommandBufferDrawArrays / DrawElements / DrawArraysInstanced /
DrawElementsInstanced / DrawElementsBaseVertex / DrawArraysIndirect /
DrawElementsIndirect / DispatchCompute / DispatchComputeIndirect
```

`registry.json`（从标题自己的 getproc 字符串提取）里本来就有这些名字，iOS 侧
M3.21 起也已经实现；**只有 harness 的 `m3_nvn.py` 漏了这一族**，所以那套装置的
`DRAWS[0]` 一直是 0，而游戏其实每帧都在逐 draw 设置状态
（`nvnCommandBufferBindUniformBuffer` 9 万次、`nvnSamplerCompare` 9.1 万次）。

补齐后第一次抓到真实 draw：

```text
first draw: op=DRAW_IDX cb=0x701F1C88 mode=4(TRIANGLES) type=1(UNSIGNED_SHORT)
            count=3 indexBuf=0x141C22000 offset=0
            caller=main+0x1D2BA6C  frame#8
```

#### 2. Yaz0 的双缓冲源（并撤回 M3.65–M3.71 的"58.2 MiB"结论）

反汇编 `main+0x191F770` 可见第 `tok` 块的实际位置是：

```text
src + ((tok - 1) & 1) * stride
```

即压缩源在 guest 内存里是**两块交替缓冲**，不是逻辑连续的。旧实现按
`src + 绝对偏移` 读，从 `tok >= 3` 起就读错数据，解压到约 55% 后卡死，并在
LightEvent 上表现为死锁。

改正后 `Font_JP.sbfarc` / `agl_nin_pack` / `Bootup_Graphics` 全部 `done=1`；
回归 `_test_yazdouble.py` 用"整包喂 / 连续分块喂 / 双缓冲交替喂"三种方式，
输出与参考实现逐字节一致。

**结论撤回**：M3.65–M3.71 把"58.2 MiB 分配返回 0"当成真正的卡点（`57352 * 0x428 + 8`），
那是**解压源读错导致的次生现象**——解压出的资源长度/内容本身就是错的。
当时的 `yaz1_A83000.out` 与 `blk.bin` 不可再用。

#### 3. `0x414E01BC` 的 O(n²) 去重表

初始化期会逐条构建 0x30 字节记录表，每插入一条都对已有记录做 `memcmp` 线性扫描：

```text
0x414E01AC  cbz  x21, 0x414E040C      ; 第 0 条直接追加
0x414E01BC  loop: memcmp(tab[x21], tab[x22], 0x30)
0x414E01D0  cbz  w0, 0x414E01E4       ; 前缀命中 -> 子记录深度比较
0x414E01D4  x22++; cmp x22,x21; b.lo loop
0x414E01E4  ...深度比较...; 失败回 0x414E01D4 继续扫描
0x414E0400  ptr[x21] = ptr[x22]
0x414E040C  追加新记录
```

表长涨到 1.4 万条时单线程被拖死（300 秒只推进 7,700 条）。宿主在循环头接管：
维护"记录 → 首个下标"索引，**命中就跳回 `0x414E01E4`**，让 guest 自己完成子记录
深度比较（失败时 guest 回循环头，我们按 `x22` 续扫）；未命中跳 `0x414E040C`
走 guest 的追加路径。语义与原始循环逐指令等价，等价性用两种方式对拍：

* 采样真值线性扫描：79 次比对，**0 失配**；
* 周期整表重建比对：15 次，**0 漂移**（说明记录插入后不会被改写，这是 dict 首匹配等价的前提）。

表在 ~70 秒内走完，不再是瓶颈。

#### 4. 记录层修正（harness）

* `emit()` 写 guest 命令内存前按 4 KiB 对齐补映射——pool storage 是宿主 `alloc`
  出来的，Unicorn 里未必已映射，未对齐的 `mem_map` 会失败并被误记成"命令内存满"；
* 段满时把记录**镜像到宿主帧表**，分析数据不再缺帧；
* `EndRecording` 显式指定目标 cb——BOTW 会交织录制多个 command buffer
  （Begin A → Begin B → End A），按"当前 cb"记会把 END 记到别人头上。

#### 5. 证据：660 秒严格门禁

```text
M3: 660.0s  frames=8329 (13 fps)  status=budget
    objects: 3974 buffer / 2626 program / 450 texture / 418 pool / 366 sampler
    draw=701  overflow=236 (full=236 write=0, max ops/frame=23)
    presents=301
    first draw: op=13 vals=[cb,mode=4,type=1,count=3,indexBuf,0,0,1]
                caller=0x41D2BA6C main+0x1D2BA6C
RESULT: PASS
```

门禁 `m3_verify.py` 现在安装 `m2_yaz0`（真实解压路径）与可选的 `m2_dedup`，
`BOTW_M3_STRICT=1` 时要求 `DRAWS[0] > 0` 且 `PRESENTS[0] > 0`。

#### 还没有的

| 缺口 | 后果 |
|---|---|
| harness 侧 `nvnCommandBufferBind*` 仍是 no-op | 进 `NVN_MISS`、不产生记录；回放层还原管线还缺这些状态 |
| 段满 236 次只镜像到宿主帧表 | guest 命令内存里没有这些记录，真机语义下应触发 memory callback 扩容 |
| 去重加速默认关闭（`BOTW_FAST_DEDUP=1` 才开） | 未默认启用，避免未验证路径影响回归 |

## M4 固定功能状态对象与命令句柄（进行中）

M3.96 的严格门禁已经把真实的 guest draw 打到 701 draw / 301 present（660 s），
随后一次 700 s 复跑给出 529 draw / 228 present、api_entries=98。这些数字只证明
"游戏确实在提交绘制"。要让回放层画对，还缺两样当时完全不存在的东西：

* **固定功能状态**：blend / color / channel mask / multisample / polygon /
  depth-stencil 六类状态对象，以及 viewport、scissor、深度范围、清屏参数；
* **命令句柄**：`nvnCommandBufferEndRecording` 的返回值，以及
  `nvnCommandBufferCallCommands` 要展开的私有命令段。

这一轮把这两块从空实现变成"回放层可以直接消费的记录"，iOS 侧同步落地了状态对象
的可移植打包，Metal 入口从 134 增到 195（+61）。

### 1. 状态 setter 与 bind 从 `_nvn_miss` 里捞出来

上一轮 700 s 复跑的日志把"落到空实现"的入口按次数排了序：

```text
nvnVertexAttribStateSetDefaults              15944
nvnSamplerCompare                            14041
nvnVertexAttribStateSetFormat                12704
nvnVertexAttribStateSetStreamIndex           12704
nvnVertexStreamStateSetDefaults              10813
nvnChannelMaskStateSetChannelMask             8824
nvnColorStateSetBlendEnable                   8592
nvnBlendStateSetBlendTarget                   8592
nvnBlendStateSetBlendFunc                     8592
nvnBlendStateSetBlendEquation                 8592
```

同一份日志里 `nvnCommandBufferBindBlendState` 被调用 **7231 次**，一条记录都没
产生。这一轮补的是六类状态对象的全部 setter、6 个 `Bind*State`，以及
viewport / scissor / 深度范围 / 混合色 / 清屏的记录入口；它们分别写进
`OP_BIND_STATE` / `OP_SET_VIEWPORT` / `OP_SET_SCISSOR` / `OP_CLEAR_COLOR` /
`OP_CLEAR_DS` / `OP_SET_DEPTH_RANGE` / `OP_SET_BLEND_COLOR`。

### 2. 状态对象就地打包，不建地址侧表

NVN 的状态对象本身就是 guest 内存里的一个小 blob：`nvn*StateSet*` 往里写位域，
`nvnCommandBufferBind*State` 只是把指针交给命令流。所以记录层按 D2R
`dm/gfx/nvn/nvn_obj.h` 的实测布局，就地读、就地写：

| 对象 | 大小 | 位域 |
|---|---:|---|
| Blend | 8 B | `target:3, src_rgb:7, dst_rgb:7, src_a:7, dst_a:7, eq_rgb:3, eq_a:3` |
| Color | 4 B | `blend_en:8, logic_op:4, alpha_func:4` |
| ChannelMask | 4 B | 每个 target 4 个写使能位 |
| Multisample | 10 B | `enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid`（一字节一个字段） |
| Polygon | 4 B | `cull:2, front:1, mode:2, offset_en:3` |
| DepthStencil | 8 B | `dt:1, dw:1, df:4, st:1` + 六个 4 位 stencil 域 |

bind 当刻就把 4 / 8 / 10 字节快照写进记录。这一点不能省：游戏会复用同一个状态
对象，若按"记录末尾再读指针"的做法，前面所有 draw 都会看到最后一次 `Set*` 之后
的值，回放出来就是错的。

### 3. depth 与 polygon 的 kind 写反了

旧记录层把两个 bind 的类型编号对调：

```text
BindDepthStencilState  kind=4 / 4 B    （4 B 是 polygon）
BindPolygonState       kind=5 / 8 B    （8 B 是 depth-stencil）
```

现在 `kind 4 = polygon (4 B)`、`kind 5 = depthStencil (8 B)`。`m3_verify.py`
的严格检查按同一张表校验快照长度，写错一位就判失败——这条错误是被门禁而不是被
肉眼发现的。

### 4. 命令句柄是真的句柄

```text
nvnCommandBufferEndRecording(cb) -> NVNcommandHandle
```

M3.96 之前这个入口返回 void，游戏存下来的"句柄"永远是 0。现在：

* 句柄是 guest 内存里一块 0x40 字节零页地址（1 MiB arena，16384 个），满足
  "唯一且可解引用"；arena 用完时 `HANDLE_FAIL` 计数，不静默返回 0；
* 句柄里存的是**命令体**——去掉外层 `BEGIN` / `END`，因为那两条是 command buffer
  的边界，不是被调用的命令；
* `CallCommands` / `CopyCommands` 在 guest 命令内存里只留一条 `OP_CALL` /
  `OP_COPY`（这才是真机语义），展开只发生在宿主帧表里，带深度上限 6 与 `seen`
  环检测；
* `nvnDeviceFinalizeCommandHandle` 释放句柄。

### 5. 私有命令段：`overflow=0`

M3.96 里 guest 给的命令内存不够用时，我们只把记录镜像到宿主帧表，报告
`overflow=236`。现在每个 command buffer 拿到自己的一块 0x40000 字节段（按需扩大、
跨帧复用），游戏给的 slab 只作证据、不再往里写。游戏从不读命令内存，它看到的
`GetCommandMemorySize/Used` 也是我们答的，所以这条替换对客体是等价的；仍然是
**驱动侧做法**，没有重入 guest 的 memory callback（在 SVC 处理器里再跑一段 guest
代码风险太大）。

### 6. 离线测试

四个 harness 单测，全部 PASS：

```text
_test_cbdraw.py     draw / 索引 / 顶点路径           PASS
_test_m4state.py    bind/set/clear 记录              PASS
_test_cmdhandle.py  句柄 / CallCommands / 展开       PASS
_test_stateobj.py   6 类状态对象就地打包             PASS
                    records=19 bind_state=6 state_write_fail=0
```

### 7. 证据：700 秒严格门禁（新代码）

```text
M3: 700.0s  frames=8398 (12 fps)  status=budget
    objects: buffer 3974 / program 2626 / texture 450 / pool 419 / sampler 366 / cmdbuf 27
    frame: 768 ops / 2528 bytes, cmd mem 0x1C6D8B000+0x40000
    draw=707  overflow=0  api_entries=130
    presents=304
    first draw: op=13 vals=[0x701F1C88, 4, 1, 3, 0x141C22000, 0, 0, 1]
                caller=0x41D2BA6C main+0x1D2BA6C cb=0x701F1C88 frame#8
    state: BIND_UNIFORM 150 / BIND_STATE 50 / CALL 17 / SET_SCISSOR 15 /
           SET_VIEWPORT 15 / SET_DEPTH_RANGE 15 / SET_RT 6 /
           SET_BLEND_COLOR 5 / CLEAR_COLOR 3 / BIND_VATTR 2 / DRAW_IDX 2
    command handles: calls=5165 copies=0 submits=304 expanded=210807 miss=0
    first viewport (x,y,w,h) = (0, 0, 1920, 1080)
RESULT: PASS
```

与 M3.96 的 660 s 门禁（701 draw / 301 present / `overflow` 236）相比，draw 与
present 同量级，但三件事变了：

* `overflow` 从 236 变成 **0**；
* 命令句柄被 `CallCommands` 调用 **5165 次**，展开 **210807** 条记录，
  `CALL_MISS` = **0**（句柄查不到的次数）；
* `m3_verify.py` 对每条状态记录解码后**一条不合法都没有**（有的话会打印
  "M4 状态记录有 N 处不合法"）。这条检查包括 kind ∈ 0..5 与快照长度
  8/4/4/10/4/8 的逐项校验。

门禁本身仍只跑 700 s（`status=budget`），不是"加载完成后稳定运行"的证明。

### 8. iOS 侧：可移植状态层 + Metal 入口 195/513

新增 `ios/nvn/include/nvn_state.h` 与 `src/nvn_state.c`：

* 六类状态的**位域布局、默认值与 pack / unpack**，与 D2R `nvn_obj.h` 逐字段一致；
* **不翻译 NVN 枚举**：字段按 guest 写入的原值保存，由 Metal 层决定认不认得；
* `tests/nvn_state_test.cpp` 用真实位模式断言默认值、打包与往返，不依赖 Metal 或
  Apple SDK。

`ios/nvn/src/nvn_metal.mm`：

* `CmdBuf` 增加固定功能状态快照字段，`BeginRecording` 重置；
* 六类状态 setter 与 6 个 `Bind*State` 全部落地，bind 时快照并置 `pso = nil`；
* 补 `BindCoverageModulationTable` / `BindStorageBuffer(s)` / `BindImage(s)` /
  `BindTransformFeedbackBuffer(s)`、`ClearColori/ui` / `ClearDepthStencil` /
  `ClearBuffer` / `ClearTexture{i,ui}`、`SetAlphaRef` / `SetBlendColor` /
  `SetDepthRange` / `SetRenderEnable` / `SetScissors` / `SetViewports` /
  `SetViewportSwizzles`；
* 映射不到 Metal 的路径用 `log_once()` 明确上报，**没有静默 no-op**。

覆盖从 **134/513** 提升到 **195/513（38.0%）**，CommandBuffer 域从 23/144 到
50/144。门禁：`verify_ios: ok (metal bindings: 195/513)`、
`syntax_check_metal: ok (clang++.exe)`、
`build_abi_test: all tests passed`（本地 11 个可移植测试，含新增的
`nvn_state_test`）。

CI（run 41151003231，commit `ecfd58e`）两个 job 都通过：portable
（124387228279）编译并运行 `nvn_state_test`；iOS（124387315048）用 Apple 工具链
编译 `nvn_metal.mm`，模拟器自检通过。

### 还没有的（M4 的真实缺口）

| 缺口 | 后果 |
|---|---|
| iOS 侧 `cmdbuf_end_recording` 仍在 EndRecording 里直接 commit 并返回 handle 0 | 客体拿不到可提交的句柄，回放层没有命令流可重放 |
| iOS 侧未实现 `nvnCommandBufferCallCommands` / `nvnQueueSubmitCommands` | harness 记录得出来，回放层展不开 |
| `ClearColori/ui` / `ClearBuffer` / `ClearTexture*` 只 `log_once`（该轮状态） | 该轮整数清屏、ClearBuffer 与纹理清屏仍是占位；整数清屏已在 M4 续五、ClearBuffer 已在 M4 续六接入，ClearTexture 已在 M4 续九接入；`ClearColor`（float）有真路径；`ClearDepthStencil` 已在下一轮接入 |
| storage buffer / image / transform feedback 只记录句柄 | 计算与 UAV 路径未接线 |
| harness 的 `nvnSamplerCompare` 只比较部分已实现字段，`SamplerBuilder` 缺 wrap / compare / border setter | 采样器状态不完整 |
| 没有真机运行 | 不能声称画面，任何"能玩"的说法都不成立 |

## M4 续：六类固定功能状态进入 Metal 管线描述符

上一轮把这些状态记录下来了，但回放层并没有用它们：`program_pipeline()` 只填了颜色格式和
顶点描述符，意味着混合、深度测试、剔除、写掩码全都不生效。这一轮把它们接进管线。

### 1. 映射放在可移植层，Metal 只做最后一跳

新增 `ios/nvn/include/nvn_pipeline.h` + `src/nvn_pipeline.c`：

* 把六类状态合成一个 `nvngfx_pipeline_state`：8 个颜色目标的混合使能 / 因子 / 方程、
  逐目标写掩码、深度测试 / 写 / 比较、模板前后面各自的函数与三个操作、剔除 / 绕序 /
  填充、采样数、alpha-to-coverage；
* 把 NVN 枚举解码成 `NVNGFX_*` 中性值。取值来自 `dm/gfx/nvn/include/nvn.h`，不是猜的：
  `NVN_BLEND_FUNC_ONE=0x2`、`NVN_BLEND_EQUATION_ADD=0x1`、`NVN_DEPTH_FUNC_LESS=0x2`、
  `NVN_FACE_BACK=0x2`、`NVN_STENCIL_OP_INCR=0x4` …
* 表示不了的字段进 `unsupported` 位图，由后端逐项上报，**不静默丢弃**。

`tests/nvn_pipeline_test.cpp` 在 Windows 上就能跑，钉住了两处"看着对、其实错"的转换：

| 转换 | 陷阱 |
|---|---|
| 写掩码 | NVN 是 `r=1,g=2,b=4,a=8`，Metal 是 `r=8,g=4,b=2,a=1`；直接照抄会把红和 alpha 对调，在灰色测试图上几乎看不出来 |
| 混合目标 | NVN 的 blend 对象**自带 `target` 字段**，bind 必须落进 `state.target` 对应的槽，而不是永远写 0 号目标 |

### 2. 修掉一个真实 bug：混合状态不分目标

`CmdBuf` 之前只有一个 `blendState`。游戏用 `nvnBlendStateSetBlendTarget` 指定这份状态属于
哪个颜色目标，全部塞进 0 号槽等于让后面的目标拿不到自己的混合参数，而 0 号目标又被最后
一次 bind 覆盖。现在按目标存 8 份、`hasBlendState[8]` 独立记录，bind 时按 `target & 7` 落槽。

### 3. 管线描述符里到底填了什么

`program_pipeline()` 现在按状态快照填写：

* `colorAttachments[0].blendingEnabled`、`sourceRGBBlendFactor`、`destinationRGBBlendFactor`、
  `rgbBlendOperation`，以及 alpha 侧的三项；
* `colorAttachments[0].writeMask`（已转成 Metal 位序）；
* `alphaToCoverageEnabled`（只有 0 号目标有定义）；
* 深度/模板附件按纹理的真实格式分开填：有深度位才填 `depthAttachmentPixelFormat`，
  有模板位才填 `stencilAttachmentPixelFormat`。`Depth32Float_Stencil8` 两个都填，
  `Stencil8` 只填模板，`Depth16Unorm` / `Depth32Float` 只填深度——填错 Metal 会直接
  拒绝建管线；
* `rasterSampleCount` 先取多重采样状态，和颜色附件实际采样数不一致时以附件为准并上报。

管线缓存键从 `(格式, 顶点布局)` 扩成 `(颜色格式, 深度格式, 模板格式, 采样数, 顶点布局,
状态哈希)`。否则改一次混合、换一张深度目标或切换 MSAA 都会命中旧管线，表现是"状态设置无效"。

### 4. 编码器侧：剔除、绕序、填充、深度模板对象、polygon offset

`apply_fixed_state()` 在每次 draw 前执行（同一 pass 内的两个 draw 之间可能改了状态）：

* `setCullMode` / `setFrontFacingWinding` / `setTriangleFillMode`；
* `MTLDepthStencilState`：深度测试关闭时强制 `always` 且不写深度（NVN 语义如此），
  模板按前 / 后面各自的比较函数与失败 / z-fail / z-pass 三个操作建对象；缓存键只含
  深度 / 模板字段，改混合不会重建；即使 guest 没 bind 状态对象，也按 NVN 默认值建一份，
  因为带深度附件的 Metal pass 不能在没有 depth-stencil state 时 draw；
* 深度 / 模板纹理现在会挂进同一个 render pass；组合格式用同一张纹理同时挂到两个附件。

顺带实现了 `nvnCommandBufferSetPolygonOffsetClamp`（之前完全没实现）：三个 float 直接喂
`setDepthBias:slopeScale:clamp:`，polygon state 里的 `offset_en` 不再是空话；关闭偏置时
显式归零，避免同一 pass 内前一条 draw 的 bias 泄漏到下一条。
覆盖 **196/513（38.2%）**。

### 5. 表示不了、只能上报的

| 字段 | 为什么 |
|---|---|
| logic op（≠ COPY） | Metal 没有逻辑运算，要 shader |
| alpha test（≠ ALWAYS） | 同上，需要 shader 或 discard |
| polygon mode POINT | Metal 只有 fill / line |
| cull FRONT_AND_BACK | Metal 只有一个 cull mode；回落成 none，免得整帧消失 |
| polygon offset 的 POINT / LINE 使能 | Metal 的 depth bias 只作用于填充三角形；只有 FILL 位（0x4）会被采用 |
| coverage modulation / to-color / sample locations / raster samples / a2c dither | 没有对应路径 |
| alpha-to-coverage + 单采样附件 | Metal 会拒绝建管线；日志后关闭 a2c |
| 启用 polygon offset 但没收到常量 | 会用 0 偏置，等于静默失效，所以明确上报 |

### 6. 验证

```text
nvn_pipeline_test: ok (defaults, enum tables, write-mask order, blend targets,
                     unsupported reporting, cache key)
build_abi_test: all tests passed
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 196/513)
```

### 还没有的

* 只有 0 号颜色目标进入描述符和 render pass；MRT 要等 pass 支持多附件（M4 续三已补上）；
* 管线缓存当时只有 4 个槽（后续 M4 续七已扩到 16 槽），状态组合一多就会反复重编译（真机上每次数毫秒）；
* 这一轮结束时 `ClearDepthStencil` 仍是日志占位；下一节已把它接入 load action；
* 没有真机运行，不能声称画面正确。

## M4 续二：深度/模板清屏与模板/深度命令状态

上一轮的 `MTLDepthStencilState` 已经能描述模板测试，但命令缓冲的模板引用值和掩码没有
入口，深度/模板清屏也仍是日志。这一轮把两者接进回放层。

### 1. 清屏变成 load action，并且没有 draw 也会落地

`ClearColor`、`ClearDepthStencil` 不再要求后面紧跟一条 draw。清屏先挂在 `CmdBuf` 上，
`ensure_encoder()` 按目标纹理的真实格式填 `MTLLoadActionClear` / `MTLLoadActionLoad`：

* 组合格式 `Depth32Float_Stencil8` 同时挂 depth/stencil 附件；
* 纯 `Depth32Float` / `Depth16Unorm` 只清深度；
* 纯 `Stencil8` 只清模板；
* `clearDepth` / `clearStencil` 分别是两个平面的清屏值。

`flush_pending_clears()` 在 `EndRecording` 和 `SetRenderTargets` 换目标前调用。它解决了
“整段只有清屏、没有 draw，结果没有 render pass，提交了个空 command buffer”的回归；
换目标前先落地也避免把旧目标的清屏值套到新纹理上。

`ClearColor` 目前仍只支持 0 号目标和完整 `0xF` 掩码；其他组合明确 `log_once`。
`ClearDepthStencil` 的模板清屏目前只接受 `0xFF` / `0xFFFFFFFF` 完整掩码，部分掩码无法
用 Metal load action 表达（该轮当时未支持；后续 M4 续十已用独立 depth/stencil-only pass 实现）。

### 2. 三个模板命令入口

新增并登记：

* `nvnCommandBufferSetStencilRef` -> `setStencilFrontReferenceValue:backReferenceValue:`；
* `nvnCommandBufferSetStencilMask` -> `MTLStencilDescriptor.writeMask`；
* `nvnCommandBufferSetStencilValueMask` -> `MTLStencilDescriptor.readMask`。

`NVNface` 按位解释：front=1、back=2、both=3。引用值是 encoder 状态，不进入
`MTLDepthStencilState` 缓存键；前后脸读/写掩码进入缓存键，改掩码会重建状态对象。
超过 8 位的值截取并上报。

### 3. 新增模拟器自测

`nvngfx_metal_selftest()` 末尾增加两项：

1. BeginRecording -> SetRenderTargets -> ClearColor 蓝色 -> EndRecording，
   不绑定程序、不 draw，回读中心像素必须为蓝色。这直接覆盖
   `flush_pending_clears()` 修复的回归点。
2. 建一张 `DEPTH24_STENCIL8`（0x35）的 64x64 纹理，与颜色目标一起挂载，
   设置前后脸模板 ref / 读掩码 / 写掩码，再清深度 0.25、模板 0x5A，并 draw 一条
   全屏三角形，让 `apply_fixed_state()` 真正调用模板引用设置、建立带深度/模板格式的
   管线。检查挂起状态和最终 Metal command buffer 无错误。

### 4. 深度裁剪与深度边界

`nvnCommandBufferSetDepthClamp` 是真实映射：NVN 的 enable 对应
`MTLDepthClipModeClamp`，关闭时恢复 `MTLDepthClipModeClip`；每次 draw 前重新应用，
避免 encoder 状态泄漏到下一条 draw。

`nvnCommandBufferSetDepthBounds` 没有 Metal 对应能力。状态会记录 enable / min / max，
`[0,1]` 的默认范围等价于关闭，不额外报错；启用更窄范围时用 `log_once` 明确说明
Metal 不会执行深度边界测试。反转的范围会被规范化并上报，不冒充已经生效。

模拟器的深度/模板用例现在同时调用这两个入口，检查 clamp / bounds 状态，再 draw 一条
全屏三角形，让 `setDepthClipMode:` 真正执行。

### 5. 数组形式的深度范围

`nvnCommandBufferSetDepthRanges(cb, first, count, const float* ranges)` 每项是两个
float（near, far），对应同下标的 viewport。Metal 后端只有一个 viewport，因此：

* `first == 0` 时取第一项，写入与 `SetDepthRange` 相同的状态字段；
* 多于一项时应用第一项，其余用 `log_once` 上报；
* `first != 0` 表示只改其他 viewport，单 viewport 的深度范围保持原值，并明确上报。

两个入口共用 `apply_depth_range()`，避免标量与数组路径逐渐分叉。模拟器的深度/模板
用例现在先设 viewport，再用两个范围调用数组入口，断言 `depthNear/depthFar` 与
`MTLViewport` 的 znear/zfar 都变成 0.2/0.8。

### 6. 覆盖与仍然缺的

覆盖从 **196/513** 提升到 **202/513（39.4%）**，未实现 311；CommandBuffer 域
**57/144**。门禁输出：

```text
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 202/513)
build_abi_test: all tests passed
```

仍未支持：

* `SetDepthBounds` 只有记录与上报，Metal 没有深度边界测试；
* 部分模板清屏掩码；
* `ClearTexture*` 仍是日志占位（整数清屏已在 M4 续五、ClearBuffer 已在 M4 续六落地；该轮当时未支持，M4 续九已实现）；
* 没有真机像素验证，不能声称画面正确。

## M4 续三：多颜色附件（MRT）

`nvnCommandBufferSetRenderTargets` 一直能记录 8 个颜色目标，但管线、render pass 和
清屏都只用了 0 号槽。这一轮把 1–7 号真正接进 Metal 路径。

### 1. 管线与缓存

`MTLRenderPipelineColorAttachmentDescriptor[0..numColors-1]` 现在逐个填写：

* 每个附件用自己的 `pixelFormat`；
* 每个附件用自己的 blend enable / factor / equation 与 write mask；
* alpha-to-coverage 仍只作用于 0 号附件，这是 Metal 的定义；
* 管线缓存键从单个 `format` 扩成 8 个格式加附件数量，未使用的槽保持 0 并参与比较。

`ensure_pipeline()` 从 `CmdBuf::color[]` 收集全部格式；任一目标缺失或采样数不一致时
拒绝建管线并明确上报，避免把一个只声明了部分附件的 pipeline 交给 Metal。

### 2. Render pass 与逐目标清屏

`ensure_encoder()` 按 `numColor` 循环挂载所有颜色附件。清屏状态从一组标量改成
`pendingClear[8]` / `clear[8][4]`，因此同一 pass 可以给每个目标不同的
`MTLLoadActionClear` 和 clear colour；未清屏的目标用 `MTLLoadActionLoad`。

`ClearColor` 的 `index` 现在接受任何已绑定的颜色目标（仍要求完整 `0xF` 通道掩码），
不再只接受 0 号。`BeginRecording`、`SetRenderTargets` 和 `flush_pending_clears()`
都会清掉整组 pending 状态，避免旧目标的清屏值泄漏到新 pass。

### 3. 模拟器自测

新增 MRT 用例，使用同一张 RGBA8 纹理作为 0 号目标，另建一张同尺寸 RGBA8 作为 1 号目标：

1. 只清屏，0 号清黑、1 号清洋红；回读两张纹理的中心像素，分别必须是黑和洋红。
   这直接验证逐目标 clear colour，而不是两个目标共用 0 号的值。
2. 同一组目标上绑定一个输出 `[[color(0)]]` 红、`[[color(1)]]` 蓝的 MSL fixture，draw
   全屏三角形；回读 0 号为红、1 号为蓝，验证 MRT 管线描述符、render pass 和 shader
   输出槽三者一致。

任一阶段失败都会以 non-zero rc 退出，不会把“声明了第二附件”误报成 MRT 可用。

### 4. 覆盖与仍然缺的

这一轮没有新增 NVN 入口，覆盖率保持 **202/513（39.4%）**，未实现 311；CommandBuffer
域 **57/144**。门禁输出：

```text
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 202/513)
build_abi_test: all tests passed
```

仍未支持：

* 没有颜色目标的纯深度 pass 仍打不开 encoder；
* `SetDepthBounds` 只有记录与上报，Metal 没有深度边界测试；
* 部分模板清屏掩码；
* `ClearTexture*` 仍是日志占位（整数清屏已在 M4 续五、ClearBuffer 已在 M4 续六落地；该轮当时未支持，M4 续九已实现）；
* 没有真机像素验证，不能声称画面正确。

## M4 续四：纯深度 pass

前两轮把颜色附件和 MRT 打开，但 `ensure_pipeline()` / `ensure_encoder()` 仍要求
`numColor >= 1`，所以 `SetRenderTargets(0, ..., depth)` 加深度清屏和 draw 会静默没有
render pass。这一轮去掉这个前提。

### 1. 管线与 render pass

* `ensure_pipeline()` 在 `numColor == 0` 时改用深度纹理的采样数，并要求至少存在一个
  深度目标；颜色与深度采样数不一致时拒绝建管线。
* `program_pipeline()` 允许 `numColors == 0`，此时不填任何颜色附件，只填
  `depthAttachmentPixelFormat` / `stencilAttachmentPixelFormat`。alpha-to-coverage 在
  没有颜色附件时关闭，避免 Metal 拒绝无效组合。
* `ensure_encoder()` 在没有颜色目标时只挂载深度/模板附件；若所谓 depth 纹理没有任何
  深度/模板平面，则明确上报并拒绝打开 pass，而不是交给 Metal 报一个难懂的验证错误。
* 管线缓存键仍包含附件数量；0 颜色 + 深度格式和 1 颜色 + 深度格式不会互相命中。

### 2. 模拟器自测

新增两段 pass：

1. `SetRenderTargets(0, depth)`，清深度到 1.0，绑定深度测试 LESS / 写使能，draw 一个
   z=0.5 的全屏三角形。
2. 把同一张深度纹理挂到“颜色 + 深度”pass，颜色清黑，深度测试改为 EQUAL、写关闭，
   再 draw 同一 z 的三角形。只有第一段真的把 0.5 写进深度缓冲，第二段才会通过 EQUAL
   并输出绿色；若纯深度 pass 没打开，颜色保持黑色，自测以 non-zero rc 失败。

这比只检查“draw 被编码”多走了一步：它验证了深度写入的实际结果。

### 3. 覆盖与仍然缺的

没有新增 NVN 入口，覆盖率保持 **202/513（39.4%）**，未实现 311；CommandBuffer 域
**57/144**。门禁输出：

```text
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 202/513)
build_abi_test: all tests passed
```

仍未支持：

* `SetDepthBounds` 只有记录与上报，Metal 没有深度边界测试；
* 部分模板清屏掩码；
* `ClearTexture*` 仍是日志占位（整数清屏已在 M4 续五、ClearBuffer 已在 M4 续六落地；该轮当时未支持，M4 续九已实现）；
* 没有真机像素验证，不能声称画面正确。

## M4 续五：整数颜色清屏

`nvnCommandBufferClearColori` 与 `nvnCommandBufferClearColorui` 之前只记录日志。Metal 的
`MTLClearColor` 以 double 保存四个分量，而 int32 / uint32 都能被 double 精确表示，因此整数
清屏不需要猜测附件格式或改写位模式。

### 1. 统一 pending-clear 路径

* `CmdBuf::clear` 从 `float[8][4]` 扩成 `double[8][4]`；
* `ClearColor`、`ClearColori`、`ClearColorui` 共用 `queue_color_clear()`，统一做目标下标与
  完整 `0xF` 掩码检查，再写入 pending clear；
* 浮点入口先把四个 float 提升为 double；整数入口分别读取 guest 的 `int32_t[4]` 或
  `uint32_t[4]`，逐项精确转换成 double，避免经过 float 丢失整数位；
* 三者仍在清屏时关闭当前 encoder，由下一次 `ensure_encoder()` 通过附件的
  `MTLLoadActionClear` 落地。部分通道掩码在该轮仍明确上报，不冒充已执行；
  颜色部分已在 M4 续八实现。

### 2. 模拟器自测

新增 `0x8900` 命令缓冲用例：

1. 建 `R32Uint` 目标，调用 `ClearColorui(0, {0x12345678, 0, 0, 0}, 0xF)`，回读原始字节必须是
   `78 56 34 12`；
2. 建 `R32Sint` 目标，调用 `ClearColori(0, {-123456789, 0, 0, 0}, 0xF)`，回读原始字节必须是
   `EB 32 A4 F8`。

两步都用零容差比较，并检查 Metal command buffer 没有进入 error 状态。这样验证的不只是
“入口被调用”，而是整数分量真的到达整数 render target。

### 3. 覆盖与仍然缺的

没有新增 NVN 入口，覆盖率保持 **202/513（39.4%）**，未实现 311；CommandBuffer 域
**57/144**。门禁输出：

```text
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 202/513)
build_abi_test: all tests passed
```

仍未支持：

* `SetDepthBounds` 只有记录与上报，Metal 没有深度边界测试；
* 模板与颜色清屏的部分通道掩码（颜色部分已在 M4 续八实现；模板仍未支持）；
* `ClearTexture{i,ui}`（ClearBuffer 已在 M4 续六落地；该轮当时未支持，M4 续九已实现）；
* 没有真机像素验证，不能声称画面正确。

## M4 续六：任意 32 位 ClearBuffer

`nvnCommandBufferClearBuffer(cb, address, size, value)` 之前只打日志。NVN 的 `value` 是
重复的 32 位字；Metal blit 的 `fillBuffer` 只能重复一个字节，因此不能拿它冒充这个入口。

### 1. 宿主 compute 清填

* `Ctx` 懒加载一个很小的 MSL kernel `nvngfx_clear_buffer`：`device uint*` 目标、
  `constant uint value` 与 `constant uint count`，线程先检查 `gid < count`；
* 清填地址和长度必须 4 字节对齐，且地址必须落在已注册 memory pool 的 span 内；否则明确
  上报，不修改内存；
* 同一个 `MTLCommandBuffer` 内先结束 render encoder，再开 compute encoder，后续 draw 会
  重新打开 render pass 并保持相对顺序；
* 这是宿主工具 kernel，不冒充已经支持 guest `DispatchCompute`；后者仍在 TODO。

### 2. 模拟器自测

在现有 guest pool 上把 32 字节清零，再调用
`ClearBuffer(address, 32, 0xA1B2C3D4)`。等 command buffer 完成后逐 word 回读，8 个 word
必须全部等于 `0xA1B2C3D4`。这个值的四个字节不同，单字节 `fillBuffer` 无法通过，正好证明
compute kernel 真的执行了。

### 3. 覆盖与仍然缺的

没有新增 NVN 入口，覆盖率保持 **202/513（39.4%）**，未实现 311；CommandBuffer 域
**57/144**。门禁输出：

```text
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 202/513)
build_abi_test: all tests passed
```

仍未支持：

* `SetDepthBounds` 只有记录与上报，Metal 没有深度边界测试；
* 模板与颜色清屏的部分通道掩码（颜色部分已在 M4 续八实现；模板仍未支持）；
* `ClearTexture{i,ui}`（该轮当时未支持；后续 M4 续九已实现）；
* guest `DispatchCompute` / image / storage buffer 路径；
* 没有真机像素验证，不能声称画面正确。

## M4 续七：每 Program 的 PSO 缓存扩到 16 槽

`Program` 过去给每个 program 留 4 个 `MTLRenderPipelineState` 槽。MRT、深度/模板格式、
采样数、顶点布局与固定功能状态哈希都进缓存键以后，场景在几种常见组合之间来回切时，
4 槽很快被冲掉，下一次切回就要重新编译管线；真机上每次是毫秒级卡顿。

### 1. 16 槽 MRU

* `kPsoCacheSlots = 16`，`Program::slots` 按这个常量分配；
* 命中时把该槽移到队首（`pso_lookup()` 内做逐项搬移），未命中时 `pso_store()` 从最后一槽
  向下移位，淘汰最久未用项；
* 缓存键仍包含颜色格式数组与附件数量、深度/模板格式、采样数、顶点布局 id 与状态哈希；
* `pso_drop()` 按新容量释放所有槽，避免 program 销毁时漏掉引用。

### 2. 模拟器自测

新增 `0x8B00` 探针 program：

1. 从 `0x5300` 的真实 draw program 里取一个已有的 `MTLRenderPipelineState` 作为样本；
2. 用 16 个不同的状态键依次 `pso_store()`，填满全部槽；
3. 再按原顺序逐一 `pso_lookup()`，16 个键必须全部命中。

旧的 4 槽实现在第 5 次存储时就会丢掉第一个键，测试会直接失败；因此这条用例验证的是
“容量与 MRU 顺序”，而不是 shader 编译。

### 3. 覆盖与仍然缺的

没有新增 NVN 入口，覆盖率保持 **202/513（39.4%）**，未实现 311；CommandBuffer 域
**57/144**。门禁输出：

```text
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 202/513)
build_abi_test: all tests passed
```

仍未支持：

* 模板与颜色清屏的部分通道掩码（颜色部分已在 M4 续八实现；模板仍未支持）；
* `ClearTexture{i,ui}`（该轮当时未支持；后续 M4 续九已实现）；
* guest `DispatchCompute` / image / storage buffer 路径；
* 真实标题 shader 的离线 MSL 接入（`nvn_shader_blobs.gen.c` 仍为空表）；
* 没有真机像素验证，不能声称画面正确。


## M4 续八：ClearColor 部分通道掩码

Metal 的 `MTLLoadActionClear` 会覆盖四个通道，无法表达 NVN 的 mask。此前非 `0xF`
的 `ClearColor*` 会明确上报并丢弃；这一轮把它接上，而不是继续假装。

### 1. 合并 pending clear

* `CmdBuf` 为每个颜色目标保存 `clearMask[8]`（NVN 位序 r=1, g=2, b=4, a=8）；
* `mask == 0` 是 no-op；`mask == 0xF` 继续走 load action；
* 部分掩码按通道把值合并进 `clear[i][4]`，同一 pass 打开前的多次部分清屏会合并；
* 打开 pass 时，部分目标使用 `MTLLoadActionLoad`，保留未选通道。

### 2. 宿主 clear 管线

* 首次需要部分清屏时编译一个宿主 MSL：顶点阶段生成覆盖整个 viewport 的全屏三角形，
  片元阶段从 `constant uint4* bits` 读取每个目标的清屏值；
* 片元输出类型按目标的 NVN format 选择 `float4` / `uint4` / `int4`，因此
  unorm / sRGB / float 与整数目标都能走同一条路径；
* 管线描述符的每个颜色附件写入 `nvngfx_write_mask()` 转换后的 Metal 写掩码，
  未参与部分清屏的目标写掩码为 0，混合关闭；
* 管线按颜色/深度/模板格式、写掩码与采样数缓存在 `Ctx`，最多 8 项，满后轮换淘汰；
* 清屏前若已有 viewport/scissor，临时改成整个附件并在画完后恢复，避免 clear 被裁掉。

### 3. 模拟器自测

* RGBA8Unorm 目标先全量清成 (0.25,0.5,0.75,1)，再在同一 pass 打开前依次
  `ClearColor(mask=0x1)` 与 `ClearColor(mask=0x2)`；回读必须是 (255,255,191,255)，
  证明两次部分清屏合并且 B/A 未被覆盖；
* RGBA8Uint 目标先全量清成 (0x11,0x22,0x33,0x44)，再以 mask=0x3 清成
  (0xAA,0xBB,0xCC,0xDD)；回读必须是 (AA,BB,33,44)，证明 `uint4` 输出路径与
  B/A 保留。

### 4. 覆盖与仍然缺的

没有新增 NVN 入口，覆盖率保持 **202/513（39.4%）**，未实现 311；CommandBuffer 域
**57/144**。门禁输出：

```text
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 202/513)
build_abi_test: all tests passed
```

仍未支持：

* 模板清屏的部分通道掩码（该轮当时未支持；后续 M4 续十已实现）；
* `ClearTexture{i,ui}`（该轮当时未支持；后续 M4 续九已实现）；
* guest `DispatchCompute` / image / storage buffer 路径；
* 真实标题 shader 的离线 MSL 接入（`nvn_shader_blobs.gen.c` 仍为空表）；
* 没有真机像素验证，不能声称画面正确。

## M4 续九：ClearTexture 完整级别、区域与掩码清屏

`nvnCommandBufferClearTexture` / `ClearTexturei` / `ClearTextureui` 此前只打印
`unsupported`。Metal 没有纹理填充 API，这一轮按“完整级别用 load action、局部或
掩码用 scissor 全屏三角形”的方式接上，并且逐 mip、逐 array slice / cube face
挂载颜色附件。

### 1. 两条清屏路径

* 新增 `clear_texture_common()`，三个入口只负责把颜色指针解释为 `float4`、
  `int4` 或 `uint4`；
* 无 region 且 `mask == 0xF` 时，每个 level/slice 用 `MTLLoadActionClear`；
* 有 region、部分掩码或非整级清屏时，使用 `MTLLoadActionLoad`，以附件尺寸设置
  viewport、以 region 设置 scissor，再走 M4 续八的宿主 clear 管线；
* 颜色附件通过 `level` / `slice` 选择 mip 与层；`Tex::layers` 而不是
  `MTLTexture.arrayLength` 决定层数，因为 cube 的 arrayLength 是 1 但有六个面，
  cube array 的 arrayLength 是 cube 数而不是 slice 数；
* 支持 2D、2D array、cube、cube array、2D MSAA 颜色纹理；完整多 mip 清屏可用，
  显式 region 仅接受单 level 视图；
* 1D、3D、depth/stencil 目标与格式变更的 texture view 明确上报并拒绝，避免清错；
* 清屏前先落地 pending ClearColor/ClearDepthStencil，再结束当前 render encoder，
  保证命令顺序。

### 2. 模拟器自测

* RGBA8 目标先整级清蓝，再用 region 把右下四分之一清红：中心必须红、左上仍蓝；
* 同一区域再以 `mask=0x2` 清绿：中心由红变黄 `(255,255,0,255)`，证明只写 G；
* RGBA8Uint 完整清屏回读 `(0x11,0x22,0x33,0x44)`；
* cube 目标完整清屏后逐面回读六个 face 都为蓝，证明 cube 不能只清 face 0；
* 两级 mip 的 2D 目标完整清屏后回读 mip 1 为绿，证明附件的 `level` 选择生效。

### 3. 覆盖与仍然缺的

没有新增 NVN 入口，覆盖率保持 **202/513（39.4%）**，未实现 311；CommandBuffer 域
**57/144**。门禁输出：

```text
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 202/513)
build_abi_test: all tests passed
```

仍未支持：

* 模板清屏的部分通道掩码（该轮当时未支持；后续 M4 续十已实现）；
* 1D / 3D / depth-stencil 的 `ClearTexture{i,ui}`；
* guest `DispatchCompute` / image / storage buffer 路径；
* 真实标题 shader 的离线 MSL 接入（`nvn_shader_blobs.gen.c` 仍为空表）；
* 没有真机像素验证，不能声称画面正确。

## M4 续十：模板部分通道掩码清屏

上一轮的 `ClearDepthStencil` 只把完整模板掩码 `0xFF` 映射成附件的
`MTLLoadActionClear`；部分掩码无法用 load action 表达。本轮补上 8 位模板写掩码路径。

### 1. 独立 depth/stencil-only pass

* `clear_stencil_state()` 按 8 位写掩码缓存 `MTLDepthStencilState`；深度比较 `ALWAYS`、
  不写深度，模板比较 `ALWAYS`，失败与 z-fail 操作为 `KEEP`，z-pass 操作为 `REPLACE`，
  读掩码 `0xFF`，前后脸使用同一规则；
* `clear_stencil_pipeline()` 创建无颜色附件、`fragment void` 的管线，缓存键为深度格式、
  模板格式和采样数；
* `encode_partial_stencil_clear()` 先结束当前 encoder，再开一个仅含 depth/stencil 附件的
  pass：模板用 `Load` 保留旧值，全屏三角形以 `REPLACE` 和写掩码修改选中的位；若同一次
  调用还要求清深度，深度附件改用 `Clear`。写回后标记纹理已渲染；
* 完整 `0xFF` 仍走原来的 pending clear + load action，避免为常见整字节清屏引入额外 draw。

### 2. 模拟器自测

* 先把模板整字节清成 `0xF0`，再用 mask `0x0F` 和值 `0x5A` 做部分清屏，预期字节 `0xFA`；
* 随后用模板测试 `EQUAL 0xFA` 画一条全屏三角形；只有高半字节被保留且低半字节被更新时，
  颜色目标才会出现预期像素。回读中心像素验证结果。

### 3. 覆盖与边界

没有新增 NVN 入口，覆盖率保持 **202/513（39.4%）**，未实现 311；CommandBuffer 域
**57/144**。本地门禁输出：

```text
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 202/513)
build_abi_test: all tests passed
```

仍未支持：

* 1D / 3D / depth-stencil 的 `ClearTexture{i,ui}`；
* guest `DispatchCompute` / image / storage buffer 路径；
* 真实标题 shader 的离线 MSL 接入（`nvn_shader_blobs.gen.c` 仍为空表）；
* 没有真机像素验证，不能声称画面正确。

## M4 续十一：guest compute dispatch（direct binding）

`nvnCommandBufferDispatchCompute` / `DispatchComputeIndirect` 以前只写命令流、
返回日志桩。本轮把 compute-only 程序的 direct-binding 路径接进回放，覆盖率从
**202/513（39.4%）** 提到 **204/513（39.8%）**，未实现 309；CommandBuffer 域
**59/144**。

### 1. compute program 与 graphics program 分开

* `CmdBuf` 新增 `computeProgram`；`nvnCommandBufferBindProgram` 解释第三个参数的
  stage 掩码：bits 0..4 是图形阶段，bit 5 是 compute。带 bit 5 的绑定只更新
  compute 槽，不会覆盖下一条 draw 使用的图形 program；`stages == 0` 仍按图形
  program 处理，兼容已有调用；
* compute 目前只接受 `nvnProgramSetShaders(..., n=1, ...)` 的 compute-only 程序，
  使用 `key[0]` / `code[0]`；混合 graphics + compute stage 明确拒绝。

### 2. 管线与线程组侧车

* `Program` 增加 `MTLComputePipelineState` 与三维线程组大小；MSL 从 shader key
  查离线 blob，查不到时仍可走 `nvngfx_shader_to_msl()` 的宿主直供路径；
* Metal 不在 kernel 函数里保存 launch size，因此 compute MSL 必须带
  `// nvngfx-threadgroup X Y Z`。`nvngfx_shader_threadgroup_size()` 解析并校验
  三个维度均为正、乘积不超过 1024；缺失或非法时拒绝建立管线；
* 入口点固定为 `nvngfx_main_cs`。

### 3. direct binding 契约

```text
NVN uniform buffer slot r  -> MSL [[buffer(r)]]          r = 0..15
NVN storage buffer slot r  -> MSL [[buffer(16 + r)]]     r = 0..14
NVN image slot r           -> MSL [[texture(r)]]         r = 0..15
NVN sampled texture slot r -> MSL [[texture(16 + r)]]    r = 0..14
NVN sampler slot r         -> MSL [[sampler(r)]]         r = 0..15
```

* Metal 每个阶段只有 31 个 buffer / texture 参数（索引 0..30）和 16 个
  sampler；NVN 自己允许 32 个槽，所以 direct 表必须切分。越界绑定在记录时
  明确拒绝并 `log_once`，不会等到真机 Metal 校验才失败；
* compute 使用独立的 `cuUbo` / `cuTex` / `cuSepTex` / `cuSepSamp` 表，
  一次 dispatch 不会覆盖下一条 draw 的图形阶段资源；
* `begin_compute_dispatch()` 先落地 pending clears，再结束 render pass；
  `prepare_compute_textures()` 在创建 compute encoder 之前对 sampled texture
  调用 `tex_upload()`（解块 / BCn 解码走 blit），失败则整次 dispatch 不编码；
* `BindTexture` 的组合句柄同时提供纹理和 sampler；`BindSeparateTexture` /
  `BindSeparateSampler` 进入独立表。组合绑定的 sampler 槽位与纹理槽位相同，
  分离绑定按各自槽位解析；
* direct dispatch 的三个参数是 threadgroup 数，乘以侧车里的线程组大小；
* indirect dispatch 把 guest 地址映射到 pool 的 `MTLBuffer`，交给
  `dispatchThreadgroupsWithIndirectBuffer:...` 由 GPU 读取 `{x, y, z}`，CPU
  不提前读参数，因此同一 command buffer 里前一个 kernel 写出的组数可见；
* dispatch 后只把 image 表的槽位标记为已写入，避免后续采样从 guest 原始纹理
  存储重传并覆盖 compute 结果；sampled texture 不会被误标为写目标。

### 4. 模拟器自测

* compute-only 程序 + `// nvngfx-threadgroup 16 1 1`；
* uniform buffer 0 提供 `base = 500`；storage buffer 0 为源
  （`src[0] = 1000`），buffer 1 为 direct 输出，随后重新绑定为 indirect 输出；
  indirect 参数 `{1, 16, 1}`；
* sampled texture 是 guest pool 中 2×2 RGBA8 的全红纹理，绑定到
  `[[texture(16)]]`；sampler pool 槽位 9 绑定到 `[[sampler(0)]]`；
* direct dispatch 使用组合纹理句柄（同时带 sampler）；indirect dispatch 先清掉
  组合槽，再走分离纹理 + 分离 sampler，两条路径都真实执行；
* 16×16 RGBA8 image 同时由 kernel 写入；
* 回读 256 个 storage word（`500 + 1000 + x + y`）与 image 的 `(0,0)` /
  `(15,15)` 像素；红色通道来自 sampled texel，期望像素约为
  `(255, 0, 191, 255)` 与 `(255, 255, 191, 255)`；
* 额外断言 sampled texture 处于 `uploaded` 且没有被 image 写回逻辑标成
  `rendered`。

### 5. 覆盖与仍然缺的

本地门禁：

```text
syntax_check_metal: ok (clang++.exe)
verify_ios: ok (metal bindings: 204/513)
build_abi_test: all tests passed
```

仍未支持：

* mixed graphics + compute 的 `nvnProgramSetShaders`；
* 离线 manifest 的 compute 条目与真实标题 compute shader 翻译（`gen_blobs.py`
  仍只有 `vs` / `ps`）；
* compute image 写回 guest 存储 / CPU 回读的完整一致性；
* texture view 与 compute 的 bindless / 参数缓冲区路径；
* 没有真机像素验证，不能声称画面正确。

## M4 续十二：实例化、base-vertex 与 indirect draw

真实标题是 Unity，绝大多数网格走 `DrawElements`，并大量使用实例化和
GPU-driven 的 indirect draw。上一轮只有 `DrawArrays` / `DrawElements`，所以
这一轮补上五个入口，Metal 覆盖从 **204/513（39.8%）** 提到
**209/513（40.7%）**，未实现 304；CommandBuffer 域从 **59/144** 到
**64/144**。

### 1. 五个入口与参数

按 nxnative 的公开原型（MIT）核对，参数顺序如下：

```text
DrawArraysInstanced(
    cb, primitive, first, count, baseInstance, instanceCount)
DrawElementsInstanced(
    cb, primitive, indexType, count, indexBuffer,
    baseVertex, baseInstance, instanceCount)
DrawElementsBaseVertex(
    cb, primitive, indexType, count, indexBuffer, baseVertex)
DrawArraysIndirect(cb, primitive, indirectAddress)
DrawElementsIndirect(
    cb, primitive, indexType, indexBuffer, indirectAddress)
```

实例化直接映射到 Metal：

```text
drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:
drawIndexedPrimitives:indexCount:indexType:indexBuffer:
    indexBufferOffset:instanceCount:baseVertex:baseInstance:
```

`baseVertex` 作为 Metal 参数传入，不修改 guest 索引缓冲；`baseInstance`
作为实例 ID 的起点传给 Metal，因此 shader 的 `[[instance_id]]` 与 NVN 语义
一致。普通 `DrawElements` 也复用了同一套索引解析，并额外检查
`indexBufferOffset` 必须按索引宽度对齐。

### 2. Indirect record 与 GPU 读取

两种 indirect record 的布局和 Metal 的
`MTLDrawPrimitivesIndirectArguments` / `MTLDrawIndexedPrimitivesIndirectArguments`
一致：

```text
DrawArraysIndirect:   { vertexCount, instanceCount, firstVertex, baseInstance }
DrawElementsIndirect: { indexCount, instanceCount, firstIndex, baseVertex,
                        baseInstance }
```

地址通过 `pool_span()` 映射到对应的 `MTLBuffer`，不把 guest 指针直接交给
Metal。CPU 只检查 span 长度（16 / 20 字节）和 4 字节偏移对齐；record 内容
由 GPU 在提交时读取，因此同一 command buffer 里 compute 先写出的参数可见。
索引缓冲仍要求至少有一个元素在 pool 内，且偏移按索引宽度对齐。

### 3. 边界行为

* `count == 0` 或 `instanceCount == 0` 是合法 no-op，不编码 Metal draw，
  也不要求先建管线；避免 Metal 校验层拒绝零实例 draw；
* 负 count / first / baseVertex / baseInstance / instanceCount 明确拒绝并
  `log_once`；
* u8 索引仍不接（Metal 没有对应的 `MTLIndexType`），只接受 u16 / u32；
* 索引或 indirect 地址不在任何已注册 pool、或偏移不对齐时，整条 draw 丢弃，
  不猜测地址；
* 仍未接 `MultiDraw*IndirectCount`、u8 索引转换和真实标题的 GPU-driven 场景。

### 4. 模拟器回归

新增一组共七个阶段，目标仍是 64×64 RGBA8，使用 guest pool 中的顶点流、
索引缓冲和两份 indirect record。顶点 0 是故意放在屏幕外的哨兵，顶点 1..3
组成真实三角形；实例 0 移到左边，实例 1 移到右边，其他实例移到屏幕外：

1. 零 instance：不编码，目标保持黑色；
2. `DrawArraysInstanced(instanceCount=2, baseInstance=0)`：左右都绿；
3. `DrawArraysInstanced(instanceCount=2, baseInstance=1)`：左黑右绿，
   证明 baseInstance 真的传给了 Metal；
4. `DrawElementsInstanced(baseVertex=1, instanceCount=2)`：左右都绿；
5. `DrawElementsBaseVertex(baseVertex=1)`：只有左侧绿，证明非实例化
   base-vertex 路径也生效；
6. `DrawArraysIndirect`（record 中 `firstVertex=1`, `instanceCount=2`）：
   左右都绿，证明参数由 GPU 读取且顺序正确；
7. `DrawElementsIndirect`（record 中 `baseVertex=1`, `instanceCount=2`）：
   左右都绿，证明 indexed indirect 与 record 内的 baseVertex 都生效。

### 5. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo .
python scripts/gen_nvn.py --repo . --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
```

本地已通过；iOS 模拟器的实际像素断言由 GitHub Actions 的 macOS job 执行。
这些结果只证明**合成 MSL** 的实例化 / indirect 路径，不证明真实标题 shader、
真实资源或真机画面已经可玩。

## M4 续十三：NVN buffer 对象与命令缓冲资源更新

Unity 类标题不只用“pool + 原始地址”：它还会从 `NVNbufferBuilder` 建 buffer
对象，查地址后把地址写进 uniform / draw 参数；动态常量则常用
`nvnCommandBufferUpdateUniformBuffer`，命令段之间也会做 buffer copy 和 barrier。
上一轮这些入口仍全部落在日志桩，本轮补齐：

* Buffer：11/11
* BufferBuilder：7/7
* MemoryPool：9/10
* MemoryPoolBuilder：8/8
* CommandBuffer：67/144

Metal 覆盖从 **209/513（40.7%）** 到 **241/513（47.0%）**，未实现 272。

### 1. buffer 是 pool 的视图，不是第二次分配

`NvnBufferSpec` 只记录 builder 收到的 `(device, pool, offset, size)`；
`nvnBufferInitialize` 校验 pool 存在、offset 非负且整个范围落在 pool 内，然后把
同一组字段存进宿主侧表。`nvnBufferGetAddress` / `nvnBufferMap` 都返回
`pool.storage + offset`，不新建 `MTLBuffer`，也不复制 guest 数据。

builder 的 getter 同样回答 setter 记录的值，不读取尚未验证的 guest builder
布局。`nvnBufferGetMemoryPool`、`nvnBufferGetMemoryOffset`、`nvnBufferGetSize`
和 `nvnBufferGetDebugID` 都从对象表返回；`Finalize` 只移除宿主侧表项。
`nvnBufferMap` 与 `nvnMemoryPoolMap` 之所以可以直接返回 storage，是因为 iOS
这边 pool 本来就是 `MTLResourceStorageModeShared` 的零拷贝内存。

MemoryPool 侧同时补了 `GetBufferAddress`、`GetFlags`、`Map`、
`Get/SetDebugLabel` 与 mapped-range flush / invalidate。后两者是 coherency hint，
统一内存下是合法空操作。`MapVirtual` 仍然未接：它不是简单查询，要求真正的
虚拟地址映射语义，不能用 `Map` 冒充。

### 2. UpdateUniformBuffer：录制时快照，提交时写入

NVN 的语义是“调用时把 CPU 数据拷进命令内存，提交时再写进目标 buffer”；调用者
在录制后可以立刻改写原数据。实现据此分两步：

1. 录制时用 `newBufferWithBytes:` 把 `data` 复制到 shared staging buffer；
2. 结束当前 render / compute encoder，开 blit encoder，把 staging 内容
   `copyFromBuffer` 到 guest pool 的目标 span，再结束 blit。

写入前会检查目标地址是否在已注册 pool、源指针是否是 guest 内存、offset 是否
非负 / 是否溢出，以及 size、source offset、destination offset 是否 4 字节对齐。
pending clear 会先落地，避免资源写入越过尚未物化的清屏。下一条 draw 或 dispatch
重新打开 pass，viewport / scissor 由 `ensure_encoder()` 恢复。

### 3. CopyBufferToBuffer 与 Barrier

`CopyBufferToBuffer` 的两端都必须是同一个已注册 pool 内的 span，复制由
`MTLBlitCommandEncoder` 在 GPU 上完成。因为 blit、draw 和 dispatch 都编码进
同一个 `MTLCommandBuffer`，Metal 自己会处理资源 hazard；源 buffer 由 GPU 在
提交时读取，前一条 compute 的写入对后一条 copy 可见。

`nvnCommandBufferBarrier` 因此记录为宿主侧空操作：当前后端是单 `MTLCommandQueue`
单 command buffer 顺序提交，Metal 已在命令之间建立依赖。它还不是通用 fence /
event 实现；`nvnSync*`、`nvnEvent*` 与跨提交等待仍未接。

仍未实现的相关入口：

* `CopyBufferToTexture` / `CopyTextureToBuffer` / `CopyTextureToTexture`（M4 续十四 已补齐）；
* `nvnMemoryPoolMapVirtual`；
* texture / buffer 的完整 CPU 回读与 compute image 写回 guest 存储；
* 真实标题 shader 与真机画面。

### 4. 模拟器自测

新增一组 buffer / copy 回归，复用上一组的 16×16 compute kernel：

1. 用第二个 guest pool 建 `NVNbufferBuilder` 与 `NVNbuffer`，逐一比对 device、
   pool、offset、size、debug id 和 `Map` 返回地址；
2. `UpdateUniformBuffer` 写入 777，随后立刻把源栈变量改成 999，证明录制时已快照；
3. `CopyBufferToBuffer` 把另一个 16 字节 uniform 记录（888）从源 span 复制到目标；
4. 插入 `Barrier`，再分别 dispatch：第一次读 777 的目标，第二次读 888 的目标；
5. 回读两块 256-word storage，逐字核对 `777/888 + 1000 + x + y`，并确认两块
   uniform 目标本身保存的也是 777 / 888。

失败码为 `16424`。这一组只证明合成 compute shader、NVN buffer 查询和 Metal
blit 的顺序路径；不是标题 buffer 内容，也不是画面正确。

### 5. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo .
python scripts/gen_nvn.py --repo . --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
```

本地已通过；GitHub Actions run `41178632664` 的 macOS `ios` job 也已通过，模拟器输出确认：

```text
[selftest] buffers/copies: NVN buffer getters and map matched pool storage;
UpdateUniformBuffer snapshotted 777 and CopyBufferToBuffer supplied 888 to GPU dispatches
BOTW_SELFTEST rc=0
```

顺手修了 `extract_nvn_signatures.py` 的返回类型正则：`void*` / `NVNmemoryPool*` 这类指针返回此前被漏掉。`nvnBufferMap`、`nvnBufferGetMemoryPool`、`nvnMemoryPoolMap` 因此进入实测集合，形状计数从 470 + 4 变为 **477 + 6**。

## M4 续十四：buffer ↔ texture / texture ↔ texture copy

上一轮把 `CopyBufferToBuffer` 接上了 Metal blit，但三个跨资源 copy 仍落在日志桩。
真实标题既会用线性 staging 区给纹理传 mip / cube 面，也会把渲染结果回读到
线性 buffer，还会做 texture → texture 的拷贝。本轮补齐：

* CommandBuffer：74/144（+7）

Metal 覆盖从 **241/513（47.0%）** 到 **248/513（48.3%）**，未实现 265。

### 1. region 与 view 的解析

三个入口共用 `NVNcopyRegion` 读取：拒绝空 region 与负偏移，`depth == 0` 按 1
处理。`NVNtextureView` 是可选参数，`NULL` 表示 base level 0、min layer 0；非空时
只接受“设置 base level / min layer”这一类视图，如果视图试图改 format 或
texture target，就明确拒绝，而不是假装没看见。

边界检查分两层：

1. 对着 mip 级别检查 `xoffset + width`、`yoffset + height`；
2. 3D texture 检查 `zoffset + depth` 不超过该级 depth，其余类型检查
   `minLayer + zoffset + depth` 不超过层数。

压缩格式（BCn 类）要求 region 的起点和尺寸都按块对齐，否则拒绝；行 / 面大小按
块数乘块字节计算，不会把 4×4 块当成 4 个纹素。

### 2. copy row / image stride

`nvnCommandBufferSetCopyRowStride` 与 `SetCopyImageStride` 记录在命令缓冲上，
`BeginRecording` 时归零。约定与 NVN 一致：

* **0 表示 tight packing**，即用 region 自身的行宽 / 面大小；
* 非零值不得小于 tight 大小，且必须是 4 字节对齐；
* 一次 copy 的总跨度是 `imageStride × depth`，对 buffer 一侧做这个范围的
  pool 命中与偏移对齐检查。

getter 返回当前记录值，供自测与后续入口复用。

### 3. 三个入口

`CopyBufferToTexture`：buffer 一侧是**线性**的 region 映像，不是纹理的
block-linear 存储，所以 stride 来自命令缓冲而不是 guest 纹理布局。3D 纹理用一次
`copyFromBuffer` 带 `sourceSize.depth = N`；2D / cube / array 则按 slice 逐层编码，
每层 `sourceOffset` 前进一个 image stride。

`CopyTextureToBuffer` 与 `CopyTextureToTexture` 在读取前会调用 `tex_upload()`：
被采样的纹理可能只在 guest pool 里有字节，Metal texture 还是空的，不回传就会拷到
未初始化内容。

三个入口写完目标纹理后统一调用 `mark_texture_copy_dest()`，把目标标成
`rendered / uploaded / uploadTried = true`、`dirty = false`。否则下一次采样会拿
guest pool 里的旧 block-linear 数据把刚拷进去的像素覆盖掉。

明确拒绝的情况：

* BCn transcode 纹理（解码后的 RGBA8 与 guest 存储不再一一对应）；
* 多重采样纹理；
* 同一个 texture 的 T2T copy；
* 源 / 目标 pixel format 或 texture type 不同的 T2T；
* 除 `ENGINE_2D` / `LINEAR_FILTER` 之外的 copy flag（mirror 需要另一套 blit，
  不能静默忽略）；
* 修改 format / target 的 texture view。

### 4. 模拟器自测

在第二个 16 KiB guest pool 里建两张 4×4 RGBA8 纹理：

1. 源 buffer 用 row/image stride `32/128`（行尾 16 字节填充为 0xEE），
   `CopyBufferToTexture` 写入纹理 A；
2. 改成 `24/96` 用 `CopyTextureToBuffer` 回读，逐字节核对 4×4 像素，并确认每行
   8 字节 padding 仍是 0——忽略 stride 的实现会在这里失败；
3. `CopyTextureToTexture` A → B；
4. 改成 tight `16/64` 回读 B，逐字节核对；
5. 顺带验证 copy stride getter 的默认值 `0/0` 与设置后的值。

失败码 `16425`。这一组只证明合成纹理与 Metal blit 的 stride 路径；不是标题资源，
也不是画面正确。

### 5. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 248 implemented / 477 measured + 6 inferred）；GitHub
Actions run `41181832245` 的 macOS `ios` job 也已通过，模拟器输出确认：

```text
[selftest] buffer/texture copies: B2T/T2B/T2T round-tripped a 4x4 RGBA8
image through row/image strides 32/128 -> 24/96 -> tight
BOTW_SELFTEST rc=0
```

## M4 续十五：command buffer 生命周期、内存记账与调试标记

标题启动时会按 NVN 的惯例初始化命令缓冲、给它补命令内存，并在录制里插入 debug
group；这些入口此前全是日志桩。本轮补齐 23 个：

* CommandBuffer：97/144（+23）

Metal 覆盖从 **248/513（48.3%）** 到 **271/513（52.8%）**，未实现 242。

### 1. Initialize / Finalize / IsRecording

`nvnCommandBufferInitialize` 在宿主侧登记对象并返回 1；设备未就绪或对象正在录制
时返回 0，而不是悄悄重置一个仍持有 Metal 命令缓冲的会话。`Finalize` 先检查是否
仍在录制：是则记一条日志并提交已经编码的工作，再删除宿主对象，避免 Metal 命令
缓冲永远不提交。

`IsRecording` 直接回答记录位：`BeginRecording` 之后为真、`EndRecording` 之后为假。

### 2. 命令内存与 control memory 只记账

回放后端把命令直接编码进 `MTLCommandBuffer`，从不写 guest 的命令内存。因此：

* `AddCommandMemory` / `AddControlMemory` 只累加 size，绝不写、也不解引用那段内存；
* `GetCommandMemorySize` / `Free` 返回累计值，`Used` 恒为 0——客体按“Free 快用完
  就结束录制”的习惯只会把整段留在一个 command buffer 里，正是这个后端想要的；
* pool 为空的调用是标题分配器耗尽后的“宿主补段”请求，接受并计入补段数，不解
  引用空指针（参考实现在这里崩过一次）；size 为 0 的调用直接忽略。
* 负数 offset、size 为 0、累加溢出都明确拒绝或饱和，并各记一条日志。

### 3. memory callback 只保存、不回调

`SetMemoryCallback` / `SetMemoryCallbackData` / `GetMemoryCallback` 存取指针。回调
永远不会被调用：这个后端不需要 guest 再补命令内存，假装“内存不够”会诱使客体重复
扩容。第一次注册时记一条日志说明这一点。

### 4. debug label / marker / group

名称从 guest 内存读取，最多 255 字节。录制期间：

* `SetDebugLabel` → `MTLCommandBuffer.label`
* `InsertDebugMarker{,Static,Dynamic}` → `insertDebugSignpost:`
* `PushDebugGroup{,Static,Dynamic}` → `pushDebugGroup:`，并压入宿主侧栈
* `PopDebugGroup` / `PopDebugGroupId` → `popDebugGroup:`；id 不匹配会记日志但仍弹
  一层，保证 Metal 的 push/pop 成对

空栈 pop 只记一次日志，不会下溢。每次 `BeginRecording` 清空 group 栈，并把 label
重新设到新的 `MTLCommandBuffer` 上。

第一次 CI（run `41183203391`）在 Xcode 16.4 上编译失败：`insertDebugSignpost:`
属于 `MTLCommandEncoder`，不是 `MTLCommandBuffer`。本地 stub 之前把它声明在后者
上，`syntax_check_metal` 因此放过了。现在 stub 按真实协议声明，marker 在 pass 内写
到当前 render encoder，pass 外用只放一个 signpost 的 blit encoder 承载。

### 5. 模拟器自测

新增失败码 `16426` 的回归：

1. Initialize 返回 1、IsRecording 为 0；
2. 两次 AddCommandMemory（4096 + 2048）与一次 AddControlMemory（64）后，
   Size / Used / Free 为 6144 / 0 / 6144，control size 为 64；
3. 空 pool 的补段调用被接受且不改变计数；
4. memory callback 地址原样取回；
5. 空栈 pop 被记录；
6. 开始录制，SetDebugLabel、两层 push、一个 marker、按 id / 普通各 pop 一次，
   EndRecording 后检查 `MTLCommandBufferStatusError`；
7. Finalize 删除对象，之后的 IsRecording 与查询不会把它复活。

这一组证明的是宿主侧记账与 Metal 调试通道，不是标题命令流。

### 6. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 271 implemented / 477 measured + 6 inferred）；GitHub
Actions run `41183353243` 的 macOS `ios` job 也已通过，模拟器输出确认：

```text
[selftest] lifecycle: initialize/is-recording, command and control memory
accounting, the memory callback and three debug entry points survived a commit
BOTW_SELFTEST rc=0
```

## M4 续十六：逐 draw 固定功能状态与 rasterizer discard

NVN 的固定功能状态里有一批 Metal 没有对应 API 的东西：点大小、线宽、全局采样
掩码、保守光栅、亚像素精度偏置、tiled cache、ZCull 提示、shader scratch。此前这些
入口全是日志桩。本轮补齐 13 个，并把其中真正能执行的 rasterizer discard 接进
draw 路径：

* CommandBuffer：110/144（+13）

Metal 覆盖从 **271/513（52.8%）** 到 **284/513（55.4%）**，未实现 229。

### 1. 记录并上报的状态

下面这些值在 `CmdBuf` 上记录、`BeginRecording` 时归零/复位，并在第一次遇到非默认
值时上报一次（`log_once`），不伪装成已经生效：

| 入口 | Metal 的现实 |
|---|---|
| `SetPointSize` | 点大小由顶点着色器的 `[[point_size]]` 输出决定 |
| `SetLineWidth` | Metal 的线宽固定 1 像素 |
| `SetSampleMask` | 没有全局 sample mask API |
| `SetConservativeRasterEnable/Dilate` | Metal 没有保守光栅 |
| `SetSubpixelPrecisionBias` | 没有对应能力 |
| `SetShaderScratchMemory` | Metal shader 不能寻址 scratch pool，值留给翻译器 |

tiled cache 的 action / tile size 与 ZCull 的 save / restore 是纯驱动提示，不改变
结果，因此只记录、不刷日志；ZCull 的 span 仍然检查是否为 guest 内存，坏指针会报
一次而不是让指针穿过。

### 2. primitive restart 只在固定值上成立

Metal 的 restart index 是固定的：16 位索引为 `0xFFFF`，32 位为 `0xFFFFFFFF`。
请求这两个值时行为与 NVN 一致，直接成立；请求别的索引、或试图关闭 restart，都
无法表达，记录并上报一次——静默忽略会让三角带被错误地缝合。

### 3. rasterizer discard 真正生效

`SetRasterizerDiscard(cb, 1)` 之后，`prepare_render_draw()` 在**管线检查之前**就把
draw 丢掉：

* `draws` 照常加一（客体确实要求了这次 draw）；
* `discardedDraws` 加一；
* `encoded` 不变，`prepare_render_draw` 返回 false，不会开 encoder、不会绑资源；
* 不会打出 `noPipeline` 之类的假错误。

关闭后下一次 draw 恢复正常路径。这样“被丢弃”和“缺 shader”在自测里是两个可区分
的计数。

### 4. 模拟器自测

新增失败码 `16427`：

1. 逐个调用 13 个 setter（浮点参数通过 `nvngfx_arg_set_f32` 放进 `nvngfx_args.f`）；
2. 直接读取 `CmdBuf` 的记录字段，核对 point size 2.5、line width 1.5、restart index
   `0xFFFFFFFF`、sample mask `0x00FF00FF`、conservative dilate 0.5、subpixel bias
   `(1, -2)`、tiled cache `3 / 64×64`、scratch `pool / 0 / 128`——空实现的 setter
   过不了这一步；
3. ZCull save / restore 接受一个 64 字节 span；
4. 开始录制、打开 rasterizer discard、draw 一次：`discardedDraws == 1`、
   `encoded == 0`、`draws == 1`；
5. 关闭 discard 再 draw：`discardedDraws` 仍为 1、`draws == 2`（这一次因为自测没有
   绑定 program 而停在管线检查）；
6. 提交后检查 `MTLCommandBufferStatusError`，再 Finalize。

这一组证明记录语义和 discard 的门控位置，不是标题画面。

### 5. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 284 implemented / 477 measured + 6 inferred）；GitHub
Actions run `41184203852` 的 macOS `ios` job 也已通过，模拟器输出确认：

```text
[selftest] state: point size, line width, sample mask, conservative raster,
subpixel bias, tiled cache and scratch memory were recorded, and rasterizer
discard consumed one draw
BOTW_SELFTEST rc=0
```

## M4 续十七：纹理对象查询、finalize 与失效

纹理侧此前只有 initialize / WriteTexels / WriteTexelsStrided / FlushTexels 四个入口，
其余 41 个全是日志桩。客体创建纹理后会查询宽高、格式、存储大小，也会在销毁时调
Finalize；查询返回 0 会让上层把纹理当成非法。本轮补齐 18 个：

* Texture：22/45（+18）

Metal 覆盖从 **284/513（55.4%）** 到 **302/513（58.9%）**，未实现 211。

### 1. getter 回答 builder 给过的值

`nvnTextureGetWidth/Height/Depth/Levels/Samples/Format/Target/Flags/MemoryPool/
MemoryOffset/Stride/DebugID` 都从宿主侧 `Tex` 记录返回，而那份记录正是
`nvnTextureBuilderSet*` 存下来的值，所以设置什么就取回什么，包括来自默认值的字段。
`GetTextureAddress` 返回 `pool + offset`（没有 pool 时为 0）。

`GetStorageSize` 用 `nvngfx_tex_layout` 重算整张纹理的字节数，和上传路径用的是同一份
布局代码；超过 `INT_MAX` 时饱和并上报一次。

`GetSwizzle` 有四个输出指针，逐个检查 guest 内存后写入 builder 的 R/G/B/A 值
（默认 2/3/4/5）。`GetDebugID` 是宿主分配的单调 id，不是从 guest 结构体里读的。

### 2. SetDebugLabel / Finalize / InvalidateTexels

* `SetDebugLabel` 存进 `Tex`，同时设到 `MTLTexture.label` 上，Xcode 捕获里能看见；
* `Finalize` 删除宿主 `Tex`。纹理池的槽位仍然保留，但解析时 `find<Tex>` 返回 null，
  拿着旧句柄的 draw 会被丢弃，而不是读已释放对象；
* `InvalidateTexels` 表示内容变成未定义。对本后端：只有 guest 池副本的纹理清掉一次性
  上传标志，下次采样重新上传（未定义数据允许这种选择）；已经渲染过的纹理保留 Metal
  内容，不会用陈旧的 guest 字节覆盖画面。部分区域不跟踪，按整张纹理处理并上报一次。

仍未接的纹理入口主要是 `ReadTexels*`（需要 GPU 回读并按 block-linear 重新编码）、
`GetStorageClass` / `GetSparseTileLayout` / `GetZCullStorageSize`（枚举或结构体语义未
核实）、`View*` 系列与 `Compare`。

### 3. 模拟器自测

新增失败码 `16428`：

1. 用 builder 建一张 4×4 RGBA8 纹理；
2. 逐个调 14 个 getter，核对 4/4/1、levels 1、samples 0（builder 没调 SetSamples，
   NVN 用 0 表示单采样）、format 0x25、target 0、flags 0、pool 0、offset 0、stride 0、
   非零 debug id、address 0、storage size 等于本地重算的 `nvngfx_tex_layout().size`；
3. `GetSwizzle` 的四个输出等于默认的 2/3/4/5；
4. `SetDebugLabel` 后 `Tex.label` 与 guest 字符串一致；
5. `InvalidateTexels` 把 `uploaded` / `uploadTried` 清零；
6. `Finalize` 之后 `find<Tex>` 为空，且再查宽高 / debug id 都返回 0。

这一组证明宿主侧查询与生命周期，不是标题纹理内容。

第一次跑这条回归（run `41184723718`）时，自测错把单采样期望成 `samples == 1`，
而 builder 默认值是 0；日志里 `samples 0` 与 `BOTW_SELFTEST rc=16428` 就是这次
不一致，断言已改为 0。

### 4. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 302 implemented / 477 measured + 6 inferred）。
随后 M4 续十八的 CI run `41185328486` 一并跑过这一轮的 16428 回归，macOS `ios`
job 的模拟器自测以 `BOTW_SELFTEST rc=0` 通过。

## M4 续十八：纹理 builder 查询

上一轮把纹理对象的 getter 接完了，但客体在 `nvnTextureInitialize` 之前也会查询 builder：
它用 `GetStorageSize` / `GetStorageAlignment` 决定从内存池里切多少字节，用宽高和
format 做校验。此前这些 14 个入口都是日志桩，返回 0 会让上层把纹理当成非法。
本轮补齐：

* TextureBuilder：28/43（+14）

Metal 覆盖从 **302/513（58.9%）** 到 **316/513（61.6%）**，未实现 197。

### 1. 回答 setter 记录的值，缺省回答 NVN 默认值

builder 是 guest 对象，本后端从不读它的内存布局；所有 getter 都查
`g_tex_specs[builder]`，也就是 `nvnTextureBuilderSet*` 写下的那份 `nvngfx_texdesc`。
没有条目的 builder（从未 SetDefaults / Set*）返回 `nvngfx_texdesc_defaults`：1×1、
1 级、swizzle R/G/B/A，而不是全零——全零会让 `GetLevels` 得到 0，比“未知”更糟。

覆盖的 getter：`GetWidth` / `GetHeight` / `GetDepth` / `GetLevels` / `GetSamples` /
`GetFormat` / `GetTarget` / `GetFlags` / `GetMemoryPool` / `GetMemoryOffset` /
`GetStride` / `GetSwizzle`。

### 2. storage size / alignment 与上传路径同源

`GetStorageSize` 和 `GetStorageAlignment` 调用 `nvngfx_tex_layout`，和
`nvnTextureInitialize` 建 Metal 纹理、`tex_upload()` 解块时用的是同一份布局代码。
客体按这两个数字切池，实际上传就不会越界或重叠。布局失败（未知 format）时返回 0。

未接的 builder 入口是 `GetDevice` / `GetPackagedTextureLayout` /
`GetRawStorageClass`（这三个在注册表里没有签名形状，不能安全绑定）、
`GetPackagedTextureData`（需要 packaged 格式语义）、`GetSparseTileLayout`（结构体布局
未核实）、`GetStorageClass` / `GetDepthStencilMode`（枚举值未核实）、
`GetGLTextureName` / `GetZCullStorageSize`，以及 7 个 setter。

### 3. 模拟器自测

新增失败码 `16429`：

1. 对一个 builder 设 format 0x25、8×4、3 级、1 采样、pool 0x1000 / offset 128、
   stride 256、flags 0x40；
2. 11 个标量 getter 与 storage size / alignment 逐个核对，size/alignment 与本地
   `nvngfx_tex_layout` 重算结果一致；
3. `GetSwizzle` 的四个输出为默认 2/3/4/5；
4. 对一个从未设置过的 builder，宽 / 高 / 级数为 1、format 为 0——证明缺省走的是
   NVN 默认值。

这一组证明 builder 侧查询，不是标题资源。

### 4. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 316 implemented / 477 measured + 6 inferred）。CI run
`41185328486` 的 `portable` 与 macOS `ios` job 均通过；模拟器执行新增的 16429
回归后写出 `BOTW_SELFTEST rc=0`，设备 Debug 构建与 Release 拒绝门禁也通过。

## M4 续十九：Queue / Sync / Event 对象

上一轮把纹理 builder 补齐后，客体在第一次 `nvnQueueInitialize` 时会先走 QueueBuilder：
它要问 command / compute / control 内存大小、flush 阈值和内存指针，再据此切池。此前这些
入口都是日志桩，返回 0 会让队列初始化直接失败。本轮把 QueueBuilder、Queue、Sync、Event
一起接上：

* QueueBuilder：19/19（+19）
* Queue：15/15（+14）
* Event：6/7（+6）
* Sync：4/6（+4）
* CommandBuffer：110/144 → 114/144（+4）

Metal 覆盖从 **316/513（61.6%）** 到 **363/513（70.8%）**，未实现 150。

### 1. QueueBuilder 的缺省值与内存总和

builder 仍是 guest 对象，宿主不读它的内存布局。setter 写进 `g_queue_specs[builder]`，
getter 原样回答；从未 `SetDefaults` 过的 builder 返回与 nxnative / D2R 一致的设备缺省：

* command 64 KiB（65536）
* compute 256 KiB（262144）
* control 16 KiB（16384）
* flush threshold 4 KiB（4096）

`GetQueueMemorySize` 返回 command + compute + control 之和向上按 4 KiB 对齐，缺省即
**344064** 字节。它不含 flush threshold，也不含 `SetQueueMemory` 给的那块客体存储——
前者是队列内部需要的总大小，后者是客体提供的后备内存，两者语义不同。

10 个新签名是推断的：9 个 getter 按 Set/Get 对称性，`SetQueuePriority` 按同族 setter
推断。`inferred_signatures.json` 从 6 涨到 16，`verify_ios.py` 单独计数；这些不是官方
头文件实测，绑定错误会在真机第一次调用时暴露，不能当成已验证原型。

### 2. Queue 生命周期、提交记账与 fence

* `Initialize` 记录 device，清零 submits / command / compute / control used；
* `Finalize` 删除宿主记录；`Flush` 是空操作（单队列，Metal 没有对应的 flush）；
* `SubmitCommands` 只做记账：submits 加一，按 command / control memory callback 的区间
  累加 used；不再重复提交 Metal 工作，因为 `EndRecording` 已经 commit；
* `AcquireTexture` 在 `Win::numTextures` 内轮转索引，并写进 `x[2]` 指向的 `int`；
* `ResetMemoryUsage` 清零三类 used；
* `QueueGetError` 返回 `GPU_NO_ERROR`；`NVNqueueErrorInfo` 布局未核实，故意不写
  out-struct；
* `FenceSync` 把 sync 绑到 `g_last_mtl_buffer`；
* `QueueFinish` 等待所有已知 CmdBuf 的最后 command buffer 和全局最后一个 buffer。
  present blit 的独立 command buffer 还没纳入，按缺口记。

### 3. EndRecording 的句柄语义变了

`nvnCommandBufferEndRecording` 现在返回 **guest command buffer 地址**，不再是 0。
`NVNcommandHandle` 本来就是客体用来标识这次提交的东西；返回 0 会让 `SubmitCommands`
拿到一个空句柄。Metal 工作仍在 EndRecording 时立即 commit，SubmitCommands 只记账，
不重复回放。

### 4. Sync：fence 与超时

`nvnSync` 保存最后一次 fence 的 `MTLCommandBuffer`：

* 已 fence 且 buffer 已完成 → `ALREADY_SIGNALED`（0）；
* buffer 出错 → `FAILED`（3）；
* timeout=0 → `TIMEOUT_EXPIRED`（2）；
* 有限 timeout → 1 ms 轮询到 deadline，完成返回 `SIGNALED`（1），否则超时；
* 无限 timeout（≥ 1 小时）→ `waitUntilCompleted`；
* 未 fence 的 sync 永远不会 signal：有限 timeout 睡到超时，无限 timeout 只睡 1 ms
  后明确返回超时，避免宿主永久挂死。

`nvnCommandBufferWaitEvent` / `WaitSync` 在单 Metal 队列里按录制顺序天然满足，因此记录
一次日志后直接返回；没有设备端计数器可轮询。

### 5. Event：CPU 可见点放在 GPU 完成之后

`nvnEventInitialize` 把 event 绑到 memory pool 的 `pool + offset`，要求至少 4 字节。
`EventSignal` 的 `mode == 1` 视为 add，否则 store。`CommandBufferSignalEvent` 不在录制
时写值，而是把 `{address, add, value}` 挂到 CmdBuf；`EndRecording` 在 commit 前注册
`addCompletedHandler`，GPU 完成后才写回 guest 内存。自测最终读到 9，证明这条回调真的
执行了。

### 6. 模拟器自测

新增失败码 `16430`：

1. 19 个 builder 入口：缺省值 65536 / 262144 / 16384 / 4096 / 344064，逐个 setter 后
   再回读；
2. Queue initialize 记录 device 与 debug label；submit 后 submits=1、commandUsed=0x1000、
   controlUsed=0x400；reset 后归零；
3. Event initialize 到 pool + offset，CPU signal 后 GetValue，CommandBufferSignalEvent
   后 GPU 完成再读到 9；
4. Sync initialize / debug label / fenced wait / unfenced wait 的返回码；
5. EndRecording 返回 guest command buffer 地址；SubmitCommands 记账；
6. AcquireTexture 两次返回 0、1 的轮转；
7. Finalize 后四类对象都从表里消失。

这一组证明队列/同步对象的宿主记账与完成回调，不是标题的真实多队列负载。

### 7. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 363 implemented / 477 measured + 16 inferred）。CI run
`41187102748` 的 `portable` 与 macOS `ios` job 均通过；模拟器执行新增的 16430 回归后
写出 `BOTW_SELFTEST rc=0`，设备 Debug 构建与 Release 拒绝门禁也通过。
## M4 续二十：六类固定功能状态 getter

上一轮接完 Queue / Sync / Event 后，客体已经能建队列，但状态对象仍然只能写、不能读。
NVN 允许游戏把 `NVNblendState` / `NVNcolorState` 等小对象按值拷贝后再查询，所以 getter
必须从 guest 自己的字节里解包回答，不能靠宿主侧旁表。本轮补齐：

* BlendState：4/15 → 13/15（+9：6 个 getter + 3 个 advanced setter）
* MultisampleState：12/23 → 22/23（+10）
* DepthStencilState：7/13 → 13/13（+6）
* PolygonState：5/9 → 9/9（+4）
* ColorState：4/7 → 7/7（+3）
* ChannelMaskState：2/3 → 3/3（+1）

Metal 覆盖从 **363/513（70.8%）** 到 **396/513（77.2%）**，未实现 117。

### 1. 先进混合的真实位布局

nxnative 的实测 `nvn_obj.h`（MIT）把 advanced blend 放在同一个 64 位对象里：

```text
target:3  src_rgb:7  dst_rgb:7  src_a:7  dst_a:7  eq_rgb:3  eq_a:3
adv:6 (bit 37)   adv_npm:1 (bit 43)   adv_ovl:2 (bit 44)
```

`adv_npm` 存的是 `SetAdvancedPremultipliedSrc` 的**反值**，所以
`GetAdvancedPremultipliedSrc` 要再取反；默认 0 表示“源已预乘”。
`SetAdvancedMode` 对大于 `0x32`（`HSL_LUMINOSITY`）的值回落为 0，
`SetAdvancedOverlap` 只保留低 2 位。这三个 setter 与三个 getter 都已接上。

`Get/SetAdvancedNormalizedDst` 仍然不绑定：nxnative 的实测布局和 HLE 里都没有这个
字段，公开原型只给了名字，返回任何值都是猜的。

### 2. getter 直接从 guest 对象解包

状态对象仍是 guest 的 4 / 8 / 10 字节存储。getter 走既有的
`read_blend` / `read_color` / `read_channel` / `read_multisample` /
`read_polygon` / `read_depth`，先解包再回答，没有地址键控的旁表，因此客体 memcpy
或复制状态对象后不会读到陈旧值。

* 输出指针为 null 时跳过，不解引用；
* `NVNboolean` 统一写成 0/1；
* 双面 stencil：setter 仍接受 face=3 同时更新正面和背面；getter 跟 nxnative 一致，
  只有 `NVN_FACE_BACK`（2）才取背面，其余取正面；
* Multisample 的 10 个单字节字段逐个回读；`GetSampleLocationsGrid` 不绑定，因为
  10 字节状态里只有 grid-enable 字节，没有网格宽高，公开实现也没有说明两个 `int*`
  应该收到什么。

### 3. 模拟器自测

新增失败码 `16431`：

1. 六类状态先 `SetDefaults`，核对默认值（blend ONE/ZERO/ADD、color COPY/ALWAYS、
   channel 全开、multisample 仅 enable=1、polygon 无剔除/CCW/FILL、depth 写开/LESS/
   stencil ALWAYS+KEEP）；
2. blend 设 target 3、func 5/6/7/8、equation 2/3，再设 advanced SCREEN（0x16）、
   非预乘源、conjoined，逐个 getter 回读；
3. color：target 3 开 blend、target 2 显式关闭，另设 logic op 0xC、alpha test 7；
4. channel mask：target 1 只开 R/B，读回 1/0/1/0；
5. multisample：10 个字节设成不同值后逐个回读；
6. polygon：背面剔除、CW、LINE、offset 5；
7. depth/stencil：深度测试开、写关、func 3、stencil 测试开，正面 func 2 / 背面 4，
   正面 op 2/3/4、背面 5/6/7，分别回读。

这一组证明状态对象的 setter/getter 回环；advanced blend 目前只记录，还没有翻译成
Metal 的 blend operation，不能据此声称高级混合已经能画。

### 4. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 396 implemented / 477 measured + 16 inferred）。
CI run `41188353513` 的 `portable` 与 macOS `ios` job 均成功；模拟器自测包含新增的
16431 回归（失败会以 rc=16431 退出并使 job 失败），设备 Debug 构建与 Release 拒绝
门禁也通过。

## M4 续二十一：NVNtextureView 状态对象

上一轮把六类固定功能状态的 getter 接上后，纹理侧下一个低风险缺口是
`NVNtextureView`：游戏会在栈上放一个 view，先 `SetDefaults`，再按需设
level / layer / format / target，然后把它传给采样或渲染目标。此前这些入口
全部是日志桩，`Get*` 也返回 0，guest 只要读一次就会把“没有 view”当成
“view 的 level 0 / 整个纹理”。

本轮补齐 14 个入口：

* 7 个 setter：`SetDefaults`、`SetLevels`、`SetLayers`、`SetFormat`、
  `SetTarget`、`SetSwizzle`、`SetDepthStencilMode`；
* 6 个 getter：`GetLevels`、`GetLayers`、`GetFormat`、`GetTarget`、
  `GetSwizzle`、`GetDepthStencilMode`；
* `nvnTextureViewCompare`。

Metal 覆盖从 **396/513（77.2%）** 到 **410/513（79.9%）**，未实现 103。

### 1. 16 字节实测布局

布局来自 nxnative（MIT）的 `dm/gfx/nvn/nvn_obj.h`，不是猜的：

```text
byte 0: bit0 set_levels  bit1 set_layers  bit2 set_format
        bit3 set_target  bit4 set_swizzle bit5 set_dsmode
        bit6-7 dsmode
byte 1: target
byte 2: base_level
byte 3: num_levels
byte 4-5: format (u16)
byte 6-7: min_layer (u16)
byte 8-9: num_layers (u16)
byte 10-13: swizzle[4]
byte 14-15: reserved
```

setter 直接写 guest 的 16 字节对象，getter 从 guest 字节解包，没有地址键控
旁表；因此客体 memcpy 一个 view 后仍然能读到正确值。未设置 swizzle 时，
getter 返回 identity `2/3/4/5`（R/G/B/A）。

### 2. Compare 只比较有效字段

`nvnTextureViewCompare` 比较 flags、target、base/number of levels、
format、min/number of layers 与 swizzle，不比较 reserved 字节。这样处理是
保守选择：guest 可能把一个未完整初始化的 view 原样拷贝，reserved 字节不同
不应让两个语义相同的 view 判为不同。Nintendo 的精确语义未核实，文档与注释
都按“字段比较”描述，没有声称与官方逐字节 memcmp 一致。

### 3. 模拟器自测

新增失败码 `16432`：

1. `SetDefaults` 把 16 字节全部清零；
2. 默认 getter 返回 0 / 0 / 0 / 0 / identity swizzle / dsmode 0；
3. 六个 setter 全部置位后，byte 0 必须是 `0x7F`；
4. 六个 getter 逐个回读 level / layer / format / target / swizzle / dsmode；
5. 四个 swizzle 输出指针全为 null 时跳过，不解引用；
6. 相同副本比较为真；只改 reserved 字节仍为真；改 base level 后为假。

### 4. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 410 implemented / 477 measured + 16 inferred）。
CI run `41189358752` 的 `portable` 与 macOS `ios` job 均成功；模拟器自测包含
16432 回归，设备 Debug 构建与 Release 拒绝门禁也通过。

### 5. 仍然没有闭环的部分

view 状态对象本身可读写了，但通用 view 绑定还没有把 `swizzle` 应用到采样，
也没有把 depth-stencil mode 应用到深度/模板采样；渲染目标的 view 参数仍只
记日志。这一轮不能声称“通用 texture view 已经闭环”。

## M4 续二十二：纹理 builder / 对象状态、view offset 与 compare

续二十一之后，texture builder 仍缺 device / swizzle / GL 名称 /
depth-stencil mode 的读写，texture 对象也缺 `GetViewOffset` 与 `Compare`。
这些入口不碰 GPU 像素，只回答 guest 自己写进 builder/object 的值，属于低风险
补齐；本轮新增 12 个入口：

* `nvnTextureBuilderSetDevice` / `GetDevice`（后者不在公开原型表里，按
  `nvnBufferBuilderGetDevice` 的命名对称性推断，单独记入
  `inferred_signatures.json`，推断总数 16 → 17）；
* `nvnTextureBuilderSetSwizzle`（四路写入 `swz[4]`，低 3 位，与 nxnative 一致）；
* `nvnTextureBuilderSetGLTextureName` / `GetGLTextureName`；
* `nvnTextureBuilderSetDepthStencilMode` / `GetDepthStencilMode` 与
  `nvnTextureGetDepthStencilMode`；
* `nvnTextureGetViewOffset`；
* `nvnTextureCompare`；
* `nvnTexturePoolGetMemoryPool` 与 `nvnSamplerPoolGetMemoryPool`。

Metal 覆盖从 **410/513（79.9%）** 到 **422/513（82.3%）**，未实现 91。

### 1. Builder 的新字段

`nvngfx_texdesc` 增加三个字段：

* `device`：`SetDevice` 记录；`SetDefaults` 会重置描述符但**保留** device，
  与 nxnative 的 builder 行为一致；
* `gl_name`：原样保存 32 位值。Switch NVN 没有 GL，后端也没有 GL interop，
  getter 只是把 setter 的值还回去，不承诺背后存在一个 GL 纹理；
* `dsmode`：0 = depth，1 = stencil；其他值按 depth 记录，和 nxnative 的
  布尔语义一致。这个字段不影响存储布局，只说明采样时读哪个平面。

SWIZZLE 的 NVN 通道枚举是 0 zero / 1 one / 2 R / 3 G / 4 B / 5 A，
setter 存低 3 位；未调用 setter 时默认仍是 identity `2/3/4/5`。

### 2. GetViewOffset 的公式与边界

`nvnTextureGetViewOffset(texture, view)` 返回 view 第一个 texel 相对纹理存储
起点的字节偏移。公式与 nxnative 相同：

```text
offset = (view.set_levels ? nvngfx_tex_level_offset(desc, base_level) : 0)
       + (view.set_layers ? min_layer * layout.layer_size : 0)
```

`layer_size` 来自 `nvngfx_tex_layout`，即一整层所有 mip 的字节数。view 为
null 时返回 0。与 nxnative 不同的是，本实现对越界的 `base_level` /
`min_layer` 返回 0 并打一条一次性日志，而不是继续算出存储外的偏移；这是
保守选择，避免 guest 拿到一个必然越界的地址。

### 3. nvnTextureCompare 的字段集

比较以下字段：rank、target、format、flags、samples、width、height、depth、
levels、pool、offset、stride、四路 swizzle 与 packaged block height。
device、GL 名称、depth-stencil mode、debug id、label 与 MTLTexture 都是
宿主侧元数据，不参与比较。字段集参考 nxnative 的 `nvnTextureCompare`（描述符
+ 存储身份 + 通道映射），Nintendo 的精确语义未核实。只有后端初始化过的两个
对象才能比较；未知对象返回 0。

### 4. 模拟器自测

新增失败码 `16433`：

1. `SetDevice(0xB0B0)` 后 `SetDefaults`，`GetDevice` 仍返回 `0xB0B0`；
2. swizzle 设 `5/4/3/2`、dsmode 设 stencil、GL 名称设 `0x1234ABCD`，
   builder getter 逐个回读；
3. 用同一个 builder 建 8×4、两层、三 mip 的 RGBA8 2D array，texture 对象的
   `GetSwizzle` / `GetDepthStencilMode` 与 builder 一致；
4. view 设 level 1 + layer 1，`GetViewOffset` 必须等于
   `level_offset(1) + layer_size`；无 view 与无 flag 的 view 返回 0；越界
   level 3 返回 0；
5. 同一 builder 再建一个对象，`Compare` 为真；改成 identity swizzle 再建，
   `Compare` 为假；与未初始化地址比较为假；
6. texture / sampler descriptor pool 用已知值重新 Initialize，
   `GetMemoryPool` 分别回读该值。

### 5. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 422 implemented / 477 measured + 17 inferred）。
本轮 CI 结果在推送后补记。

### 6. 仍然没有闭环的部分

GL 名称没有 interop；view 的 swizzle/dsmode 仍未应用到实际采样；`Compare`
与 `GetViewOffset` 的语义来自公开实现而非 Nintendo 文档。真实标题的 draw
仍是 0 条，不能据此声称游戏已经能在 iPhone 上运行。

### 6. CI 反馈与修复

run `41190680145` 的 `portable` job 成功，但 macOS `ios` job 在模拟器自测阶段
以 `BOTW_SELFTEST rc=16433` 失败。原因不在产品逻辑：本轮新增自测错误地把
`nvnTextureBuilderGetSwizzle` 和 `nvnTextureGetSwizzle` 的 **void** 返回值
当作 `NVNboolean` 读取，断言了 `x[0]`。提交 `f0d2e35` 只修自测，调用后改为
只检查四个 swizzle 输出指针。

修复推送后的 run `41191476518` 的 `portable` 与 macOS `ios` job 均成功；模拟器
自测通过 16433 回归，设备 Debug 构建与 Release 拒绝门禁也通过。这个修复不改变
texture builder / view 状态的产品代码，也不改变 422 implemented 的覆盖数字。

## M4 续二十三：Window / WindowBuilder 状态

Window 侧此前只实现了少数 setter，builder 和 window 的查询入口仍会落到日志桩。
本轮补齐 13 个低风险入口，不触碰 present 像素：

* WindowBuilder：`SetDevice` / `GetDevice`、`GetNativeWindow`、
  `GetPresentInterval`、`GetNumTextures`、`GetNumActiveTextures`、
  `GetTexture`；
* Window：`GetNumActiveTextures`、`GetNativeWindow`、`GetPresentInterval`、
  `SetDebugLabel`、`SetCrop` / `GetCrop`。

Metal 覆盖从 **422/513（82.3%）** 到 **435/513（84.8%）**，未实现 78。
推断签名从 17 增加到 22；实测签名仍为 477，max arity 8。

### 1. Builder 默认值与持久字段

`nvnWindowBuilderSetDefaults` 清空记录的纹理列表、native window 和 active
count，把 present interval 恢复为 1，但保留先前 `SetDevice` 记录的 device。
这与 nxnative 的 builder 行为一致，也避免复用的 builder 把上一个窗口的
交换链纹理带进来。

builder getter 从 setter 记录的 `WinSpec` 回读；没有调用过 `SetDefaults` 的
builder 按默认状态回答：interval 1、无纹理。`GetTexture` 对负索引和越界索引
返回 0。`nvnWindowBuilderGetDevice`、`GetNumTextures`、`GetNumActiveTextures`
和 `GetTexture` 不在公开原型表中，签名按 setter/兄弟入口推断，并单独记入
`inferred_signatures.json`，没有混进实测集合。

### 2. Window 状态与 crop 边界

`nvnWindowInitialize` 现在把 builder 的 native window、纹理列表、active count
和 present interval 复制到 window 对象；复用同一个 window 地址时会重置
crop、debug label 和交换链游标。window getter 回读这些记录值。

`nvnWindowSetCrop` 记录 `x/y/width/height`，`nvnWindowGetCrop` 在没有设置过
有效 crop 时返回第一个交换链纹理的尺寸（测试纹理为 64×64，因此默认值是
`0,0,64,64`）。**crop 目前只记录，present blit 还没有消费它**，不能声称
窗口裁剪已经真正影响上屏。debug label 也只保存在宿主对象上，CAMetalLayer
没有对应的跨帧调试名称。

### 3. 模拟器自测

新增失败码 `16434`，覆盖：

1. 未默认化的 builder 返回 interval 1、无纹理；
2. `SetDevice` 后 `SetDefaults` 保留 device、清空纹理并把 interval 归 1；
3. native window、present interval、纹理列表、active count 与逐个纹理索引回读；
4. window 初始化后的 getter 回读；
5. crop 的 `1,2,3,4` 往返与未设置时的 `0,0,64,64` 默认值；
6. debug label 存储和读取。

### 4. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 435 implemented / 477 measured + 22 inferred）。
Window 批次的 CI run `41192204572` 的 `portable` 与 macOS `ios` job 均成功；
模拟器自测通过 16434 回归，设备 Debug 构建与 Release 拒绝门禁也通过。

### 5. 仍然没有闭环的部分

窗口 crop 没有进入 present blit，native window 仍不能被当作 CAMetalLayer
使用，present interval 也只在 guest 侧记录；这次不能声称“窗口呈现链路已经
闭环”。真实标题的 draw 仍为 0 条，核心的 Maxwell SASS → MSL 离线翻译和
通用 texture view 绑定仍未完成。

## M4 续二十四：Program 状态、扩展 shader 记录与 sampler 查询

Window 批次之后，Program 只剩日志桩的入口已经集中在状态记录上。本轮补齐
5 个低风险入口：

* `nvnProgramSetDebugLabel`：把标签保存在 Program 对象上；
* `nvnProgramSetShadersExt`：把每 stage 0x18 字节的
  `{data, control, ext}` 转成普通 `NVNshaderData`，再复用现有
  `SetShaders`；
* `nvnProgramSetSampleShading`：记录 mode 并返回 true；
* `nvnSamplerBuilderGetDevice`：从 builder 侧表回读 device；
* `nvnSamplerGetDebugID`：返回宿主在 `NVNsamplerInitialize` 时分配的
  非零 debug id。

Metal 覆盖从 **435/513（84.8%）** 到 **440/513（85.8%）**，未实现 73。
推断签名从 22 增加到 25；实测签名仍为 477，max arity 8。Program 达到
**6/7**，只剩 `nvnProgramSetSubroutineLinkage`；Sampler 达到 **15/15**，
SamplerBuilder 达到 **25/25**。

### 1. SetShadersExt 的实际转换

扩展记录的布局来自公开 nxnative HLE：

```text
每个 stage 24 字节：
  0x00  NVNbufferAddress data
  0x08  const void*      control
  0x10  uint64_t         ext
```

本后端不消费 `ext`，但会保留 data/control 并打一条一次性日志。当前
`Program` 只有 3 个 stage 槽，因此入口接受 0..3；超过 3 仍明确返回失败，
没有假装支持全部六个 NVN stage。该入口不在公开原型表中，签名记入
`inferred_signatures.json`。

### 2. SampleShading 的诚实边界

`nvnProgramSetSampleShading(program, mode)` 的 mode 被记录，并返回
`NVNboolean` true，避免标题把程序判为不可用。**Metal 没有与 NVN mode
一一对应的独立开关**：fragment shader 只有在读取 sample id 或按 sample
插值时才会逐 sample 执行。因此本轮没有把 mode 翻译成管线状态，也没有声称
多重采样着色已经等价。自测只验证记录与返回值。

### 3. Sampler 状态

`nvnSamplerBuilderSetDefaults` 已有的行为保持不变：清空采样器状态，但保留
builder 的 device；新增 getter 回读该 device。`nvnSamplerInitialize` 分配
宿主侧 debug id（从 0x30000 起递增），`nvnSamplerGetDebugID` 在对象存在时
返回它，`Finalize` 后返回 0。这与 buffer / texture 已有的 debug id 语义一致，
不是 guest 自己写入的字段。

### 4. 模拟器自测

新增失败码 `16435`，覆盖：

1. Program 初始化、debug label 存储、sample shading mode 回读；
2. 24 字节扩展 shader 记录转换后 `code/control` 与输入一致；
3. Program Finalize 移除对象；
4. sampler builder 的 SetDevice / SetDefaults / GetDevice：defaults 后
   device 仍为原值；
5. sampler 初始化后 debug id 非零，Finalize 后 getter 返回 0。

### 5. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 440 implemented / 477 measured + 25 inferred）。
本批 CI 结果在推送后补记。

### 6. 仍然没有闭环的部分

`nvnProgramSetShadersExt` 只处理最多 3 个 stage；`SetSampleShading` 没有
改变 Metal 的逐样本行为；sampler debug id 是宿主命名，不是官方语义的精确
复刻。真实标题的 draw 仍为 0 条，Maxwell SASS → MSL 离线翻译、真实 shader
绑定和通用 texture view 采样仍未闭环。

## M4 续二十五：Device 状态、帧计数器与 packaged texture 布局

Device 域此前只有 7 个入口，是所有域里最薄的一块。标题每次启动会调用
`nvnDeviceGetInteger` 十多万次（nxnative 的 1.9.0 usage 里是 123,219 次），
用返回值算自己的池大小、对齐和纹理 tile 高度；`GetTimestampInNanoseconds`
和 `ReportCounter` 则被帧间隔控制与动态分辨率读取。少了这些，标题即使画
出第一帧也会立刻用错误的常量继续配置。

本轮补 26 个入口：Device 20 个，CommandBuffer 3 个，Texture 1 个，
TextureBuilder 2 个。Metal 覆盖从 **440/513（85.8%）** 到
**466/513（90.8%）**，未实现 47。Device 达到 **27/27**，CommandBuffer
117/144，Texture 40/45，TextureBuilder 37/43。实测签名仍为 477，推断 25，
max arity 8——这批入口都在公开原型表里。

### 1. Device 生命周期与调试入口

* `nvnDeviceFinalize` / `ApplyDeferredFinalizes` / `FinalizeCommandHandle`：
  本后端在 finalize buffer/texture 时就地释放，没有延迟队列，因此前两个只
  记录次数，第三个是空操作。这三者不假装有异步回收。
* `nvnDeviceGenerateDebugDomainId`：按域名分配稳定且互不相同的 id，同名
  重复调用返回同一个值。
* `nvnDeviceInstallDebugCallback` / `WalkDebugDatabase`：回调是 **guest 函数
  指针**，宿主直接调用会跳进模拟代码而没有寄存器与栈准备，所以只记录参数，
  并统计该 type 下已登记标签的对象数量。
* `nvnDeviceRegisterFastClearColor(ui/i)` / `RegisterFastClearDepth`：Tegra
  驱动的优化提示。真正的清屏仍然走 `ClearColor` / `ClearDepthStencil`
  传进来的值，因此只记录调用并返回 true，不消费注册的颜色。
* 每个对象（window / memory pool / texture pool / sampler pool / program /
  command buffer / sync / queue / device）的 `SetDebugLabel` 现在同时写进
  宿主侧的 `g_debug_objects` 表，`WalkDebugDatabase` 才有东西可数。

### 2. GetInteger 的属性表

`nvnDeviceGetInteger(device, what, out*)` 用一张 0x60 项的静态表回答，
数据取自 nxnative 的 Tegra X1 profile（MIT）。索引 0/1 是 API 版本
**55.15**，`0x67`（部分新标题会问的 shader code 对齐）单独返回 256，
表外的 `what` 打一条一次性日志后返回 0。

必须说清两件事：这张表描述的是 **NVN 在 Tegra X1 上的能力**，不是 Metal 的
能力，也不是本后端的实际限制；55.15 来自 **BOTW 1.9.0 / SDK 20** 的
profile，本项目的目标是 1.6.0，是否同样报 55.15 还没有实机或私有运行日志
可以确认。表里数值若有偏差，最可能的症状是标题按错误的 stride/tile 高度
配置池，而不是直接报错。

### 3. 时间戳与帧计数器

NVN 的 counter 时间戳是 614.4 MHz tick（384 tick = 625 ns），不是纳秒。
两个方向都保留在同一个单位下互换：

```text
ticks_from_ns(ns) = (ns / 625) * 384 + (ns % 625) * 384 / 625
ns_from_ticks(t)  = (t / 384) * 625 + (t % 384) * 625 / 384
```

这样 `ReportCounter` 写进去的值可以直接喂给
`nvnDeviceGetTimestampInNanoseconds`，也能和标题自己算出的差值比较。算术
拆成两段避免 MSVC 上没有 `__int128` 导致的 64 位溢出。

`nvnCommandBufferReportCounter(cb, type, address)` 把 16 字节
`{counter, timestamp}` 记在 pending 列表里，`EndRecording` 提交 Metal
command buffer 时挂 completion handler 写回 guest 内存，所以 guest 在 GPU
完成之前读不到时间戳，与 NVN 的可见性顺序一致。类型 `0x10`
（TIMESTAMP_TOP）填 BeginRecording 时刻，其余 timestamp 填 EndRecording
时刻。`nvnCommandBufferReportValue` 复用同一完成路径写 32 位值，
`nvnCommandBufferResetCounter` 只把类型记进 64 位掩码，供后续诊断。

**这些是宿主 `steady_clock` 的提交时刻，不是 Metal GPU 时间。** 真实 GPU
时间戳需要 `MTLCounterSampleBuffer` / MTL4 counter heap，本轮没有接。因此
帧间隔控制器看到的是“CPU 提交到 CPU 提交”的间隔。它足够让启动流程走下去，
但不能当作性能测量的依据。

samples-passed 之类的计数目前一律写 0：没有 visibility query，写 0 等价于
“没有样本通过”。这**不是** occlusion query 已实现的说法，日志里对每个
非时间戳类型只报一次。

### 4. Packaged texture layout 与 ZCull

`nvnTextureBuilderSetPackagedTextureLayout(builder, const void*)` 只读第一个
字节：`0xFF` 视为“未打包”，其余值 +1 后存进 `nvngfx_texdesc::pkg_block_h`，
再由 `nvgfx_tex_layout` 拆成低半字节的 tile height、高二进制的 tile depth。
用 +1 是因为 0 已经被“从未调用”占用，而 0 是合法的 block height 编码。

`nvnTextureBuilderGetZCullStorageSize` 与 `nvnTextureGetZCullStorageSize`
返回 0。Metal 不为 ZCull 预留存储，0 是真实答案而不是占位符；调用方通常
只用它算池偏移，返回 0 不会破坏后续分配。

### 5. 模拟器自测

新增失败码 `16436`，覆盖：

1. `Initialize` 建立的默认 origin/depth mode，setter/getter 回环；
2. `ApplyDeferredFinalizes` / `FinalizeCommandHandle` / `SetIntermediateShaderCache`
   / `IsExternalDebuggerAttached` 的记录值与返回值；
3. `GetInteger` 的 55.15、单项属性与未知 `what` 返回 0；
4. `GetCurrentTimestampInNanoseconds` 单调递增，`384 tick -> 625 ns`；
5. debug domain id 同名复用、不同名递增；
6. fast-clear 注册计数与最后格式；
7. packaged layout 首字节 3 -> `pkg_block_h = 4` -> `tile_h = 3`；
8. 两个 ZCull 入口返回 0；
9. `ReportCounter`（类型 0x10 与 0）、`ReportValue`、`ResetCounter` 在
   `waitUntilCompleted` 之后回读：类型字段为 0、时间戳非 0、底部的
   时间戳不早于顶部、值等于写入的 `0x12345678`、重置掩码置位第 0 位。

所有 GPU 写回都在 `waitUntilCompleted` 之后检查，不靠睡眠猜时序。

### 6. 门禁

```text
python scripts/syntax_check_metal.py
python scripts/gen_nvn.py --repo . --check
python scripts/extract_nvn_signatures.py --check
python scripts/verify_ios.py
powershell -ExecutionPolicy Bypass -File scripts/build_abi_test.ps1
git diff --check
```

本地已通过（513 procs / 466 implemented / 477 measured + 25 inferred）。
推送后 CI run `41193853309` 的 `portable`（job 124515942326）与 `ios`
（job 124516016377）两个 job 均成功，模拟器里 `16436` 全部通过。

### 7. 仍然没有闭环的部分

`GetInteger` 的属性表是 1.9.0 profile 的移植值，1.6.0 未确认；时间戳是宿主
提交时刻而非 GPU 时间；samples-passed 等计数恒为 0；`SetWindowOriginMode` /
`SetDepthMode` 只记录，尚未改变 Metal 渲染路径（标题设的是 upper-left，
与 Metal 默认一致，但仍需验证）；fast-clear 注册不改变清屏路径；debug
callback 与 walk callback 不会被宿主调用。真实标题的 draw 依然为 0 条，
Maxwell SASS -> MSL 翻译器仍未开始。

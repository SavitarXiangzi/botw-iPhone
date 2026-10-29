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

### M3.16 当前卡点：空闲链表里的块被纹理解码覆盖

无钩子跑到 175 帧后停在 `main+0x125B1D4`（`str x13, [x15, #8]`，
`x15=0x32577E6E6871858B`）。`x19=0x19F190170` 是游戏自己的分配器，
`x8=0x1A026D000` 是它从最佳适配链表里挑出的块。对 `0x1A026D000` 这一页做写入
监视，时间线非常干净：

| 时刻 | 写入者 | 动作 |
|---|---|---|
| t=82.0 s | `main+0x125B0D0` | 分配器在 `0x1A026D000` 写**空闲块头**（size `0x26B1AFE0`，flag `0xFFFF`） |
| t=82.1 s | `main+0x125B2E0` | 把该块**链入**空闲链表（`prev=next=0x19F190250` 表头） |
| t=96.6 s | `main+0x181D9E4` | **纹理解码器**（`fcvt`+`strb` 逐字节，浮点 clamp 到 [0,1]，典型法线图解码）把整块覆盖成像素数据 |
| t=111 s | `main+0x125B1A8/1D4` | 分配器再次扫描空闲链表、挑中这个块，取 `[x8]`/`[x8+8]` 当 prev/next 去摘链 → 野指针 → FAULT |

也就是说：**一块还在空闲链表里的内存，被一个已经拿到的纹理缓冲区覆盖了**。
两种可能，下一步二分：

1. 更早的空闲链表中被插入过重复块 / 尺寸错乱（分配器元数据先行损坏）
2. 解码器的目标指针（`main+0x181DCA8` 的返回值链 `0x4180C49C` → `0x418357F4`
   → `0x40E3AA00`）来自我们某个 HLE 返回的错误指针，落进了空闲块里

验证手段已经就位：`_probe_watch4.py` 那一套"只监视某 64 字节 + 打时间戳和 PC"，
把监视窗口挪到解码器缓冲区的起点，就能看到它到底是从哪次分配拿到的。

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

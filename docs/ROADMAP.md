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

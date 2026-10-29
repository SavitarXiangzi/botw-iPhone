# HLE 边界与实现约定

## 边界在哪

**有两条边界，按代价从低到高选择**：

1. **HLE**：把“游戏调用 SDK 的第一个 `nn::*` 函数”整段替换掉（导入表符号）。
   便宜，但绕过了 SDK 的校验/序列化，容易和真机行为分叉。
2. **服务端（IPC/CMIF）**：guest 的 `nn::sf` 序列化层照常运行，只接管
   `svcSendSyncRequest` 并把“对端服务”用 Python 实现（`m2_svc.py`）。
   贵一点，但一条通路服务所有命令，且参数校验、句柄语义都与真机一致。

本轮起，能用第 2 种就用第 2 种；HLE 只留给“SDK 内部帮手 / 无法用服务表达”的东西。

宿主实现方式：

```
函数首条指令  ->  svc #(0x100 + slot)      ; 静态表
动态入口      ->  0xA000000 起的 stub arena ; NVN 按名字取函数时用
```

## 三个必须记住的坑

1. **SVC 异常时 PC 已经是 svc+4**。stub arena 是连续 svc，按 `pc` 找会把调用派发到下一个桩。
   必须用“最后实际执行的 PC”。
2. **符号表有两套地址**（shndx 1 / 2），其中一套常落在 `udf` 填充区。
   SDK 内部互相调用走 `BL` 直连，所以每个合法地址都要打补丁。
3. **`symbols.json` 里 `shndx==0` 是未定义导入**，必须过滤，否则会命中 `main+0`。

## 已接管的 SDK 内部（非公开 API）

这些不是游戏调用的，而是 SDK 自己会断言的地方，必须一起接管：

| 函数 | 原因 |
|---|---|
| `ThreadManagerImplByHorizon::MapAliasStack` | `svcMapMemory` 物理页别名，Unicorn 无此能力 |
| `nn::lm::Initialize` / `Finalize` | 日志库依赖 `lm` 服务，无 IPC 时构造失败 |
| `OsResourceManager` 构造 | 依赖 `svcGetInfo(12/13)` 的地址空间基址/大小 |

## 已知技术债

* **页别名**：Fiber 真正跑起来时需要（复制 + 回写，或自建页表）。当前是“假装成功”。
* **线程调度**：协作式，只在 SVC 边界换上下文，没有抢占（也没有真时间片）。
* **`AddressRegionManager` 基址**：当前拿到垃圾值（`0x2`），待补 `svcGetInfo` 其余 id。
* **NVN 纹理存储大小**：`_tex_size()` 用"线性 pitch 64B 对齐"近似，真实 NVN 是
  block-linear 布局；等 M4 接后端时要换成真布局，否则上传的数据是错的。
* **命令内存用完**：`nvnCommandBufferSetMemoryCallback` 只记了回调地址，
  没有真的回调 guest 申请新段；目前靠 0x30000/帧 够用（实测只用 416 字节）。
* **present 未实现**：`nvnQueuePresentTexture` 一次都没被调用；即便被调用，现在也只记一条
  `OP_PRESENT`，没有交换链。
* **符号表两套地址**：`symbols.json` 里每个模块有两张重叠的表（shndx 1/2），
  查符号要和 `valid_entry()` 一起用，否则会落到 `udf` 填充上。


## 已踩过的第 4 个坑：`svcGetInfo` 的值也要放 x1

SDK 的包装器长这样：

```asm
str x0,[sp,#-0x10]!
svc #0x29
ldr x2,[sp]
str w1,[x2]        ; 把 x1 的低 32 位写回调用者的 out 指针
ret
```

也就是说内核既要写 out 内存，**也要**把值放在 `x1`。只写内存的话，低 32 位会被残留的
`x1` 覆盖——`nn::os::detail::VammManager` 就是因此拿到 `base=0x2`，
进而 `AddressRegionManager` 用 2 当堆地址、`CreateUnitHeap` 写 `[0,8]` 直接 fault。

同类需要“out 内存 + 返回值”双写的：

| SVC | 说明 |
|---|---|
| 0x24 GetProcessId / 0x25 GetThreadId | out 是指针，返回 Result |
| 0x29 GetInfo | `x0` 返回值，`x1` 也要带值 |
| 0x08 CreateThread | 写句柄到 out |

## 进入 nnMain 之后

启动链跑通后到达 `nnMain`（`main+0xE53710`），当前停在 `main+0xE5454C`：
`0x42127FB0(w0=[x26+0x648]+0x88)` 返回 0，随后 `ldr x20,[x0,#0xB0]` 读空指针。
这里是**游戏自己的数据查找**，不再是 SDK 层，说明 HLE 边界已经越过 SDK。

## IPC/CMIF 协议实测

全部由反汇编 + 单函数探针确认（`HipcMessageReader::Initialize` @ `sdk+0x17E9B0`，
`HipcClientCoreProcessor` @ `sdk+0x2607E0`，`Hipc2ClientCoreProcessor` @ `sdk+0x77B80`）。

### 缓冲区

* `svcSendSyncRequest(handle)` 的消息缓冲区 = **`tpidrro_el0`**（`sf::hipc::GetMessageBufferOnTls`），
  长度 0x100。
* 只有当缓冲区 ≠ `tpidrro_el0` 时才走 `svc #0x22 SendSyncRequestWithUserBuffer`。

### 请求布局

```
+0x00  w0 = num_data_words | (type << 16)   ; type: 0 = 无 buffer 描述符, 1 = 有
+0x04  w1 = 0x80000000 | handle_words(10bit) ; bit31 = "有 special header 字"
+0x08  w2 = pid(bit0) | copy_handles(bit1-4) | move_handles(bit5-8)
+0x0C  句柄区起点（reader 记账里头部只算 3 word！）
+0x10  pid 字（请求里常见 1）
+0x20  CMIF 头：'SFCI', version=1, cmd_id, token
+0x30  数据
```

`data_offset(words) = 3 + pid + copy + move`，CMIF 头在 `align16(data_offset*4)`。

### 响应布局（我们写回的）

```
+0x00  w0 = 4 + len(payload)
+0x04  w1 = 0x8000000A
+0x08  w2 = (n<<1) | (n<<5)       ; 同时声明 n 个 copy 句柄和 n 个 move 句柄
+0x0C  copy handles                ; cmif v1 客户端读 reader+0x44 指向这里
+0x0C+4n  move handles             ; hipc2 客户端读 reader+0x48 指向这里
+0x20  CMIF 头：'SFCO', 1, Result, token
+0x30  payload
```

### 五个必须记住的点

1. **两套客户端读句柄的偏移不同**：`HipcClientCoreProcessor`（对象方法/子会话）读
   `reader+0x44`（copy 句柄区），`Hipc2ClientCoreProcessor`（服务本体）读 `reader+0x48`
   （move 句柄区）。同一个句柄写两处即可通用，否则子会话拿到的是 `word0` 当句柄。
2. **响应里头部只占 3 个 word**。写 `struct.pack('<IIII', ...)` 会顺手把 0x0C 的 copy
   句柄清 0——踩过一次，表现为 `MapSharedMemory(handle=0)`。
3. **CMIF 结果字在 `cmif+8`**，客户端先比 `cmif+0 == 'SFCO'`，再读 `cmif+8` 当 Result，
   出参从 `cmif+0x10` 起。
4. **`QueryPointerBufferSize` 必须先答**：客户端调“带 buffer 的命令”前会先做一次
   light-IPC 询问对端 pointer buffer 大小；不答的话返回 `0x11A0B`，随后
   `NN_ASSERT` 直接 abort（`ProcessRequestWithBuffer` 里 `cbnz w0` → `diag::AbortImpl`）。
5. **`CreateAppletResource` 的回复必须是“移交句柄”**，不是 payload。hid 之后就靠这个句柄
   建 `IAppletResource` 子会话，再 `GetSharedMemoryHandle` 拿 0x40000 的共享内存
   （手柄状态就在里面）。

### 请求头的 CMIF 位置不固定

`data_offset = 3 + pid + copy_handles + move_handles`（word），CMIF 在
`align16(data_offset*4)`。实测出现过 **0x10 / 0x20 / 0x30** 三种；带 buffer 描述符
（`ProcessRequestWithBuffer`）时更靠后。解析时从 0x10 起按 16 字节步进扫描 `'SFCI'`，
不要写死两个位置。

## 进入主循环之后

`nn::init::Start` 现在能跑完并返回；游戏线程在渲染循环里每帧：

```
nvnCommandBufferBeginRecording → nvnCommandBufferAddCommandMemory/AddControlMemory
→ SetSamplerPool/SetTexturePool/SetShaderScratchMemory → EndRecording
→ nvnWindowSetPresentInterval → 等 vi 的 vsync 事件
```

也就是说 **SDK 层已经不再是瓶颈**，瓶颈变成 NVN 的语义（资源、管线、命令回放）——
那是 M3/M4 的工作。

## 执行器（Unicorn）的两个坑

这两个坑都不是"游戏慢"，是**我们的 harness 慢**，而且症状极具误导性（表现为"卡住"）。

### 千万不要给 `emu_start` 传 `timeout`

Unicorn 2.1.4 里，只要 `uc_emu_start(..., timeout=X)` 带了非零 timeout，TCG 主循环的
行为就退化了：同一段 guest 代码慢约 **100 倍**，而且 `count` 与 `UC_HOOK_BLOCK`
几乎不再触发（看起来像"guest 卡在一个循环里"）。实测同一份 BOTW：

| 调用方式 | present 循环帧率 |
|---|---|
| `count=20_000_000`，无 timeout | 305 fps |
| `timeout=剩余秒数*1e6` | 2 fps |

正确做法：**分块 + 自己看表**。`count` 用完就回 Python 一次（0.06 ms），
块之间检查 `deadline`、采样 PC。块边界和"guest 真返回"用 `PC == END` 区分。

顺带一个已测数据：Unicorn 本身跑 ARM64 约 **100 MIPS**（1M 条指令 7.6 ms），
所以"每帧 0.29 秒"这种数字一定是 harness 的问题，不是 TCG 的极限。

### `find_svc` 不能依赖逐指令 ring

老实现用 `ring[-1]`（逐指令回调记录的 PC）定位出事的那条 `svc`。关掉逐指令回调后
ring 是空的，回退到读 `PC` —— 而 Unicorn 报的是 `svc+4`，动态 stub 又是 4 字节紧排，
于是**读到了下一个 stub 的 `svc`，派发到错的处理函数**。症状：`nvnDeviceGetProcAddress`
循环里反复调用同一个 API（512 次），最后由一个 0 函数指针跳到 `pc=0`。

修法两条一起用：

1. stub 槽改成 **16 字节步长**：`svc #imm` + 12 字节 0。这样 `pc+4` 落在槽内填充区，
   不会读到邻居的 `svc`。
2. `find_svc` 依次试 `pc-4` → `pc` →（可选）`ring[-1]`，并对 stub 区校验
   `(addr-STUB_BASE) % 16 == 0`、`imm` 必须已在 `hle_slots` 注册。

另外 stub 区改成**启动时一次性预写**（`_prep_stubs`）：运行期不再写可执行页，
避免翻译缓存反复失效。

## M3：NVN 对象与命令流（`tools/m3_nvn.py`）

### 对象就地存放

对象写在**游戏分配**的 NVN 对象内存里，不另建指针表。预留大小直接取 `nvn.h`
（这些数字要在 HLE 里当硬约束）：

| 对象 | 大小 | 对象 | 大小 |
|---|---|---|---|
| `NVNdevice` | 0x3000 | `NVNmemoryPool` | 0x100 |
| `NVNbuffer` | 0x30 | `NVNtexture` | 0xC0 |
| `NVNsampler` | 0x30 | `NVNprogram` | 0xC0 |
| `NVNtexturePool` | 0x20 | `NVNsamplerPool` | 0x20 |
| `NVNqueue` | 0x2000 | `NVNwindow` | 0x180 |
| `NVNcommandBuffer` | 0xA0 | `NVNsync` | 0x40 |

### 命令内存的地址从哪来

`nvnCommandBufferAddCommandMemory(cb, NVNmemoryPool, offset, size)` —— 第二个参数是
**memory pool 对象**，不是裸指针；命令区 VA = `pool.storage + offset`。
（之前按"裸地址"理解，记录区就写成 0，录制全落空。）

### 记录格式

```
{op:u32, size:u32}  + payload      # size 含头，8 字节对齐
OP_BEGIN / OP_END / OP_API / ...
OP_API payload: name_idx:u32, caller:u32, a0..a3:u64
```

`OP_API` 的价值：**没实现语义的入口也照样记录**，所以 dump 是完整调用序列。
`caller` 存 guest 返回地址，用来反查"是哪段游戏代码发的这条命令"。

### 当前 BOTW 每帧只有 10 条记录，且没有 draw

实测帧内容：`BEGIN`、`BufferGetAddress`×2、`ReportCounter`×2、
`SetSamplerPool`、`SetTexturePool`、`SetShaderScratchMemory`、`EndRecording`、`END`。
**没有 render target、没有 clear、没有 draw、没有 present**（`nvnQueuePresentTexture` 0 次）。
渲染线程在提交空帧 = 游戏还在加载阶段，不是 HLE 丢了调用。

---

## 实测补充：同名 API 的**重载**是独立的符号（M3.5）

BOTW 1.6.0 调的是带 `ReadOption` 的那个：

```
_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE
```

只挂 `..._PmNS0_10FileHandleElPvm`（不带 Option）会**静默失效**：调用进入真 SDK，
真 SDK 在自己的 handle→`FileAccessor` 表里找不到我们的假句柄（我们 HLE 了
`OpenFile`），于是把句柄值 `0x11` 当成对象指针 → `ldr w8,[x0,#0x24]` 读 0x35 崩溃。

结论：**HLE 名单要按 mangled name 逐条核对**，不能按"函数名"想当然。
`nn::fs::ReadFile` 一共 4 个重载，`WriteFile` 又另有 `WriteOption` 版本。

## 实测补充：CMIF 命令号要反查，不要猜（M3.5）

`set`（`nn::settings`）我们一开始让未知命令走"回 0 + 空 payload"，
结果 `MakeLanguageCode` 的 `LanguageCode`（8 字节 ASCII）读成垃圾，
游戏把系统语言判成日语，于是去加载 `Font_JP.sbfarc` / `Bootup_JPja.pack`。

定位方法：hook `svcSendSyncRequest` 的 HLE 入口，抓**调用栈**，再拿返回地址在
`symbols.json` 里取最近的符号。例如：

* `set` cmd 2 → `ISettingsServer::MakeLanguageCode`
* `set` cmd 5 → `ISettingsServer::GetAvailableLanguageCodes2`
* `aoc:u` cmd 3 → `IAddOnContentManager::ListAddOnContent`
* `acc:u0` cmd 0x8C → `IAccountServiceForApplication::InitializeApplicationInfo`

## 实测补充：宿主写 guest 内存要自己提交页（M3.5）

`uc.mem_write` **不会**触发 `UC_HOOK_MEM_UNMAPPED`。BOTW 的 `.pack` 单次读 140 MB，
目标缓冲区在 8 GiB 的 guest 堆窗口里、尚未 `mem_map`，直接写会
`UC_ERR_WRITE_UNMAPPED`。所以 HLE 侧统一走 `Kernel.wmem()`（内部先
`commit_range()` 再写）。

另外 lazy 映射窗口**不能重叠**：原来堆窗口 `[0xB0000000,0x2B0000000)` 把线程栈
窗口 `[0x100000000,0x180000000)` 整个包住，一次 64 MiB 的堆提交就会让后续
64 KiB 的线程栈槽 `uc.mem_map` 失败（`UC_ERR_MAP`）。现在按四段互不重叠的窗口
提交，并记录已映射区间。


## 实测补充：NVN 的"空实现"是静默陷阱（M3.6）

`nvnDeviceGetProcAddress` 对不在表里的名字返回一个 **no-op 桩**（`return 0`，不写出参）。
这比直接崩更危险：调用"成功"了，但游戏的状态没变。

本轮就是这么丢掉纹理尺寸的：BOTW 建纹理走的是

```
nvnTextureBuilderSetDefaults -> SetTarget -> SetFormat -> SetLevels
nvnTextureBuilderSetWidth / SetHeight     <- 我们表里没有，静默 no-op
nvnTextureBuilderGetStorageSize           -> 0
```

于是 26 个格式的纹理尺寸全是 0，游戏跳过分配，随后拿 NULL 当基址写格式表，
在 `main+0x12AFF34` 写地址 0 崩溃。

现在 `botw_hle` 会把"被调用但只有空实现"的 NVN 入口按次数记进 `NVN_MISS`，
每次长跑结束打印 top-N；补实现前先看这张表，不要靠猜。

## 实测补充：memory pool builder 不给 storage 是合法的（M3.6）

真机上 `nvnMemoryPoolInitialize` 的 builder `storage==0` 表示"由驱动分配"。
宿主没有 NVN 驱动，必须自己补一块 guest 内存，否则
`nvnMemoryPoolGetStorage` / `nvnMemoryPoolGetBufferAddress` 返回 0，
游戏会把整块 pool（例如 120 MiB）当成地址 0 的资源区。
现在 `nvnMemoryPoolInitialize` 在 `mem==0 && size>0` 时用 `Kernel.alloc()`
给一块 2 MiB 对齐的 guest 存储。

## 实测补充：动态 stub 区不能被当成数据（M3.6）

`stub_for()` 把每个 HLE 入口写成 `STUB_BASE + i*16` 上的 `svc`。
guest 会把这些地址存进自己的函数表并在运行期调用，所以这块内存**只读可执行**：

* 任何 HLE 出参写落进 `[STUB_BASE, STUB_BASE+0x20000)` 都会把桩打烂，
  下一次调用就跳进数据里，报成 `UC_ERR_EXCEPTION pc=stub+4`（极难定位）。
  现在 `wr64/wr32/wmem` 统一拦截并打印是哪个 HLE 干的（实测抓到
  `nvnBufferMap` 把 pool 地址写到 `0xA0008A0`）。
* `find_svc()` 以前会**跳过**未登记的 stub 槽，导致"未登记"和"没有 svc"
  两种情况表现一样。现在 `handle_svc` 对未登记槽单独记 `stub_unregistered`
  并当作空实现返回，不再直接掉进 `udf`。

## 实测补充：服务端协议不必补全，客户端入口可以整体替身（M3.17）

边界原则里最容易走偏的一条是"游戏调了 IPC，就去把那个服务实现出来"。音频给出了反例。

`audren:u` 的调用序列是 `cmd=1`（问工作区大小）×4 → `cmd=0`（OpenAudioRenderer，带 transfer
memory）→ 之后每帧 `cmd=7` / `cmd=4`（Update / RequestUpdateAudioRenderer）。这两条命令的
出参是一整套 `AudioRendererUpdateDataHeader`（voices / mixes / sinks / memory pools 的 in/out
参数块）。我们回了空 payload，客户端读到栈上垃圾 → `NN_ASSERT` → `Abort(0)`
（注意 `Abort(Result*)` 的参数是 0，是纯断言，不是 Result 码——不要按错误码去查）。

正面补齐这套协议 = 把 yuzu 的 audren 服务端重写一遍，而它对"第一帧 NVN Draw"毫无贡献。
正确的切法是**按客户端入口切，而不是按服务端命令切**：

* 碰 IPC / 设备的那几个入口整体替身（`Open/Start/Stop/Close AudioRenderer`、
  `RequestUpdateAudioRenderer`、`GetMemoryPoolState`、`ListAudioDeviceName` …）
* 只改 guest 内存的纯计算入口继续让 guest 真跑
  （`InitializeAudioRendererParameter`、`InitializeAudioRendererConfig`、
  `AcquireVoiceSlot`、`SetVoiceXxx`、`AddDeviceSink`）

替身以后有两个必须自己回答的状态机，否则游戏会安静地卡死：

1. `RequestAttachMemoryPool` 之后服务端本该在下一帧把状态翻成 `Attached`。替身掉的渲染器
   永远不会翻，音频线程就卡在 `main+0x1CF9698` 的 5 ms 轮询里（实测 16370 次/10 s）。
   → `GetMemoryPoolState` 直接返回 1。
2. `nn::account::Initialize` 走真实 IPC 时会被我们伪造的空服务"成功"骗过去，SDK 全局保持
   NULL，后面的 `ListAllUsers` 解引用 0（`pc=0x2003B710 ldr x8,[x0]`，`x0=0`）。
   真机上这一步是系统层的"选用户"，移植版只需造一个固定 UID 的已登录用户。

同类判据：**如果补齐它需要实现一整套"帧间状态机"，而它不产出 draw / 不产出资源，就替身。**

## 实测补充：返回 `NVNboolean` 的入口不能用"返回 0"兜底（M3.20）

`_nvn_miss` 的 `return 0` 只对 `void` 入口安全。凡是返回值有语义的入口，0 就是**一个答案**，
游戏会照它走失败分支。

M3.19/M3.20 的实例：

* `0x41D41150` 的 `blr` 走 `GOT 0x42C80310 → 0x42D2F120 → 0x0A0005C0`，
  `0x0A0005C0` 是动态 stub 槽 `#0x105C`，反查回来是 `nvnapi:nvnProgramSetShadersExt`。
* 它的真身签名是 `NVNboolean nvnProgramSetShadersExt(NVNprogram, int, const NVNshaderData*)`，
  `0` = 失败。我们返回 0，游戏就把"program 建好了"的后续（写 `mgr+0xD0`/`+0xD8`）整段跳过，
  **几十条资源之后**才在别处以空指针崩掉——崩溃点离根因隔着一整个资源循环，栈上看不出关系。

判据与做法：

1. 遇到 `blr` 到 `GOT → 0x0A000xxx` 的调用点，先反查符号：
   `k.stub_by_desc`（desc → 地址）、`k.hle_slots[0x1000+((addr-0x0A000000)//0x10)]`。
   不知道调的是谁，就别猜返回值。
2. 返回值是 `NVNboolean`（1=成功）、指针、句柄的入口一律显式实现；
   只有真正的 `void` 入口才可以交给 `_nvn_miss`。
3. 同一类坑还有"`w0` 低字节被当布尔用"（`tst w0,#0xff`）和"返回 -1 表示找不到"：
   这类入口的返回值是**协议**，不是随便填的整数。

附带一条命名坑：mangled name 里的数字是**名字长度**，
`_ZN2nn3nfp19AttachActivateEventE…` 写成 `…21AttachActivateEvent…` 不会报错，
只会让那条 HLE 静默失效（`m2_nfp.py` 里真出现过）。接线后用导入表逐条比对。

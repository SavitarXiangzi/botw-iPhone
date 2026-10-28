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
* **线程调度**：目前只登记 `svcCreateThread`，尚未真正并发执行。
* **`AddressRegionManager` 基址**：当前拿到垃圾值（`0x2`），待补 `svcGetInfo` 其余 id。


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

## 进入主循环之后

`nn::init::Start` 现在能跑完并返回；游戏线程在渲染循环里每帧：

```
nvnCommandBufferBeginRecording → nvnCommandBufferAddCommandMemory/AddControlMemory
→ SetSamplerPool/SetTexturePool/SetShaderScratchMemory → EndRecording
→ nvnWindowSetPresentInterval → 等 vi 的 vsync 事件
```

也就是说 **SDK 层已经不再是瓶颈**，瓶颈变成 NVN 的语义（资源、管线、命令回放）——
那是 M3/M4 的工作。

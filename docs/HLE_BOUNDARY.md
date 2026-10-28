# HLE 边界与实现约定

## 边界在哪

**不是** IPC/cmif 服务层（D2R 全树里没有一条 `SendSyncRequest` 实现）。
边界是“游戏代码调用 SDK 的第一个 `nn::*` 函数”，即模块导入表里的符号。

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

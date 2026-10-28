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
| M3 | NVN 设备/队列/窗口语义（真实 GPU 资源） | 🟢 录制循环在跑，未出画面 |
| M4 | NVN → Metal 回放（管线/纹理/命令缓冲） | ⬜ |
| M5 | 音频（nn::audio → AVAudioEngine）、输入 | ⬜ |
| M6 | 真机性能与内存（6 GB 机型起步） | ⬜ |
| M5 | 音频（nn::audio → AVAudioEngine）、输入 | ⬜ |
| M6 | 真机性能与内存（6 GB 机型起步） | ⬜ |

## M2 进展（最近一轮）

本轮把 **IPC/CMIF 服务端**做出来了，游戏于是从"启动即 abort"变成**真的在跑主循环**：

* 启动改走真实链路 `nn::init::Start(heap, arg, nninitStartup, nnMain)`，**完整返回**
* NVN 引导：`nvnBootstrapLoader` → `nvnDeviceGetProcAddress` → **512 个入口全部解析**
* 3.22 GiB 主堆（`AllocateMemoryBlock(0xC1000000)`）分配成功
* 4 个 guest 线程（协作式调度：SVC 边界换上下文，含 x0-x28/Q0-Q31/SP/PC/NZPCR/TLS）
* **hid 服务打通**：`CreateAppletResource` → `GetSharedMemoryHandle` → `svcMapSharedMemory(0x40000)`
  → `SetSupportedNpadStyleSet` / `SetSupportedNpadIdType` 等命令全部返回成功
* 之后进入渲染主循环：每帧 `nvnCommandBufferBeginRecording … EndRecording` +
  `nvnWindowSetPresentInterval` + 等 vsync，回归跑 45 秒可录到 **8000+ 帧**
* 当前一轮 IPC 命令表**零缺失**：`hid` 0/0x64/0x66/0x6D、`IAppletResource` 0、
  `pctl` 0/1/3 全部返回成功（家长控制统一回"无限制"）

当前"跑起来"的含义要说明白：**游戏逻辑在跑，画面还什么都没有**。所有 `nvn*`
仍是记账式桩（只记参数、返回假缓冲），下一阶段才是把它们翻译成 Metal。

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

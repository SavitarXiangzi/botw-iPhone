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

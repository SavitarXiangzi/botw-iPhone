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

配套修复（都属于"宿主替身"层，不含任何游戏数据）：
* `nn::fs::ReadFile` 补 `ReadOption` 重载，否则绕进真 SDK 把句柄当对象指针
* HLE 写 guest 缓冲区前按需提交 lazy 页（BOTW 的 `.pack` 单次读上百 MB）
* lazy 映射窗口改为互不重叠（堆窗口原来把线程栈窗口整个包住）
* `nn::oe` 查询类 API 直接给值；`nn::account` 整体直通（只影响存档/UI）
* `nn::settings` 的 `MakeLanguageCode` / `GetAvailableLanguageCodes2` 等按命令号实现

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

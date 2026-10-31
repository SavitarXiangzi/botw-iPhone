# 着色器链：Maxwell SASS → SPIR-V → MSL

M3.77 把这条链**在 Windows 上真的跑通了**：从《旷野之息》的
`Bootup_Graphics.pack` 里取出真实 shader，经 Ryujinx 的翻译器转成 SPIR-V，
再经 SPIRV-Cross 转成 MSL。产物是 `.metal` 文本 + 反射 JSON。

**没有**做到的部分只有最后一步：`xcrun metal` 编成 `.metallib`，那一步只能在
macOS 上做（我们的 CI 有 macOS runner）。**没有**真机验证。

参考实现：<https://github.com/xbunax/nxnative>（与 `seedds/nxnative` 同一项目），
MIT。它的 `dm/gfx/README.md`、`dm/gfx/tools/dmsh_build.py`、`dm/gfx/tools/dmsh_xlate/Program.cs`
是本文件所有结论的来源；我们复用的是**工具和约定**，不是它的运行时。

## 1. 为什么必须是离线

客体交给 NVN 的是**已经编译好的 Maxwell 机器码**（`NVNshaderData::data` 指向
代码段，配套一个控制段），不是源码。Metal 只吃 MSL / `.metallib`，iOS 又不允许
JIT 现译。所以只能在电脑上译好、打包进 App。

```
game .pack/.sbfsha (Yaz0 + SARC + BNSH)
  → 取出每个 stage 的 code area + control section
  → dmsh_xlate        （.NET 10，内嵌 Ryujinx 的 shader 翻译库）→ SPIR-V
  → spirv-cross       （固定 commit f11ba9f）                 → MSL (.metal)
  → xcrun -sdk iphoneos metal / metallib                      → .metallib   ← 只有 macOS
```

## 2. 本机已验证的事实（2026-10-30，Windows 11 + MSVC 2022）

工具**全部从源码在本机构建**，没有用任何 macOS 预编译二进制：

| 工具 | 版本 / 来源 | 本机产物 |
|---|---|---|
| dmsh_xlate | nxnative 的 `dm/gfx/tools/dmsh_xlate`（net10.0）+ 仓库内 vendored Ryujinx；`--source-sha` = `b6c47e73596a2e34` | `dmsh_xlate.exe`（.NET 10.0.403 SDK） |
| spirv-cross | 上游 tag `vulkan-sdk-1.4.363.0`，commit `f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7` | `spirv-cross.exe`（CMake 4.1.1 + VS2022） |

跑出来的数字（可复现）：

```
extract: 1 file(s), 5102 shader(s): 2033 vs, 3069 ps
translate: 2/2 shader(s)
  d756ff62a57fc508_vs → 3,411 行 MSL，vertex 入口，翻译器 notes = 0
  029de06ee5b5d21b_ps → 4,573 行 MSL，fragment 入口，翻译器 notes = 0
```

（`Bootup_Graphics.pack` 只是这个游戏 shader 的一小部分；整作 19,592 个。）

## 3. 怎么跑

```powershell
# 一次：构建两个工具（需要 .NET 10 SDK + CMake/VS2022）
powershell -File scripts/shader_chain/build_tools.ps1 -Src C:\botw-work\nxsrc\nxnative-git

# 1) 从游戏包取出 shader（Yaz0 / SARC / BNSH 自动剥）
C:\Python314\python.exe scripts\shader_chain\nx_shader.py extract `
  "C:\...\RomFS\update\Pack\Bootup_Graphics.pack" --out build\shader-chain\work

# 2) 翻译（全部、或 --keys / --limit 选一部分）
C:\Python314\python.exe scripts\shader_chain\nx_shader.py translate `
  --work build\shader-chain\work --out build\shader-chain\msl `
  --xlate build\shader-chain\tools\xlate\dmsh_xlate.exe `
  --spirv-cross build\shader-chain\tools\spirv-cross.exe
```

产物：`<key>_<stage>.metal`（入口已改名成 `nvngfx_main_vs` / `nvngfx_main_fs`）、
`<key>_<stage>.json`（反射：cbuf/sbuf/texture 的 binding 与槽位、翻译器 notes），
以及仅在 MSL 声明了 c1 时出现的 `<key>_<stage>.c1`（原始常量段字节）。

## 4. 格式要点（写代码时别再踩）

这些是这次实际调试出来的，照抄 `shader_split.py` 的注释不够：

* **BNSH 的 stage 指针顺序是 `vs, hs, ds, gs, ps, cs`**，不是 NVN 枚举的
  `vs, ps, gs, hs, ds, cs`。按后者读会**静默地把顶点和像素着色器对调**，
  翻译器照样出 MSL，只是 `vertex` 变 `fragment`。
* 每个 stage 的 **code area 前 0x30 字节是头**，0x50 字节的 SPH 从 `+0x30`
  开始；翻译器要的是**去掉这 0x30 头**的缓冲（SPH + 指令）。
* SPH word0 是 `0x12345678`（图形）/ `0x12345679`（计算），**stage 信息在
  去掉 0x30 之后的 word0 低 5 位与 bit10..13**（`Bootup_Graphics` 里顶点是
  `0x02060461`、像素是 `0x00061462`）。
* **c1 常量段**：`control+0x700` = c1 相对 code area 的偏移，`+0x704` = code area
  总长，c1 = `code[c1o:c1t]`。间接跳转（BRX）的跳转表在这里，不给它翻译出来
  的控制流是错的（粒子着色器会死循环）。M3.85 起 `nx_shader.py` 把这段
  抽成 `.c1` 侧车，`gen_blobs.py` 原样写进 blob，运行时按
  `NVNGFX_ARG_C1` 绑定。
* shader key（与 nxnative 的 `shader_map.tsv` 第 4 列、我们 `prog_key` 一致）：
  `sha256(control 前 0x880 字节（控制段更短时取全长）+ code area)[0..8]`，
  小写 16 进制。code area **含** 0x30 头。
* 包是三层：`SARC`（BOTW 的 pack 把 SFAT 紧跟在头后面、`data_offset` 指向数据区，
  不是标准的指向 SFAT；两种都要试）→ 成员常常是 `Yaz0` → 解开后又是 SARC 或 BNSH。
* **顶点入口必须用 `[[stage_in]]` 结构体**，属性写在成员上：
  `struct In { float2 p [[attribute(0)]]; }; vertex Out vs(In in [[stage_in]])`。
  裸的函数参数 `float2 p [[attribute(0)]]` 在本项目的模拟器上让管线建不出来
  （M3.80，run 41007654642 失败 / 41008588834 通过）。spirv-cross 本来就这么发，
  所以只影响手写的自检 MSL。

## 5. 与运行时的接口

我们这一侧新增了 `nvngfx_shader_lookup(key, stage, why)` 与
`nvngfx_shader_set_blobs()`（`ios/nvn/include/nvn_shader.h`）：离线产物按 key
装进一张静态表，后端编译管线时按 key 取 MSL。入口名固定为
`NVNGFX_ENTRY_VERTEX` / `NVNGFX_ENTRY_FRAGMENT`，由生成脚本改名，两边不会漂。

### 5.1 运行时按 key 取 MSL（M3.78 已接线）

`nvnProgramSetShaders` 拿到的 `NVNshaderData{data, control}` 是**宿主可读的地址**
（客体把这两块内存放在自己被映射的堆里），所以 key 可以当场算，不用等地址翻译：

| 位置 | 做了什么 |
|---|---|
| `ios/nvn/src/nvn_shader_key.c` | 纯 C SHA-256 + `nvngfx_shader_key_from_guest()`，规则同 §4 最后一条 |
| `Program::key[3][17]`（`nvn_metal.mm`） | `prog_set_shaders` 时算出并留着；客体之后释放 control 段也不影响 |
| `program_pipeline()` | 先 `nvngfx_shader_lookup(key, stage)`，取不到才回落到 `nvngfx_shader_to_msl()`（自检的 passthrough 走这条） |
| `nvngfx_metal_create()` | 调 `nvngfx_shader_install_offline_blobs()` 装表 |

控制段不描述 code area（`u32@0x704 == 0` 或越界）时**不猜**：key 留空，退回
旧路径。SHA-256 在 `tests/nvn_shader_test.cpp` 里钉了两个标准向量和两个
Python 独立算出的 key，改规则会让测试先红。

**在真实 shader 上对过账**（这是这条接线唯一有意义的验证）：

```
nx_shader.py extract <Bootup_Graphics.pack> --out build\shader-chain\work --keep-full-code
keycheck build\shader-chain\work\code
  keycheck: 5102 shader(s), 5102 match, 0 bad, 0 skipped
```

`scripts/shader_chain/keycheck.cpp` 把**运行时那个 C 函数**跑在包里取出的
control + code 上，和 Python 命名文件用的 key 逐个比。5102 个全中说明三处
（`nx_shader.py` / `nvn_shader_key.c` / nxnative 的 `hle_nvn.c keys_hash`）
现在是同一个算法。

一个坑值得记下来：key 哈希的 code area **包含**开头 0x30 字节的头
（`78 56 34 12 ...`），而给翻译器的输入是从 SPH（+0x30）开始的。第一版检查
拿 `.bin`（已去掉头）去算，5102 个全对不上——离线产物全部会被判成"没找到"，
而且**运行时不报错**，只是静默回落到旧路径。`--keep-full-code` 就是为这条
检查加的。

### 5.2 离线产物怎么进 App

有两条路，产物都只在本机或 CI 生成，**不入库**：

1. **C 表**：`nvngfx_shader_blobs.gen.c` 里的
   `nvngfx_shader_install_offline_blobs()`。入库的那份是空表。

   ```powershell
   C:\Python314\python.exe scripts\shader_chain\gen_blobs.py --msl build\shader-chain\msl `
     --out ios\nvn\src\nvn_shader_blobs.gen.c
   ```

   一条 shader 一个 C 字面量（每 4000 字符切一段）；5,102 个 key 全量塞进去
   大约几十 MB，**不适合入库**。

2. **文件 bundle**（M3.90）：`manifest.tsv` 加 `<key>_<stage>.msl` / `.c1`。
   把目录放进 App 的 `<app>/shaders`，或用 `NVNGFX_SHADER_DIR` 指向它；
   `nvngfx_shader_load_bundle()` 启动时读取，和生成的 C 表合并，同一 bundle
   重复加载只替换不累积。

   ```powershell
   C:\Python314\python.exe scripts\shader_chain\gen_blobs.py --msl build\shader-chain\msl `
     --bundle-out build\shader-chain\shaders
   ```

   manifest 是 tab 分隔的五列：
   `key  stage  msl-file  c1-file  args`，其中 args 是
   `id:slot:packed:kind[:offset]`；`kind` 是 `nvngfx_arg_kind` 的**数值**
   （1..10），不是 C 枚举名。空 c1 必须写成真实文件名而不是 `-`。
   这仍是 MSL 文本，运行时由 Metal 编译；`.metallib` 打包与 `xcrun metal`
   的编译期校验还没做。

### 5.3 还差的接线（下一步）

1. ~~顶点输入~~：M3.79 已接。`BindVertexAttribState` /
   `BindVertexStreamState` 把客体的 4 / 8 字节记录译成
   `MTLVertexDescriptor`，顶点地址按 pool 找到对应 `MTLBuffer`。
2. 资源绑定：MSL 用的是 `--msl-argument-buffers`，所有常量/纹理走
   `buffer(0)` 的 `spvDescriptorSetBuffer0`，槽位约定见 §6。
   **常量已经接上**（M3.81）：`nvnCommandBufferBindUniformBuffer` 记录地址。
   **texture / sampler 也已接上**（M3.83，见 §5.5）：texture pool /
   sampler pool 给出稳定句柄，`BindTexture` / `BindSeparateSampler` 记录句柄，
   draw 前按 `[[id(k)]]` 写一张每项 8 字节的参数表：buffer 写
   `gpuAddress + offset`，texture / sampler 写 `gpuResourceID`。
   **真实 shader 的槽位映射也已接上**（M3.84，见 §5.5.1）：`gen_blobs.py`
   从 `spvDescriptorSetBuffer0` 的成员名解出布局，和 MSL 一起写进同一个 blob；
   `prog_set_shaders()` 查找 blob、把布局拷进 `Program::args`，运行时不再猜。

   为什么不用 `MTLArgumentEncoder`：模拟器返回的对象 `encodedLength == 0`，
   而模拟器的纹理 `gpuResourceID` 又是 0——"让 Metal 自己算布局"这条路在那里
   走不通。固定 8 字节 ABI 与 d2r-mobile 的翻译产物一致，也不需要运行时编译。
3. 几何路径：`DrawArrays` / `DrawElements`（16/32 位索引）以及
   `DrawArraysInstanced` / `DrawElementsInstanced` / `DrawElementsBaseVertex` /
   两种 indirect draw 都已编码，顶点与索引都按 pool 零拷贝绑定（M3.79 /
   M3.80；实例化与 indirect 见 M4 续十二）。也就是说**画什么形状已经不缺了**；
   常量、纹理、sampler 三条绑定都通了（模拟器上纹理/sampler 走直连，见 §5.5），
   真实 shader 的槽位映射也已由 M3.84 侧车补上；但 c1、bindless、guest 纹理
   上传仍未解，所以仍然没有一条**真实** shader 在 Metal 里执行过。u8 索引仍
   需要 CPU/GPU 转换为 u16；indirect record 的合法性由 GPU 读取，CPU 只校验
   pool span 与对齐。

### 5.4 Sampler 状态对象（M3.82）

`nvnSamplerBuilder*` / `nvnSampler*` 已绑定 38 个入口，默认值按 nxnative
`hle_nvn.c:546`，可以创建真实 `MTLSamplerState`。getter 保存并返回客体原始
NVN 值（例如 wrap 0 与 7 不会被 Metal 映射“合并”），`nvnSamplerCompare`
也按原始状态比较。`MTLSamplerReductionMode` 只在 iOS 26 SDK/系统上设置，
旧系统保留 guest 值但不假装 Metal 已应用它。

M3.83 之后这**已经**是资源绑定的一部分：`nvnSamplerPoolInitialize` /
`RegisterSampler` / `RegisterSamplerBuilder` 给出池内句柄，
`nvnCommandBufferSetSamplerPool` 与 `nvnCommandBufferBindSeparateSampler(s)`
把它接到 draw 上，argument buffer 的 `[[id(k)]]` sampler 槽位由
`setSamplerState:` 填。具体见 §5.5。

### 5.5 描述符池与句柄（M3.83）

`nvnTexturePool*` / `nvnSamplerPool*` 的 14 个入口与 8 个 command buffer
绑定入口已接。句柄按 nxnative 的定义打包：

| 入口 | 返回值 |
|---|---|
| `nvnDeviceGetTextureHandle(d, tex, samp)` | `tex \| samp << 20` |
| `nvnDeviceGetTexelFetchHandle(d, tex)` | `tex` |
| `nvnDeviceGetSeparateTextureHandle(d, id)` | `id` |
| `nvnDeviceGetSeparateSamplerHandle(d, id)` | `id << 20` |
| `nvnDeviceGetImageHandle(d, id)` | `id`（与纹理共用池索引空间） |

句柄里的 ID 是**客体池内 ID**，不是宿主槽位。查表时才加绑定池的 base：
`nvngfx_pool_slot()`（`ios/nvn/include/nvn_pool.h`）。base 按池创建顺序分配、
向上取整到 256 项，和 nxnative 的 `dpool_add` 一致；`Finalize` 不回收编号，
避免旧 command buffer 的句柄指到新池。

`nvnCommandBufferBindTexture(s)`、`BindSeparateTexture(s)`、
`BindSeparateSampler(s)` 只消费 pixel stage；其他 stage 记一次日志后忽略，
因为当前后端只建 fragment 的参数缓冲区。

#### 5.5.1 真实 shader 的参数布局（M3.84）

`gen_blobs.py` 不只收起 MSL：它同时解析 `--msl-argument-buffers` 产物里的
`struct spvDescriptorSetBuffer0`，把每个成员翻译成 NVN 资源槽位，并生成
`kArgs_<key>_<stage>[]`。生成器按成员名分类，不靠描述符出现顺序猜：

| MSL 成员名 | 含义 | 侧车 kind / slot |
|---|---|---|
| `fp_c3` / `vp_c5` | 常量段 N -> 统一缓冲槽 N−3 | `UBO` 0 / 2 |
| `fp_t_tcb_1C` | c2 合并纹理句柄 | `TEXTURE` 10 |
| `fp_t_tcb_1CSmplr` | 同一合并句柄中的采样器 | `SAMPLER_OF_TEXTURE` 10 |
| `fp_t_tcb_5A` | 独立纹理 | `SEPARATE_TEXTURE` 0 |
| `fp_t_cb2_1C` | c2 驱动表纹理 | `TEXTURE` |
| `fp_t_cb7_2A` | bindless，句柄在统一缓冲里 | `TEXTURE_IN_UBO` |
| `fp_c1` | c1 常量段（`<key>_<stage>.c1`） | `C1`，slot -1 |
| `fp_c2` | NVN c2 句柄表 | `DRIVER_C2`，slot -1 |
| `support_buffer` | Ryujinx 支撑缓冲 | `SUPPORT`，slot -1 |
| `fp_i_*` / `fp_s3` / `fp_c0` | image / storage buffer / c0 驱动表 | `UNSUPPORTED` |

本地 `Bootup_Graphics` 的两个真实 shader（数据不入库）暴露了尺寸问题：

| stage | `[[id(k)]]` 成员 | 最高 id | 分类 |
|---|---:|---:|---|
| `d756ff62a57fc508_vs` | 7 | 6 | 3 UBO + 2 纹理 + 2 采样器 |
| `029de06ee5b5d21b_ps` | 42 | 41 | 5 UBO + 18 纹理 + 18 采样器 + 1 c1 |

两项合计 49 项：8 UBO、20 纹理、20 采样器、1 个 c1。
`Program::args` 因此扩到 `NVNGFX_MAX_ARGS`（64），参数表按最高 id 分配；
纹理和采样器槽位各 32 个。布局随 blob 下发，`nvngfx_shader_lookup_blob()` /
`nvngfx_shader_copy_args()` 在 `prog_set_shaders()` 里取同一份记录，避免 MSL 与
布局来自不同版本。

c1 也随 blob 下发：`<key>_<stage>.c1` 存在且长度为 0 表示显式空 c1，缺失表示
该 stage 不声明 c1；MSL 声明了 c1 却没有 sidecar 时生成器直接失败。运行时限
64 KiB，并复制进 `Program`，第一次 draw 时才分配共享缓冲。

参数缓冲区的字节偏移还有一条 Metal 规则：成员按**声明顺序**紧凑排布，
`[[id(k)]]` 只是 encoder 的绑定号，不是字节偏移。当前表按 `id*8` 写，只在 id
连续时成立；BOTW 实测的 0..41 连续，所以这条路径正确。带空洞的布局需要把
packed slot 写进 blob，尚未接（M3.85 的自检暴露了这一点）。

`UNSUPPORTED` 不会被静默填零：`argument_buffer()` 直接丢 draw，并记
`nvnCommandBufferDraw.argUnsupported`；缺 UBO 同理。生成器对 id ≥ 64、
重复 id、非法 kind 和越界槽位**直接让构建失败**。顶点阶段的布局会保存，
但当前不消费——`buffer(0)` 还是第一条顶点流，顶点参数缓冲区的槽位偏移
约定未定。

`SUPPORT` 和 `DRIVER_C2` 已由后端填好。前者是一块 64 KiB 静态缓冲，
`render_scale[0..72]` 全为 1.0（偏移 192 起，每项 16 字节）；后者按当前
绑定重建 4 KiB c2 段：合并纹理槽 r 在 `0x20+8r`、图像在 `0x120+8r`、
独立纹理在 `0x168+8r`、独立采样器在 `0x568+8r`。每次 draw 新建一份，
避免同一 Metal 命令缓冲里的后续 draw 覆盖前一份内容。

### 模拟器上走直连，Metal 3 上走参数缓冲区

自检打印了真实 ID：模拟器 `apple7=0 metal3=0 unifiedMemory=0`，
`MTLTexture.gpuResourceID == 0`，`MTLSamplerState.gpuResourceID != 0`。
`program_pipeline()` 因此按 fragment MSL 文本选路：

| fragment MSL 里出现 | 路径 |
|---|---|
| `[[buffer(0)]]` | 参数缓冲区（每项 8 字节；buffer 写 `gpuAddress + off`，texture/sampler 写 `gpuResourceID`） |
| 只有 `[[texture(k)]]` / `[[sampler(k)]]` | `setFragmentTexture:atIndex:` / `setFragmentSamplerState:atIndex:` 直连 |

限制：

- 纹理注册只保存客体地址；`nvnTextureFinalize` 后再采样会解析失败并丢 draw。
- `nvnTexturePoolGetMemoryPool` / `nvnSamplerPoolGetMemoryPool` 已在
  `signatures.json` 的 477 个形状里（指针返回类型此前被提取器漏掉），但后端仍未绑。
- c1 已由 M3.85 侧车接上；M4 续三十一又接上了 bindless、
  `support_buffer` 和 c2 句柄表。image / storage buffer、typed buffer 和
  c0 驱动表仍标为 `UNSUPPORTED`。
- 参数缓冲区的 texture / sampler 条目要写 `gpuResourceID`。iOS 模拟器
  （`apple7=0 metal3=0`）的纹理 ID 恒为 0，所以那里只能走直连绑定；
  该条目本身**只在 Metal 3 硬件上有意义，尚未验证**。

## 6. 绑定约定（nxnative 的驱动段布局）

运行时填 argument buffer 时按这个来（`dmsh_build.py` 与 `nvn_mtl.m` 一致）：

| 资源 | 位置 |
|---|---|
| uniform buffer `r` | c[3 + r] |
| combined texture `r` | c2 `0x20 + 8r` 处的句柄 |
| image `r` | c2 `0x120 + 8r`（宿主槽 128+r 之前） |
| separate texture `r` | c2 `0x168 + 8r`（宿主槽 128+r） |
| separate sampler `r` | c2 `0x568 + 8r` |
| storage buffer `r` | c0 base + 16r，base：vs `0x110` / cs `0x310` / gs `0x410` / ps `0x510` |

窗口原点左上、深度 0..1，与 Metal 一致，viewport 不做翻转。但**纹理采样坐标
要翻 V**：NDC 的 +y 朝上，Metal 的纹理原点在左上。自检里少了
`v.uv.y = 1.0 - v.uv.y;` 这一行，四角回读就整幅上下镜像
（run 41033107396：`top-left pixel is (0,0,255,255), want (255,0,0,255)`）。

## 7. 明确没做到的

| 项 | 状态 |
|---|---|
| `xcrun metal` → `.metallib` | 未做；只能在 macOS/CI 上做，本机没有 |
| 整作 19,592 个 shader 全量构建 | 未做；只验证了 1 个 pack 的 5,102 个 key 中的 2 个 |
| 编译期校验（MSL 语法） | 未做；Windows 没有 Metal 前端，MSL 只在 CI 的 macOS 上才能编 |
| 真机 / 模拟器上跑真实 shader | 未做。自检用的是内置 MSL 和自写合成 bundle（十条 draw：`[[vertex_id]]`、顶点属性、16 位索引的 `DrawElements`、`BindUniformBuffer` 参数缓冲区、texture pool + sampler pool 采样、只通过 `[[id(0)]]` 读 c1 常量段、bindless 句柄表、顶点阶段参数缓冲区、从 guest 池解块上传的 2x2 纹理、从离线 bundle 按 guest key 取 MSL；另加一条 sampler 状态创建/比较路径） |
| compute / geometry shader | 后端已有 host 自测 MSL 的 compute-only direct / indirect dispatch 路径（uniform / storage buffer、image、sampled texture、sampler，组合与分离句柄）；`extract` 仍跳过 cs，geometry 仍跳过 |
| 运行时按 key 取 MSL | **已接线**（M3.78，见 §5.1） |
| 顶点描述符 | **已接**（M3.79） |
| argument buffer（UBO / 纹理 / sampler / c1） | UBO 已接（M3.81）；texture/sampler 条目按固定 8 字节 ABI 写入（M3.83）；真实 shader 的 `[[id(k)]]` 布局侧车已接（M3.84）；c1 已接（M3.85），但 bindless / 顶点参数缓冲区仍未解 |
| Metal 3 参数缓冲区的纹理条目 | 未验证；模拟器 `gpuResourceID == 0` 改走直连绑定，需原生 macOS Metal 离屏测试或真机 |

一句话：**链的前 2/3 在 Windows 上真的通了，后 1/3（`xcrun metal`）要在 CI 的
macOS 上补；运行时按 key 取 MSL 已接线（M3.78），顶点与索引都已接（M3.79 / M3.80），
UBO 已进 argument buffer（M3.81），sampler 状态对象已能创建（M3.82），
texture/sampler 条目也已接（M3.83，模拟器走直连绑定），真实 shader 的
`[[id(k)]]` 布局侧车已接（M3.84），c1 已接（M3.85）；bindless 与顶点参数缓冲区已接（M3.86 / M3.87）；
block-linear 解块与 guest 池纹理上传已接（M3.88 / M3.89），离线 MSL bundle
也已在模拟器里按 guest key 命中和绘制（M3.90）。**真实标题 shader 仍然没有在
Metal 里执行过**：现在缺的是把真实翻译产物放进 bundle 后跑通首次标题 draw，
以及 `xcrun metal` 的编译期校验和 `.metallib` 打包。

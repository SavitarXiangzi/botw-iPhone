# 着色器链：Maxwell SASS → SPIR-V → MSL

本轮（M3.77）把这条链**在 Windows 上真的跑通了**：从《旷野之息》的
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

产物：`<key>_<stage>.metal`（入口已改名成 `nvngfx_main_vs` / `nvngfx_main_fs`）
和 `<key>_<stage>.json`（反射：cbuf/sbuf/texture 的 binding 与槽位、翻译器 notes）。

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
  的控制流是错的（粒子着色器会死循环）。
* shader key（与 nxnative 的 `shader_map.tsv` 第 4 列、我们 `prog_key` 一致）：
  `sha256(control 前 0x880 字节（控制段更短时取全长）+ code area)[0..8]`，
  小写 16 进制。code area **含** 0x30 头。
* 包是三层：`SARC`（BOTW 的 pack 把 SFAT 紧跟在头后面、`data_offset` 指向数据区，
  不是标准的指向 SFAT；两种都要试）→ 成员常常是 `Yaz0` → 解开后又是 SARC 或 BNSH。

## 5. 与运行时的接口

我们这一侧新增了 `nvngfx_shader_lookup(key, stage, why)` 与
`nvngfx_shader_set_blobs()`（`ios/nvn/include/nvn_shader.h`）：离线产物按 key
装进一张静态表，后端编译管线时按 key 取 MSL。入口名固定为
`NVNGFX_ENTRY_VERTEX` / `NVNGFX_ENTRY_FRAGMENT`，由生成脚本改名，两边不会漂。

**还差的接线**（下一步）：

1. `nvnProgramSetShaders` 时算出每个 stage 的 key —— 需要从客体的 control+code
   读内存（`prog_key` 的注释已经写死了算法，缺的是宿主侧地址翻译）。
2. `program_pipeline()` 用 key 去 `nvngfx_shader_lookup()`，而不是只看指针。
3. 顶点输入：生成的 MSL 是 `[[attribute(0)]] ...` 形式，需要按反射建
   `MTLVertexDescriptor` —— 这正是还挂着的 `BindVertexAttribState`。
4. 资源绑定：MSL 用的是 `--msl-argument-buffers`，所有常量/纹理走
   `buffer(0)` 的 `spvDescriptorSetBuffer0`，槽位约定见 §6。

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

窗口原点左上、深度 0..1，与 Metal 一致，翻译时**不做**翻转。

## 7. 明确没做到的

| 项 | 状态 |
|---|---|
| `xcrun metal` → `.metallib` | 未做；只能在 macOS/CI 上做，本机没有 |
| 整作 19,592 个 shader 全量构建 | 未做；只验证了 1 个 pack 的 5,102 个 key 中的 2 个 |
| 编译期校验（MSL 语法） | 未做；Windows 没有 Metal 前端，MSL 只在 CI 的 macOS 上才能编 |
| 真机 / 模拟器上跑真实 shader | 未做。自检用的仍是内置 MSL |
| compute / geometry shader | 未做（`extract` 目前跳过 cs） |
| 顶点描述符与 argument buffer | 未接线，见 §5 |

一句话：**链的前 2/3 在 Windows 上真的通了，后 1/3 要在 CI 的 macOS 上补，
运行时接线还没做。**

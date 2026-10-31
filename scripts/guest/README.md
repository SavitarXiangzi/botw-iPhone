# 客体核心（guest core）工具链

目标：让 1.6.0 的客体 ARM64 代码在 iOS 上**原生执行**，而不是把 Unicorn
解释器搬进 app。iOS 不允许运行时生成可执行代码，因此路线是构建期
**静态改写 + 静态嵌入**（与 nxnative 相同）：

```text
retail NSO  --rewrite_botw.py-->  patched.bin + fixups/gates/sites
                                      |
                             iOS 构建期嵌入 __TEXT
                                      |
                         运行时映射 + HLE 导入解析 + 入口调用
```

## 当前进度

- [x] 1.6.0 四个模块的 build ID 固定并拒绝其他版本
- [x] 通用改写规则在 1.6.0 上跑通：main / sdk / subsdk0 / rtld
- [x] 输出结构校验：无残留 `svc`、每个 gate/site 都跳进 patch 区
- [x] 运行时 loader 的可移植部分（fixups 解析、私有复制、重定位、导入解析）
- [ ] 可执行映射（text 按 RX、data 按 RW、patch 区单独映射）
- [x] 每线程槽布局 + key page 发布（`guest_slot.c`）
- [ ] `tpidrro_el0` 指向槽（需要 AArch64 汇编 + 线程切换）
- [ ] HLE 服务层（init / fs / vi / nv / NVN bootstrap）
- [ ] 把 NVN 调用桥到 `ios/nvn` 的 Metal 后端
- [ ] 在 iOS 模拟器/真机执行第一条客体指令

## 用法

需要本机有 1.6.0 的 ExeFS（`main`、`sdk`、`subsdk0`、`rtld`，均为
`NSO0`）以及 Python 3 + `capstone` + `lz4`：

```powershell
python scripts/guest/scan_nso.py <ExeFS/update>/main <ExeFS/update>/sdk ...
python scripts/guest/rewrite_botw.py --nso-dir <ExeFS/update> --out build/guest
python scripts/guest/verify_rewrite.py build/guest main
python scripts/guest/probe_relocs.py build/guest/main.patched.bin --fixups build/guest/main.fixups.tsv
# 有 MSVC 时：把真实镜像完整装载一遍（只读，不执行客体代码）
cl /std:c++20 /I core\guest scripts/guest/reloc_run.cpp core/guest/guest_image.c /Fe:build\reloc_run.exe
build\reloc_run.exe build/guest/main.patched.bin build/guest/main.fixups.tsv
```

`rewrite_botw.py` 会先核对 0x20 字节 build ID，再调用 vendored 的
nxnative 改写器；任何版本不符都会在写文件前退出。

## 2026-10-31 的实测统计（1.6.0，v786432）

| 模块 | text | svc | x18 站点 | TLS 读写 | patch 大小 | 结构校验 |
|---|---:|---:|---:|---:|---:|---|
| main | 0x2129BA4 | 94 | 15,188 | 0 | 0xAB994 | ok |
| sdk | 0x52D880 | 60 | 11,297 | 55 | 0x7041C | ok（3 条 1.6.0 SDK 定点补丁） |
| subsdk0 | 0x3732B0 | 0 | 11,093 | 0 | 0x6CCF4 | ok |
| rtld | 0x1A00 | 4 | 24 | 0 | 0x438 | ok |

这些数字只说明“改写完成且结构自洽”，不说明 HLE 已经能跑。`main` 的
patch 区约 703 KiB，远小于 iOS 的地址空间；真正的下一堵墙是 loader 和
HLE 服务。

## 重定位画像（1.6.0）

`probe_relocs.py` 与 `reloc_run` 在四个模块上得到的结果：

| 模块 | R_RELATIVE | R_ABS64 | GOT/PLT | 其中需 HLE | 唯一 HLE 名 |
|---|---:|---:|---:|---:|---:|
| main | 210,547 | 959 | 455 | 1,414 | 458 |
| sdk | 9,376 | 13,453 | 8,838 | 16 | 13 |
| subsdk0 | 1,677 | 12,694 | 4,804 | 6,545 | 647 |
| rtld | 15 | 0 | 19 | 19 | 19 |

两个容易踩的点，都已经写进 `guest_image.c`：

1. `R_ABS64` / `R_GLOB_DAT` / `R_JUMP_SLOT` 都是 `S + A`，不是 `base + A`；
2. 动态符号绝大多数是模块**自己定义**的（sdk 的 8,822 个 GOT 槽），只有
   `st_shndx == 0` 的才走 HLE。

## 第三方代码

`third_party/nxnative/` 是 [nxnative](https://github.com/seedds/nxnative)
的 MIT 代码快照（`nso*.py`、`patch_nso.py`），只做了两处改动：

1. `sdk_patches.py` 裁成只保留 1.6.0 SDK 的三条 `AddressRegionManager`
   补丁，并关闭 SDK-wide `MATCH_CUTS`；
2. 新增本目录的 profile（`profiles/01007EF00011E000/`），不含任何游戏数据。

原版权声明见 `third_party/nxnative/LICENSE`。所有 1.6.0 地址都来自本机
重新解析，**没有**照抄 nxnative 的 1.9.0 补丁表。

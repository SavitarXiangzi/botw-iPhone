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
- [x] 可执行映射（`guest_map.c`）：整块 mmap → 重定位 → 按页 mprotect；Linux 4 KiB / macOS 16 KiB 实测
- [x] 每线程槽布局 + key page 发布（`guest_slot.c` + `botw_guest_map_publish_slot()`）
- [x] 槽地址发布（`SLOT_BASE=abs`；`TPIDRRO_EL0` 在 EL0 只读，见 ROADMAP 续三十七）
- [x] gate trampoline（`guest_gate_arm64.S`）：x0..x30 / sp / pc / nzcv 完整帧，CI 的 arm64 job 实测往返
- [x] 链式测试（`guest_chain_test`）：真实映射 → key page 发布 → KGATE → trampoline → gate 块 → 寄存器往返
- [x] HLE 注册表 + libc 直通（`guest_hle.c`）：未知名字共享零返回 stub，46 个 libc 实现（含 3 个本地小包装）
- [x] 页级保护计划（`guest_segments.c`）：真实四模块 5/5/5/4 段；rtld 在 16 KiB 页上 rwx 冲突，如实报告
- [x] `guest_run`：独立客体栈 + 守卫页 + 入口/退出哨兵；合成客体在 macOS arm64 CI 真正执行
- [x] 协作式客体线程（`guest_thread_test`）：独立栈、272 字节帧保存/恢复、轮转切换、线程退出；三线程在 arm64 CI 通过
- [ ] `nn::os` 线程/同步原语接到线程表（CreateThread / StartThread / ExitThread / WaitSynchronization）
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
cl /std:c++20 /I core\guest scripts/guest/reloc_run.cpp core/guest/guest_image.c core/guest/guest_slot.c core/guest/guest_segments.c core/guest/guest_hle.c core/guest/guest_hle_nn.c core/guest/guest_hle_fs.c /Fe:build\reloc_run.exe
build\reloc_run.exe build/guest/main.patched.bin build/guest/main.fixups.tsv
# --slot: 把合成槽发布进真实镜像的 key page 并核对四个索引（不执行客体代码）
build\reloc_run.exe build/guest/main.patched.bin build/guest/main.fixups.tsv 0x4000 --slot
# --hle: 用 HLE 注册表解析导入，并报告实现/未知名字数量
build\reloc_run.exe build/guest/main.patched.bin build/guest/main.fixups.tsv 0x4000 --hle
# --names: 逐条列出每个导入命中了实现还是 stub，最后按 C++ 命名空间汇总剩余 stub
build\reloc_run.exe build/guest/main.patched.bin build/guest/main.fixups.tsv 0x4000 --names
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
2. 新增本目录的 profile（`profiles/01007EF00011E000/`），不含任何游戏数据；
3. 新增 `SLOT_ABS` 模式（`DM_SLOT_ABS=1`）：key page 存槽字段的绝对地址，
   块里不再读 `TPIDRRO_EL0`（Apple arm64 上 EL0 写它会 SIGILL）。默认关闭，
   上游行为不变。

原版权声明见 `third_party/nxnative/LICENSE`。所有 1.6.0 地址都来自本机
重新解析，**没有**照抄 nxnative 的 1.9.0 补丁表。

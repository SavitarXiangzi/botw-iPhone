# -*- coding: utf-8 -*-
import io

# ---------- README ----------
p = 'README.md'
s = io.open(p, encoding='utf-8').read()

row_anchor = "| M4 | NVN → Metal 回放（管线/纹理/命令缓冲） | ⬜ |"
assert s.count(row_anchor) == 1
rows = (
"| M3.24 | mutex 的 `WAITERS_BIT`：解锁走了用户态快路径，等待者永远醒不过来（位算错，见 M3.26） | ✅ 帧泵活了 |\n"
"| M3.25 | 反汇编纠正 M3.24：等待位是 bit30；`DT_RELR` 在 1.6.0 不存在（loader 无需改） | ✅ |\n"
"| M3.26 | 互斥真正修好：主线程 tag=0 让 M3.24 的互斥形同虚设；现在线程真同步、资源真加载 | 🟢 20 帧 + 541 MiB，卡在吞吐 |\n"
)
s = s.replace(row_anchor, rows + row_anchor)
io.open(p, 'w', encoding='utf-8', newline='\n').write(s)

# ---------- ROADMAP ----------
p = 'docs/ROADMAP.md'
s = io.open(p, encoding='utf-8').read()
anchor = "### M4 Metal 回放"
assert s.count(anchor) == 1

section = u"""### M3.26 互斥真的修好了：帧数掉下来，是因为 guest 开始干真活

M3.25 的结论是"主线程持锁自旋"，修法是"给主线程分配句柄再对齐 `+0x1B0`"。这一轮
按 M3.25 的待办插桩，发现**修法是错的**：问题不在交接，在于 guest 自己读到的 tag 是 0。

#### 1. 三点插桩：先把"谁在转"钉死

在 `sdk+0x32F79C`（`Lock` 刚把 tag 读进 w20）、`0x32F7D0`（准备 `svcArbitrateLock`）、
`0x32F7E8`（SVC 之后的比较）三点各记一次 `(tid, cs, word, w20)`：

```
lock   406 次
slow   89,908 次      <- 全在同一个 Lock 调用内部的 retry 循环里
retry  89,906 次
svc 1A 105,798 次     svc 1B 1 次
最热 (tid=0x100, cs=0x1080BE140, word=0x40000100, w20=0x0)
```

`word` 里的 owner `0x100` 是**我们内核写的**（`tid_handle`），而 guest 的 `w20` 是
**0**。`Lock` 的退出条件是 `(*cs & 0xBFFFFFFF) == w20`，于是它永远不相等，
在 12 s 里空转 18 万次 SVC——这就是 M3.25 看到的"持锁方自己打转"。

#### 2. 根因：`ThreadType+0x1B0` 一直是 0，而且我们补得太晚

`nn::os::detail::ThreadManager` 的构造函数（`sdk+0x32A2F0`）在 `0x2032A340` 执行
`str x20, [x8, #0x1f8]`，把 `[tpidrro_el0+0x1F8]` 从我们 boot 写的 `0x61000000`
换成自己的成员 `0x20CA5BC0`；那个对象的 `+0x1B0` 没有任何代码去填（扫描 sdk/main/
subsdk0 里全部 357 处 `#0x1b0` 访问，没有一处命中它）。

关键细节：`Lock` 在**函数入口**（`0x32F798`）就把 `+0x1B0` 读进 `w20`，整个重试
循环只用寄存器里的值。所以 M3.25 那种"在 SVC 处理里补写内存"对**当前这次调用无效**，
补完照样自旋——那版"1 帧"不是死锁，是补丁生效得太晚。

#### 3. 三处修正

| # | 改动 | 为什么 |
|---|---|---|
| a | 在 `[tpidrro+0x1F8]` 被换掉的那一刻（`UC_HOOK_MEM_WRITE`）把 `+0x1B0` 填成本线程句柄 | 必须早于任何一次 `Lock`；晚一步就白补 |
| b | `svcArbitrateLock` 的身份改用 **guest 传进来的 `x2`**，不再用内核的 `tid_handle` | 内核与 guest 各有一套编号时，任何一边"自以为是"都会让退出条件不成立 |
| c | 交接时队列里还有人，就保留 bit30 | M3.25 说"带 bit30 会退不出"是错的：退出条件掩的是 `0xBFFFFFFF`，正好清掉 bit30 |

#### 4. 数据：帧数暴跌不是退步

| 观测（`BOTW_BARE=1`，无钩子） | vA（假互斥） | M3.26（真互斥） |
|---|---:|---:|
| `m2_verify` 45 s | 11,759 帧 | 13 帧 |
| `m3_verify` 30 s | 7,353 帧 | 13 帧 |
| 裸跑 120 s | — | 175 帧 / 870 MiB |
| 裸跑 300 s | 115,686 帧 / 335 MiB | 20 帧 / 541 MiB |
| `svcArbitrateLock` 累计 | 105,798 | 95 |

vA 的 11 万帧是假互斥喂出来的：主线程 tag=0，`(word & 0xBFFFFFFF) == 0` 在低 30 位为 0
时恒真，每次争用都"静默成功"，工作线程全被饿死，present 循环空转。现在主线程真的等锁、
工作线程真的跑，guest 开始构建资源表。

#### 5. 凭据：这次是"真在干活"

* 40 s 内 `memcmp` 677,521 次、**43,309 个不同键**
* 调用方 `main+0x14E01D0`（48 字节记录）占 649,875 次；`main+0x13ABEF4` 比的是
  `gys_skeleton` / `gys_shape` / `gys_material` / `gys_shader_options` 这类名字
* 堆 335 MiB → 541～870 MiB，`fs` 读与 `nvnBufferInitialize` 都在推进
* 没有 `DEADLOCK`，没有 CS 自旋

#### 6. 新墙：吞吐，不是正确性

300 s 只推进 4,151 个 chunk ≈ 83 亿条指令 ≈ **28 M instr/s**，且几乎全部花在主线程的
`memcmp` 扫描上（无 SVC、无 I/O、无分配）。这是 x86 主机上 Unicorn TCG 的量级。
要继续往下走，只有两条路：把 sdk 的热叶子函数（`memcmp`/`memcpy`/`memset`）HLE 掉，
或者换执行后端（TCG JIT / AOT）。前者当天就能验证收益。

#### 7. 下一步

1. HLE `memcmp`/`memcpy`/`memset`（先只 HLE 被调用最多的那几个入口），量 `chunks/s` 与
   `MiB` 的变化；同时确认键数继续增长（不增长就是退化成了死循环）。
2. 把 `+0x1B0` 的补齐收敛成一条通用规则，并加断言：交给 `svcArbitrateLock` 的 tag
   必须等于 `[[tpidrro_el0+0x1F8]+0x1B0]`，不等就打印一次。
3. 采样器记录 `(frames, MiB, 当前热点函数)` 三元组——单看帧数会再次被假互斥误导。

"""
s = s.replace(anchor, section + anchor)
io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')

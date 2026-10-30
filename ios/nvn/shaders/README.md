## 现在这里有一个合成 fixture（不是游戏数据）

`manifest.tsv` / `bundle-vs.msl` / `bundle-fs.msl` 是**本仓库自己写的**
最小 bundle，用来验证“文件 -> blob 表 -> 按 key 建管线”这条路：

```
key              stage  file            c1  args
e01e5955f6f39e29  vs    bundle-vs.msl   -   -
e01e5955f6f39e29  ps    bundle-fs.msl   -   -
```

- `key` 是那对合成 control+code 的 SHA-256 前 8 字节（规则见
  `docs/SHADER_CHAIN.md` §4），自检第十条 draw 现场算出来再查。
- 运行时 `nvngfx_shader_load_bundle()` 读这个目录；App 里默认找
  `<app>/shaders`，`NVNGFX_SHADER_DIR` 可以覆盖。
- 真实翻译产物（几千个 `.msl` + `.c1`）同样按这个格式放，**不要入库**；
  `gen_blobs.py --bundle-out <dir>` 可以直接生成这一目录。
- manifest 的 args 列中，`kind` 是 `nvngfx_arg_kind` 的数值（1..10），
  不是 C 枚举名。

这个目录现在**只有合成 fixture，没有真实标题着色器**。原因是 NVN 和 Metal 在这一点上
没有可自动翻译的公共层：

- 标题交给 NVN 的不是着色器源码，而是**已经编译好的 Maxwell（Tegra X1）GPU
  机器码**，以 `NVNprogram` 的形式传给 `nvnProgramInitialize`。
- Metal 只接受 Metal Shading Language 源码或 `.metallib`，没有接受
  SASS/PTX 的入口，也没有公开的驱动层可以挂。

所以路线只能是**离线翻译**，产物放这个目录：

```
Maxwell SASS  --(反汇编)-->  Maxwell IR  --(提升)-->  SPIR-V  --(spirv-cross)-->  MSL
```

待办（按依赖顺序）：

1. 从 `yaz1_*` 解出的 `main` 里定位 `NVNprogramInitialize` 的调用参数，
   拿到 SASS 段的地址和长度（M3 阶段只做到 NVN 调用序列，没做到 program）。
2. 用 `maxwell3d` 之类的反汇编器把 SASS 转成可分析的 IR。
3. 先只覆盖《旷野之息》真正用到的指令子集（`FMUL/FFMA/LDG/STG/TEX/BRX` 等），
   不要一上来追求通用。
4. 生成的 MSL 按 pipeline key 缓存成 `.metallib`，运行时用
   `newLibraryWithData:` 加载。

在 shader 跑通之前，Metal 后端能做的“有画面”的事只有 clear + present，
这一点已经写在 `docs/IOS_PORTING.md` 的“渲染管线分阶段”一节里。
---

## 2026-10-30 更新（M3.77）

上面的"待办"第 2、3 条已经不成立了：**不需要自己写反汇编器/提升器**。
参考实现 nxnative 用 Ryujinx 的 shader 翻译库把 Maxwell SASS 直接译成 SPIR-V，
再用 SPIRV-Cross 转 MSL；这条链已经在 Windows 上跑通并验证过真实 shader：

```powershell
powershell -File scripts/shader_chain/build_tools.ps1 -Src <nxnative checkout>
C:\Python314\python.exe scripts\shader_chain\nx_shader.py extract <pack> --out build\shader-chain\work
C:\Python314\python.exe scripts\shader_chain\nx_shader.py translate --work ... --out ...
```

细节、已验证的数字、格式要点和还缺的步骤见
[`docs/SHADER_CHAIN.md`](../../../docs/SHADER_CHAIN.md)。

这个目录仍然**不存放任何游戏派生产物**（`.metal` / `.metallib` 都在
`build/` 下，已 gitignore）；要用的时候由 CI 或本机生成。

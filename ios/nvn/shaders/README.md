# shader/ -- 空目录，但必须存在

这个目录现在**没有可用的着色器**，只放说明。原因是 NVN 和 Metal 在这一点上
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
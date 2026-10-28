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
| M2 | 启动链 HLE（nn::oe / fs / vi / nv / NVN 引导） | 🟡 进行中 |
| M3 | 线程模型 + NVN 设备/队列/窗口 | ⬜ |
| M4 | NVN → Metal 回放（管线/纹理/命令缓冲） | ⬜ |
| M5 | 音频（nn::audio → AVAudioEngine）、输入 | ⬜ |
| M6 | 真机性能与内存（6 GB 机型起步） | ⬜ |

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

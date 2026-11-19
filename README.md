# Pokémon Let's Go —— HarmonyOS（鸿蒙）启动器 / 移植层

把 `Let's Go` 静态重编译运行时（LASRecomp 体系）移植到 HarmonyOS / OpenHarmony 的
**启动器 + 平台桥 + 移植层源码**。

本项目是**非官方、学习性质**的移植，**不包含、也不会提供**游戏本体、游戏数据、
重编译后的游戏代码或任何解密/导入工具。要运行，你必须使用自己合法拥有的游戏。

> 本项目基于以下开源项目移植，请一并遵守它们的许可证与致谢：
> - [raulzrrs / letsgo-recomp](https://github.com/raulzrrs/letsgo-recomp) —— PC 原版（静态重编译）
> - [CowKillers / letsgo-recomp-android](https://github.com/CowKillers/letsgo-recomp-android) —— Android 移植参考
> - [peiikai / LASRecomp](https://github.com/peiikai/LASRecomp) —— 运行时基础（MIT，见 `LICENSE-LASRecomp`）

---

## 免责声明

- 本项目与 Nintendo / The Pokémon Company / Game Freak **没有任何关系**。
- 仓库只包含**启动器与移植层代码**，不包含 `Pokémon: Let's Go, Eevee! / Pikachu!` 的
  任何游戏内容（ROM、NSP、XCI、romfs、exefs、模型、贴图、音频等）。
- 请使用你自己合法购买并导出的游戏数据；本项目**不提供**提取/解密/导入工具，也不教授这些操作。
- 仅供个人学习、研究与技术交流，请勿用于商业用途，请支持正版。

代码许可证沿用上游 **MIT**（见 `LICENSE`、`LICENSE-LASRecomp`）。MIT 允许再分发，
但上述免责声明与“不含游戏本体”的边界不因许可证而改变。

---

## 仓库结构

```
harmony-launcher/            HarmonyOS 应用（ArkTS + C++ 桥）
  AppScope/                  应用级配置
  entry/src/main/ets/        ArkTS 界面：启动器封面、数据导入、显示设置、游戏设置、虚拟手柄
  entry/src/main/cpp/        C++ 桥：XComponent/Surface、dlopen 运行时、NAPI、启动游戏
  entry/src/main/resources/  UI 资源（注意：不含 rawfile/data 游戏数据）
  build-profile.json5        ★ 已脱敏：不含签名证书，请自行在 DevEco 配置签名

runtime/                     移植层 / 运行时源码（非游戏代码）
  src/runtime/               HLE（fs/am/hid/movie…）、NVN→Vulkan、SDL3 shim、输入、音频
  src/third_party/           imgui / vma / smaa 等第三方源码（各自许可证）
  src/docs/                  上游文档
  platform/ohos/             OpenHarmony 平台层：SDL shim、Vulkan 设备、FFmpeg stub、OHCodec 解码封装
  compile_*.sh / link-*.sh   OHOS 交叉编译与链接脚本（参考用）
  BUILD.md / README-upstream.md  上游构建说明

screenshots/                 运行截图（仅展示，不含游戏数据）
```

**不在仓库中的东西（需要你自己准备/构建）：**

- `repo/src/generated-aarch64`、`generated-x86_64`：游戏重编译代码（由你的游戏 dump 生成）
- `exefs/`、`romfs.bin`、`romfs.gz`：游戏数据（由你的合法 dump 得到）
- `liblasgame.so`：重编译后的游戏库（需要你生成）
- `liblasrecomp.so`：运行时二进制（请用本仓库源码自行构建）
- 预编译的 FFmpeg / libopus / shaderc 静态库（请按官方源码自行构建，见下）
- NSP/XCI 导入、解密工具（本项目不提供）

---

## 工作原理（简述）

- **HLE（High Level Emulation）**：`runtime/src/runtime/hle` 用宿主实现 Switch 系统服务
  （文件系统、AM、HID、movie:: 等），游戏代码通过重编译后的地址调用这些服务。
- **图形**：把游戏的 NVN 调用翻译成 Vulkan（`runtime/src/runtime/nvn/vulkan`），
  `platform/ohos` 负责 OHOS 原生 Surface / XComponent / `vkCreateSurfaceOHOS` 等接入。
- **视频动画**：`movie::`（Android MediaExtractor/MediaCodec 风格）由 FFmpeg 实现，支持
  软件解码，并可选用 OHCodec 硬解封装（`platform/ohos/ffmpeg-ohdec-vp9`）。
- **输入/音频**：SDL3 shim 把触摸/手柄映射成 Npad，音频走 SDL/OHAudio。
- **启动器**：ArkTS 负责封面、数据导入（zip/romfs.gz）、显示设置、游戏设置与虚拟按键，
  C++ 桥把 XComponent 的 Surface 交给运行时并 `dlopen` 它。

---

## 构建步骤

### 0. 前置

- DevEco Studio（含 HarmonyOS SDK，`compatibleSdkVersion 26.0.0`）与 OHOS clang
- CMake / Ninja
- 你自己合法游戏 dump 得到的 `exefs/` 与 `romfs.bin`（**不提供**）
- 自备编译好的第三方静态库：FFmpeg（libavcodec/libavformat/libswscale/libswresample/libavutil）、
  libopus、shaderc、Vulkan 头文件（参考 `runtime/compile_*.sh`、`runtime/link-*.sh`）
- 用上游工具链把游戏重编译成 `generated-aarch64` 与 `liblasgame.so`（**本项目不包含该工具链与产物**）

### 1. 构建运行时 `liblasrecomp.so`

参考 `runtime/compile_a64.sh`、`runtime/link-ohos-vp9.sh`，用 OHOS clang 把
`runtime/src` + `runtime/platform/ohos` 交叉编译为 `aarch64-linux-ohos` 的 `.so`，
链接你自备的 FFmpeg/opus/shaderc/Vulkan 库。

把产物放到：

```
harmony-launcher/entry/src/main/cpp/third_party/liblasrecomp.so
```

### 2. 准备游戏库 `liblasgame.so`

把你自己生成/编译的 `liblasgame.so` 放到同一目录：

```
harmony-launcher/entry/src/main/cpp/third_party/liblasgame.so
```

（这两个 `.so` 都在 `.gitignore` 里，不会被提交。）

### 3. 构建 HAP

用 DevEco Studio 打开 `harmony-launcher/`：

1. `File → Project Structure → Signing Configs` 配置你自己的调试/发布签名（仓库里的
   `build-profile.json5` 已移除签名信息）；
2. 同步并构建 `entry`，生成 HAP；
3. 安装到鸿蒙设备。

### 4. 导入数据

应用启动后点「导入数据」，选择你用自己游戏构造的 `romfs.gz` 或数据 zip。
**本项目不提供任何游戏数据包，也不提供制作/提取工具。**

---

## 已知问题 / 现状

- 帧率受设备 GPU/CPU 影响较大（野外植被 draw call 多时掉帧明显）；
- 标题动画呈现阶段可能有掉帧；
- 稳定版运行时下，「＋/－」触发的 ImGui 菜单可能崩溃（已回退到稳定运行库以优先保证游戏不闪退）；
- 语言与「跳过选择手柄画面」依赖游戏本体行为，可能不响应；
- 不同 OHOS 版本的 Vulkan/媒体服务行为存在差异。

欢迎提 Issue 讨论技术问题；请勿在 Issue 中上传游戏数据或任何受版权保护的文件。

---

## 致谢

- raulzrrs（letsgo-recomp）、peiikai（LASRecomp）、CowKillers（Android 移植）
- FFmpeg、SDL3、Dear ImGui、Vulkan Memory Allocator、SMAA 等开源项目
- 所有为移动端重编译/Vulkan 移植做出贡献的人

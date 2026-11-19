# 第三方依赖与许可证（THIRD_PARTY）

本项目**只开源自己的启动器与移植层代码**；下面这些是第三方开源组件。
我们遵循“**注明来源与许可证**”的原则，不把这些说成自研。

| 组件 | 版本 / 来源 | 许可证 | 是否随本仓库 git 分发 |
|---|---|---|---|
| **FFmpeg** | 8.1.2，OHOS universal 构建；构建工程参考 [debugly/MRFFToolChainBuildShell](https://github.com/debugly/MRFFToolChainBuildShell) | LGPL-2.1-or-later（若该构建启用了 GPL 组件，则为 GPL，以其 `configure` 为准） | **否**（`.a` 被 `.gitignore` 排除；许可证文本放在 `third_party/ffmpeg/`） |
| libdav1d | 随 FFmpeg 构建 | BSD-2-Clause | 否 |
| libopus | 随运行库构建 | BSD-3-Clause | 否 |
| OpenSSL（libcrypto/libssl） | 随 FFmpeg 构建 | Apache-2.0 | 否 |
| libxml2 | 随 FFmpeg 构建 | MIT | 否 |
| libbluray | 随 FFmpeg 构建 | LGPL-2.1 | 否 |
| SoundTouch | 随 FFmpeg 构建 | LGPL-2.1 | 否 |
| libuavs3d | 随 FFmpeg 构建 | 见其源码仓库 | 否 |
| SDL3（shim） | 运行时窗口/输入/音频 | zlib | 源码在 `runtime/platform/ohos/sdl3shim` |
| Dear ImGui | `runtime/src/third_party/imgui` | MIT | 是（源码） |
| Vulkan Memory Allocator | `runtime/src/third_party/vma` | MIT | 是（源码） |
| SMAA | `runtime/src/third_party/smaa` | MIT | 是（源码） |
| shaderc / glslang / SPIRV-Tools | 运行时 GLSL→SPIR-V | Apache-2.0 / BSD-3-Clause | 否（构建时自行准备） |

## FFmpeg 的发布说明（重要）

- 本仓库**不把** FFmpeg 的预编译 `.a` 放进 git（体积大、二进制，且不便于审计）；
- 如果你要提供预编译库给他人，建议放到 **GitHub Release**（而不是源码仓库），并同时提供：
  1. 本文件与 `third_party/ffmpeg/COPYING.LGPLv2.1`（以及启用 GPL 时 `COPYING.GPLv2`）；
  2. 构建来源（例如 `https://github.com/debugly/MRFFToolChainBuildShell`）与 FFmpeg 版本 8.1.2；
  3. 对应的 FFmpeg 源码获取方式（官方 `ffmpeg-8.1.2` 源码 + 该工程的构建配置）；
  4. 如果是**静态链接**到你的程序里，LGPL 还要求允许使用者替换该库/提供目标文件等；建议直接让使用者自行构建，或走动态库方式。
- 使用本项目构建时，请自行准备兼容的 FFmpeg 静态库或换成动态库；本项目不附带二进制。

## 游戏内容（不在本仓库）

- 游戏本体、ROM/NSP/XCI、`exefs/`、`romfs.bin`、重编译后的游戏代码（`liblasgame.so`、`generated-*`）**均不包含**；
- `screenshots/` 与封面仅用于展示项目效果，相关美术/角色版权归 Nintendo / The Pokémon Company / Game Freak；如你二次发布请自行评估并注明来源。

## 许可证文件位置

- 本项目代码：`LICENSE`（MIT，继承上游）
- LASRecomp：`LICENSE-LASRecomp`
- FFmpeg 相关：`third_party/ffmpeg/`

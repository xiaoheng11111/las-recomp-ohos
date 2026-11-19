# 架构说明（ARCHITECTURE）

本文只讲**结构与原理**，不包含任何游戏数据、游戏代码或受版权保护的内容。
目标是让拿到源码的人能理解每一层在做什么、接口是什么、缺的部分如何用自己的代码独立实现。

---

## 1. 分层总览

```
┌─────────────────────────────────────────────────────────────┐
│ ArkTS 启动器 (harmony-launcher/entry/src/main/ets)          │
│  封面 / 数据导入 / 显示设置 / 游戏设置 / 虚拟手柄 / 日志         │
└───────────────▲─────────────────────────────────────────────┘
                │ NAPI (libentry.so)
┌───────────────┴─────────────────────────────────────────────┐
│ C++ 平台桥 (harmony-launcher/entry/src/main/cpp)             │
│  XComponent → OHNativeWindow → Vulkan Surface                 │
│  dlopen liblasrecomp.so / 设置环境变量 / 数据桥（exefs,romfs）  │
└───────────────▲─────────────────────────────────────────────┘
                │ dlopen / SDL_main
┌───────────────┴─────────────────────────────────────────────┐
│ 运行时 liblasrecomp.so (runtime/src + runtime/platform/ohos) │
│  HLE 服务层  +  NVN→Vulkan 图形层  +  SDL3 shim / 输入 / 音频   │
└───────────────▲─────────────────────────────────────────────┘
                │ 游戏重编译产物（用户自备，不随仓库分发）
┌───────────────┴─────────────────────────────────────────────┐
│ liblasgame.so（游戏逻辑，用户用自己的合法 dump 生成）            │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 启动链（从点击图标到游戏画面）

1. `EntryAbility.onWindowStageCreate()`：加载页面，设置沉浸式全屏；
2. 用户在封面点「开始游戏」→ ArkTS 调 `startGame(rm, hapsFilesDir, appFilesDir)`；
3. `game_entry.cpp`：
   - 计算数据根 `<appFiles>/lasrecomp`；
   - 从 `larecomp.ini` 读取显示/音频/movie 等设置并设置 `LARECOMP_*` 环境变量；
   - `dlopen("liblasrecomp.so")`，取 `SDL_main` 并起游戏线程；
   - 把 XComponent 的 `OHNativeWindow` 交给运行时（Vulkan Surface）；
4. 运行时 `main.c`：
   - `plat_init` → `paths` → `gamelib_load`（加载 `liblasgame.so`）→ `settings_load` → `kernel_init`；
   - 进入游戏主循环，游戏通过 HLE 调用系统服务、通过 NVN 调用 GPU。

---

## 3. 数据桥（游戏数据如何被读取）

- `romfs.bin` + `exefs/` 位于数据根：
  ```
  <appFiles>/lasrecomp/
    exefs/{main,main.npdm,rtld,sdk,subsdk0,subsdk1}
    romfs.bin
    save/  cache/  larecomp.ini
  ```
- 数据可由两种方式导入：
  - **zip**：解压出 `extracted/exefs/*` 与 `extracted/romfs.bin`，native 以 `rename` 原子迁移到数据根；
  - **romfs.gz**：native 流式 gzip 解压（带 `.progress` 进度），同时迁移 exefs。
- 运行时 `services/fs.c` 解析 RomFS 目录/文件元数据，建立“文件路径 → (fd, offset, size)”索引；
- 视频 `movie.c` 直接把 `romfs.bin` 的字节区间当作 `AVIOContext` 交给 FFmpeg（`avformat_open_input` + 自定义 seek），所以游戏动画不需要先解包成独立文件。

> 游戏数据属于用户自己的合法副本；本仓库不包含、不提供制作工具。

---

## 4. HLE 服务层（runtime/src/runtime/hle + services）

HLE 用宿主代码实现 Switch 的系统服务，游戏通过重编译后的地址调用它们：

| 服务 | 作用 | 关键文件 |
|---|---|---|
| `fs` / `IStorage` | 读取 romfs/exefs/save | `services/fs.c` |
| `am` / `set` | 应用/locale/语言 | `services/am.c`、`hid_set.c` |
| `hid` | 手柄状态、语言 | `hid_set.c`、`hle/input.c` |
| `movie::` | MediaExtractor/MediaCodec 风格动画 | `hle/movie.c` |
| `kernel` / `dispatch` | 线程、SVC、模块加载 | `kernel.c`、`dispatch.c` |
| 其他 | 音频、网络、时间、存档等 | `services/*` |

注册方式：`HLE_FUNC("mangled_name", impl)` 在启动时建立符号表，游戏调用被分发到宿主实现。

---

## 5. 图形层（NVN → Vulkan）

- 游戏使用 NVN API；`runtime/nvn/objects.c` 把 NVN 对象/调用映射到 Vulkan：
  - 纹理/缓冲/采样器/管线/命令缓冲 → `textures.c`、`exec.c`；
  - 着色器：游戏给 GLSL/SPIR-V，运行时用 shaderc 编译并缓存到 `<root>/cache/spirv`；
  - 呈现：`device.c::backend_present` + `output_scale.c`（内部分辨率、窗口适配、FSR/SMAA 后处理）；
  - 渲染线程：`renderthread.c` 可选独立线程提交。
- 平台层（`platform/ohos`）只负责“把 OHOS 的 Surface/驱动接进来”，不改变 NVN 语义。

---

## 6. 平台层（platform/ohos）

| 文件/目录 | 作用 |
|---|---|
| `sdl3shim/`、`sdl_shim.cpp` | SDL3 子集 shim（窗口/输入/音频/线程），让运行时不需要完整 SDL 移植 |
| `vma_impl.cpp` | Vulkan Memory Allocator 的 OHOS 适配 |
| `ffmpeg_stub.c` | 无 FFmpeg 时的占位实现（返回失败，让动画被跳过而不是崩溃） |
| `ffmpeg-ohdec-vp9/` | 可选：FFmpeg 的 OHCodec 硬解封装（`ohdec.c`/`ohcodec.c`），把 H.264/HEVC/VP9 接到 `OH_AVCodec` |
| `ui_stub.cpp` / `smb2_stub.c` | UI/网络的最小占位 |

---

## 7. 视频动画（movie::）

- 接口是 Android 的 `MediaExtractor` / `MediaCodec` 风格；
- `movie.c` 用 FFmpeg 实现：
  - Extractor：`avformat_open_input`（本地文件或 `romfs.bin` 字节区间）、选轨、读包；
  - Decoder：`avcodec_find_decoder` 软解；输出 NV12 写入游戏自己的帧缓冲，或转 RGBA 上传纹理；
  - 音频：FFmpeg 解码 → float planar → 交给游戏混音。
- 设计要点：输出缓冲必须落在**游戏的可被 GPU 读取的堆**里，否则会出现绿屏/掉帧。

---

## 8. 输入 / 音频 / 设置

- 输入：`hle/input.c` 把触摸/键盘/手柄 → SDL 事件 → Npad 状态；支持脚本注入（测试/自动操作）；
- 音频：`services/audio.c` 实现游戏的 AudioRenderer，输出到 SDL（OHOS 下由 shim 接 OHAudio）；
- 设置：`larecomp.ini`（分辨率、上采样、SMAA、60fps、渲染线程、顶点读取、音量、movie 等），启动时读取；
- 存档：数据根下 `save/`，由 HLE 文件服务读写。

---

## 9. 哪些东西不在仓库里，以及为什么

| 缺失部分 | 原因 | 你如何自己得到 |
|---|---|---|
| `generated-aarch64`、`liblasgame.so` | 游戏重编译代码，版权内容 | 用你合法拥有的游戏 + 你自己的重编译工具链生成 |
| `exefs/`、`romfs.bin` | 游戏数据 | 从你自己的合法副本导出 |
| FFmpeg / opus / shaderc 静态库 | 第三方构建产物，体积大 | 按官方源码与 `runtime/compile_*.sh` 自行构建 |
| HAP 成品 | 内含游戏代码与签名 | 按 README 自行构建；不公开分发 |

这些是“版权内容/构建产物”，不是本项目代码；其余代码均为 MIT（见 `LICENSE`）。

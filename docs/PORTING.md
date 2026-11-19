# 移植原理与独立实现指南（PORTING）

这份文档的目的：**讲清楚原理，让缺少的部分可以被任何人用自己的代码独立复刻**，
而不是依赖某个成品、某个工具或“盲猜”。

> 边界：本项目只讨论代码架构与公开 API 的用法；不提供、也不教授游戏提取/解密/绕过保护的方法。
> 游戏数据与重编译后的游戏代码必须由使用者从自己合法拥有的游戏中产生。

---

## 1. 移植的核心思路：分层替换，接口不变

移植不是“把 Android 代码搬过来”，而是把每个平台相关层替换掉，保持**接口和语义**不变：

| 层 | 原平台（Android / PC） | 鸿蒙（OHOS）实现 |
|---|---|---|
| 窗口/Surface | `ANativeWindow` + `vkCreateAndroidSurfaceKHR` | `OHNativeWindow`（XComponent）+ `vkCreateSurfaceOHOS` |
| 应用生命周期 | Activity + JNI | `UIAbility` + `WindowStage` + ArkTS 页面 |
| 输入 | Android 输入 / SDL | XComponent 触摸 + `hle/input.c` 映射 Npad |
| 音频输出 | AAudio / OpenSL / SDL | SDL3 shim → OHAudio（`libohaudio`） |
| 视频解码 | MediaCodec / FFmpeg | FFmpeg 软解（可选 `OH_AVCodec` 硬解封装） |
| 图形 API | NVN → Vulkan（桌面/Android） | 同一套 NVN→Vulkan 后端，Vulkan loader 换成 OHOS 的 |
| 文件系统 | 直接文件 / romfs 解包 | `romfs.bin` + `exefs/` + HLE 文件服务 |
| 界面 | Android UI / ImGui | ArkTS 启动器 + 少量 ImGui（可选） |

只要每一层的**输入/输出契约**不变，游戏代码和运行时其余部分就不用改。

---

## 2. 关键接口（可以据此自己实现）

### 2.1 启动器 → 运行时（C++ 桥）

`game_entry.cpp` 的核心约定：
- 通过 `dlopen("liblasrecomp.so", RTLD_NOW | RTLD_GLOBAL)` 加载运行时；
- 调用 `SDL_main(argc, argv)`；`argv[1]` 是数据根（含 `exefs/`、`romfs.bin`）；
- 通过环境变量传配置：`LARECOMP_DATA_ROOT`、`LARECOMP_SETTINGS`、`LARECOMP_MOVIE_MAX`、
  `LARECOMP_AUDOUT_MS`、`LARECOMP_VK_PROF` 等（见 `game_entry.cpp::apply_display_ini_env`）；
- XComponent 的 `OHNativeWindow` 交给平台层（Vulkan Surface / EGL）。

只要你能提供同样的数据根、同样的环境变量和 Surface，运行时就能跑起来。

### 2.2 运行时 → 游戏

- 运行时加载 `liblasgame.so`（游戏重编译产物），从 `exefs/` 读取模块元数据；
- 游戏通过 HLE 符号表和 NVN API 调用宿主；宿主只需要实现这些接口。

### 2.3 数据格式

- `exefs/`：Switch 可执行模块（`main`、`rtld`、`sdk`、`subsdk*`、`main.npdm`）；
- `romfs.bin`：RomFS 镜像；运行时解析目录/文件元数据，按字节区间读取；
- 存档：`save/` 目录（HLE 文件服务）。
- 导入 zip 的约定：`extracted/exefs/*` + `extracted/romfs.bin`，native 原子 rename 到数据根。

---

## 3. 缺失部分如何“靠自己实现”

### 3.1 游戏重编译代码（`generated-*` / `liblasgame.so`）

这部分不是本项目能提供的（版权），但**原理是公开的**：
- 输入：你自己的游戏可执行模块（`exefs/main` 等）；
- 输出：可在 ARM64 上运行的重编译代码及导出符号；
- 关键点：静态重编译需要“函数地址表 + 调用约定 + 系统接口调用点”；
  运行时通过 HLE 接管这些调用，所以重编译代码只依赖导出符号与 HLE 约定。
- 你需要的工具链/脚本属于另一类工具，本项目**不提供**；请使用你自己合法获得/编写的工具。

> 换句话说：仓库给了你“运行时 + 接口”，重编译器和游戏本体需要你自己准备；
> 这就像模拟器项目——模拟器开源，BIOS/游戏自备。

### 3.2 运行时依赖的第三方静态库

`runtime/BUILD.md` 与 `compile_*.sh` 给出了交叉编译方式；你需要自己构建：
- FFmpeg：`avformat`/`avcodec`/`swscale`/`swresample`/`avutil`（按 OHOS 目标配置）；
- libopus（音频）；
- shaderc（运行时把游戏的 GLSL 编成 SPIR-V）；
- Vulkan 头文件与 OHOS NDK 库（`native_window`、`native_media_*`、`ohaudio`、`hilog` 等）。

### 3.3 自己写一个替代实现（不抄袭的做法）

你可以：
1. 只参考本仓库的**接口与数据流**（本文档），不复制代码；
2. 按同样的契约实现自己的启动器 / HLE / 后端；
3. 上游 LASRecomp 的 HLE 设计是 MIT，可以合法参考与复用（保留版权声明）。

这就属于“独立实现/合规复用”，而不是抄袭受版权保护的资源。

---

## 4. 调试与验证方法（便于别人接手）

- **日志**：hilog tag `LasRT` / `LasBridge`；`LARECOMP_VK_PROF=1` 打 `[hitch]` 帧时间；
- **崩溃**：`hidumper -e --list <bundle>` / `hidumper -e --print <record_id>` 看 CppCrash 栈；
- **动画**：`[movie] open/decoder ... configured` 确认走的解码器与文件；
- **渲染**：`[vk]`、`[gcopy]`、`[bc]`、`[tex-audit]` 等日志可定位上传/格式问题；
- **数据**：`[fs] romfs index: N files`、`[fs] read ...` 验证镜像被正确索引。

---

## 5. 合规清单（开源前自查）

- [x] 不含游戏 ROM/NSP/XCI/romfs/exefs/模型/贴图/音频
- [x] 不含重编译后的游戏代码（`generated-*`、`liblasgame.so`）
- [x] 不含预编译的第三方 `.a/.so`（由使用者自行构建）
- [x] 不含签名证书/私钥
- [x] 保留上游 LICENSE 与致谢
- [x] README 明确“非官方、仅供学习、请支持正版”
- [x] 不提供提取/解密/导入工具

如果以后新增文件，请对照 `README` 的“不包含内容”与 `.gitignore` 再提交。

## 6. 文字可见性：字体回退问题（上游遗留，已在本项目修复）

- **现象**：切到中文/日文/韩文等语言后，游戏内文字不可见或空白。
- **原因**：运行时/游戏使用的字体回退只覆盖拉丁与英文；上游默认葡萄牙语/拉丁语环境不会触发。
- **修复思路**：
  1. 把运行时字体指向一个**包含目标语言字形的完整字体**：
     `LARECOMP_FONT=/system/fonts/HarmonyOS_Sans_SC.ttf`（HarmonyOS 自带、含中文）；
  2. 把游戏语言设为目标语言（`game_lang=zh-CN` 等）；
  3. 字体与语言同时正确，游戏文本（开场、对话、菜单）即可正常显示。
- **可替换性**：`LARECOMP_FONT` 可以指向任意 TTF/OTF；只要字形覆盖目标语言即可。这条也是“原理可复刻”的例子：
  不需要改游戏代码，只改字体资源指向就能恢复文字。

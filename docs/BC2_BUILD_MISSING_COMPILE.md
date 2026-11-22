# BC2 图标花屏 · 构建漏编译事故 —— 完整复现与踩坑记录

> **文档目的**：让下一次任何人（包括 AI）遇到「主菜单图标彩色横条纹花屏 / 某类纹理独独错乱」时，能照着本文一步步复现并定位，**不要再重走我们走过的 7 个版本的弯路**。
>
> **事故性质**：不是渲染管线 bug，不是纹理解码算法写错，而是 **构建脚本的编译清单漏了一个 `.c` 文件** —— 源码改了，二进制没改。
>
> **时间**：2026-11-21 ~ 2026-11-22　**设备**：Mate 80 Pro（Kirin9030Pro / Maleoon GPU）HarmonyOS 7.0　**工程**：`Switch Pokémon Let's Go` 静态重编译移植到 HarmonyOS
>
> **最终产物**：`build\texture_fixed_v8.so`（25,825,368 字节）
> **最终结论**：v8 装机后菜单 5 个图标全部正确，帧率仍 30fps / 43ms，其它画面无回归。

---

## 目录

- [0. 一句话摘要（TL;DR）](#0-一句话摘要tldr)
- [1. 问题现象（一眼认出）](#1-问题现象一眼认出)
- [2. 排查全过程时间线（命令级）](#2-排查全过程时间线命令级)
- [3. 根因（一句话 + 完整机制）](#3-根因一句话--完整机制)
- [4. 证据链（每条都能独立复核）](#4-证据链每条都能独立复核)
- [5. 修复步骤（照着敲就能复现）](#5-修复步骤照着敲就能复现)
- [6. ★★ 极度容易踩的坑（本文核心）](#6--极度容易踩的坑本文核心)
- [7. 验证清单](#7-验证清单)
- [8. 教训总结](#8-教训总结)
- [9. 附录](#9-附录)

---

## 0. 一句话摘要（TL;DR）

**现象**：主菜单 5 个图标（图鉴 / 包包 / 同行宝可梦 / 记录 / 设置）变成**彩色横条纹花屏**，但图标的位置、大小、圆角形状、透明外形全部正确；3D 场景、门板、中文字、底部按钮底图都正常。

**根因**：这 5 个图标是 **NVN 格式 `0x43` = BC2(DXT3) 128×128** 纹理。`bc.c` 里的 BC2 解码器确实修好了（把「颜色块 / alpha 块」的读取顺序改正），**源码修改时间 2026-11-22 10:45**；但 **构建脚本 `build_texture_fixed_v7.py` 的 `JOBS` 编译清单里没有 `bc.c`**，链接时直接用了 `build/obja64` 里 **2026-11-20 10:53:27** 的旧目标文件 —— 旧文件里 BC2 还是「颜色和 alpha 交换」的错误实现。**所以 v7 里 BC2 修复根本没进 `.so`**，用户看到「花屏完全不变」。

**修复**：`build_texture_fixed_v8.py` 只改一条 —— `JOBS`（以及隐式声明预检 `STRICT`）里加入 `bc.c`，重新编译并链接。**没有改任何渲染/采样/着色逻辑。**

**验证**：v8 装机后 5 个图标显示为正确的红卡 / 橙包 / 绿球 / 灰箭头 / 紫卡；`[tex-audit-v3]` 日志确认 5~9 张 `0x43 128x128 bc_kind 2` 正常创建；30fps / 43ms 不变。

---

## 1. 问题现象（一眼认出）

### 1.1 用户视角

> 「主菜单 5 个图标是彩色横条纹花屏。」「不是纹理只是颜色没渲染变色或者渐变的颜色处理」「就像门牌的玻璃」「底层是黑白」

- 错误截图：`build\v7_actual.jpg`
- Android 版正确参照（**注意：这是另一个平台的正确结果，不是我们的**）：`build\v7_shots\v7_menu.jpg`

### 1.2 关键特征（用来和「别的花屏」区分）

| 特征 | 本事故 | 其它花屏（如 v4 回归） |
|---|---|---|
| 图标位置 / 间距 / 排布 | ✅ 正确 | 经常一起错 |
| 图标大小 / 圆角形状 / 透明外形 | ✅ 正确 | 常错 |
| 图标**像素内容** | ❌ 彩色横条纹（相邻行颜色剧变） | 撕裂、错位、大块错乱 |
| 条纹边缘 | 有 1~2px 的「彩虹状」杂色边（红黄绿蓝青） | 无此规律 |
| 受影响范围 | **只有 UI 美术资源**（菜单图标、背包道具图标、地名横幅装饰、路牌图集） | 全画面 |
| 3D 场景 / 门板 / 中文字 / 按钮底图 | ✅ 正确 | 常一起坏 |
| 帧率 | 30fps / 41~43ms 正常 | 有时掉帧 |

### 1.3 为什么「形状对、只有像素错」是极其重要的线索

形状/透明外形正确 ⇒ **几何、UV、alpha 遮罩、混合、布局全部没问题**，问题只在「采到的颜色」。
⇒ 排查范围立刻收缩到：**纹理内容（RAM 里的像素）** 或 **上传路径** 或 **采样/着色** 三选一。
⇒ 而「横条纹」这种逐行错乱的形态，最典型的原因就是 **每行从错误的偏移读取**（行距/块布局/字节序错）。

---

## 2. 排查全过程时间线（命令级）

### 2.0 版本流水（先看全貌，避免重复劳动）

| 版本 | 谁 | 主要改动 | 结果 | 是否解决了图标 |
|---|---|---|---|---|
| v1~v3 | 早期 | 各种格式映射、block_height 探索 | 画面能用 | ❌ |
| **v4** | `texture-v4-fixer` | 把 `tegra_default_block_height_log2` 换成「1.5× 台阶对齐」的 official 规则 | **纯回归**：门板 / UI 图标 / 横幅装饰全部花屏 | ❌（更糟） |
| **v5** | `texture-v4-fixer` | ① block_height 回退到「最小够用」；② `objects.c` packaged 布局按字节解析；③ 加 `[tex-audit-v3]` 审计日志 | 门板修好、30fps；图标仍花屏 | ❌ |
| **v6 / v6b / v6c** | `texture-v4-fixer` | 启用 BC→ASTC 4x4 转码（`astc_rt.c` 完整实现）；v6c 增加 dump 基建（产出 `bc_all` 8605 个 dump） | 图标仍花屏；**这一轮 dump 是用旧 `bc.o` 产生的** | ❌ |
| **v7** | 前一个子 AI | **改了 `bc.c` 的 BC2 块顺序**（10:45），然后 10:45:49 跑 v7 构建 | 用户实测「花屏完全不变」 | ❌（**改动没进二进制**） |
| **v8** | `gpu-render-fixer`（本次） | 定位到「bc.c 没编译」；构建脚本 JOBS 加 `bc.c`；重新编译 + 打包 + 装机 | 图标全部正确 | ✅ |

> **教训**：v4~v7 花了 6 个版本在「解码算法 / 块布局 / GOB 规则 / ASTC」上打转，而真正的问题在**构建清单**。任何「改了却没效果」的改动，第一反应应该是**先验证改动有没有进二进制**。

### 2.1 阶段 A：接手时的已知信息与错误前提

接手时（v7 之后）手上的「已知」：

1. 前一个子 AI 声称「**2868 张 `bc_*.rgba` dump 里 0 张有横条纹**（判据：行内色差 <60 且相邻行均值差 >15）」→ 推出「花屏是在显示阶段产生的」。
2. 前一个子 AI 声称「**BC2 解码有 bug（颜色和 alpha 交换），已在 v7 修复**」。
3. lead 转来的「最强嫌疑：`vk_transient` chunk 复用竞态」。
4. 用户断言「**不是纹理，只是颜色没渲染变色/渐变**」「就像门牌的玻璃」。

**这 4 条里有 3 条后来被证明是错的或无效的**（见 §6-C 判断类坑）。所以第一件事不是继续按这些结论往下推，而是**重新建立「图标到底是哪张纹理」的事实**。

### 2.2 阶段 B：先纠正「dump 覆盖范围」这个致命前提

读代码发现两条硬事实：

**(1) `bc_all` 那 8605 个 dump 只覆盖 BC 纹理。** `textures.c` 的 dump 条件是 `bc_kind != 0`（BC1/2/3/4/5/7）。任何非 BC 格式（`0x25` RGBA8、`0x38` RGBA8-sRGB、`0x3B` RGB5A1、`0x3F` B10G11R11 …）**根本不会被 dump**。

> ⇒ 「2868 张没有横条纹」这条证据**不能证明图标没问题**，它只证明了「被 dump 到的那些 BC 纹理没问题」。**这是整条排查链里最容易被忽略的采样偏差。**

**(2) `textures.c:956` 还躺着一行前一 AI 留下的注释：**

```c
// src/runtime/nvn/vulkan/textures.c
} else {   // R 0-4, G 5-9, B 10-14, A 15 (the Pokémon icons in Let's Go)
```

说明前一 AI 曾怀疑图标是 **`0x3B`（16 位 RGB5A1）**，走「解 swizzle → 5-5-5-1 手动展开成 RGBA8」的非 BC 路径。这条假设**必须用真机日志验证**，不能靠推理。

### 2.3 阶段 C：真机抓日志（含 `hilog -x` 陷阱）

#### 环境准备（设备无线调试）

```powershell
$hdc = "<DevEco Studio>\sdk\default\openharmony\toolchains\hdc.exe"
$dev = "<设备IP:端口>"
& $hdc list targets
& $hdc tconn $dev          # 掉线时：tconn -remove → 等 1s → tconn → 等 3~4s
```

`aa start` 报错 → **屏幕锁了，必须用户物理解锁**：

```
error: failed to start ability.
Error Code:10106102  Error Message:The device screen is locked during the application launch, unlock screen failed.
Error cause: The current mode is developer mode, and the screen cannot be unlocked automatically
```

#### ❌ 第一次抓日志失败（重要陷阱）

按「旧流程」用 `hilog -x`：

```powershell
& $hdc -t $dev shell "nohup sh -c 'timeout 200 hilog -x > /data/local/tmp/x.log 2>&1' &"
```

结果：**只抓到 389 行、0 条 `tex-audit`**。原因查 `hilog -h`：

```
Querying logs options:
  No option performs a blocking read and keeps printing.     ← 无参数才是「流式」
  -x --exit
    Performs a non-blocking read and exits when all logs in buffer are printed.   ← -x 是 dump 完就退出
```

⇒ `-x` 在应用启动**之前**执行，等于「把当时的缓冲倒出来就退出」，后面的日志全丢。

#### ✅ 正确抓法（无参数 = 阻塞流式）

```powershell
& $hdc -t $dev shell "pkill hilog 2>/dev/null; hilog -r"                       # 清缓冲
& $hdc -t $dev shell "rm -f /data/local/tmp/v8b.log; nohup sh -c 'timeout 400 hilog > /data/local/tmp/v8b.log 2>&1' >/dev/null 2>&1 &"
Start-Sleep -Seconds 1
& $hdc -t $dev shell "aa force-stop com.las.launcher"
& $hdc -t $dev shell "aa start -a EntryAbility -b com.las.launcher"
Start-Sleep -Seconds 55        # 等游戏恢复到菜单
& $hdc -t $dev shell "wc -l /data/local/tmp/v8b.log; grep -c 'tex-audit' /data/local/tmp/v8b.log"
```

输出：`202798 /data/local/tmp/v8b.log`　`367`（367 条纹理创建审计，够用）

> 说明：v7 二进制里 `game_entry.cpp:962-963` 已经**内置**了 `setenv("LARECOMP_TEX_AUDIT","1",1)` 和 `setenv("LARECOMP_FMT_LOG_ALL","1",1)`，所以不需要额外设环境变量，**只要抓取窗口覆盖「启动 → 开菜单」全过程**即可。

#### 顺带：无线调试与截图

```powershell
& $hdc -t $dev shell "snapshot_display -f /data/local/tmp/s4.jpeg"
& $hdc -t $dev file recv /data/local/tmp/s4.jpeg "build\png\s4.jpeg"
```

返回 `success: snapshot display 0 ... width: 2832, height: 1280`（应用在前台时是横屏 2832×1280；在桌面时是竖屏 1280×2832 —— 可用来判断应用是否真的在前台）。

### 2.4 阶段 D：日志直接锁死「图标是哪张纹理」

把 `tex-audit` 行拉下来：

```powershell
& $hdc -t $dev shell "grep 'tex-audit' /data/local/tmp/v8b.log > /data/local/tmp/audit9.txt"
& $hdc -t $dev file recv /data/local/tmp/audit9.txt "build\audit9.txt"
```

`[tex-audit-v3]` 行格式（`textures.c:375` 附近）：

```
11-22 11:30:08.948 65383 245 I .../LasRT: [tex-audit-v3] nvn fmt 0x43 128x128 lv 1 target 1 flags 0x4 \
  -> vk fmt 37 128x128 bc_kind 2 srgb 0 astc_bw 0 bpb 16 bw 4 bh 4 bh_log2 2 wa 0 addr 72d636c200 stride 0
```

关键字段：`nvn fmt`（游戏请求的 NVN 格式）、`尺寸`、`lv`（mip 层数）、`vk fmt`（我们实际用的 Vulkan 格式）、`bc_kind`（BC 家族编号）、`srgb`、`bpb/bw/bh`、`bh_log2`、`wa`、`addr`（guest 地址）、`stride`。

**统计 350 条记录的格式分布：**

| nvn fmt | 次数 | 含义 |
|---|---|---|
| 0x46 | 70 | BC1 sRGB |
| 0x25 | 51 | RGBA8 UNORM |
| 0x3f | 50 | B10G11R11（HDR RT） |
| 0x4e | 32 | BC7 sRGB |
| 0x49 | 32 | BC4（单通道遮罩） |
| 0x48 | 17 | BC3 sRGB |
| 0x29 | 16 | RGBA16F |
| 0x35 | 12 | D32S8（深度） |
| 0x42 | 12 | BC1 UNORM |
| 0x38 | 9 | RGBA8 sRGB（渲染目标） |
| 0x4d | 6 | BC7 UNORM |
| 0x4b | 6 | BC5 |
| **0x43** | **5** | **BC2 UNORM ← 菜单图标** |
| 0x79 | 2 | ASTC |

**`0x3B` 一条都没有** ⇒ 前一 AI 的「RGB5A1 展开路径」假设**直接排除**（`textures.c:956` 那段代码与本 bug 无关，别去动它）。

**在「菜单打开那一刻」的连续创建里，出现了 5 张 `0x43`：**

```
11-22 11:30:08.948  nvn fmt 0x43 128x128 -> vk 37 bc_kind 2 srgb 0 addr=72d636c200
11-22 11:30:08.950  nvn fmt 0x43 128x128 -> vk 37 bc_kind 2 srgb 0 addr=72d6378200
11-22 11:30:08.951  nvn fmt 0x43 128x128 -> vk 37 bc_kind 2 srgb 0 addr=72d6380200
11-22 11:30:08.953  nvn fmt 0x43 128x128 -> vk 37 bc_kind 2 srgb 0 addr=72d639c200
11-22 11:30:08.986  nvn fmt 0x43 128x128 -> vk 37 bc_kind 2 srgb 0 addr=72d6394200
```

- **43 毫秒内连续 5 张、128×128、`lv 1`（单层）、`bc_kind 2`（BC2）、`target 1`（含 alpha）** —— 与「5 个圆角图标」完全吻合。
- 整份 350 条审计里 `0x43` **只出现这 5 次** ⇒ 本作里 **BC2 专供 UI 美术资源**。
- 这也直接否掉了 lead 转来的「chunk 复用竞态（拷贝量大才坏）」：图标每张解码后仅 64 KB，是最小的一批。

> ⚠ **地址每次运行都会变**（堆分配顺序不同）。例如 v7 运行是 `72d636c200...`，v8 运行是 `72d636b200...`。**不要把地址写死成判据**，判据应该是「格式 `0x43` + 128×128 + 菜单打开时刻创建的 5 张」。

### 2.5 阶段 E：dump 对照 —— 同批 BC4 全对，BC2 全花

在 `bc_all` 里按这 5 个地址找 dump（dump 命名规则 `bc_<addr>_L<lv>_<w>x<h>_k<kind>.rgba`）：

```
bc_72d636c200_L0_128x128_k2.rgba
bc_72d6378200_L0_128x128_k2.rgba
bc_72d6380200_L0_128x128_k2.rgba
bc_72d639c200_L0_128x128_k2.rgba
bc_72d6394200_L0_128x128_k2.rgba
```

**观察结果（关键对比）：**

| 同一时刻创建的纹理 | 格式 | dump 观察 |
|---|---|---|
| 5 张菜单图标 | `0x43` BC2 128×128 | ❌ 绿底 + 横条纹 + 边缘彩虹杂色（**与屏幕花屏形态一致**） |
| 圆角方块遮罩 | `0x4b/0x49` BC4 128×128 | ✅ 白色圆角方块（正确） |
| 伊布剪影 | `0x49` BC4 256×256 | ✅ 白色伊布轮廓（正确） |
| 精灵球图形 | `0x49` BC4 100×100 | ✅ 白色精灵球（正确） |

⇒ **反 swizzle、block_height、上传布局对同批次纹理全部正确**，只有 BC2 这一条路径的输出是垃圾。
⇒ 前一个子 AI「BC2 已修复」的说法与实际 dump 矛盾 ⇒ 必须查「是不是修复没进二进制」。

### 2.6 阶段 F：Python 复算 —— 证明是「字节序反了」（这一步一锤定音）

dump 目录里同时有 3 种产物，含义必须分清：

| 文件 | 内容 |
|---|---|
| `bcraw_<addr>_L0_<w>x<h>_k<kind>.bin` | guest 内存里的**原始字节**（仍是 block-linear 排布） |
| `bcblk_<addr>_L0_<w>x<h>_k<kind>.wb<..>_hb<..>_bh<..>.bin` | **反 swizzle 之后**的紧凑块表（16 字节/块） |
| `bc_<addr>_L0_<w>x<h>_k<kind>.rgba` | 解码器输出的 RGBA8（**这是「旧 `bc.o`」的输出**） |

用 `bcblk` 数据，按**两种字节序**各解一遍，与 dump 对比（脚本见 §9.3）：

```
correct 与 dump 的平均像素差 = 160.27
swapped 与 dump 的平均像素差 = 0.00      ← dump 就是「交换序」解出来的
```

再按**正确顺序**（DXT3 规范）解码 5 张，得到：

| 地址 | 平均 RGB | 对应图标 | 与 Android 参照 |
|---|---|---|---|
| `72d636c200` | (255,156,140) 橙红 | **图鉴**（红卡） | ✅ |
| `72d6378200` | (255,217,144) 橙黄 | **包包**（橙包） | ✅ |
| `72d6380200` | (160,227,124) 绿 | **同行宝可梦**（绿球） | ✅ |
| `72d6394200` | (205,205,205) 灰 | **记录**（灰箭头） | ✅ |
| `72d639c200` | (190,153,253) 紫 | **设置**（紫卡） | ✅ |

⇒ **纹理内容本身是可以完全正确解出来的**，算法没问题，问题只在「跑的是哪一版算法」。

### 2.7 阶段 G：时间戳自证 —— 找到「旧目标文件」

```powershell
$R = "<源码树>/letsgo-recomp-android-master"
Get-Item "$R\src\runtime\nvn\bc.c"
Get-Item "$R\build\obja64\src_runtime_nvn_bc.c.c.o"
```

| 文件 | 时间 |
|---|---|
| `src\runtime\nvn\bc.c`（含正确 BC2 实现） | **2026-11-22 10:45** |
| `build\obja64\src_runtime_nvn_bc.c.c.o`（实际被链接的） | **2026-11-20 10:53:27** |

再看 v7 构建脚本的编译清单：

```python
# build\build_texture_fixed_v7.py
JOBS = [
    (R + r"\src\runtime\nvn\astc_rt.c",         "src_runtime_nvn_astc_rt.c.c.o",         ["-std=c11"], INC_NOFF),
    (R + r"\src\runtime\nvn\tegra.c",           "src_runtime_nvn_tegra.c.c.o",           ["-std=c11"], INC_NOFF),
    (R + r"\src\runtime\nvn\objects.c",         "src_runtime_nvn_objects.c.c.o",         ["-std=c11"], INC_NOFF),
    (R + r"\src\runtime\nvn\formats.c",         "src_runtime_nvn_formats.c.c.o",         ["-std=c11"], INC_NOFF),
    (R + r"\src\runtime\nvn\vulkan\exec.c",     "src_runtime_nvn_vulkan_exec.c.c.o",     ["-std=c11"], INC_NOFF),
    (R + r"\src\runtime\nvn\vulkan\textures.c", "src_runtime_nvn_vulkan_textures.c.c.o", ["-std=c11"], INC_NOFF),
    (R + r"\src\runtime\services\hwopus.c",     "src_runtime_services_hwopus.c.c.o",     ["-std=c11"], INC_NOFF),
    (R + r"\src\runtime\platform\ohos.c",       "src_runtime_platform_ohos.c.c.o",       ["-std=c11"], INC_NOFF),
    (R + r"\src\runtime\services\audio.c",      "src_runtime_services_audio.c.c.o",      ["-std=c11"], INC_NOFF),
    (R + r"\src\runtime\hle\movie.c",           "src_runtime_hle_movie.c.c.o",           ["-std=c11"], INC),
]
# ★ 没有 bc.c ★
```

构建脚本的流程是：`BASE（build/obja64，70 个预编译 .o）→ 整个复制到 WORK → 只重编 JOBS 里的文件覆盖对应 .o → 链接 WORK 里所有 .o`。
**`bc.c` 不在 JOBS ⇒ 复制过来的旧 `bc.o` 原样被链接。**

### 2.8 阶段 H：二进制级确认（不能只靠「图标好了」推断）

把新旧 `bc.o` 一起反汇编，比对 `bc_decode_block`（BC2 分支就在这个函数里）：

```powershell
$od = "<DevEco Studio>\sdk\default\openharmony\native\llvm\bin\llvm-objdump.exe"
& $od -d --disassemble-symbols=bc_decode_block <旧 .o> > old.txt
& $od -d --disassemble-symbols=bc_decode_block <新 .o> > new.txt
```

结果：两者函数大小都是 `0x120c`、指令数都是 **1155 条**，**只有 26 条不同**，且全部落在 BC2 分支：

| | 颜色块指针 | 颜色/索引读取 | alpha 读取 |
|---|---|---|---|
| **旧 `.o`（被 v7 链接的）** | `mov x0, x20`（= block+0） | `ldrb w8..w0, [x20,#4..#7]` | `ldrb w8..w0, [x20,#8..#15]` ❌ |
| **v8 `.o`（正确）** | `add x0, x20, #8`（= block+8） | `ldrb w8..w0, [x20,#12..#15]` | `ldrb w8..w0, [x20,#0..#7]` ✅ |

⇒ **v8 的二进制里，BC2 分支确实换成了正确的 DXT3 顺序**。
（顺便证明：`bc_decode_block` 是**唯一**相关变化，前 25 条指令相同是正常的 —— 函数入口 + switch 分派没变。）

### 2.9 阶段 I：v8 构建 / 打包 / 装机 / 验证

见 §5、§7。关键时间点：

| 时间 | 动作 |
|---|---|
| 11:41:38 | 生成 `build_texture_fixed_v8.py` |
| 11:43:06 | 新 `bc.o` 编译完成（哈希与旧的不同：`02EB496E…` → `E9BD2B7A…`） |
| 11:43:14 | `build\texture_fixed_v8.so` 链接完成（25,825,368 字节，sha256 `ED1FE3F3…`） |
| 11:45:19 | HAP 打包完成（`entry-default-signed.hap`，209.00 MB） |
| 11:46 | `hdc install` 成功 |
| 11:48 | 启动 + 55s 后截图 → **图标正确** |

---

## 3. 根因（一句话 + 完整机制）

### 3.1 一句话

**`bc.c` 的源码改了，但构建脚本的编译清单（`JOBS`）没包含它，链接时用了两天前的旧目标文件 —— 「BC2 块顺序修复」从未进入 `.so`。**

### 3.2 完整机制图

```
源码 bc.c（2026-11-22 10:45，BC2 顺序已修正）
        │
        │  ❌ 不在 build_*_v7.py 的 JOBS 里
        ▼
build/obja64/src_runtime_nvn_bc.c.c.o（2026-11-20 10:53，BC2 顺序 = 反的）
        │
        ▼
复制到 WORK → 不重编 → 直接链接
        ▼
texture_fixed_v7.so  ← 装的、跑的、用户看到的都是这个（BC2 依旧错）
```

### 3.3 为什么「改了源码」和「改了二进制」是两件事

- 这套移植工程的构建**不是「全量重编」**：`BASE = build/obja64` 里放着 **70 个预编译目标文件**（包括大量 Android 期遗留/第三方对象），每次出新版本只重编**少数几个**热点文件（为了把编译时间压到 2~3 分钟、也为了不改动已验证的文件）。
- 因此「源码目录里的文件是最新的」 ≠ 「`.so` 里的代码是最新的」。**唯一的真相来源是 `JOBS` 列表 + 每个 `.o` 的时间戳/哈希。**
- v7 的脚本作者显然在 `bc.c` 里写了非常详细的注释（说明他意识到了 BC2 的问题），甚至写好了修复 —— 但**忘了把这个文件加进编译清单**，于是「修了个寂寞」。

### 3.4 为什么「只有菜单图标」出问题

| 格式 | 本作中的用途 | 解码路径 | 状态 |
|---|---|---|---|
| `0x46/0x48` BC1/BC3 | 场景、门板、大量贴图 | `bc_decode_block kind=1/3` | ✅ 正常（旧 `.o` 里也是对的） |
| `0x49/0x4b` BC4/BC5 | 遮罩、法线、剪影 | `kind=4/5` | ✅ 正常 |
| `0x4d/0x4e` BC7 | 高精度美术 | `bcdec_bc7` | ✅ 正常 |
| **`0x43` BC2** | **仅 UI 美术资源（5 个菜单图标等）** | **`kind=2`** | ❌ **旧 `.o` 按「颜色/alpha 交换」解 → 花屏** |

⇒ 因为 BC2 在本作里**专供 UI**，所以花屏精确地只出现在 UI 图标上，反而**掩盖**了「这是解码器问题」的直觉（大家会以为是 UI 渲染/采样/混合的问题）。

---

## 4. 证据链（每条都能独立复核）

| # | 证据 | 复核命令 / 方法 | 关键输出 |
|---|---|---|---|
| 1 | 菜单打开瞬间创建 5 张 `0x43` 128×128 | `grep 'tex-audit' /data/local/tmp/v8b.log`，筛 `nvn fmt 0x43` | 5 行，`bc_kind 2 srgb 0 addr …` |
| 2 | 全量格式分布里 `0x3B` 出现 0 次 | 同上，统计 `nvn fmt` 分组 | 见 §2.4 表格 |
| 3 | 5 张 BC2 的 dump 是花屏，同批 BC4 正确 | 拼图查看 `bc_all\bc_*_L0_128x128_k2.rgba` | 绿底横条纹 vs 白色圆角方块 |
| 4 | 「交换序」解码与 dump 逐像素差 = 0.00 | §9.3 脚本 | `swapped … 0.00` / `correct … 160.27` |
| 5 | 「正确序」解码 = 5 个正确图标 | 同上 + 与 Android 参照对比 | 平均色红/橙/绿/紫/灰 |
| 6 | 源码 vs `.o` 时间戳 | `Get-Item bc.c / bc.o` | 11-22 10:45 vs 11-20 10:53 |
| 7 | v7 脚本 JOBS 无 `bc.c` | 读 `build_texture_fixed_v7.py` | 见 §2.7 |
| 8 | 新旧 `bc.o` 哈希不同、反汇编 BC2 分支偏移不同 | `Get-FileHash` + `llvm-objdump -d --disassemble-symbols=bc_decode_block` | 26 条差异全在 BC2 |
| 9 | 装机后图标正确、帧率不变 | `snapshot_display` 截图 + 日志 | 见 `v8_menu_fixed.jpg` |
| 10 | HAP 内 .so 的哈希 ≠ 源 .so（被 strip） | 解压 HAP 取 `libs/arm64-v8a/liblasrecomp.so` | 21,323,024 vs 25,825,368 字节 |

---

## 5. 修复步骤（照着敲就能复现）

### 5.0 前置：确认 `bc.c` 里的 BC2 实现是正确的

```c
// src/runtime/nvn/bc.c  第 71~85 行（正确版本，DXT3 规范）
if (kind == 2) {
    // BC2/DXT3 块布局 = 「8 字节显式 4-bit alpha + 8 字节 BC1 风格颜色」
    //   block[0..7]  = 16 个 4-bit alpha（每字节低半字节在前），a = nib * 17
    //   block[8..11] = c0 / c1（RGB565）
    //   block[12..15] = 16 个 2-bit 索引
    // ★ 曾经的 bug：把颜色读成 block[0..7]、alpha 读成 block[8..15] —— 两者交换，
    //   结果 RGB 由 alpha 数据解出（大量纯黑）、alpha 由颜色数据解出，整张图花屏。
    uint8_t pal[4][4];
    bc1_block_colors(block + 8, pal);          // 颜色在 block[8..11]
    bc1_indices(out, block + 12, pal);         // 索引在 block[12..15]
    uint64_t abits = 0;
    for (int i = 0; i < 8; i++) abits |= (uint64_t)block[i] << (8 * i);   // alpha 在 block[0..7]
    for (int i = 0; i < 16; i++) out[i * 4 + 3] = (uint8_t)(((abits >> (4 * i)) & 0xF) * 17);
    return;
}
```

> 如果源码已经是这样，**本步骤不需要改任何代码** —— 事故里真正缺的是「编译」而不是「修代码」。

### 5.1 构建脚本加 `bc.c` 到 JOBS（**这是唯一的关键修复**）

```python
# build_texture_fixed_v8.py（由 v7 脚本改）
JOBS = [
    # ★★ v8 核心：BC2(DXT3) 解码器必须重新编译！
    #    bc.c 里 BC2 的分块顺序（block[0..7]=4bit 显式 alpha，block[8..15]=颜色+索引）曾写反，
    #    2026-11-22 已修正；但 v7 的构建脚本漏了这一项，链接到的是 build/obja64 里 2026-11-20 的旧 .o。
    #    ⚠ 以后只要改了 bc.c / bc.h，必须确保它在这个 JOBS 列表里，否则改动不会进 .so。
    (R + r"\src\runtime\nvn\bc.c", "src_runtime_nvn_bc.c.c.o", ["-std=c11"], INC_NOFF),
    ...
]
STRICT = {"bc.c", "astc_rt.c", "tegra.c", ...}   # 隐式声明预检也要加上
WORK = r"build\obja64-texfix8"
OUT  = r"build\texture_fixed_v8.so"
rsp  = r"build\texture_fixed_v8-link.rsp"
```

### 5.2 编译

```powershell
$py = "<用户目录>\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
& $py "build\build_texture_fixed_v8.py" 2>&1 | Tee-Object -FilePath "build\build_v8.log"
```

期望输出：

```
工作副本: 70 个 .o
--- 隐式声明预检 ---
  ok bc.c
  ...
OK bc.c (10488 bytes)
...
链接...
OK build\texture_fixed_v8.so 25825368 bytes
```

> 编译器必须用 **ohsdk 的 clang**：`<工作目录>\hdc\ohsdk\native\llvm\bin\clang.exe`
> `clang --version` → `OHOS (dev) clang version 15.0.4 (llvm-project feef13a36e78b7a2ff3e9e3f180a958f2782be1e)`
> **不要用 DevEco 自带的**（`<DevEco Studio>\sdk\default\openharmony\native\llvm\bin\clang.exe`，commit `00dcd9cc…`）。
> 两者 ABI 兼容，但混用会让「哪些对象是哪个编译器产的」变得不可追（本工程已出现过这种混编：`obja64-work` 里是 `00dcd9cc`，其余是 `feef13a3`）。

### 5.3 验符号 / 验内容

```powershell
$nm = "<DevEco Studio>\sdk\default\openharmony\native\llvm\bin\llvm-nm.exe"
& $nm --defined-only "build\texture_fixed_v8.so" | Select-String 'ff_h264_oh_decoder|menu_input_captured|g_opus_scratch|bc_decode_ldr'
```

期望（v8 实测）：

```
00000000007f4374 T bc_decode_ldr
000000000134b6d8 D ff_h264_oh_decoder
00000000021a604c b g_opus_scratch
000000000083bf8c T menu_input_captured
```

更硬的验证 —— **确认 BC2 分支真的变了**：

```powershell
$od = "<DevEco Studio>\sdk\default\openharmony\native\llvm\bin\llvm-objdump.exe"
& $od -d --disassemble-symbols=bc_decode_block "build\obja64-texfix8\src_runtime_nvn_bc.c.c.o"
```

应看到 `add x0, x20, #8`（颜色指针 = block+8）与 `ldrb … [x20,#0..#7]`（alpha = block[0..7]）。
**旧对象**是 `mov x0, x20`（= block+0）+ `[x20,#8..#15]` 取 alpha。

### 5.4 替换 / 打包 / 装机

```powershell
# ① 替换两处（intermediates 里的是 hvigor 打包实际取用的，cpp/third_party 里的是版本留档）
Copy-Item "build\texture_fixed_v8.so" "F:\lsg-harmony\entry\build\default\intermediates\libs\default\arm64-v8a\liblasrecomp.so" -Force
Copy-Item "build\texture_fixed_v8.so" "F:\lsg-harmony\entry\src\main\cpp\third_party\liblasrecomp.so" -Force

# ② 必须删掉旧的 strip 产物，否则会拿旧库打包
Remove-Item "F:\lsg-harmony\entry\build\default\intermediates\stripped_native_libs" -Recurse -Force -ErrorAction SilentlyContinue

# ③ 打包（必须用 ASCII 路径的工程目录 F:\lsg-harmony，不能用 <项目工作区>）
Set-Location "F:\lsg-harmony"
$env:DEVECO_SDK_HOME = "<DevEco Studio>\sdk"
& "<DevEco Studio>\tools\hvigor\bin\hvigorw.bat" --mode module -p product=default -p buildMode=debug assembleHap --no-daemon

# ④ 装机 + 启动
$hdc = "<DevEco Studio>\sdk\default\openharmony\toolchains\hdc.exe"
$dev = "<设备IP:端口>"
& $hdc -t $dev shell "aa force-stop com.las.launcher"
& $hdc -t $dev install "F:\lsg-harmony\entry\build\default\outputs\default\entry-default-signed.hap"
& $hdc -t $dev shell "power-shell wakeup"
& $hdc -t $dev shell "aa start -a EntryAbility -b com.las.launcher"
```

### 5.5 验证

见 §7。

---

## 6. ★★ 极度容易踩的坑（本文核心）

### A. 构建类坑（最隐蔽，本次事故本体）

#### A-1【本次事故】改了 `.c`，但构建脚本 `JOBS` 没包含它 → 链接旧 `.o`，改动不生效

- **现象**：改了源码、重新编译、重新打包、装机 —— 行为**完全没有变化**。日志/截图看起来「修复无效」。
- **原因**：这套工程是「增量式」构建：70 个预编译 `.o` 在 `build/obja64`，每次只重编 `JOBS` 里列出的少数文件。
- **检测方法（必做）**：

  ```powershell
  # 逐个对比「源码 mtime」vs「对应 .o mtime」
  # 源码比 .o 新 且 不在 JOBS 里 → 就是漏编译
  ```

  本工程全量审计结果（2026-11-22，v8 时点，70 个 `.o`）：

  | 状态 | 数量 | 文件 |
  |---|---|---|
  | 最新（源码不比 .o 新） | 47 | — |
  | 陈旧但**在 JOBS**（会重编，安全） | 9 | `movie.c` `objects.c` `tegra.c` `exec.c` `textures.c` `ohos.c` `audio.c` `hwopus.c` `bc.c`(v8 已加) |
  | 无源码对应（Android/第三方固定对象） | 13 | `ffmpeg_ohcodec.c.o` `ffmpeg_ohdec_vp9.c.o` `imgui*.o` `vma.o` `smb2_stub.o` `ui_stub.o` `src_runtime_ui_menu.mm.o` `platform_ohos_sdl_shim.cpp.cpp.o` |
  | **陈旧且不在 JOBS（★ 静默陷阱）** | **1** | **`device.c`**（见 A-2） |

- **正确做法**：每次改 `.c` 后，**先确认它在不在构建脚本的 `JOBS` 里**；不在就加进去（并同时加进 `STRICT` 预检集合）。

#### A-2 同类隐患：`device.c` 也是「改了但没重编」（本次已查清，尚未修）

- 源码 `src/runtime/nvn/vulkan/device.c` = **2026-11-21 13:10**，`build/obja64/…device.c.c.o` = **2026-11-21 02:29**，且不在 v8 的 JOBS 里。
- 用「字符串差分」定位出那次改动新增了什么（新 `.o` 有、旧 `.o` 无，且能在当前源码里找到）：

  ```
  + LARECOMP_NO_DYN_RENDER
  + LARECOMP_NO_SYNC2
  + [vk] dynamic rendering forced OFF (LARECOMP_NO_DYN_RENDER), using render-pass fallback
  + [vk] sync2 forced off (LARECOMP_NO_SYNC2): using 1.0 barriers/submit
  ```

- **结论**：这两个**诊断开关在当前所有已装 `.so`（v5~v8）里是无效的**（设了也不会有任何效果）。
- **建议**：下一次出 v9 时把 `device.c` 加进 `JOBS` 一起重编（它在 11-21 13:22 的 `obja64-work` 里**编译成功过**，风险低）；或者明确标注「这两个开关不可用」。
- **通用检测法（很实用）**：如果怀疑某个 `.o` 陈旧，可以**对比「源码里的字符串字面量」是否出现在 `.o` 里** —— 新加/删除的日志文本、环境变量名会立刻暴露差异（注意要排除 `#ifdef` 编译掉的 Android 分支字符串，否则会有大量假阳性，`device.c` 初查时 93 条里绝大多数就是这种）。

#### A-3 `hvigor` 打包时必须先删 `intermediates\stripped_native_libs`

- 不删的话，`DoNativeStrip` 任务可能直接用旧的 strip 产物，导致**换进去的新 `.so` 没进 HAP**。
- 正确顺序：替换 `.so` → **删 `stripped_native_libs`** → `assembleHap`。

#### A-4 工程路径必须用 ASCII（`F:\lsg-harmony`）

- 用含中文的 `<项目工作区>` 会报 `00306003 Invalid project path`。
- 所以出现一个容易困惑的布局：**源码仓库在 `<项目工作区>`，实际参与打包的 HAP 工程是 `F:\lsg-harmony`**，`.so` 要**手工复制**到 `F:\lsg-harmony` 的两个位置。

#### A-5 `DEVECO_SDK_HOME` 必须设

```powershell
$env:DEVECO_SDK_HOME = "<DevEco Studio>\sdk"
```

否则 hvigor 报 `00303217`。

#### A-6 编译器版本必须统一（ohsdk clang `feef13a3`）

- 构建脚本用的是 `<工作目录>\hdc\ohsdk\native\llvm\bin\clang.exe`（`feef13a3`）。
- DevEco 自带的是 `00dcd9cc`。混用不报错、ABI 兼容，但会让「某个 `.o` 是谁编的」不可考（历史里已经混过一次：`obja64-work` 用 DevEco 编译器重编过 `device.c`）。

#### A-7 HAP 内的 `.so` 会被 strip，**不要用哈希验证「打包是否用了新库」**

- 源 `.so` = 25,825,368 字节；HAP 内 = 21,323,024 字节（`DoNativeStrip` 处理过）。哈希必然不同。
- 正确的验证方式是 ①看构建日志里 `DoNativeStrip` 是否真的跑了、②**装机后看行为/日志**（最可靠）、③或对比反汇编里某个函数是否与预期一致。

### B. 诊断类坑

#### B-1【亲自踩】`hilog -x` 不是「流式抓取」，是「dump 完就退出」

- 用 `timeout 200 hilog -x > x.log` 抓启动日志 → 只得到启动前的 389 行、0 条 `tex-audit`。
- `hilog -h` 原文：
  ```
  No option performs a blocking read and keeps printing.
  -x --exit   Performs a non-blocking read and exits when all logs in buffer are printed.
  ```
- 正确：**无参数** `hilog`（阻塞流式）。清理缓冲用 `hilog -r`。
- 抓窗必须覆盖「应用启动 → 进游戏 → 打开菜单」，并在启动**之前**开始抓。

#### B-2 `LARECOMP_DATA_ROOT` 绝对不能覆盖成 dump 目录

- 它是**游戏数据根**（romfs 数据目录）。历史事故：有人为了指定 dump 目录覆盖了它 → 魔改后 `present.vert` 编译失败 → `in_ch=6` → FATAL 崩溃。
- dump 产物就落在 `$LARECOMP_DATA_ROOT` 的顶层（`bc_*.rgba` / `bcraw_*.bin` / `bcblk_*.bin`），不需要另指目录。

#### B-3 dump 覆盖范围有「格式盲区」

- `bc_all` 只 dump `bc_kind != 0` 的纹理。**非 BC 格式（`0x25`/`0x38`/`0x3B`/`0x3F`/`0x29` 等）根本不在 dump 里。**
- ⇒ 绝对不能因为「dump 里有 2868 张正常」就断言「所有纹理都正常」。**先确认你关心的那张纹理属于被 dump 的格式。**

#### B-4 `LARECOMP_BC_DUMP_*` 系列曾有几个已知坑（v6c 已修，但要知道）

| 坑 | 说明 |
|---|---|
| `LARECOMP_BC_DUMP_BLOCKS=0` 曾被解析成 32（现在 0 = 不限） | 想「不限」却被限 |
| 只抓 `lv == 0` | 需要 `LARECOMP_BC_DUMP_MIPS=1` 才能抓 mip |
| 用 `nblocks` 而非「布局大小」分配 | 非 4 倍数尺寸会缺 padding：如 `146×146` 布局 40960 vs 块表 21904 |

#### B-5 `tex-audit` 只在「纹理创建时」打印

- 所以抓窗必须覆盖「创建那一刻」。菜单图标的纹理是在**打开菜单时**（本例 `11:30:08.948~986`）才创建的，不是开机时创建的。
- 另外 `LARECOMP_TEX_AUDIT` 默认打 **400 条**上限（`na++ < 400`）——本次整轮 350~367 条，刚好够；如果哪天超了，会静默截断后面的（重点看最后一个审计时间戳是否早于你要的那一刻）。

#### B-6 地址不是稳定标识

- 每次运行、每次改动堆布局，纹理的 guest 地址都会变（v7 是 `72d636c200`，v8 是 `72d636b200`）。
- **判据要用「格式 + 尺寸 + 创建时刻 + 数量」**，不要写死地址写进脚本。

### C. 判断类坑（AI 最容易犯，本次一半时间花在这）

#### C-1 把「Android 版截图」误当成「我们自己的运行结果」

- `build\v7_shots\v7_menu.jpg` 是 **Android 版正确参照**，不是我们的 v7 结果。我们自己的结果是 `v7_actual.jpg`（花屏）。
- 曾有 AI 看着 Android 截图以为「已经修好了」。

#### C-2 把「源码已修」误当成「二进制已修」★ 本次事故根源

- 一定要做**二进制级**验证：`.o` 的 mtime/hash、`.so` 的反汇编、装机后的行为。三者缺一不可。
- 「改了源码 + 重新构建成功」并不等于「改动进了产物」。

#### C-3 统计判据选错导致漏抓样本 ★

- 前一 AI 的判据：「行内色差 < 60 且相邻行均值差 > 15」→ 判 2868 张无横条纹。
- 但那 5 张 BC2 dump 是**绿底 + 条纹**（行内有彩色变化、行间均值差不大），恰好**躲过了这个判据**。
- **教训**：判据只能用来辅助排序，不能用来「证明不存在」；**发现异常要人眼看图**（本次就是靠肉眼看拼图发现那 5 张）。

#### C-4 把「纹理内容正确」直接推成「显示正确」

- 中间还有：反 swizzle → 解码 → 上传（`vkCmdCopyBufferToImage` / transient 布局）→ 采样器 → 着色器 → 混合。
- 反之也一样：**显示正确也不能推出「每一步都正确」。**（本次就是「解码器错」这一环，其它环都对。）

#### C-5 把「打开过程的过渡帧」当成最终界面

- 历史事故：背包页截图全白，其实那是过渡动画中的一帧，不是 bug。
- 同理：`aa start` 后马上截图可能是黑屏（游戏还在加载/电影阶段）。**要等到目标界面稳定再截**（本次等 45~55s 让游戏恢复到菜单）。

#### C-6 同一个变量被多个模块复用，误判「它只管一件事」

- `LARECOMP_DATA_ROOT` 被 6 个模块用（`textures.c` / `pl.c` / `device.c` / `exec.c`(2处) / `shaders.c`(2处) / `sdl_shim.cpp`）。
- 改一个「看似无关」的变量，可能在别处引发崩溃。

#### C-7 沿用了「上一轮的输出」当「这一轮的事实」（本次我差点踩）

- `bc_all` 那 8605 个 dump 是 **v6c 的构建**（`build_texture_dump_v6c.py`，03:06）产出的，用的是**旧 `bc.o`**。
- 所以那批 dump 里的 BC2 花屏，正好就是「旧解码器」的证据 —— **但如果换成了 v8 后再去 dump，同样的地址会变成正常图像。**
- **规则：dump 产物必须记录「它由哪个 `.so` / 哪个 `.o` 产生」**，否则跨版本比对会得出反向结论。

### D. 环境类坑

#### D-1 无线调试 `<设备IP:端口>` 会掉

```powershell
& $hdc tconn -remove
Start-Sleep -Seconds 1
& $hdc tconn <设备IP:端口>
Start-Sleep -Seconds 4
& $hdc list targets
```

#### D-2 `aa start` 报 `10106102 The device screen is locked` → 必须用户物理解锁

- 开发者模式下不能自动解锁。`power-shell wakeup` 只能唤醒，不能解锁。
- 装 209MB HAP 要 1~2 分钟，期间屏幕可能自动锁 —— **装机前提醒用户「保持亮屏」，或装完再让他解锁**。

#### D-3 应用沙箱路径

- 应用数据根：`/data/storage/el2/base/files/lasrecomp`（**hdc 访问会 Permission denied**）
- 可用的：`/data/app/el2/100/base/com.las.launcher/files`（dump 实际落点，hdc 可读）
- 临时目录：`/data/local/tmp`（日志、截图放这里最方便）

#### D-4 截图工具

```powershell
& $hdc -t $dev shell "snapshot_display -f /data/local/tmp/x.jpeg"
```

- 应用在前台（横屏）→ 2832×1280；在桌面（竖屏）→ 1280×2832。**看尺寸就能判断应用在不在前台。**

#### D-5 自动化点按可用（不必让用户动手）

```powershell
& $hdc -t $dev shell "uitest uiInput click <x> <y>"
& $hdc -t $dev shell "which uitest; which snapshot_display"    # 两个都在 /system/bin
```

本次因为游戏会自动恢复到「菜单」状态，没有用到点按；但需要时可用 `uitest uiInput click / swipe / keyEvent`。

---

## 7. 验证清单

### 7.1 必查项

1. **`[tex-audit-v3]` 里出现 5~9 条 `nvn fmt 0x43 128x128 … bc_kind 2`**（数量可变，因为菜单可能被重复初始化）。
   ```powershell
   & $hdc -t $dev shell "grep 'tex-audit' /data/local/tmp/v8b.log | grep -c 'nvn fmt 0x43'"
   & $hdc -t $dev shell "grep 'tex-audit' /data/local/tmp/v8b.log | grep 'nvn fmt 0x43' | head -6"
   ```
2. **截图确认 5 个图标显示正确**（颜色：橙红 / 橙黄 / 绿 / 灰 / 紫；形状：圆角方块 + 白色线稿）。
3. **其它元素无回归**：3D 背景（房间、窗帘、桌子、伊布）、中文字、底部按钮、左下角提示、右上角帧率。
4. **帧率**：应仍是 30fps / 41~43ms。
5. **无新增崩溃**：`grep 'LasStderr'` 里不应出现新类型的致命错误（`'double' not supported` / `arrays of arrays` 是既有的 glslang 预编译噪音，可忽略）。

### 7.2 期望的日志片段（v8 实测）

```
11-22 11:48:43.148 ... [tex-audit-v3] nvn fmt 0x43 128x128 lv 1 target 1 flags 0x4 -> vk fmt 37 128x128 bc_kind 2 srgb 0 ... addr 72d636b200 stride 0
...（共 9 条）
```

### 7.3 期望的截图

- `build\v8_menu_fixed.jpg`（v8 正确）
- `build\png\v7_vs_v8_icons.png`（v7 花屏 vs v8 正确，直接对比）
- `build\png\icons_fixed.png` / `v8_icons_decoded.png`（用 Python 正确解码 5 张 BC2 得到的图标，可与截图互相印证）

---

## 8. 教训总结

1. **「改了源码」≠「改了二进制」。**
   每次改 `.c` 后，必须三查：① 它在不在构建脚本 `JOBS` 里；② 对应 `.o` 的 mtime/hash 变了没有；③ `.so` 的反汇编里那个函数变了没有。**三者缺一不可。**

2. **「改了没效果」的第一嫌疑永远是「改动没进产物」，而不是「这个 bug 比我想的复杂」。**
   v4~v7 六个版本都栽在这一点上：越改越复杂，却从没验证过「上一轮改动到底有没有生效」。

3. **构建脚本的 `JOBS` 列表就是这套工程的「真相来源」。**
   它不是注释、不是文档、不是「我记得我加过」——只有列在里面的文件才会被编译。

4. **诊断先确认「样本覆盖范围」，再做统计。**
   「2868 张 dump 都正常」这句话本身没错，错在它**不覆盖非 BC 格式**。**先问「我关心的对象在不在样本里」。**

5. **统计判据不能用来「证明不存在」，只能用来「辅助排序」。**
   自动判据漏掉了 5 张，人眼看拼图 10 秒就发现了。**异常现象一定要落到「看图/看真机」。**

6. **AI 的推断必须落到可复核的证据上。**
   「我认为是 X」「大概率是 Y」都不是结论。能拿出来的应该是：命令 + 输出 + 可重复的脚本。
   本次的定案证据是「Python 复算逐像素差 = 0.00」和「反汇编里 26 条指令的偏移变化」，不是推理。

7. **用户的现象描述要反复确认，但也要允许「用户描述不精确、结论正确」。**
   用户说「不是纹理，只是颜色没渲染变色/渐变」——**方向上他指对了「不是贴图内容本身坏了」**（真正的坏点在「跑的哪一版解码器」），虽然他给的机制（颜色空间/渐变处理）不是根因。**不要因为机制不对就整条否定用户的判断。**

8. **跨平台的「正确参照」要标清楚。**
   `v7_shots/*` 是 Android 的，`v7_actual.jpg` 是我们的。混了就会得出「已经修好了」的灾难性结论。

9. **版本化 dump 产物，记录产生它的构建版本。**
   否则拿 v6c 的 dump 去评价 v8 的行为，会得出完全相反的结论。

10. **给未来的自己留注释。**
    本次在 `bc.c`（BC2 块顺序）与 `build_texture_fixed_v8.py`（为什么必须编 `bc.c`）都留了中文注释说明「为什么必须保留 / 为什么必须编译」，并用「★ / ⚠」标出。**下一个 AI 若把 `bc.c` 从 JOBS 里删掉或把 BC2 顺序「改回去」，会立刻看到警告。**

---

## 9. 附录

### 9.1 v8 构建脚本的完整 JOBS 清单（11 个文件）

| 文件 | 说明 |
|---|---|
| **`src/runtime/nvn/bc.c`** | **★ v8 新增：BC2(DXT3) 解码器** |
| `src/runtime/nvn/astc_rt.c` | BC→ASTC 转码（本机被 `LARECOMP_NO_RT_ASTC=1` 关闭） |
| `src/runtime/nvn/tegra.c` | block-linear / GOB / block_height 规则（**v4 回归的现场，别乱改**） |
| `src/runtime/nvn/objects.c` | packaged 布局按字节解析 |
| `src/runtime/nvn/formats.c` | NVN 格式表 |
| `src/runtime/nvn/vulkan/exec.c` | 命令提交、transient 分配、draw |
| `src/runtime/nvn/vulkan/textures.c` | 纹理创建 / 上传 / 审计 / dump |
| `src/runtime/services/hwopus.c` | 音频 Opus 硬件解码 |
| `src/runtime/platform/ohos.c` | OHOS 平台层 |
| `src/runtime/services/audio.c` | 音频输出 |
| `src/runtime/hle/movie.c` | 视频（**注意：其中 `|| 1` 相关代码是用户明令不许动的**） |

**不可重编的 13 个固定对象**（没有对应源码或属于 Android/第三方）：`ffmpeg_ohcodec.c.o`、`ffmpeg_ohdec_vp9.c.o`、`imgui*.o`(5 个)、`vma.o`、`smb2_stub.o`、`ui_stub.o`、`src_runtime_ui_menu.mm.o`、`platform_ohos_sdl_shim.cpp.cpp.o`。

**已知的唯一「陈旧且不可重编」**：`src_runtime_nvn_vulkan_device.c.c.o`（对应 `device.c`），缺 `LARECOMP_NO_DYN_RENDER` / `LARECOMP_NO_SYNC2` 两个开关。

### 9.2 一次性诊断命令速查

```powershell
# —— 环境 ——
$hdc = "<DevEco Studio>\sdk\default\openharmony\toolchains\hdc.exe"
$dev = "<设备IP:端口>"
$py  = "<用户目录>\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
$od  = "<DevEco Studio>\sdk\default\openharmony\native\llvm\bin\llvm-objdump.exe"
$nm  = "<DevEco Studio>\sdk\default\openharmony\native\llvm\bin\llvm-nm.exe"
$R   = "<源码树>/letsgo-recomp-android-master"

# —— 1. 抓纹理审计日志（流式！） ——
& $hdc -t $dev shell "pkill hilog 2>/dev/null; hilog -r"
& $hdc -t $dev shell "rm -f /data/local/tmp/a.log; nohup sh -c 'timeout 400 hilog > /data/local/tmp/a.log 2>&1' >/dev/null 2>&1 &"
& $hdc -t $dev shell "aa force-stop com.las.launcher; aa start -a EntryAbility -b com.las.launcher"
Start-Sleep -Seconds 55
& $hdc -t $dev shell "grep 'tex-audit' /data/local/tmp/a.log > /data/local/tmp/audit.txt"
& $hdc -t $dev file recv /data/local/tmp/audit.txt "build\audit.txt"

# —— 2. 截图 ——
& $hdc -t $dev shell "snapshot_display -f /data/local/tmp/s.jpeg"
& $hdc -t $dev file recv /data/local/tmp/s.jpeg "build\png\s.jpeg"

# —— 3. 检查某格式是否出现（例：0x43 BC2） ——
#     在本地 audit.txt 里统计 nvn fmt 分布：按第一列分组计数

# —— 4. 检查陈旧目标文件（源码比 .o 新） ——
#     见 9.4 脚本

# —— 5. 反汇编某函数并对比新旧 .o ——
& $od -d --disassemble-symbols=bc_decode_block "<某 .o>" 
```

### 9.3 Python 复算脚本（BC2 两种字节序对照）

> 目的：**证明某张 BC2 的 dump 是「交换序」解出来的**，以及「正确序」解出来应该是什么样。

```python
import numpy as np, os
from PIL import Image

blocks = np.fromfile(<bcblk 文件>, np.uint8).reshape(bh, bw, 16)
dump   = np.fromfile(<bc_*.rgba 文件>, np.uint8).reshape(H, W, 4)

def rgb565(c):
    r=(c>>11)&0x1F; g=(c>>5)&0x3F; b=c&0x1F
    return [(r<<3)|(r>>2), (g<<2)|(g>>4), (b<<3)|(b>>2)]

def palette(c0,c1):
    p0=rgb565(c0); p1=rgb565(c1)
    if c0>c1:
        p2=[(2*p0[i]+p1[i])//3 for i in range(3)]; p3=[(p0[i]+2*p1[i])//3 for i in range(3)]; a2=a3=255
    else:
        p2=[(p0[i]+p1[i])//2 for i in range(3)]; a2=255; p3=[0,0,0]; a3=0
    return [p0+[255], p1+[255], p2+[a2], p3+[a3]]

def decode_block(blk, order):
    out = np.zeros((4,4,4), np.uint8)
    if order == "correct":     # DXT3 规范：block[0..7]=alpha, block[8..15]=颜色
        alpha_b, c_off = blk[0:8], 8
    else:                      # 旧 bug：反了
        alpha_b, c_off = blk[8:16], 0
    c0 = int(blk[c_off]) | (int(blk[c_off+1]) << 8)
    c1 = int(blk[c_off+2]) | (int(blk[c_off+3]) << 8)
    idx = blk[c_off+4:c_off+8]
    pal = palette(c0, c1)
    bits = int(idx[0]) | (int(idx[1])<<8) | (int(idx[2])<<16) | (int(idx[3])<<24)
    ab   = sum(int(alpha_b[i]) << (8*i) for i in range(8))
    for i in range(16):
        out[i//4, i%4][:3] = pal[(bits >> (2*i)) & 3][:3]
        out[i//4, i%4][3]   = ((ab >> (4*i)) & 0xF) * 17
    return out

# 对整图：按 (by,bx) 遍历 blocks 拼回 H×W，然后
#   np.abs(img_correct - dump).mean()  -> 160.27（说明 dump 不是正确序）
#   np.abs(img_swapped - dump).mean()  -> 0.00  （说明 dump 就是交换序）
```

**本次实测输出：**

```
correct 与 dump 的平均像素差 = 160.27
swapped 与 dump 的平均像素差 = 0.00
```

### 9.4 全量「陈旧目标文件」审计脚本

```python
import os, re, glob, datetime
from collections import Counter

R    = r"<源码树>/letsgo-recomp-android-master"
BASE = os.path.join(R, "build", "obja64")
JOBS = {"bc.c","astc_rt.c","tegra.c","objects.c","formats.c","exec.c","textures.c",
        "hwopus.c","ohos.c","audio.c","movie.c"}      # ← 与当前构建脚本保持一致

# 建立 src 下所有源文件 -> stem（路径中的 / 换成 _，去掉扩展名）
mapping = {}
srcroot = os.path.join(R, "src")
for root, dirs, files in os.walk(srcroot):
    for f in files:
        if not f.endswith((".c",".cpp",".mm",".cc")): continue
        p = os.path.join(root, f)
        rel = os.path.relpath(p, srcroot).replace("\\","/")
        for ext in (".mm",".cpp",".cc",".c"):
            if rel.endswith(ext):
                rel = rel[:-len(ext)]; break
        mapping.setdefault(rel.replace("/","_"), []).append(p)

def mt(p): return datetime.datetime.fromtimestamp(os.path.getmtime(p))

for o in sorted(glob.glob(os.path.join(BASE, "*.o"))):
    name = os.path.basename(o); body = name[4:]
    stem = None
    for suf in (".c.c.o",".cpp.c.o",".mm.c.o",".cc.c.o",".c.o",".o"):
        if body.endswith(suf): stem = body[:-len(suf)]; break
    srcs = mapping.get(stem, [])
    if not srcs: print(f"[no-source] {name}"); continue
    s = max(srcs, key=mt)
    if mt(s) > mt(o):
        st = "STALE-IN-JOBS" if os.path.basename(s) in JOBS else "STALE-NOT-IN-JOBS ★"
        print(f"[{st}] {name}\n    src={os.path.relpath(s,R)} srcTime={mt(s):%Y-%m-%d %H:%M} objTime={mt(o):%Y-%m-%d %H:%M}")
```

**本工程实测结果（2026-11-22，v8 时点）：**

```
BASE 目标文件: 70
陈旧总数: 10   （9 个在 JOBS 会被重编；1 个不在 = device.c ★）
Counter({'ok': 47, '?': 13, 'STALE-IN-JOBS': 9, 'STALE-NOT-IN-JOBS': 1})
```

### 9.5 关键文件与产物清单

| 类别 | 路径 |
|---|---|
| 源码仓库 | `<源码树>/letsgo-recomp-android-master` |
| 预编译目标文件库 | `…\repo\letsgo-recomp-android-master\build\obja64`（70 个 `.o`） |
| HAP 工程 | `F:\lsg-harmony`（必须 ASCII 路径） |
| 构建脚本 v7 / v8 | `build\build_texture_fixed_v7.py` / `build\build_texture_fixed_v8.py` |
| **产物 .so** | **`build\texture_fixed_v8.so`**（25,825,368 字节，sha256 `ED1FE3F3…`） |
| HAP | `F:\lsg-harmony\entry\build\default\outputs\default\entry-default-signed.hap` |
| 图标 dump | `build\bc_all\bc_*_L0_128x128_k2.rgba`（5 张） |
| 审计日志 | `build\audit9.txt`（v7 那轮，350 条）、设备上 `/data/local/tmp/v8b.log`（v8 轮） |
| 截图（错） | `build\v7_actual.jpg` |
| 截图（对） | `build\v8_menu_fixed.jpg` |
| 对比图 | `build\png\v7_vs_v8_icons.png` |
| 解码图标图 | `build\png\icons_fixed.png` |
| 前一 AI 的踩坑记录 | `<项目工作区>\踩坑全记录_可复现.md`（§19 是 block_height 那段，注意其结论部分已被证伪/推翻） |

### 9.6 下一步建议

1. **v9**：把 `device.c` 加进 `JOBS` 重编一次（补上 `LARECOMP_NO_DYN_RENDER` / `LARECOMP_NO_SYNC2` 两个诊断开关），并把这条写进构建脚本注释。
2. **把「陈旧检查」做成构建前置步骤**：构建脚本开头自动跑 §9.4 的审计，发现 `STALE-NOT-IN-JOBS` 就**报错退出**，从机制上根治此类事故。
3. **dump 产物加版本标记**：在 dump 文件名或同目录 `manifest.txt` 里写入「本轮由哪个 `.so`（sha256）产生」。
4. **`bc.c` 的 BC2 注释已足够详细**，勿改；若后续有人把「颜色/alpha 顺序」改回去，`v8` 会立刻退回花屏。
5. **`textures.c:956` 那段 `0x3B`(RGB5A1) 展开代码与本 bug 无关**，但它是真实存在的格式路径（未来若出现真正的 0x3B 纹理可能用到），**不要因为「图标与它无关」就删掉**。

---

*本文所有命令均在本次实际会话中执行过；所有截图/日志/脚本均有实体文件路径可复核。*
*文档版本：2026-11-22 12:00　作者：gpu-render-fixer（小月）*

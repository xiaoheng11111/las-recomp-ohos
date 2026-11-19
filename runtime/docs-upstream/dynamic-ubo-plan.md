# Dynamic UBO + Set 缓存改造施工图

状态：设计定稿，待实现。 Maleoon 935 真机已确认 `descriptorBuffer`/`pushDescriptor`/动态偏移全部可用，
本方案是 V1（不依赖扩展，核心 Vulkan 1.0 能力），V2（descriptor_buffer）留给 V1 验证后。

## 目标与证据

ISSUES.md §5：森林 720p 天花板 13-14fps，约 1.7 万植被 draw。每次 draw × 2 stage 走 `stage_set()` →
`vk_alloc_set()` → `vkAllocateDescriptorSets` + `vkUpdateDescriptorSets`，每帧约 34000 次分配，
估算 68ms/帧（≈14.7fps）。Maleoon 935 真机探针（2026-11-16）确认瓶颈不在 GPU：
`maxPerStageSampledImages=500000`，HARDWARE，驱动 B312。

## 现状机制（已核实）

- `vk_alloc_set` (exec.c:358)：每个 Sub 最多 16 个 pool，pool `maxSets=8192`，无任何复用。
- `stage_set` (exec.c:2711)：per draw × per stage 全量重写 descriptor。
- `copy_dedupe` (exec.c:557)：6 个全局 slot（RSCALE_V/F、TINT、DRV_V/F/C），同 seq 同内容 → 复用 span。
  已把「驱动常量、rscale、tint」收敛成稳定 span，但 set 里仍每次重写。
- `cbuf_info` (exec.c:2117)：
  - binding 0：DEDUPE_DRV_*（稳定）
  - binding 1：编译器常量 `guest_copy(code_addr+const_offset)` —— 内容恒定，但每次拷贝产生新 offset
  - binding 3+：游戏 uniform，`st.ubuf[stage][idx]` 每 draw 不同（`cbuf_dynamic` 位只 clamp 大小，exec.c:2152）
- GMAP（`gmap_copy()`）与 GFAST（`gfast_win`）per draw 变化。
- transient ring (exec.c:441 `transient_from`)：多 chunk，`VkSpan.buf` 可能跨 buffer（chunk 切换时）。
- Sub (exec.c:69)：slot 复用时 `vkResetDescriptorPool` 全部 pool（exec.c:115）→ 缓存必须挂在 Sub 粒度。
- draw 点 (exec.c:3469-3472)：`vset`/`fset` → `vkCmdBindDescriptorSets(..., 2, sets, 0, NULL)`。

## 方案

核心：**把 per-draw 变化的 binding 全部 dynamic 化，使 set 内容只由 (program, 纹理组) 决定**；
再按指纹缓存 set。34k alloc/帧 → 每 program·纹理组 2 次（顶点+片段各一），bind 时传 offset 数组。

### dynamic 化清单（仅 graphics pipeline；compute 的 DRV_C 保留 static+dedupe）

| binding | 类型 | 动作 | dynamic offset 来源 |
|---|---|---|---|
| 0 | UNIFORM_BUFFER | **不动**（dedupe 已稳定） | — |
| 1 | UNIFORM_BUFFER | → UNIFORM_BUFFER_DYNAMIC，range=`si->cbuf_size[1]` 固定 | `span.off` |
| 3..16（游戏 cbuf） | UNIFORM_BUFFER | → UNIFORM_BUFFER_DYNAMIC，range=`si->cbuf_size[k]` 固定 | `span.off` |
| 18/19 | UNIFORM_BUFFER | **不动**（dedupe） | — |
| GMAP_BUFFER_INDEX | UNIFORM_BUFFER | → UNIFORM_BUFFER_DYNAMIC，range=`sizeof gmap_tab` | `span.off` |
| GFAST_BUFFER_INDEX | STORAGE_BUFFER | → STORAGE_BUFFER_DYNAMIC，range 固定（gfast 窗口大小上限） | `span.off` |
| 纹理（≥32） | COMBINED_IMAGE_SAMPLER | **不动**（view memo 一帧内稳定） | — |

改造入口：`compile_program`/`build_compute` 里 `vk_reflect` 之后、`make_layout` 之前，遍历
`p->binds` 改 type（一次改动同时作用于 set layout 与 pipeline layout，二者必须一致）。
compute program 跳过（用 `stage==5`/compute 分支区分）。

### stage_set 改造（exec.c:2711）

1. 签名追加 out 参数：`uint32_t dyn_off[MAX_BIND], int *n_dyn`。
2. dynamic binding 的 write：`{buf, 0, range_fixed}`，offset 写入 `dyn_off[n_dyn++]`（**必须按 binding 升序**，
   现有循环按 binds 数组序，reflect 输出已是 binding 升序，需断言确认）。
3. set 指纹缓存（per-Sub）：
   - 键：hash(layout, 逐 binding 的 (type, buf, off, range | sampler, view, layout))。
     注意 static 部分含 dedupe span（每提交首 draw 后即稳定）、纹理 view（memo 稳定）。
   - 存储：挂在 `Sub`（`struct Sub` 加 `struct SetCache { uint64_t key; VkDescriptorSet ds; uint32_t off[MAX_BIND]; int n; } cache[512]; int ncache;`）。
     sub() 复用 slot 时（exec.c:115 附近）`s->ncache = 0`——pool 已 reset，缓存同步失效，正确性自然成立。
4. 命中：直接返回 ds 并拷出 offsets，跳过 alloc+write。

### cbuf_info 改造（exec.c:2117）

- dynamic 路径返回 `{span.buf, 0, range_fixed}`（off 由调用方从 write 里取不到——**改为返回 span 本身**，
  让 stage_set 拿 `span.off` 填 offsets、拿 `span.buf` 填 write。签名改成返回
  `struct { VkDescriptorBufferInfo d; VkSpan span; }` 或出参。）
- binding 1 的 `guest_copy` 无需 dedupe（dynamic 化后 offset 进数组）。
- `skinned_vs` 的 skin_identity 特例（exec.c:2160）保持原逻辑。

### draw 点改造（exec.c:3469-3472）

```c
uint32_t doff[2][MAX_BIND]; int ndyn[2];
vset = stage_set(..., doff[0], &ndyn[0]);
fset = stage_set(..., doff[1], &ndyn[1]);
...
uint32_t alldyn[2 * MAX_BIND];
memcpy(alldyn, doff[0], ndyn[0] * 4); memcpy(alldyn + ndyn[0], doff[1], ndyn[1] * 4);
vkCmdBindDescriptorSets(cb, GRAPHICS, pr->layout, 0, 2, sets, ndyn[0] + ndyn[1], alldyn);
```

Vulkan 规定 offset 数组按 set 序、set 内 binding 升序拼接——与 stage_set 写入顺序一致即可。

### 开关与验收

- 环境变量 `LARECOMP_VK_DYN_UBO`（默认开；=0 走旧路径）做 A/B。
- 验收：`LARECOMP_VK_PROF=1` 下 profiler 行 `sets` 段从 ~几十 ms 降到 <5ms；
  `stat` 里 `vk_alloc_set` 调用数（临时加计数）每帧 ≤ program×纹理组×2；
  森林场景帧率 ≥35fps（保守目标）。
- 回滚：开关关掉即回到现状，无持久化状态。

## 风险清单

1. dynamic offset+range 不得越界 chunk buffer：range 取 `min(cbuf_size[k], chunk 大小)`，cbuf_size 实测 <64KB，chunk 为 MB 级，安全；仍加防御 clamp。
2. `guest_copy` 常量在跨提交 seq 更新后 offset 变化——dynamic 化后 set 不含 offset，无影响。
3. chunk 切换（单帧超 CHUNK_SIZE）会改 `span.buf` → 指纹 miss → 重建 set（偶发，可接受）。
4. Maleoon 的 `maxPerStageDescriptorSets`/dynamic 限制：探针显示 124 扩展、Vulkan 1.3，无已知问题；真机验证时注意验证层输出。
5. STORAGE_BUFFER_DYNAMIC 在个别驱动上有性能陷阱（转 dynamic UBO 路径）——GFAST 也可以只 dynamic 化 GMAP、
   GFAST 保留 static（内容每 draw 变则 set 每 draw 重建，退化为现状）——先全 dynamic，profiler 对比后再定。

## 后续（V2，验证后）

Maleoon 支持 VK_EXT_descriptor_buffer + bufferDeviceAddress（真机探针已确认）：
set 布局预查 `vkGetDescriptorSetLayoutSizeEXT/BindingOffsetEXT`，描述符直接 memcpy 进 mapped
descriptor buffer，`vkCmdBindDescriptorBuffersEXT + SetDescriptorBufferOffsetsEXT`，alloc/ write 彻底归零。

---

# 鸿蒙第一版迁移状态（2026-11-16 18:40）

**目标**（用户拍板）：第一版鸿蒙原生（移植层跑起来）→ 测试 → 逐步替换鸿蒙硬件加速。

**语法验证基线**：54 个核心 C 文件，OHOS NDK (`hdc/ohsdk` clang) `-fsyntax-only` 通过 **41 个**；
13 个失败全部因缺 SDL3 主头（`SDL3/SDL.h`）——已开始写 SDL-OHOS shim（`platform/ohos/sdl3shim/`）。

**SDL 依赖面**：runtime 共引用约 58 个 SDL3 符号（计时/窗口/事件/键鼠/手柄/音频流/PNG/路径）。
计划：计时类用 clock_gettime 真实现；输入事件接 XComponent 回调；音频第一版 stub（后续 OHOS AudioRenderer）；
手柄接 GameControllerKit；`SDL_Vulkan_CreateSurface` 由 `vkCreateSurfaceOHOS` 实现（链路真机已验证）。

**待完成**：shim 枚举重定义与 SDL_Event union 成员（key/text/gbutton 需 which 成员）修完 → 54/54 →
写 `platform/ohos.c`（替代 android.c）与 main 入口（XComponent 驱动）→ OHOS 全量编译 → 接入 harmony-launcher。
**已确认无关项**：adrenotools 仅在 `-DLETSGO_ADRENOTOOLS` 时编入（可选宏，OHOS 版不定义即绕开）。

// ============================================================================
// 游戏内可控的运行时数据采集系统 —— 对外接口（头文件）
// ----------------------------------------------------------------------------
// 这是一个「开关式」采集器：
//   用户在游戏里点一下按钮 → 开始采集 → 再点一下 → 停止，数据落沙箱目录。
//
// ★ 为什么要有这个东西（以后不要当成冗余代码删掉）
//   本工程排查性能问题的唯一数据来源是 exec.c 的 renderer_stats()，它每 1 秒
//   往 hilog 打一行超密集文本。但那条路有三个致命限制：
//     1) 文本里 gpu/cpu 等字段是 %.0f 取整，拿不到亚毫秒差异；
//     2) 格式一改，所有解析脚本全崩；
//     3) 最要命：renderer_stats() 内部会把每个计数器清零（清零动作还藏在
//        snprintf 的参数里，见 exec.c:799-805），所以不能"稍后再解析"，
//        必须在它清零之前把原始变量抄出来。
//   本采集器就是在那个位置取数，直接拿原始变量，精度和稳定性都远好于解析日志。
//
// ★ 设计原则：只读、不干扰
//   - 本模块的函数只读运行时已有的计数器，不做任何"额外计时"，
//     所以采集本身几乎零开销（见 §回归测试：开/关采集的 fps 差应 < 1）。
//   - 所有采集器（Sink）回调里禁止 malloc 大块、禁止调用 renderer_stats()、
//     禁止任何会清零的写操作。违反这三条会污染被测量对象或破坏既有统计。
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 一次统计窗口（1 秒）的全部标量指标
// ---------------------------------------------------------------------------
// ★ 为什么用结构体而不是 48 个参数：
//   可读性；更重要的是「以后加字段不用改函数签名」—— 新增一个采集项时，
//   在结构体末尾加一行，老代码零改动（复合字面量里没写的字段自动为 0）。
//
// ★ 为什么 pass 分布数组不放在这里：
//   prof_end_line 是 4096 项的 uint32_t（16KB）。塞进复合字面量既写不出来，
//   也会白白吃掉 present 线程 16KB 栈。所以它走第二参数单独传。
//
// ★ 单位约定（务必遵守，别在 sink 里再换算）：
//   *_ms    = 毫秒（double）
//   *_mb    = 兆字节（double，已用 1048576 换算）
//   其余整数 = 次数/个数
typedef struct {
    // ---- 时间轴与帧 ----
    uint64_t frame;               // renderer_present_count()：本窗口结束时的累计 present 数
    double   t_ms;                // 相对「开始采集」那一刻的毫秒数（filled by collector）

    // ---- 帧率 ----
    double   fps;                 // 本窗口平均 fps（g_current_fps）
    double   max_frame_ms;        // ★ 本窗口内【最大单帧间隔】，不是"最后一帧耗时"
                                  //   来源 g_frame_ms，原变量注释写的是 "max frame time in ms"，
                                  //   实际语义是窗口内 max gap。改名的原因就是怕后来人误读。

    // ---- 绘制量 ----
    uint64_t draws, skipped, passes, dispatches, flushes;

    // ---- 等待 / shader 编译 ----
    uint64_t wait_ms;             // 等 GPU 的毫秒（stat_wait_ns / 1e6，整数化与原文本一致）
    uint64_t compile_n;           // 本窗口编译的 program 数
    double   translate_ms;        // GLSL -> SPIR-V 翻译
    double   spirv_ms;
    double   pipelines_ms;        // VkPipeline 创建（卡顿主要元凶之一）
    double   draws_waited_ms;     // draw 等 shader/pipeline 的时间

    // ---- 上传 / 顶点 / 回读 ----
    uint64_t fbcopy;              // 反馈回读次数
    double   upload_mb;           // 本窗口上传总量
    uint64_t vtx_copied;          // 顶点/索引数据被拷到 VRAM 的 draw 数
    uint64_t vtx_inplace;         // 就地零拷贝读的 draw 数

    // ---- 跳过原因分项（对应原 stat_skip_why[0..6]）----
    //   0 no program bound / 1 program compiling / 2 program failed /
    //   3 no program object / 4 primitive mode / 5 pipeline building / 6 pipeline failed
    uint64_t skip[7];

    // ---- CPU / GPU 分段 ----
    double   gpu_ms;              // GPU 时间（query pool 时间戳）
    double   cpu_exec_ms;         // 整条命令列表
    double   cpu_draw_ms;         // draw() 本体
    double   cpu_pre_ms;
    double   cpu_key_ms;
    double   cpu_dyn_ms;
    double   cpu_sets_ms;         // 描述符集（含 cbuf 拷贝）
    double   cpu_tex_ms;
    double   cpu_tex_upload_ms;
    double   cpu_vtx_ms;          // 索引扫描 + 顶点拷贝
    double   submit_ms;           // vkQueueSubmit2

    // ---- 纹理 ----
    uint64_t tex_checks;
    uint64_t tex_samples;
    uint64_t tex_full;
    double   tex_full_mb;
    uint64_t tex_dyn;

    uint64_t views_created;       // image view 缓存未命中数

    // ---- v9 细分（把 sets / vtx 两个大项再拆开）----
    double   sets_alloc_ms;       // vkAllocateDescriptorSets
    double   sets_upd_ms;         // vkUpdateDescriptorSets
    double   vtx_copy_ms;         // memcpy
    double   vtx_bind_ms;         // vkCmdBindVertexBuffers

    // ==========================================================================
    // v16 新增：cpu_vtx_ms 的 2.70ms「其余」拆解（3 项）
    // --------------------------------------------------------------------------
    // 【为什么要拆】
    //   v15 真机数据里 cpu_vtx_ms = 4.12ms/帧，但其中：
    //     vtx_copy_ms 1.37ms (33.2%)、vtx_bind_ms 0.05ms (1.2%)
    //     ★ 其余 2.70ms (65.7%) 来源不明
    //   perf_vtx 的计时窗口（exec.c:3791→3938）覆盖了索引处理、索引扫描、
    //   vk_transient 分配、8bit 索引展开、顶点拷贝等。必须拆开才知道优化方向。
    //
    // 【判据（拿到数据后一眼定性）】
    //   vtx_scan_ms 占 cpu_vtx 50%+  -> 写 NEON 优化 scan_max_index
    //   ★ 但先看这个再决定：源码注释实测「标量 1.4us/draw、AVX2 0.15us/draw」，
    //     327 draws/帧 × 1.4us ≈ 0.46ms/帧 就是扫描的【上限】。
    //     所以 NEON 最多省 ~0.41ms/帧，不会更多。别指望它能省 2ms。
    //   vtx_expand_ms 占比高       -> 优化 8bit→16bit 展开（可向量化）
    //   vtx_idx - scan - expand 大  -> 大头是 expand_indices() 或 vk_transient 分配，
    //                                 NEON 完全没用在扫描上，要往那两个方向查
    //
    // 【为什么 vtx_idx 的「剩余」最值得看】
    //   ★ 已核实：Maleoon 没有 VK_EXT_external_memory_host，
    //     所以 gmem_buf 永远为 NULL -> guest_direct() 永远 false ->
    //     所有顶点/索引都必须经过 vk_transient 分配 + memcpy。
    //   这是压在渲染线程上的硬成本，但 vk_transient 是【全文件共用】的累加器，
    //   拆不出「索引专用」的部分，所以用 vtx_idx 的剩余项来近似它。
    //
    // 【口径提醒】
    //   vtx_idx_ms 是「索引处理段」的总时长，
    //   vtx_scan_ms 和 vtx_expand_ms 都是它的【子集】（各自内部还有别的）。
    //   ★ 输出时不要把它们相加当总量；判断占比时要用 vtx_idx 做分母。
    // ==========================================================================
    double   vtx_idx_ms;          // 索引处理段总时长（expand_indices + scan + vk_transient + 8bit 展开）
    double   vtx_scan_ms;         // 其中：scan_max_index（索引扫描，NEON 优化的精确目标）
    double   vtx_expand_ms;       // 其中：8bit→16bit 展开循环

    // ==========================================================================
    // v13 新增：交换链 / 帧并行度（★ 用来定性「串行 vs 并行」）
    // --------------------------------------------------------------------------
    // 【为什么加这 8 个字段】
    //   上一轮数据的反证推翻了「渲染太多」的假设：
    //     低帧窗口 draws=7059 < 正常窗口 11230，gpu_ms=511 < 729  ——
    //     工作量更少，帧率却更低。说明瓶颈不是"量"，而是"帧没有并行起来"。
    //   要证明这一点，必须看：能不能同时在飞多帧、每次 acquire/present 阻塞多久。
    //
    // 【判据（一眼定性）】
    //   in_flight_max == 1                        -> 铁证：完全串行
    //   in_flight_max >= 2 但 in_flight_end 常为 0 -> 突发提交后立刻等完，仍是串行
    //   acq_block_ms 高                            -> 交换链太浅
    //   sw_images == 2                             -> 只够双缓冲，容易卡
    //   sw_present_mode == 0(FIFO) 且 fps 被压在 30 -> vsync 强制同步
    // ==========================================================================

    // 交换链有几张图（vkGetSwapchainImagesKHR 返回的 g_vk.sc_count）
    //   -1 = 未知（还没建 swapchain）
    int      sw_images;
    // 呈现模式，转成数字便于 CSV 统计：
    //   0 = FIFO（vsync 同步）, 1 = MAILBOX（不阻塞，默认）, 2 = IMMEDIATE（可能撕裂）
    //   3 = FIFO_RELAXED, -1 = 未知
    int      sw_present_mode;

    // 本窗口内累计阻塞时长（毫秒）。
    // ★ 注意是【累计】，不是单次平均 —— 和现有的 wait_ms 口径一致。
    //   要算单帧平均：acq_block_ms / frames_submitted。
    double   acq_block_ms;        // vkAcquireNextImageKHR 阻塞（拿不到空闲图时的等待）
    double   pres_block_ms;       // vkQueuePresentKHR 阻塞（呈现队列满/背压）
    double   fence_wait_ms;       // vkWaitSemaphores(timeline) 阻塞（等 GPU 做完）

    // 在飞帧数（已提交但 GPU 还没做完的帧）。
    //   in_flight_max = 本窗口内的峰值 —— 判断"架构允许多少并行，实际用上了几帧"
    //   in_flight_end = 本窗口结束瞬间的值 —— 判断"稳态是几张在飞"
    //   两个都要：只有 max==1 是铁证；max>1 但 end 常为 0 说明是突发后立刻等完。
    int      in_flight_max;
    int      in_flight_end;

    // 本窗口提交了几帧（vkQueueSubmit 的次数）。
    // 用来把"累计阻塞毫秒"换算成"单帧阻塞毫秒"，也能识别掉帧/空转。
    int      frames_submitted;

    // ==========================================================================
    // v13 B 组：把 cpu_draw_ms（每帧 20~23ms，占 CPU 80~88%）拆成 4 个分项
    // --------------------------------------------------------------------------
    // 【为什么要拆】
    //   数据已确认 cpu_draw_ms 是唯一瓶颈，但不知道 20ms 花在哪：
    //     - 是驱动调用本身慢（只能削 draw 数量）
    //     - 还是冗余绑定多（加状态缓存就能省，低风险）
    //     - 还是状态切换多（pipeline 排序）
    //   这 4 项就是判据。全部由 exec.c 的 draw_() 内部计时。
    //
    // 【重要：四项之和 ≠ cpu_draw_ms】
    //   draw_() 里还有索引扫描、顶点范围计算、vtx 拷贝等业务逻辑，
    //   那些由已有的 cpu_vtx_ms / vtx_copy_ms 单独统计。
    //   分析时看「四项之和 / cpu_draw_ms」的占比，差额就是业务逻辑开销。
    // ==========================================================================
    double   cmd_draw_ms;         // vkCmdDraw / vkCmdDrawIndexed 调用本身
    double   bind_ms;             // vkCmdBindIndexBuffer + vkCmdBindVertexBuffers
    double   desc_bind_ms;        // vkCmdBindDescriptorSets（图形管线，draw_() 内那次）
    double   state_ms;            // vkCmdBindPipeline + SetViewport/Scissor + PushConstants + 各种 Set*
    // draw_per_call_us 不在这里存 —— 它 = cmd_draw_ms / draws * 1000，由 CSV 输出时算，
    // 存一份反而容易和 draws 不同步（口径不一致就会算出假值）。
    //
    // ★ v13.1 新增自检字段（perf-optimizer2 要求）
    //   = cpu_draw_ms - (cmd_draw_ms + bind_ms + desc_bind_ms + state_ms)
    //
    //   【为什么需要这个字段】
    //     cpu_draw_ms 是整个 draw_() 函数（exec.c 的 PROF_T0/PROF_ADD(prof_draw)）。
    //     如果 B 组 4 项加起来远小于它，说明还有大块开销没被拆出来 ——
    //     那 perf-optimizer2 的分析就会失真，而且他自己无法判断"是拆得不够"还是"真的没别的大头"。
    //   有了这个字段，他一眼就能看出拆分完整度：
    //     < 20%  -> 拆得够细，可以直接定位
    //     20~50% -> 还有未归类开销，需要补字段
    //     > 50%  -> 拆分不全，分析会失真
    //
    //   ★ 它是算出来的，不是一个独立采集计数器 —— 不增加任何运行时开销。
    double   draw_ms_minus_parts_ms;

    // ==========================================================================
    // v13 C 组：GPU 总量与气泡
    // --------------------------------------------------------------------------
    // 【为什么只有 3 项，没有 pass 分类】
    //   目标本来想按 shadow/main/post/ui 分 pass 统计 GPU 时间，但实测
    //   vk_gpuprof_report() 的 mark 标签只有 "pass: N color WxH fmt X"
    //   （exec.c:1071），**游戏不下发 pass 名称**。
    //   硬猜会把"post"当成"main"，直接误导优化方向 —— 那比采不到更糟。
    //   所以 pass 分类全部填 -1（见下方 gpu_pass_named），只留能精确算的 3 项。
    //
    //   要真正拿到 pass 分类，只能做对照实验（关阴影/关后处理各跑一次），
    //   或者改 vk_gpuprof_report 让它记录 pass 的 imageView 目标 —— 那是动诊断框架，
    //   不属"最小动刀"，留到阶段 2。
    // ==========================================================================
    double   gpu_total_ms;        // GPU 总忙时（= 已有 prof_gpu / 1e6，与 gpu_ms 同口径）
    // GPU 气泡 = 理论上可用时间 - 实际忙时。
    //   算法：(帧时间 × 帧数) - gpu_total_ms
    //   判据：bubble 大 => GPU 有空转，瓶颈在别处（CPU 或同步）；
    //         bubble 接近 0 => GPU 真的被压满，必须降 GPU 负载。
    //   注意：帧时间用 max_frame_ms 会高估、用 1000/fps 更准，这里用后者。
    double   gpu_bubble_ms;
    // 本窗口 GPU 采样用掉的 timestamp 数（GP_MAX=4096）。
    //   用途：为将来开启逐 draw GPU 计时做容量评估。
    //   实测每帧最多 569 个 draw（14408 draws/s ÷ 25.3fps），4096 上限有 7.2 倍余量，
    //   所以将来真要开逐 draw timestamp 是可行的 —— 但需要把 LARECOMP_GPUPROF_AT
    //   从"只测一帧"改成"持续采样"，那是动诊断框架，留阶段 2。
    int      gpu_marks_used;
    // 6 个 pass 分类字段：恒为 -1（游戏不下发 pass 名称，不硬猜）。
    //   保留这些列是为了让分析脚本能明确区分"没采到" vs "采到 0"，
    //   而不是靠缺列去猜。
    int      gpu_pass_named;      // 恒 -1

    // ==========================================================================
    // v13 D 组：附加计数（8 项，全部易取、零风险）
    // ==========================================================================
    uint64_t tex_upload_count;    // 本窗口纹理上传次数
    double   tex_upload_bytes_mb; // 本窗口纹理上传总字节（MB）
    uint64_t tex_view_create_cnt; // 本窗口 vkCreateImageView 次数（= 已有 views_created）
    uint64_t tex_hash_scan_cnt;   // 本窗口全量 hash 扫描次数
    uint64_t pipe_create_cnt;     // 本窗口 vkCreateGraphicsPipelines + Compute 次数
    uint64_t pipe_bind_cnt;       // 本窗口 vkCmdBindPipeline 次数
    double   mem_used_mb;         // VMA 已分配显存（MB）。取不到填 -1。
    // 超预算时长 = 所有超过 33.3ms 的帧，其超出部分之和。
    //   比"掉帧计数"更有用：能直接算出"总共浪费了多少毫秒"。
    double   frame_budget_over_ms;

    // ==========================================================================
    // v14 新增：串行根因定位（9 项）
    // --------------------------------------------------------------------------
    // 【为什么加这 9 个】
    //   v13 数据发现 in_flight_max 稳恒为 1（62/63 窗口，end 100% 为 1），
    //   GPU/CPU 完全串行：每秒 cpu 492ms + gpu 596ms = 1088ms = 108.9%。
    //   理论上限 max(18.7, 22.7) = 22.7ms/帧 => 43.9fps（现在 26.2）。
    //
    //   但「能不能改」取决于串行是谁造成的：
    //     · 游戏主动调 nvnQueueFinish（NVN 契约，改不了）
    //     · 还是我们内部 renderer_flush/renderer_sync 太频繁（能改）
    //   这 9 个计数就是为了区分这两者。
    //
    // 【判据】
    //   nvn_queue_finish_cnt 高  -> 游戏主动 finish，是 NVN 契约，改不了
    //   renderer_sync_cnt 高但 nvn_queue_finish_cnt 低 -> 我们自己造成的，能改
    //   wait_dist_1 占比高        -> CPU 总在等最新一帧（并行没起来）
    //   wait_dist_4plus 占比高    -> CPU 跑太前，GPU 落后（这是好事）
    // ==========================================================================

    // 游戏调用 nvnQueueFinish 的次数（objects.c）。
    //   这一项最关键：如果每帧 1 次，说明游戏自己要求同步，我们改不了。
    uint64_t nvn_queue_finish_cnt;
    // 我们内部主动提交+等待的次数（exec.c renderer_flush / renderer_sync）。
    uint64_t renderer_flush_cnt;
    uint64_t renderer_sync_cnt;

    // vk_wait_seq 的等待深度分布：back = submit_seq - seq（落后几个 submission）。
    //   back == 1 表示等最新的那个（理想）；
    //   back >= 2 表示 CPU 已跑在 GPU 前面（说明并行有效）。
    //   分桶是为了不引入直方图的复杂度，又足以看出分布形态。
    uint64_t wait_dist_1;
    uint64_t wait_dist_2;
    uint64_t wait_dist_3;
    uint64_t wait_dist_4plus;
    uint64_t wait_dist_max;       // 本窗口最大 back 值
    uint64_t vk_wait_total_cnt;   // vk_wait_seq 总调用次数

    // ---- v17：修正 v13 的三处结构性失真（新增 9 项）----
    // 为什么必须加：v13/v14 的三个判据（in_flight / wait_dist / sync_minus_finish）
    // 都是计数器的结构性产物，不是 GPU 行为。详见《02_度量缺陷审查》。
    //
    //   inflight_true_* : submit_seq − timeline 已完成值。唯一可信的「同时几帧在跑」口径。
    //                     旧 in_flight_max 恒为 1（每帧提交+1/等待-1 对冲），勿再用于判断。
    //   wait_nosub_cnt  : 请求了「尚未提交」的 seq 的次数。这个数大 => 游戏/录制跑在 GPU 前面
    //                     => 并行有空间；这个数≈0 => 每次 wait 都在等已完成/最新的工作。
    //   wait_site_ns    : 按调用站点归因的阻塞时长（纳秒）。判据见下面的索引表。
    //                     站点：[0]DEFAULT/杂项 [1]SUB 命令环复用 [2]FRAME_BEGIN 交换链槽
    //                          [3]FENCE renderer_wait_seq(NVN fence) [4]FLUSH [5]SYNC
    int      inflight_true_max;
    int      inflight_true_end;
    uint64_t wait_nosub_cnt;
    uint64_t wait_site_cnt[7];
    uint64_t wait_site_ns[7];
} LasStats;

// ---------------------------------------------------------------------------
// 采集类别（Sink）注册表 —— 扩展性的核心
// ---------------------------------------------------------------------------
// ★ 以后要加一个新采集项（比如音频 underrun、显存占用），只做三步：
//     1) 写 on_open/on_close + 一个数据回调
//     2) 在 las_collect.c 的 g_sinks[] 数组里加一行
//     3) 如果数据不在 LasStats 里，在 LasStats 加字段 + 在 hook 点填它
//   会话管理、目录、时间轴、缓冲、meta、NAPI、UI 全都不用动。
//
// ★ 三条红线（写在前面，免得后来人踩）：
//   ① 每个 Sink 各自持有 FILE*，互不共享 —— 一个写错只坏一个文件；
//   ② 回调里禁止 malloc 大块（回调跑在 present 热路径上，会拖帧）；
//   ③ 回调里禁止调用 renderer_stats() 或任何会清零的写操作。
typedef struct LasSink LasSink;
struct LasSink {
    const char *name;        // "PERF" / "FRAME" / "MARK" ...
    const char *suffix;      // 文件名后缀，如 "_perf.csv"
    int         stage;       // 1/2/3：按阶段开关，便于后续分阶段放量
    const char *header;      // CSV 表头（NULL = 不写表头）

    int  (*on_open) (LasSink *s, const char *dir, const char *session);
    void (*on_stats)(LasSink *s, const LasStats *st, const uint32_t *pass_end_line, int n_pass);
    void (*on_frame)(LasSink *s, uint64_t frame, double dt_ms);
    void (*on_mark) (LasSink *s, const char *event, const char *note, uint64_t frame, double t_ms);
    void (*on_close)(LasSink *s, const char *reason);

    void *priv;              // 每个 Sink 自己的状态（通常是 FILE*）
    int   rows;              // 已写行数（给 stop 时的摘要用）
};

// ---------------------------------------------------------------------------
// 生命周期 / 控制（由 NAPI 桥调进来）
// ---------------------------------------------------------------------------

// 开始采集。flag 目前保留（0/1 都表示开始，留给以后"只采某几类"）。
// 返回：0 = 成功；-1 = 失败（拿不到数据根 / 建目录失败 / 文件打不开）；
//       -2 = 已经在采集中（重复调用，直接忽略）。
int las_collect_start(int flag);

// 停止采集，并写 _meta.json / _mark.json 收尾。
// out 是调用方提供的栈缓冲（不返回 malloc 的指针：跨模块 free 容易出事），
// 里面填一行 JSON 摘要，如 {"ok":1,"rows":600,"frames":18023,"duration_ms":600333}
void las_collect_stop(char *out, size_t out_size);

// 打一个标记（不停止采集）。返回打点时的帧号；未在采集时返回 -1。
int las_collect_mark(const char *tag);

// 查状态。out 填 JSON，如 {"active":1,"session":"20261123_163000","rows":120}
void las_collect_status(char *out, size_t out_size);

// 当前是否在采集（0/1）。热路径上用来做最便宜的一次短路。
int las_collect_is_active(void);

// ---------------------------------------------------------------------------
// 数据入口（由运行时 hook 调用）
// ---------------------------------------------------------------------------

// 每 1 秒一次（exec.c 的 renderer_stats() 第一句）。
// ★ 必须在 renderer_stats() 清零之前调用，否则拿到的一半是 0。
void las_collect_on_stats(const LasStats *st, const uint32_t *pass_end_line, int n_pass);

// 每帧一次（device.c 的 total_frames++ 之后）。
// dt_ms 由本模块自己用 SDL_GetTicksNS 算，调用方不用传时间。
void las_collect_on_frame(uint64_t frame);

#ifdef __cplusplus
}
#endif

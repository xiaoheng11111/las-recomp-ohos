// ============================================================================
// 游戏内可控的运行时数据采集系统 —— 实现
// ----------------------------------------------------------------------------
// 结构：
//   会话管理（目录/时间轴/meta） + 采集器注册表（g_sinks[]） + 3 个内置 Sink
//
// 内置 Sink（阶段 1）：
//     PERF  _perf.csv    1 Hz   48 列，本窗口全部指标
//     FRAME _frame.csv   每帧   3 列（t_ms, frame, dt_ms）
//     MARK  _mark.csv    事件   4 列（t_ms, frame, event, note）
//   另加 _meta.json：会话指纹 + 版本 + 崩溃识别
//
// ★ 为什么 CSV 全部自己 fopen，而不用 printf
//   game_entry.cpp 的 setup_stderr_to_hilog() 把 stdout 和 stderr 都 dup2 进了
//   pipe 转 hilog（见 game_entry.cpp:111-120）。任何 printf/fprintf(stdout)
//   都会被送进 hilog 而不是文件，而且在管道里还会粘行/丢内容。
//   所以：数据落盘必须自己 open 文件，一条都不能走 stdio 的默认流。
//
// ★ 为什么不在采集器里重新计时
//   所有指标都来自运行时已有的计数器（exec.c / device.c）。重新计时会二次开销，
//   也会让"开采集"本身变成被测对象的一部分（观测者效应）。
// ============================================================================

#include "las_collect.h"

#include <SDL3/SDL_timer.h>   // SDL_GetTicksNS / SDL_GetTicks
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>    // gettimeofday：会话 ID 与时间戳（见下方 note_session_time 的说明）
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

// 数据根目录（game_entry.cpp 里 setenv 的那个 g_root_dir，也就是 .../lasrecomp）
// 由 game_entry 在启动时通过环境变量告诉我们，这里只读、绝不修改它。
// ⚠️ LARECOMP_DATA_ROOT 是移植层的命根子，任何情况下都不要 setenv 覆盖（会 FATAL）。
#define LAS_COLLECT_DIRNAME "collect"

// 移植层 present 计数（定义在 device.c:1332）。
// 声明放在文件级而不是函数内：本文件有多处要用（start 记起始帧、mark 记锚点、meta 记结束帧），
// 每个函数里单独 extern 一次容易漏（漏了会触发构建脚本的 implicit declaration 检查）。
extern uint64_t renderer_present_count(void);

// 单次会话的硬上限：防止用户忘了关采集，把沙箱塞满。
// 20 分钟 / 单文件 100MB —— 到这个量说明已经够分析了，再采只是浪费。
#define LAS_MAX_SESSION_MS   (20 * 60 * 1000)
#define LAS_MAX_FILE_BYTES   (100ull * 1024 * 1024)

// 缓冲：64KB 全缓冲。
// 为什么不全关缓冲：FRAME 是每帧一行，30fps 下每帧一次 fwrite = 每秒 30 次
// 系统调用，在当前 CPU exec 已经 294ms/秒的临界状态下不能忽视。
// 为什么又要定期 flush：崩溃时最多丢 60 帧（≈2 秒），FRAME 还能看出趋势。
#define LAS_BUF_BYTES  65536
#define LAS_FLUSH_EVERY_FRAMES  60

// ---------------------------------------------------------------------------
// 会话状态
// ---------------------------------------------------------------------------
static int      g_active;
static char     g_session[32];        // 20261123_163000
static char     g_dir[768];           // <root>/collect/20261123_163000
static char     g_root[700];          // <files>/lasrecomp
static uint64_t g_t0_ns;              // 时间原点（开始采集那一刻）
static uint64_t g_start_frame;
static uint64_t g_last_frame_ns;      // 上一帧的时间戳，用于算 dt_ms
static uint64_t g_start_wall_ms;      // 墙上时间（写 meta，跨进程可对齐）
static double   g_last_mark_t_ms;     // 最近一次 mark 的时间（给"标记当前帧"用）
static char     g_last_note[64];

static long     g_stats_rows;         // PERF 行数
static long     g_frame_rows;         // FRAME 行数
static long     g_mark_rows;          // MARK 行数
static char     g_end_reason[32];     // user_stop / auto_limit / crash
static int      g_closed;

// ★ v15 自动采集（LARECOMP_AUTOREC）的状态标记，定义在这里而不是使用处，
//   因为 _meta.json 的写入函数（下方 meta_write）在自动采集代码之前就要读它。
//   含义：本次会话是不是由自动采集启动的（1=是）。写进 _meta.json 的 "autorec" 字段，
//   方便事后区分「用户手动录的」和「自动录的」——两者的数据质量可能不同
//   （自动录的会包含启动后 N 秒内的读档过程）。
static int      g_autorec_started = 0;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static double now_ms(void) {
    if (!g_t0_ns) return 0.0;
    uint64_t t = SDL_GetTicksNS();
    return (t >= g_t0_ns) ? (double)(t - g_t0_ns) / 1e6 : 0.0;
}

static uint64_t wall_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

// ---------------------------------------------------------------------------
// 本地时间拆分 —— ★ 为什么不用 <time.h> + localtime_r
// ---------------------------------------------------------------------------
// 本文件的编译参数里有一条 -I<root>\platform\ohos\ffmpeg-arm64\include\libavutil，
// 而那个目录下正好有一个 ffmpeg 自己的 time.h。
// 于是 `#include <time.h>` 会被解析成 ffmpeg 的私有头，struct tm / time() / strftime
// 全部变成未声明 —— 编译直接报 "variable has incomplete type 'struct tm'"。
// （已用最小复现验证：单测 time.h 正常，加上那个 -I 就坏。）
//
// 所以这里不碰 <time.h>，改用 gettimeofday 拿到的 epoch 秒，自己折算成
// 「本地时区的年月日时分秒」。会话 ID 只需要精确到秒，这样完全够用。
// 时区偏移用 localtime 的替代：直接读 TZ 环境变量算不了，所以用
// OHOS 上固定 +8（本机是国内设备）的方案 —— 见下面的 LAS_TZ_OFFSET_H。
// 若以后要跨时区，把这里的偏移改成从 persist.sys.timezone 读。
#ifndef LAS_TZ_OFFSET_H
#define LAS_TZ_OFFSET_H 8      // 北京时间 UTC+8
#endif

// 把 epoch 秒折算成本地时间。返回 0 成功。
// 算法：从 1970 起按 400/100/4/1 年周期推进，再按月/日推进（经典的公历换算）。
static int local_hms(int64_t epoch_s, int *Y, int *M, int *D, int *h, int *m, int *s) {
    int64_t t = epoch_s + (int64_t)LAS_TZ_OFFSET_H * 3600;
    int64_t days = t / 86400;
    int64_t rem  = t % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    *h = (int)(rem / 3600);
    *m = (int)((rem % 3600) / 60);
    *s = (int)(rem % 60);

    // 1970-01-01 是星期四；这里只算年月日，不关心星期
    int64_t y = 1970;
    for (;;) {
        int leap = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 1 : 0;
        int64_t yd = leap ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        y++;
    }
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int leap = ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 1 : 0;
    int mo = 0;
    for (; mo < 12; mo++) {
        int d = mdays[mo] + ((mo == 1) ? leap : 0);
        if (days < d) break;
        days -= d;
    }
    *Y = (int)y;
    *M = mo + 1;
    *D = (int)days + 1;
    return 0;
}

// 递归建目录（只建一层就够：collect/ 和 collect/<会话>/）
static int mkdir_p(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return (S_ISDIR(st.st_mode)) ? 0 : -1;
    return mkdir(path, 0777);
}

// 给 Sink 开文件并套上缓冲。
// 统一在这里做，Sink 自己不用管缓冲策略。
//
// ★ 2026-11-23 修复（真机验证发现）：
//   原来这里传的是 name="PERF"、suffix="_perf.csv"，
//   拼出来是 "PERF_perf.csv" —— 类别名在文件名里重复了两次
//   （设备文件系统不区分大小写，实际落盘 perf_perf.csv）。
//   现在统一改成 "perf.csv / frame.csv / mark.csv"（去掉那个下划线前缀）。
//   分析工具 analyze_collect.py 已经兼容新旧两种命名，不会因这次改名读不到旧数据。
static FILE *sink_open(LasSink *s, const char *name) {
    char path[900];
    // name 现在是不带下划线的纯类别名（"perf" / "frame" / "mark"）
    snprintf(path, sizeof path, "%s/%s.csv", g_dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    // 每个 Sink 一份独立缓冲。放在 Sink 上而不是函数静态：
    // 以后加新 Sink 时不会因为共用一块静态缓冲而互相踩。
    char *buf = (char *)malloc(LAS_BUF_BYTES);
    if (buf) setvbuf(f, buf, _IOFBF, LAS_BUF_BYTES);
    if (s->header && *s->header) fputs(s->header, f);
    s->priv = f;
    return f;
}

static void sink_close(LasSink *s) {
    if (!s->priv) return;
    FILE *f = (FILE *)s->priv;
    fflush(f);
    fclose(f);
    s->priv = NULL;
}

// 一行 CSV 写完：出错（磁盘满等）就静默丢弃，绝不因为写盘失败让游戏崩。
// 采集是辅助功能，任何情况下都不能影响游戏本体。
static void emit(LasSink *s, const char *fmt, ...) {
    if (!s->priv) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf((FILE *)s->priv, fmt, ap);
    va_end(ap);
    s->rows++;
}

// ---------------------------------------------------------------------------
// Sink ①：PERF —— 1 Hz，一行一个统计窗口
// ---------------------------------------------------------------------------
static int perf_open(LasSink *s, const char *dir, const char *session) {
    (void)dir; (void)session;
    return sink_open(s, "perf") ? 0 : -1;
}

// 把 prof_end_line[] 里非零项拼成 "931:224 2872:48"。
// 阈值 > 20 与原始 LOG 保持一致（exec.c:846），避免把噪音打进来。
static void fmt_pass_ends(char *out, size_t out_size, const uint32_t *pl, int n) {
    out[0] = 0;
    if (!pl) return;
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        if (pl[i] <= 20) continue;
        int w = snprintf(out + used, out_size - used, "%s%d:%u", used ? " " : "", i, pl[i]);
        if (w <= 0 || (size_t)w >= out_size - used) break;
        used += (size_t)w;
    }
}

static void perf_stats(LasSink *s, const LasStats *st,
                       const uint32_t *pass_end_line, int n_pass) {
    char pe[512];
    fmt_pass_ends(pe, sizeof pe, pass_end_line, n_pass);
    // ★ 列顺序（v13 起 56 列）：
    //   前 47 列是数值，pass_ends 在第 48 位（加双引号，内含空格和冒号），
    //   再往后 8 列是 v13 新增的交换链/并行度指标。
    //
    //   ★ 为什么把 pass_ends 从「最后一列」挪到第 48 位：
    //     v13 要加 8 个新列。如果 pass_ends 还在最后，新列只能插在它前面，
    //     而 pass_ends 的值里有空格，靠"剩下全归最后一列"的解析假设会失效。
    //     挪到中间 + 加双引号后，任何标准 CSV 解析器（含 python csv）都能正确读。
    //     分析工具一律按列名取值，所以移位不影响老脚本。
    emit(s, "%.1f,%llu,%.1f,%.0f,"
            "%llu,%llu,%llu,%llu,%llu,"
            "%llu,%llu,%.0f,%.0f,%.0f,%.0f,"
            "%llu,%.1f,%llu,%llu,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,"
            "%llu,%llu,%llu,%.1f,%llu,%llu,"
            "%.0f,%.0f,%.0f,%.0f,"
            "\"%s\","
            "%d,%d,%.1f,%.1f,%.1f,%d,%d,%d,"
            // ---- v13 B 组：draw 内部 4 分项 + 每 draw 平均微秒 + 自检差额 ----
            "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
            // ---- v13 C 组：GPU 总量/气泡 + 4 个恒 -1 的占位 + 6 个 pass 分类 ----
            "%.2f,%.2f,-1,-1,-1,%d,-1,-1,-1,-1,-1,-1,"
            // ---- v13 D 组：附加 8 项 ----
            "%llu,%.2f,%llu,%llu,%llu,%llu,%.1f,%.2f,"
            // ---- v14：串行根因 9 项 ----
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            // ---- v16：cpu_vtx 内部拆解 3 项（纯计时；子项关系见 las_collect.h）----
            "%.2f,%.2f,%.2f,"
            // ---- v17：真实在飞 + 站点归因 9 项 ----
            "%d,%d,%llu,"
            "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
         st->t_ms, (unsigned long long)st->frame, st->fps, st->max_frame_ms,
         (unsigned long long)st->draws, (unsigned long long)st->skipped,
         (unsigned long long)st->passes, (unsigned long long)st->dispatches,
         (unsigned long long)st->flushes,
         (unsigned long long)st->wait_ms, (unsigned long long)st->compile_n,
         st->translate_ms, st->spirv_ms, st->pipelines_ms, st->draws_waited_ms,
         (unsigned long long)st->fbcopy, st->upload_mb,
         (unsigned long long)st->vtx_copied, (unsigned long long)st->vtx_inplace,
         (unsigned long long)st->skip[0], (unsigned long long)st->skip[1],
         (unsigned long long)st->skip[2], (unsigned long long)st->skip[3],
         (unsigned long long)st->skip[4], (unsigned long long)st->skip[5],
         (unsigned long long)st->skip[6],
         st->gpu_ms, st->cpu_exec_ms, st->cpu_draw_ms, st->cpu_pre_ms,
         st->cpu_key_ms, st->cpu_dyn_ms, st->cpu_sets_ms, st->cpu_tex_ms,
         st->cpu_tex_upload_ms, st->cpu_vtx_ms, st->submit_ms,
         (unsigned long long)st->tex_checks, (unsigned long long)st->tex_samples,
         (unsigned long long)st->tex_full, st->tex_full_mb,
         (unsigned long long)st->tex_dyn, (unsigned long long)st->views_created,
         st->sets_alloc_ms, st->sets_upd_ms, st->vtx_copy_ms, st->vtx_bind_ms,
         pe,
         // ---- v13 A 组：交换链 / 并行度（8 列）----
         st->sw_images, st->sw_present_mode,
         st->acq_block_ms, st->pres_block_ms, st->fence_wait_ms,
         st->in_flight_max, st->in_flight_end, st->frames_submitted,
         // ---- v13 B 组：draw 内部拆分（5 列）----
         // draw_per_call_us 在这里现算，不单独存字段：
         //   = cmd_draw_ms / draws * 1000（把毫秒换成微秒）。
         //   为什么用 cmd_draw 而不是 cpu_draw：前者才是"每次 draw 调用的净成本"，
         //   后者含业务逻辑，除出来会偏大、失去"单次调用开销"的含义。
         st->cmd_draw_ms,
         st->bind_ms,
         st->desc_bind_ms,
         st->state_ms,
         (st->draws > 0) ? (st->cmd_draw_ms * 1000.0 / (double)st->draws) : 0.0,
         // ---- v13.1 自检：B 组拆分完整度 ----
         st->draw_ms_minus_parts_ms,
         // ---- v13 C 组：GPU 总量 / 气泡（4 个恒 -1 的占位在格式串里写死）----
         st->gpu_total_ms,
         st->gpu_bubble_ms,
         st->gpu_marks_used,
         // ---- v13 D 组：附加 8 项 ----
         (unsigned long long)st->tex_upload_count,
         st->tex_upload_bytes_mb,
         (unsigned long long)st->tex_view_create_cnt,
         (unsigned long long)st->tex_hash_scan_cnt,
         (unsigned long long)st->pipe_create_cnt,
         (unsigned long long)st->pipe_bind_cnt,
         st->mem_used_mb,
         st->frame_budget_over_ms,
         // ---- v14：串行根因 9 项 ----
         (unsigned long long)st->nvn_queue_finish_cnt,
         (unsigned long long)st->renderer_flush_cnt,
         (unsigned long long)st->renderer_sync_cnt,
         (unsigned long long)st->wait_dist_1,
         (unsigned long long)st->wait_dist_2,
         (unsigned long long)st->wait_dist_3,
         (unsigned long long)st->wait_dist_4plus,
         (unsigned long long)st->wait_dist_max,
         (unsigned long long)st->vk_wait_total_cnt,
         // ---- v16：cpu_vtx 内部拆解 3 项 ----
         //   vtx_idx 是总时长，vtx_scan / vtx_expand 是它的子集（各自内部还有别的开销）。
         //   ★ 分析时不要相加；算占比用 vtx_idx 做分母。
         st->vtx_idx_ms, st->vtx_scan_ms, st->vtx_expand_ms,
         // ---- v17：真实在飞（2）+ nosub（1）+ 6 个站点阻塞 ms ----
         st->inflight_true_max, st->inflight_true_end,
         (unsigned long long)st->wait_nosub_cnt,
         (double)st->wait_site_ns[0] / 1e6, (double)st->wait_site_ns[1] / 1e6,
         (double)st->wait_site_ns[2] / 1e6, (double)st->wait_site_ns[3] / 1e6,
         (double)st->wait_site_ns[4] / 1e6, (double)st->wait_site_ns[5] / 1e6);
    // PERF 是 1Hz，每行都 flush：窗口数据丢一行就少一秒，
    // 而这个频率下 flush 的成本完全可以忽略。
    if (s->priv) fflush((FILE *)s->priv);
    g_stats_rows = s->rows;
}

// ---------------------------------------------------------------------------
// Sink ②：FRAME —— 每帧
// ---------------------------------------------------------------------------
static int frame_open(LasSink *s, const char *dir, const char *session) {
    (void)dir; (void)session;
    return sink_open(s, "frame") ? 0 : -1;
}

static void frame_on_frame(LasSink *s, uint64_t frame, double dt_ms) {
    emit(s, "%.3f,%llu,%.3f\n", now_ms(), (unsigned long long)frame, dt_ms);
    g_frame_rows = s->rows;
    // 每 60 帧 flush 一次：崩溃最多丢 2 秒数据，但省掉 97% 的 flush 调用。
    if ((frame % LAS_FLUSH_EVERY_FRAMES) == 0 && s->priv) fflush((FILE *)s->priv);
}

// ---------------------------------------------------------------------------
// Sink ③：MARK —— 用户事件（点按钮那一刻）
// ---------------------------------------------------------------------------
static int mark_open(LasSink *s, const char *dir, const char *session) {
    (void)dir; (void)session;
    return sink_open(s, "mark") ? 0 : -1;
}

static void mark_on_mark(LasSink *s, const char *event, const char *note,
                         uint64_t frame, double t_ms) {
    // note 里可能没有逗号但可能有引号：简单转义一下双引号，保证 CSV 合法。
    char esc[128];
    size_t w = 0;
    if (note) {
        for (const char *p = note; *p && w + 2 < sizeof esc; p++) {
            if (*p == '"') esc[w++] = '"';   // CSV 里双引号用两个双引号转义
            esc[w++] = *p;
        }
    }
    esc[w] = 0;
    emit(s, "%.1f,%llu,%s,\"%s\"\n", t_ms, (unsigned long long)frame, event, esc);
    if (s->priv) fflush((FILE *)s->priv);   // 标记必须立刻落盘，这是最不能丢的数据
    g_mark_rows = s->rows;
}

// ---------------------------------------------------------------------------
// 采集器注册表
// ---------------------------------------------------------------------------
// ★ 加新采集项就在这里加一行。这是本设计「可扩展」的全部含义：
//   以后加 SHADER / TEX / PASS / MEM 等，写好自己的回调 + 在这里加一行即可，
//   会话管理、时间轴、目录、NAPI、UI 一律不动。
//
// stage 说明：1 = 阶段1（本次实现），2 = 阶段2，3 = 阶段3。
//   las_collect_start() 按 LAS_STAGE_MAX 过滤，便于分阶段放量。
#define LAS_STAGE_MAX 1

static LasSink g_sinks[] = {
    { "PERF",  "_perf.csv",  1,
      // 56 列表头（v13）。★ 解析一律按列名，不要按位置 —— 以后加列不破坏老脚本。
      "t_ms,frame,fps,max_frame_ms,"
      "draws,skipped,passes,dispatches,flushes,"
      "wait_ms,compile_n,translate_ms,spirv_ms,pipelines_ms,draws_waited_ms,"
      "fbcopy,upload_mb,vtx_copied,vtx_inplace,"
      "skip_noprog,skip_compiling,skip_failed,skip_noobj,skip_mode,skip_pipe_building,skip_pipe_failed,"
      "gpu_ms,cpu_exec_ms,cpu_draw_ms,cpu_pre_ms,cpu_key_ms,cpu_dyn_ms,"
      "cpu_sets_ms,cpu_tex_ms,cpu_tex_upload_ms,cpu_vtx_ms,submit_ms,"
      "tex_checks,tex_samples,tex_full,tex_full_mb,tex_dyn,views_created,"
      "sets_alloc_ms,sets_upd_ms,vtx_copy_ms,vtx_bind_ms,"
      // pass_ends 从 v13 起位于第 48 位（值含空格，加双引号）
      "pass_ends,"
      // ---- v13 A 组：交换链 / 帧并行度（8 列）----
      "sw_images,sw_present_mode,acq_block_ms,pres_block_ms,fence_wait_ms,"
      "in_flight_max,in_flight_end,frames_submitted,"
      // ---- v13 B 组：draw 内部拆分（5 列）----
      // draw_per_call_us 是算出来的（cmd_draw_ms / draws * 1000），不是独立采集项。
      "cmd_draw_ms,bind_ms,desc_bind_ms,state_ms,draw_per_call_us,"
      // v13.1 自检列：cpu_draw_ms 减去 B 组 4 项的差额（判断拆分完整度）
      "draw_ms_minus_parts_ms,"
      // ---- v13 C 组：GPU 总量 / 气泡 / 6 个 pass 分类（恒 -1）----
      // 6 个 pass 分类字段为什么保留：让分析脚本能明确区分"没采到(-1)"和"采到 0"，
      // 而不是靠缺列去猜。游戏不下发 pass 名称，硬猜会误导优化方向。
      "gpu_total_ms,gpu_bubble_ms,gpu_dispatch_ms,gpu_copy_ms,gpu_draws_gpu_ms,"
      "gpu_marks_used,gpu_shadow_ms,gpu_main_ms,gpu_post_ms,gpu_ui_ms,gpu_occ_ms,gpu_other_ms,"
      // ---- v13 D 组：附加计数（8 列）----
      "tex_upload_count,tex_upload_bytes_mb,tex_view_create_cnt,tex_hash_scan_cnt,"
      "pipe_create_cnt,pipe_bind_cnt,mem_used_mb,frame_budget_over_ms,"
      // ---- v14 新增 9 列：串行根因定位 ----
      // 判据：nvn_queue_finish_cnt 高 => 游戏主动 finish（NVN 契约，改不了）；
      //       renderer_sync_cnt 高但 finish_cnt 低 => 我们自己造成的，能改。
      "nvn_queue_finish_cnt,renderer_flush_cnt,renderer_sync_cnt,"
      "wait_dist_1,wait_dist_2,wait_dist_3,wait_dist_4plus,wait_dist_max,vk_wait_total_cnt,"
      // ---- v16 新增 3 列：cpu_vtx 内部拆解（判定 NEON 值不值得写）----
      // 子项关系：vtx_scan ⊂ vtx_idx，vtx_expand ⊂ vtx_idx。解析时勿相加。
      "vtx_idx_ms,vtx_scan_ms,vtx_expand_ms,"
      // ---- v17 新增 9 列：真实在飞 + 站点归因（修正 v13 三处结构性失真）----
      // 判据：
      //   inflight_true_max >= 2          -> GPU 上确实同时有 2 帧以上（并行成立）
      //   wait_nosub_cnt ≈ vk_wait_total  -> 几乎每次都在等「尚未提交」的工作（游戏领先）
      //   wait_site_fence_ms 占比最高      -> 阻塞来自 NVN fence（objects.c nvnSyncWait）
      //   wait_site_frame_begin_ms 占比最高-> 阻塞来自交换链槽复用（device.c vk_frame_begin）
      "inflight_true_max,inflight_true_end,wait_nosub_cnt,"
      "wait_site_default_ms,wait_site_sub_ms,wait_site_frame_begin_ms,"
      "wait_site_fence_ms,wait_site_flush_ms,wait_site_sync_ms\n",
      perf_open, perf_stats, NULL, NULL, NULL, NULL, 0 },

    { "FRAME", "_frame.csv", 1,
      "t_ms,frame,dt_ms\n",
      frame_open, NULL, frame_on_frame, NULL, NULL, NULL, 0 },

    { "MARK",  "_mark.csv",  1,
      "t_ms,frame,event,note\n",
      mark_open, NULL, NULL, mark_on_mark, NULL, NULL, 0 },

    // ---- 阶段 2 预留（不实现，但位置先占好，加的时候直接补函数）----
    // { "SHADER", "_shader.csv", 2, "...", shader_open, NULL, NULL, NULL, shader_close, NULL, 0 },
    // { "TEX",    "_tex.csv",    2, "...", tex_open,    NULL, NULL, NULL, tex_close,    NULL, 0 },
    // { "PASS",   "_pass.csv",   2, "...", NULL, NULL, NULL, NULL, NULL, NULL, 0 },
};

#define LAS_NSINK ((int)(sizeof g_sinks / sizeof g_sinks[0]))

// ---------------------------------------------------------------------------
// meta / mark 的 JSON 收尾
// ---------------------------------------------------------------------------
static void write_meta(const char *reason) {
    char path[900];
    snprintf(path, sizeof path, "%s/_meta.json", g_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;

    uint64_t end_wall = wall_ms();
    double dur = now_ms();
    // 结束帧号（renderer_present_count 已在文件顶部声明）
    uint64_t end_frame = renderer_present_count();

    // 时间字符串（人读用）。不用 strftime：见 local_hms 上方的说明（ffmpeg 的 time.h 遮蔽）
    char iso[32] = "";
    {
        int Y, Mo, D, hh, mm, ss;
        int64_t now_s = (int64_t)(wall_ms() / 1000ull);
        local_hms(now_s, &Y, &Mo, &D, &hh, &mm, &ss);
        snprintf(iso, sizeof iso, "%04d-%02d-%02dT%02d:%02d:%02d", Y, Mo, D, hh, mm, ss);
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": \"lasrecomp.collect/1\",\n");
    fprintf(f, "  \"session_id\": \"%s\",\n", g_session);
    fprintf(f, "  \"started_at_wall_ms\": %llu,\n", (unsigned long long)g_start_wall_ms);
    fprintf(f, "  \"ended_at_wall_ms\": %llu,\n", (unsigned long long)end_wall);
    fprintf(f, "  \"started_at_iso\": \"%s\",\n", iso);
    fprintf(f, "  \"ended_reason\": \"%s\",\n", reason ? reason : "unknown");
    // ★ v15：区分「自动录的」和「用户手动录的」。
    //   独立字段而不是复用 ended_reason —— 因为 ended_reason 记的是「怎么结束的」，
    //   而用户手动停止会把自动录的会话也标成 user_stop，无法追溯。
    fprintf(f, "  \"autorec\": %d,\n", g_autorec_started);
    fprintf(f, "  \"duration_ms\": %.0f,\n", dur);
    fprintf(f, "  \"start_frame\": %llu,\n", (unsigned long long)g_start_frame);
    fprintf(f, "  \"end_frame\": %llu,\n", (unsigned long long)end_frame);
    fprintf(f, "  \"fps_interval_s\": 1.0,\n");
    fprintf(f, "  \"categories\": [");
    {
        int first = 1;
        for (int i = 0; i < LAS_NSINK; i++) {
            if (g_sinks[i].stage > LAS_STAGE_MAX) continue;
            fprintf(f, "%s\"%s\"", first ? "" : ", ", g_sinks[i].name);
            first = 0;
        }
    }
    fprintf(f, "],\n");
    fprintf(f, "  \"counts\": { \"perf_rows\": %ld, \"frame_rows\": %ld, \"marks\": %ld },\n",
            g_stats_rows, g_frame_rows, g_mark_rows);
    // ★ build 指纹：本工程踩过「改了源码但二进制没变」的事故，
    //   所以每份数据必须能追溯它来自哪个 .so。BUILD TAG 由编译期 -D 注入。
#ifdef LAS_BUILD_TAG
    fprintf(f, "  \"build\": { \"tag\": \"%s\" },\n", LAS_BUILD_TAG);
#else
    fprintf(f, "  \"build\": { \"tag\": \"unknown\" },\n");
#endif
    fprintf(f, "  \"last_note\": \"%s\"\n", g_last_note);
    fprintf(f, "}\n");
    fflush(f);
    fclose(f);
}

// _mark.json：给人读的标记摘要（机器对齐以 _mark.csv 为准）
static void write_marks_json(void) {
    char path[900];
    snprintf(path, sizeof path, "%s/_mark.json", g_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\n  \"schema\": \"lasrecomp.collect.marks/1\",\n");
    fprintf(f, "  \"session_id\": \"%s\",\n", g_session);
    fprintf(f, "  \"last_mark_t_ms\": %.1f,\n", g_last_mark_t_ms);
    fprintf(f, "  \"last_note\": \"%s\",\n", g_last_note);
    fprintf(f, "  \"note\": \"机器对齐请用 _mark.csv；本文件只是给人看的摘要\"\n}\n");
    fflush(f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// 对外：开始 / 停止 / 标记 / 状态
// ---------------------------------------------------------------------------
int las_collect_is_active(void) { return g_active; }

int las_collect_start(int flag) {
    (void)flag;
    if (g_active) return -2;

    // 数据根：优先用移植层已经在用的 LARECOMP_DATA_ROOT（只读！绝不覆盖）。
    // 这个变量就是 game_entry.cpp:920 设的 g_data_root，也就是 .../lasrecomp。
    const char *root = getenv("LARECOMP_DATA_ROOT");
    if (!root || !*root) return -1;
    snprintf(g_root, sizeof g_root, "%s", root);

    // 会话 ID 用本地时间，和用户看手机的时间一致，方便事后对上"几点几分觉得卡"。
    // 不用 time()/localtime_r：见 local_hms 上方的说明（ffmpeg 的 time.h 会遮蔽系统头）。
    {
        int Y, Mo, D, hh, mm, ss;
        int64_t now_s = (int64_t)(wall_ms() / 1000ull);
        local_hms(now_s, &Y, &Mo, &D, &hh, &mm, &ss);
        snprintf(g_session, sizeof g_session, "%04d%02d%02d_%02d%02d%02d", Y, Mo, D, hh, mm, ss);
    }

    char collect_dir[900];
    snprintf(collect_dir, sizeof collect_dir, "%s/%s", g_root, LAS_COLLECT_DIRNAME);
    if (mkdir_p(collect_dir) != 0 && errno != EEXIST) return -1;

    snprintf(g_dir, sizeof g_dir, "%s/%s", collect_dir, g_session);
    if (mkdir_p(g_dir) != 0 && errno != EEXIST) return -1;

    // 时间轴归零。所有文件、所有标记都从这里算 t_ms → 天然可 join。
    g_t0_ns = SDL_GetTicksNS();
    g_last_frame_ns = g_t0_ns;
    g_start_wall_ms = wall_ms();
    g_start_frame = renderer_present_count();

    g_stats_rows = g_frame_rows = g_mark_rows = 0;
    g_last_mark_t_ms = 0.0;
    g_last_note[0] = 0;
    g_end_reason[0] = 0;
    g_closed = 0;

    // 打开所有启用的 Sink
    int opened = 0;
    for (int i = 0; i < LAS_NSINK; i++) {
        LasSink *s = &g_sinks[i];
        if (s->stage > LAS_STAGE_MAX) continue;
        s->rows = 0;
        s->priv = NULL;
        if (s->on_open && s->on_open(s, g_dir, g_session) == 0 && s->priv) {
            opened++;
        } else {
            s->priv = NULL;   // 打不开就禁用这个 Sink，不影响其它
        }
    }
    if (opened == 0) {
        g_t0_ns = 0;
        return -1;
    }

    g_active = 1;

    // 立刻写一条 START 标记：这样分析时能精确知道会话从哪一帧开始
    las_collect_mark("START");
    return 0;
}

void las_collect_stop(char *out, size_t out_size) {
    if (!g_active) {
        if (out && out_size) snprintf(out, out_size, "{\"ok\":0,\"err\":\"not_active\"}");
        return;
    }
    // 收尾顺序很重要：
    //   1) 先写一条 STOP 标记（此时 MARK 文件还开着）
    //   2) 再关所有 Sink（flush + fclose）
    //   3) 最后写 meta（meta 里的 counts 要用关文件之前统计的行数）
    las_collect_mark("STOP");

    double dur = now_ms();
    snprintf(g_end_reason, sizeof g_end_reason, "user_stop");

    for (int i = 0; i < LAS_NSINK; i++) {
        if (g_sinks[i].stage > LAS_STAGE_MAX) continue;
        if (g_sinks[i].on_close) g_sinks[i].on_close(&g_sinks[i], "user_stop");
        sink_close(&g_sinks[i]);
    }

    write_marks_json();
    write_meta("user_stop");

    g_active = 0;
    g_t0_ns = 0;

    if (out && out_size) {
        snprintf(out, out_size,
                 "{\"ok\":1,\"session\":\"%s\",\"perf_rows\":%ld,\"frame_rows\":%ld,"
                 "\"marks\":%ld,\"duration_ms\":%.0f}",
                 g_session, g_stats_rows, g_frame_rows, g_mark_rows, dur);
    }
}

int las_collect_mark(const char *tag) {
    if (!g_active) return -1;
    uint64_t fr = renderer_present_count();
    double t = now_ms();

    const char *ev = (tag && *tag) ? tag : "TAG";
    for (int i = 0; i < LAS_NSINK; i++) {
        LasSink *s = &g_sinks[i];
        if (s->stage > LAS_STAGE_MAX) continue;
        if (s->on_mark) s->on_mark(s, ev, g_last_note, fr, t);
    }
    g_last_mark_t_ms = t;
    return (int)fr;
}

void las_collect_status(char *out, size_t out_size) {
    if (!out || !out_size) return;
    snprintf(out, out_size,
             "{\"active\":%d,\"session\":\"%s\",\"perf_rows\":%ld,\"frame_rows\":%ld,"
             "\"marks\":%ld,\"t_ms\":%.0f}",
             g_active, g_session[0] ? g_session : "", g_stats_rows, g_frame_rows,
             g_mark_rows, g_active ? now_ms() : 0.0);
}

// ---------------------------------------------------------------------------
// 数据入口
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// v15：自动采集（LARECOMP_AUTOREC）
// ---------------------------------------------------------------------------
//
// 【为什么需要这个功能】
//   用户反馈「要进游戏 → 找按钮 → 点一下 → 录 → 再点停」流程太麻烦，
//   实际测试时忘了点按钮，导致采集目录一直是空的（等了 5 轮都没数据）。
//   加一个环境变量，让游戏启动 N 秒后自动开始采集，用户只要开游戏就行。
//
// 【为什么放在 las_collect_on_frame 的最前面（在 g_active 判断之前）】
//   这是唯一的逐帧入口（device.c 的 total_frames++ 之后调用，每帧一次），
//   而且【未采集时也会被调用】—— 正好是检查"该不该自动开始"的地方。
//   如果放在 `if (!g_active) return;` 之后，未采集时永远走不到，功能失效。
//
// 【为什么用 SDL_GetTicksNS() 的绝对值，而不用 now_ms()】
//   now_ms() 内部是 `(t - g_t0_ns) / 1e6`，而 g_t0_ns 只有在 las_collect_start()
//   里才被赋值 —— 未采集时它是 0，now_ms() 会直接 return 0.0（见其实现）。
//   所以自动开始之前根本没法用 now_ms() 计算"启动后过了多久"。
//   改用 SDL_GetTicksNS() 的绝对值做差，独立于采集状态。
//
// 【风险控制】
//   · 不设 LARECOMP_AUTOREC 时：g_autorec_state 初始为「未初始化」，
//     第一次调用读环境变量，没有就设为「已禁用」，之后每次只多一个 int 判断。
//     ★ 对现有手动模式【零行为改变】。
//   · 只尝试启动一次：启动失败（比如 LARECOMP_DATA_ROOT 没设）就标记为已尝试，
//     不会每帧重试打爆日志。
//   · las_collect_start() 里有 mkdir + fopen，属于重操作；
//     但整个进程生命周期只会调用一次，且发生在启动后 N 秒，不在热路径上。
static int g_autorec_state = 0;      // 0=未初始化 1=已禁用 2=等待中 3=已触发/已放弃
static uint64_t g_autorec_first_ns = 0;   // 首次调用 on_frame 的时刻（游戏启动后第一帧）
static int64_t  g_autorec_delay_ms = 10000;  // 默认 10 秒，给读档留时间
// ★ g_autorec_started 定义在文件顶部（见 g_end_reason 附近）——
//   因为 meta 写入函数在它之前就要用到，放在这里会编译失败。

// 返回值：1 表示本次调用刚刚启动了采集；0 表示没动作
static int autorec_maybe_start(void) {
    if (g_autorec_state == 1 || g_autorec_state == 3) return 0;   // 已禁用 / 已处理过

    uint64_t now = SDL_GetTicksNS();

    if (g_autorec_state == 0) {
        // 第一次进来：读环境变量，决定启用与否
        g_autorec_state = 1;   // 先假设禁用，读到合法值再改
        const char *v = getenv("LARECOMP_AUTOREC");
        if (v && *v) {
            int n = atoi(v);
            // 负值/0 视为禁用（用户想显式关掉时写 0 也生效）
            if (n > 0) {
                g_autorec_delay_ms = (int64_t)n * 1000;
                g_autorec_state = 2;      // 等待中
                g_autorec_first_ns = now;
                // 这一行是给用户看的关键提示：告诉他自动采集已武装、几秒后开始
                fprintf(stderr, "[collect] autorec armed: will start after %d s\n", n);
            }
        }
        return 0;
    }

    // state == 2：等待中，检查是否到点
    if (!g_autorec_first_ns) { g_autorec_first_ns = now; return 0; }
    uint64_t elapsed_ms = (now >= g_autorec_first_ns) ? (now - g_autorec_first_ns) / 1000000ull : 0;
    if ((int64_t)elapsed_ms < g_autorec_delay_ms) return 0;

    // 到点：尝试启动一次
    g_autorec_state = 3;   // 先标记，无论成功失败都不再重试

    // 已经有人在采集（用户手动点了按钮），不要重复启动
    if (g_active) return 0;

    int r = las_collect_start(1);
    if (r == 0) {
        // ★ lead 要求：自动开始时必须打一行日志，方便用户和事后排查确认
        fprintf(stderr, "[collect] autorec started after %llds\n",
                (long long)(g_autorec_delay_ms / 1000));
        // ★ 不用 g_end_reason 标记「这是自动录的」——
        //   因为用户手动停止时 las_collect_stop() 会把它覆盖成 "user_stop"，
        //   那个字段的本义是「怎么结束的」，不是「怎么开始的」。
        //   改用独立标记 g_autorec_started，在 _meta.json 里单独写一个字段。
        g_autorec_started = 1;
    } else {
        fprintf(stderr, "[collect] autorec failed to start: %d\n", r);
    }
    return (r == 0) ? 1 : 0;
}

// 每帧：device.c 的 total_frames++ 之后调用。
// 这里是【唯一】真正逐帧的点 —— renderer_stats() 被 fps_iv(1.0s) 节流，
// 所以 PERF 拿不到逐帧信息，FRAME 必须在这里采。
void las_collect_on_frame(uint64_t frame) {
    // ★ v15：自动采集检查必须在 g_active 判断【之前】——
    //   见 autorec_maybe_start 上方的说明（未采集时也要能进来检查）。
    autorec_maybe_start();

    if (!g_active) return;

    uint64_t now = SDL_GetTicksNS();
    double dt = (g_last_frame_ns && now >= g_last_frame_ns)
                    ? (double)(now - g_last_frame_ns) / 1e6 : 0.0;
    g_last_frame_ns = now;

    for (int i = 0; i < LAS_NSINK; i++) {
        LasSink *s = &g_sinks[i];
        if (s->stage > LAS_STAGE_MAX) continue;
        if (s->on_frame) s->on_frame(s, frame, dt);
    }

    // 硬上限：防止忘了关采集把沙箱塞满
    if (now_ms() > (double)LAS_MAX_SESSION_MS) {
        // 不在这里直接 stop（会在 present 线程里做文件收尾，太重）。
        // 留一个标记，由下一次 mark/status 或用户点击来处理。
        snprintf(g_end_reason, sizeof g_end_reason, "auto_limit");
    }
}

// ============================================================================
// ★ v14：串行根因计数器（9 个）—— 定义在这里，exec.c / objects.c 用 extern 引用
// ----------------------------------------------------------------------------
// 【为什么定义在 las_collect.c 而不是 exec.c】
//   归属单一：采集系统的所有计数器都由这个文件管，别人只负责 ++。
//   和 v13 的 las_fence_ns 模式略有不同（那个定义在 device.c），
//   但因为这次 9 个全属于"串行诊断"这一个主题，集中放更清晰。
//
// 【为什么用普通 uint64_t 而不是 _Atomic】
//   这些都是「单线程累加、采集器读取」的模式：
//     · vk_wait_seq / renderer_flush / renderer_sync 都只在渲染线程调用
//     · nvnQueueFinish 来自游戏线程，但同一时刻只有一个游戏线程
//   读取时最坏情况读到近似值（差一两次计数），对「判断串行是谁造成的」毫无影响。
//   而 _Atomic 的 ++ 在 ARM64 上约 15ns，普通 ++ 约 1ns；
//   vk_wait_seq 是热路径（每帧几十次），用原子会白白多花几百纳秒。
//
// 【为什么不用 volatile】
//   volatile 会阻止编译器优化，但这里本来就不需要优化掉（是全局写）。
//   加了反而可能让每次 ++ 都强制读内存，得不偿失。
// ============================================================================
uint64_t g_nvn_queue_finish_cnt;
uint64_t g_renderer_flush_cnt;
uint64_t g_renderer_sync_cnt;
uint64_t g_wait_dist_1, g_wait_dist_2, g_wait_dist_3, g_wait_dist_4plus;
uint64_t g_wait_dist_max;
uint64_t g_vk_wait_total_cnt;

// 读走 v14 的 9 个计数器并清零（和 v13 的 las_vkstat_read 同款语义）。
void las_serial_read(uint64_t *finish, uint64_t *flush, uint64_t *sync,
                     uint64_t *d1, uint64_t *d2, uint64_t *d3, uint64_t *d4p,
                     uint64_t *dmax, uint64_t *total) {
    if (finish) *finish = g_nvn_queue_finish_cnt; g_nvn_queue_finish_cnt = 0;
    if (flush)  *flush  = g_renderer_flush_cnt;   g_renderer_flush_cnt = 0;
    if (sync)   *sync   = g_renderer_sync_cnt;    g_renderer_sync_cnt = 0;
    if (d1)     *d1     = g_wait_dist_1;
    if (d2)     *d2     = g_wait_dist_2;
    if (d3)     *d3     = g_wait_dist_3;
    if (d4p)    *d4p    = g_wait_dist_4plus;
    if (dmax)   *dmax   = g_wait_dist_max;
    if (total)  *total  = g_vk_wait_total_cnt;
    // 分桶和总数一起清零（它们是同一组统计，必须同时重置，否则比例会错）
    g_wait_dist_1 = g_wait_dist_2 = g_wait_dist_3 = g_wait_dist_4plus = 0;
    g_wait_dist_max = 0;
    g_vk_wait_total_cnt = 0;
}

// 每 1 秒：exec.c 的 renderer_stats() 第一句调用。
// ★★★ 必须在清零之前调用 ★★★
//   renderer_stats() 里每个计数器都被清零了，而且清零动作藏在
//   snprintf(... atomic_exchange(&stat_compiles, 0), ...) 的参数里
//   （exec.c:799-805）。所以才只能用「函数第一句」这个位置。
void las_collect_on_stats(const LasStats *st, const uint32_t *pass_end_line, int n_pass) {
    if (!g_active) return;
    if (!st) return;

    // t_ms / fps 这类不属于 stats 窗口的字段，在这里补
    LasStats s = *st;          // 复制到栈上，不修改调用方的数据
    s.t_ms = now_ms();

    // fps / max_frame_ms 来自 device.c 的全局（它每 fps_iv 更新一次）
    {
        extern double g_current_fps, g_frame_ms;
        s.fps = g_current_fps;
        s.max_frame_ms = g_frame_ms;
    }

    // ---- v13：交换链 / 并行度 ----
    // 这 8 个字段由 device.c 的计数组件提供（las_vkstat_*，定义在 device.c）。
    // ★ 为什么用 extern 函数而不是全局变量：
    //   1) 有些是"读后清零"语义（每个统计窗口结算一次），函数能保证原子性；
    //   2) 避免在头文件里暴露一堆全局变量，device.c 内部实现可以自由调整。
    // 拿不到时（还没建 swapchain）各函数返回 -1，这里照实写入 CSV。
    {
        extern void las_vkstat_read(int *sw_images, int *sw_present_mode,
                                    double *acq_ms, double *pres_ms, double *fence_ms,
                                    int *in_flight_max, int *in_flight_end, int *frames_submitted,
                                    int *inflight_true_max, int *inflight_true_end,
                                    uint64_t *site_cnt, uint64_t *site_ns, uint64_t *nosub);
        las_vkstat_read(&s.sw_images, &s.sw_present_mode,
                        &s.acq_block_ms, &s.pres_block_ms, &s.fence_wait_ms,
                        &s.in_flight_max, &s.in_flight_end, &s.frames_submitted,
                        &s.inflight_true_max, &s.inflight_true_end,
                        s.wait_site_cnt, s.wait_site_ns, &s.wait_nosub_cnt);
    }

    // ---- v13 D 组：附加计数 ----
    {
        extern void las_vkstat_read_extra(uint64_t *tex_upload_cnt, double *tex_upload_bytes_mb,
                                          uint64_t *tex_view_create_cnt, uint64_t *tex_hash_scan_cnt,
                                          uint64_t *pipe_create_cnt, uint64_t *pipe_bind_cnt,
                                          double *mem_used_mb);
        las_vkstat_read_extra(&s.tex_upload_count, &s.tex_upload_bytes_mb,
                              &s.tex_view_create_cnt, &s.tex_hash_scan_cnt,
                              &s.pipe_create_cnt, &s.pipe_bind_cnt, &s.mem_used_mb);
    }

    // ---- v13 C 组：GPU 总量与气泡 ----
    // gpu_total_ms 直接用本窗口的 gpu_ms（同一个 prof_gpu 来源），不重复读。
    s.gpu_total_ms = s.gpu_ms;
    //
    // GPU 气泡 = 本窗口理论可用时间 - GPU 实际忙时。
    //
    // ★★ v15 修正一个真 bug（perf-optimizer2 用真机数据发现）★★
    //   【原算法】window_ms = frames_submitted × (1000 / fps)
    //   【为什么错】它假设 frames_submitted ≈ fps（每帧一次提交）。
    //     正常窗口确实 ≈1.0，但「启动/加载期批量提交」的窗口会远远超过：
    //     实测 v13 首个窗口 frames_submitted=734、fps=24.7，比值 29.7 倍，
    //     算出 window_ms = 29,717ms（应为 1000ms），**错了 30 倍**。
    //     那一个窗口就贡献了全场 52% 的 bubble 总量。
    //   【实测偏差】source 算法：每窗口 window_ms=1476ms（应 1000，偏差 +48%），
    //     bubble 平均 880ms；改成固定 1000ms 后 bubble 平均 404ms，
    //     且 404 + gpu 596 = 1000 自洽。
    //   【为什么会夸张】那个窗口的 bubble 被算成 29,131ms ——
    //     意味着「GPU 空转了 29 秒」，而整个窗口只有 1 秒，荒谬。
    //   【修法】直接用 1000ms。
    //     因为 PERF 是 1Hz 固定采样（见 _meta.json 的 fps_interval_s），
    //     每个统计窗口的物理时长【就是】1 秒，不需要任何推算。
    //     「用 frames/fps 绕远路」既多算了，又引入了对 frames_submitted 的依赖。
    //
    //   ★ 本修正不影响 gpu_total_ms / gpu_marks_used / pass 分类 —— 只影响 bubble。
    //   ★ 串行判据（cpu_exec_ms + gpu_ms > 1000）也不用 bubble，所以 v14 的核心分析
    //     结论（串行排队）不受这个 bug 影响。
    if (s.fps > 0.1) {
        const double window_ms = 1000.0;   // PERF 1Hz：窗口时长恒为 1 秒
        double bubble = window_ms - s.gpu_total_ms;
        s.gpu_bubble_ms = (bubble > 0.0) ? bubble : 0.0;
    } else {
        s.gpu_bubble_ms = -1.0;   // fps 不可用（第一秒），填 -1 而不是 0
    }
    // gpu_marks_used：当前没有持续开启逐 draw timestamp，所以恒为 -1。
    //   保留这一列是为了将来开启时有个明确的位置，且分析脚本能区分"没开"vs"用了 0 个"。
    s.gpu_marks_used = -1;
    // 6 个 pass 分类恒 -1（游戏不下发 pass 名称，不硬猜 —— 见头文件说明）
    s.gpu_pass_named = -1;

    // ---- v13 D 组：超预算时长 ----
    // frame_budget_over_ms = 所有超过 33.3ms 的帧，超出部分之和。
    //   ★ 为什么要单独算这一项而不只看 fps：
    //     fps 是平均值，看不出"浪费了多少毫秒"。这一项直接回答
    //     "如果把每帧压到 33.3ms 以内，总共能省多少时间" —— 就是优化收益上限。
    //   实现：本窗口 frames 帧，用 (1000/fps) 当作平均帧时间估算，
    //         超出的部分乘以帧数。
    //   ★ 为什么不用 max_frame_ms：那是最大值，乘帧数会严重高估。
    if (s.fps > 0.1) {
        double avg_frame_ms = 1000.0 / s.fps;
        double over = (avg_frame_ms > 33.3333) ? (avg_frame_ms - 33.3333) : 0.0;
        s.frame_budget_over_ms = over * (double)s.frames_submitted;
    } else {
        s.frame_budget_over_ms = -1.0;
    }

    // ---- v14：串行根因（9 项）----
    {
        extern void las_serial_read(uint64_t *finish, uint64_t *flush, uint64_t *sync,
                                    uint64_t *d1, uint64_t *d2, uint64_t *d3, uint64_t *d4p,
                                    uint64_t *dmax, uint64_t *total);
        las_serial_read(&s.nvn_queue_finish_cnt, &s.renderer_flush_cnt, &s.renderer_sync_cnt,
                        &s.wait_dist_1, &s.wait_dist_2, &s.wait_dist_3, &s.wait_dist_4plus,
                        &s.wait_dist_max, &s.vk_wait_total_cnt);
    }

    // ---- v13.1 自检：B 组拆分完整度（★ v14 修正算法）----
    // = cpu_draw_ms - (所有已知子项之和)
    //
    // 【v14 为什么改算法 —— 这是 v13 的一个设计缺陷】
    //   v13 只减了 B 组 4 项（cmd/bind/desc/state），结果真机数据显示
    //   "未归类 84.5%"，看起来像拆分失败。但实测加上 v11 就有的子项后：
    //     B组4项 64.6ms(14%) + cpu_sets_ms 124ms(27%) + cpu_vtx_ms 142ms(31%)
    //     + cpu_key_ms 36ms(7.8%) + cpu_tex_ms 40ms(8.7%) = 414.6ms(90%)
    //   而 cpu_draw_ms 是 460ms —— 对上了。
    //
    //   也就是说那 84.5% 绝大部分是【已有子项】，不是"采不到"。
    //   自检字段必须把所有已知子项都减掉，否则永远显示"拆分不足"，
    //   反而掩盖了真正的问题（有子项没被任何计时覆盖）。
    //
    //   ★ 注意：cpu_sets_ms / cpu_vtx_ms / cpu_key_ms / cpu_dyn_ms / cpu_tex_ms
    //     都是 draw_() 内部的计时，和 cpu_draw_ms 是【包含关系】。
    //     这里减掉它们是正确的，不是重复计算。
    //     （cmd_draw/bind/desc_bind/state 是我 v13 新加的，同样在 draw_() 内部，
    //      但它们和上面 5 项互不重叠 —— 已由 perf-optimizer2 逐行核对确认。）
    s.draw_ms_minus_parts_ms = s.cpu_draw_ms
                             - (s.cmd_draw_ms + s.bind_ms + s.desc_bind_ms + s.state_ms
                                + s.cpu_sets_ms + s.cpu_vtx_ms + s.cpu_key_ms
                                + s.cpu_dyn_ms + s.cpu_tex_ms);

    for (int i = 0; i < LAS_NSINK; i++) {
        LasSink *sink = &g_sinks[i];
        if (sink->stage > LAS_STAGE_MAX) continue;
        if (sink->on_stats) sink->on_stats(sink, &s, pass_end_line, n_pass);
    }
}

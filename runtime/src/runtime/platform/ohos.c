// OHOS 平台层（替代 android.c）。职责：guest 地址空间预留、时间、日志（hilog）、
// 崩溃处理、原生窗口注入（XComponent 侧通过 letsgo_ohos_set_window 提供）。
// 编译：见 build-ohos.sh；OHOS musl 无 glibc 的 __GLIBC_PREREQ（linux_compat 已兜底）。
#include <SDL3/SDL.h>  // shim：时间/日志
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <hilog/log.h>
#include <stdarg.h>
#include <ucontext.h>
#include <execinfo.h>
#include <dlfcn.h>   // dladdr：崩溃地址 → 模块/符号反查
#include <native_window/external_window.h>

#include "platform.h"
#include "cpu.h"     // Cpu：崩溃瞬间的客机寄存器（见 ohos_crash 的「客机寄存器快照」）
#include "rt.h"      // kernel_current_cpu：当前线程正在跑客机代码时的 Cpu

#define LOGTAG "LasOhos"
#define HLOGI(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, LOGTAG, __VA_ARGS__)
#define HLOGF(...) OH_LOG_Print(LOG_APP, LOG_FATAL, 0x0000, LOGTAG, __VA_ARGS__)

// ---- 原生窗口：harmony-launcher 的 napi_init 在 XComponent onLoad 时注入 ----
static OHNativeWindow *g_window;
void letsgo_ohos_set_window(OHNativeWindow *w) { g_window = w; }
OHNativeWindow *letsgo_ohos_get_window(void) { return g_window; }

// ---- guest address space（与 Android/Linux 相同：固定窗口 + MAP_NORESERVE）----
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    HLOGI("reserving guest range %{public}llx-%{public}llx (%{public}llu MB)",
          (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)(hi - lo) >> 20);
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        // 官方 MAP_FIXED_NOREPLACE 语义：EEXIST = 该窗口已预留（此前初始化成功过）→ 复用，不是错误
        if (errno == EEXIST) {
            HLOGI("guest range already reserved (EEXIST), reusing existing mapping");
            return true;
        }
        HLOGF("guest mmap failed: %{public}s (errno=%{public}d)", strerror(errno), errno);
        // 诊断：dump 冲突窗口的映射（谁占着 0x7000000000-0x7320000000）
        FILE *f = fopen("/proc/self/maps", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                unsigned long long a, b;
                if (sscanf(line, "%llx-%llx", &a, &b) == 2 && b > 0x6800000000ull && a < 0x8000000000ull)
                    HLOGI("maps: %{public}s", line);
            }
            fclose(f);
        }
        return false;
    }
    if (p != (void *)(uintptr_t)lo) {
        HLOGF("guest mmap landed at %{public}p, wanted %{public}llx", p, (unsigned long long)lo);
        munmap(p, hi - lo);
        return false;
    }
    return true;
}
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == (void *)(uintptr_t)lo;
}
void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time ----
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void plat_sleep_until_ns(uint64_t t) {
    uint64_t now = plat_now_ns();
    if (t <= now) return;
    struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
void plat_thread_realtime(uint64_t period_ns) { (void)period_ns; }  // OHOS: vsync 由 composer 驱动

// ---- 进程/路径 ----
void plat_init(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HLOGI("plat_init: OHOS runtime up");
}
bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);  // musl: /proc/self/exe 可用（应用沙箱）
    if (n <= 0) { buf[0] = 0; return false; }
    buf[n] = 0;
    return true;
}
void plat_exec(char *const argv[]) {
    (void)argv;
    HLOGF("plat_exec: not supported on OHOS");
}
void plat_terminate(void) { _exit(0); }

// ---- 子进程（用不上：游戏不在 OHOS 版跑外部进程）----
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    (void)argv; (void)log_path; (void)append_log;
    return (PlatProc)0;  // OHOS 版不跑外部进程
}
int plat_proc_wait(PlatProc p, bool block) { (void)p; (void)block; return -1; }
void plat_proc_kill(PlatProc p) { (void)p; }
bool plat_remove_tree(const char *dir) {
    // 简易递归删除（沙箱内使用）
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    return system(cmd) == 0;
}

// ---- 符号/回溯（诊断用，第一版从简）----
const char *plat_symbol(uint64_t pc, uint64_t *offset) { (void)pc; (void)offset; return NULL; }
// 移植层全局 LOG：直接进 hilog（stderr 在 OHOS 应用进程里不可靠）
void plat_log_v(const char *fmt, ...) {
    // hilog 对非 %{public} 的 %s 打 <private>：转发前统一改写
    // 变参先在本函数内 vsnprintf 展开（va_list 跨层转发给 hilog 会失效：0x3000000008 的根因）
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    // 再把 %s 全部转 %{public}s（vsnprintf 已展开，只剩字面文本——直接打，无 % 解析风险）
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasRT", "%{public}s", line);
}

void plat_print_backtrace(void) {
    // 官方 execinfo（musl 提供）：真实栈进 hilog
    void *frames[32];
    int n = backtrace(frames, 32);
    char **syms = backtrace_symbols(frames, n);
    for (int i = 0; i < n; i++)
        HLOGI("bt[%{public}d]: %{public}s", i, syms ? syms[i] : "?");
    free(syms);
}
void plat_fatal_notice(void) {}


// ============================================================================================
// 【崩溃采集：必须以「函数名 + 偏移」的形式报出崩溃点 —— 不要简化掉】
//
// 为什么必须这么写（三点缺一不可）：
//  1) aarch64 的 ucontext 在 musl 里是 struct sigcontext（regs[]/sp/pc）。
//     【历史教训】这里原来只有 `#if defined(__x86_64__)` 分支，鸿蒙真机上 RIP/RSP/RAX/RDI
//     永远保持初始化的 0，日志长这样：
//         signal 11 fault_addr=(nil) RIP=0 RSP=0 RAX=0 RDI=0
//     这不是「崩在空指针」，纯粹是采集代码没写对 —— 于是所有崩溃都变成「无线索」，
//     只能靠猜，浪费了整整几轮排查。aarch64 分支必须存在且用 uc_mcontext.pc/sp/regs[30]。
//  2) dladdr 把 PC 反查成「哪个 .so + 哪个符号 + 偏移」。
//     只有裸地址（bt[0]=0x5ea7a00450）在 ASLR 下毫无意义；有函数名才能直接定位。
//     实测收益：一次就查出 `crash at liblasrecomp.so + av_opt_next +0x20`。
//  3) sigaltstack：栈溢出（递归爆栈）时普通栈已不可用，没有独立信号栈 handler 根本起不来，
//     表现为「进程直接没了，什么日志都没有」，会被误判成 OOM 或系统杀进程。
//
// 【历史教训 —— 不要重犯】
//  - 不要因为「日志里 bt[] 是空的」就把这段删掉/注释掉；空是因为崩溃发生在信号处理时机，
//    保留 sigaltstack + SA_ONSTACK 才能提高成功率。
//  - 不要只捕获 SIGSEGV/SIGABRT。OHOS 的 native 崩溃很多走 SIGBUS（对齐非法）、
//    SIGILL（执行了非法指令，常见于 JIT/混淆代码）、SIGFPE（除零），漏了就等于没采。
//  - 崩溃栈同时写 /data/storage/el2/base/files/crash_stack.txt，但该路径在应用沙箱内，
//    hdc shell 无权限读取，只能靠 hilog（LasOhos 标签）。所以 hilog 那两行 HLOGF/HLOGI
//    是唯一可用的出口，务必保留。
// ============================================================================================
static void ohos_crash(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    void *pc = NULL, *sp = NULL, *lr = NULL;
#if defined(__aarch64__)
    if (uc) {
        // musl aarch64: ucontext_t.uc_mcontext 是 struct sigcontext
        pc = (void *)(uintptr_t)uc->uc_mcontext.pc;
        sp = (void *)(uintptr_t)uc->uc_mcontext.sp;
        lr = (void *)(uintptr_t)uc->uc_mcontext.regs[30];
    }
#elif defined(__x86_64__)
    if (uc) {
        pc = (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        sp = (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
        lr = (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    }
#endif
    // PC 落在哪个模块/符号：这是定位崩溃函数的唯一可靠手段
    Dl_info di = {0};
    const char *mod = "?", *sym = "?";
    unsigned long off = 0;
    if (pc && dladdr(pc, &di)) {
        if (di.dli_fname) mod = di.dli_fname;
        if (di.dli_sname) sym = di.dli_sname;
        if (di.dli_saddr) off = (unsigned long)((const char *)pc - (const char *)di.dli_saddr);
    }
    HLOGF("signal %{public}d fault_addr=%{public}p PC=%{public}p SP=%{public}p LR=%{public}p",
          sig, info ? info->si_addr : NULL, pc, sp, lr);
    HLOGF("crash at %{public}s + %{public}s +0x%{public}lx  (module offset 0x%{public}lx)",
          mod, sym, off, di.dli_fbase ? (unsigned long)((const char *)pc - (const char *)di.dli_fbase) : 0UL);
    // 【客机寄存器快照】前面的 PC/SP/LR 是���主 aarch64」的寄存器：重编译后 guest 代码就是宿主指令，
    // 所以它们是主机地址，能定位到 f_<客机地址>。但排查「客机侧的野指针」还需要客机寄存器本身 ——
    // 例如 f_700014dc10 崩溃时真正的问题是 c->x[0]（客机 x0）是什么。这里把当时线程的 Cpu 打出来。
    // 局限：只有崩溃恰好发生在该线程的客机代码里时 t_cur 才有效；渲染线程/工作线程上 t_cur 为 NULL（只打一行说明）。
    {
        Cpu *cc = kernel_current_cpu();
        if (cc) {
            HLOGF("guest cpu: x0=%{public}llx x1=%{public}llx x2=%{public}llx x3=%{public}llx sp=%{public}llx pc=%{public}llx lr=%{public}llx",
                  (unsigned long long)cc->x[0], (unsigned long long)cc->x[1], (unsigned long long)cc->x[2],
                  (unsigned long long)cc->x[3], (unsigned long long)cc->sp, (unsigned long long)cc->pc,
                  (unsigned long long)cc->x[30]);
            for (int i = 4; i < 31; i += 4)
                HLOGF("guest cpu: x%{public}d=%{public}llx x%{public}d=%{public}llx x%{public}d=%{public}llx x%{public}d=%{public}llx",
                      i, (unsigned long long)cc->x[i], i + 1, (unsigned long long)cc->x[i + 1],
                      i + 2, (unsigned long long)cc->x[i + 2], i + 3, (unsigned long long)cc->x[i + 3]);
            // 崩溃点若是一条 ldr x8,[x8,#88]（f_700014dc10+0xa4 就是），那么出错的基址是「fault_addr - 88」，
            // 把它和 x0..x3 并排打出来，能立刻看出是哪个寄存器的值被当成了指针。
            if (info && info->si_addr)
                HLOGF("guest cpu: fault_addr-88=%{public}llx  (ldr 基址；未对齐=野指针)",
                      (unsigned long long)((uintptr_t)info->si_addr - 88));
        } else {
            HLOGI("guest cpu: (no Cpu on this thread: crash outside guest code, e.g. render/worker thread)");
        }
    }
    // 栈回溯 + 逐帧 dladdr（每帧都给出模块/符号）
    void *bt[48];
    int n = backtrace(bt, 48);
    for (int i = 0; i < n && i < 24; i++) {
        Dl_info f = {0};
        if (dladdr(bt[i], &f) && f.dli_fname) {
            HLOGI("bt[%{public}d] %{public}s + %{public}s +0x%{public}lx (%{public}p)",
                  i, f.dli_fname, f.dli_sname ? f.dli_sname : "?",
                  f.dli_saddr ? (unsigned long)((const char *)bt[i] - (const char *)f.dli_saddr) : 0UL, bt[i]);
        } else {
            HLOGI("bt[%{public}d] %{public}p (no dladdr)", i, bt[i]);
        }
    }
    // 持久化到沙箱文件（hilog 会滚走；文件可 hdc file recv 拉取）
    {
        FILE *cf = fopen("/data/storage/el2/base/files/crash_stack.txt", "w");
        if (cf) {
            fprintf(cf, "signal %d fault_addr=%p PC=%p SP=%p LR=%p\n", sig,
                    info ? info->si_addr : NULL, pc, sp, lr);
            fprintf(cf, "primary: %s + %s +0x%lx\n", mod, sym, off);
            for (int i = 0; i < n; i++) {
                Dl_info f = {0};
                if (dladdr(bt[i], &f) && f.dli_fname)
                    fprintf(cf, "bt[%d] %s + %s +0x%lx (%p)\n", i, f.dli_fname,
                            f.dli_sname ? f.dli_sname : "?",
                            f.dli_saddr ? (unsigned long)((const char *)bt[i] - (const char *)f.dli_saddr) : 0UL, bt[i]);
                else
                    fprintf(cf, "bt[%d] %p\n", i, bt[i]);
            }
            fclose(cf);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
void plat_crash_handler_install(void) {
    // 独立信号栈：栈溢出时 handler 才有栈可用
    static char alt_stack[SIGSTKSZ * 8];
    stack_t ss = {0};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof alt_stack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa = {0};
    sa.sa_sigaction = ohos_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

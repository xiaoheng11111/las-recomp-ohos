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
#include <native_window/external_window.h>

#include "platform.h"

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


// ---- crash handler：SIGSEGV/SIGABRT 记 hilog 后重抛 ----
static void ohos_crash(int sig, siginfo_t *info, void *ctx) {
    // 原始崩点：ucontext 里的 rip/rsp/rax——musl dlopen 内部 SEGV 的唯一线索
    ucontext_t *uc = (ucontext_t *)ctx;
    unsigned long long rip = 0, rsp = 0, rax = 0, rdi = 0;
#if defined(__x86_64__)
    if (uc) {
        rip = (unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
        rsp = (unsigned long long)uc->uc_mcontext.gregs[REG_RSP];
        rax = (unsigned long long)uc->uc_mcontext.gregs[REG_RAX];
        rdi = (unsigned long long)uc->uc_mcontext.gregs[REG_RDI];
    }
#endif
    HLOGF("signal %{public}d fault_addr=%{public}p RIP=%{public}llx RSP=%{public}llx RAX=%{public}llx RDI=%{public}llx",
          sig, info->si_addr, rip, rsp, rax, rdi);
    // rip 落在哪个库：dladdr 不一定可用，先看地址段
    void *bt[32];
    int n = backtrace(bt, 32);
    for (int i = 0; i < n && i < 12; i++)
        HLOGI("bt[%{public}d] %{public}p", i, bt[i]);
    // 栈持久化到沙箱文件（hilog 会滚走；文件可 hdc file recv 拉取）
    {
        FILE *cf = fopen("/data/storage/el2/base/files/crash_stack.txt", "w");
        if (cf) {
            fprintf(cf, "signal %d fault_addr=%p\n", sig, info->si_addr);
            for (int i = 0; i < n; i++) fprintf(cf, "bt[%d] %p\n", i, bt[i]);
            char **syms = backtrace_symbols(bt, n);
            if (syms) { for (int i = 0; i < n; i++) fprintf(cf, "sym[%d] %s\n", i, syms[i]); free(syms); }
            fclose(cf);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
void plat_crash_handler_install(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = ohos_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

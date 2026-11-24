#include "game_entry.h"
#include "render.h"

#include <SDL3/SDL.h>  // shim（与移植层同一份，保证 SDL_main 的 SDL 符号在同一进程一致）
#include <hilog/log.h>
#include <pthread.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <initializer_list>
#include <malloc.h>
#include <zlib.h>
#include <rawfile/raw_file_manager.h>
#include <rawfile/raw_file.h>
#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>

static void *g_h;

static OHNativeWindow *g_win;
static char g_data_root[512];
static char g_files_dir[512];      // UIAbilityContext.filesDir（haps/entry/files）
static char g_app_files_dir[512];  // ApplicationContext.filesDir（应用级 files）
static char g_root_dir[600];       // 数据根（含 extracted/），移植层 LARECOMP_DATA_ROOT
static char g_dest_dir[600];       // 解压产物目录（应用级 files/lasrecomp/extracted）
static char g_dest_file[700];      // romfs.bin 完整路径
static char g_prog_path[700];      // 进度文件路径（ArkTS 轮询同一个路径）
static char g_game_lib[512];

static constexpr long kRomfsSize = 4430284852L;  // romfs.bin 解压后固定大小

// 游戏库是按 bionic/glibc 风格链接的，引用了 OHOS libc 不导出的 __register_atfork。
// 在本库里补一个同名实现并导出，游戏库加载时就能解析到（pthread_atfork 是等价语义）。
extern "C" __attribute__((visibility("default"))) int __register_atfork(
    void (*prepare)(void), void (*parent)(void), void (*child)(void), void *dso_handle) {
    int r = pthread_atfork(prepare, parent, child);
    (void)dso_handle;
    return r;
}

// 移植层按数据根下的 lasgame.so / lasgame.so.id 判断「游戏代码已构建」，
// 但 HAP 里的库只能叫 liblasgame.so；我们改为把 LARECOMP_GAME_LIB 直接指向
// bundle libs 里的真实文件（沙箱里的可写副本会被链接器拒绝加载）。
static const char *kBundledGameLib = "/data/storage/el1/bundle/libs/arm64/liblasgame.so";

void game_entry_set_window(OHNativeWindow *w) { g_win = w; }

static int g_xc_w = 0;
static int g_xc_h = 0;
static void push_window_size() {
    typedef void (*SetSizeFn)(int, int);
    SetSizeFn fn = (SetSizeFn)dlsym(RTLD_DEFAULT, "letsgo_ohos_set_window_size");
    if (fn != nullptr && g_xc_w > 0 && g_xc_h > 0) fn(g_xc_w, g_xc_h);
}


// 官方 OH_NativeXComponent 回调：OnSurfaceCreated 给 OHNativeWindow*
// 同一个 XComponent 只能注册一份回调（RegisterCallback 是整体覆盖），
// 所以回调只在这里注册一次，再转发给渲染层，避免互相覆盖导致渲染线程永远收不到窗口。
static void game_on_surface_created(OH_NativeXComponent *comp, void *window) {
    g_win = (OHNativeWindow *)window;
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(comp, window, &w, &h);
    g_xc_w = (int)w; g_xc_h = (int)h; push_window_size();
    RenderOnSurfaceCreated(comp, window);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "GameEntry",
                 "surface created window=%{public}p w=%{public}d h=%{public}d", window, (int)w, (int)h);
}
static void game_on_surface_changed(OH_NativeXComponent *comp, void *window) {
    g_win = (OHNativeWindow *)window;
    uint64_t w = 0, h = 0;
    OH_NativeXComponent_GetXComponentSize(comp, window, &w, &h);
    g_xc_w = (int)w; g_xc_h = (int)h; push_window_size();
    RenderOnSurfaceChanged(comp, window);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "GameEntry",
                 "surface changed w=%{public}d h=%{public}d", (int)w, (int)h);
}
static void game_on_surface_destroyed(OH_NativeXComponent *comp, void *window) {
    RenderOnSurfaceDestroyed(comp, window);
    g_win = nullptr;
}
void game_entry_attach_xcomp(OH_NativeXComponent *comp) {
    static OH_NativeXComponent_Callback cb;
    cb.OnSurfaceCreated = game_on_surface_created;
    cb.OnSurfaceChanged = game_on_surface_changed;
    cb.OnSurfaceDestroyed = game_on_surface_destroyed;
    OH_NativeXComponent_RegisterCallback(comp, &cb);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "GameEntry", "game surface callback registered");
}

// SDL_main 的声明（-Dmain=SDL_main 由移植层头处理；这里 extern C 取导出符号）
extern "C" void letsgo_ohos_set_window(OHNativeWindow *w);

// stderr → hilog（官方方案：pipe + pump 线程，FATAL/assert 文字不再丢失）
static int g_err_pipe[2] = {-1, -1};
static void *stderr_pump_thread(void *) {
    char buf[512];
    ssize_t n;
    while ((n = read(g_err_pipe[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0000, "LasStderr", "%{public}s", buf);
    }
    return nullptr;
}
static void setup_stderr_to_hilog(void) {
    if (pipe(g_err_pipe) != 0) return;
    dup2(g_err_pipe[1], 2);
    dup2(g_err_pipe[1], 1);  // 移植层的常规日志走 stdout，一起收进 hilog
    setvbuf(stderr, NULL, _IOLBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_t t;
    if (pthread_create(&t, nullptr, stderr_pump_thread, nullptr) != 0) return;
    pthread_detach(t);
}

static void write_progress(long pct) {
    if (!g_prog_path[0]) return;
    FILE *pf = fopen(g_prog_path, "w");
    if (!pf) return;
    fprintf(pf, "%ld", pct);
    fclose(pf);
}

// 删除目录树（清掉旧的 SPIR-V 缓存，避免换编译器后复用坏缓存）
static void wipe_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char p[900];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) wipe_tree(p);
        else unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static bool romfs_complete() {
    if (!g_dest_file[0]) return false;
    struct stat st;
    return stat(g_dest_file, &st) == 0 && st.st_size == kRomfsSize;
}

// 在 dir 下找 romfs.gz：既支持「dir/romfs.gz 是文件」，也支持「dir/romfs.gz 是文件夹
// （解压/文件管理器放出来的一层目录），里面再找 romfs.gz 或该目录下最大的普通文件」。
static FILE *open_romfs_in_dir(const char *dir, char *out_path, size_t out_n) {
    if (!dir || !dir[0]) return nullptr;
    struct stat st;
    snprintf(out_path, out_n, "%s/romfs.gz", dir);
    if (stat(out_path, &st) == 0) {
        if (S_ISREG(st.st_mode)) return fopen(out_path, "rb");
        if (S_ISDIR(st.st_mode)) {
            char inner[900];
            snprintf(inner, sizeof inner, "%s/romfs.gz", out_path);
            struct stat si;
            if (stat(inner, &si) == 0 && S_ISREG(si.st_mode)) {
                snprintf(out_path, out_n, "%s", inner);
                return fopen(inner, "rb");
            }
            DIR *d = opendir(out_path);
            if (d) {
                struct dirent *e;
                long best = 0;
                char bestp[900] = {0};
                while ((e = readdir(d)) != nullptr) {
                    if (e->d_name[0] == '.') continue;
                    char fp[1000];
                    snprintf(fp, sizeof fp, "%s/%s", out_path, e->d_name);
                    struct stat sf;
                    if (stat(fp, &sf) == 0 && S_ISREG(sf.st_mode) && sf.st_size > best) {
                        best = sf.st_size;
                        snprintf(bestp, sizeof bestp, "%s", fp);
                    }
                }
                closedir(d);
                if (best > 0) {
                    snprintf(out_path, out_n, "%s", bestp);
                    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasBridge",
                                 "romfs.gz 在目录里找到: %{public}s (%{public}ld bytes)", bestp, best);
                    return fopen(bestp, "rb");
                }
            }
        }
    }
    return nullptr;
}

// romfs.gz 源：应用级 files > UIAbility files（用户导入） > HAP rawfile
static FILE *open_romfs_source(NativeResourceManager *rm, RawFile **outRf) {
    *outRf = nullptr;
    char p[1000];
    FILE *f = nullptr;
    if (g_app_files_dir[0]) f = open_romfs_in_dir(g_app_files_dir, p, sizeof p);
    if (!f && g_files_dir[0]) f = open_romfs_in_dir(g_files_dir, p, sizeof p);
    if (!f && rm) *outRf = OH_ResourceManager_OpenRawFile(rm, "data/romfs.gz");
    return f;
}

// 幂等解压：完整就跳过；gzip 尾部 ISIZE 与目标大小（按 2^32 取模）一致才动手。
static bool ensure_romfs_extracted(NativeResourceManager *rm) {
    if (g_root_dir[0]) {
        // zip 导入（含覆盖导入）：<root>/extracted/romfs.bin -> <root>/romfs.bin。
        // 同盘 rename 原子替换旧文件，秒级完成，不重拷 4.4GB。旧布局放在 extracted/ 也是同一逻辑。
        char legacy[700];
        snprintf(legacy, sizeof legacy, "%s/extracted/romfs.bin", g_root_dir);
        struct stat ls;
        if (stat(legacy, &ls) == 0 && ls.st_size == kRomfsSize) {
            bool had = romfs_complete();
            if (rename(legacy, g_dest_file) == 0) {
                OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasBridge",
                             "romfs.bin 已从 extracted/ %{public}s到数据根", had ? "覆盖" : "迁移");
            } else {
                OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0000, "LasBridge",
                             "romfs.bin 迁移/覆盖失败: %{public}s", g_dest_file);
            }
        }
    }
    if (g_root_dir[0]) {
        // zip 导入（BUILD.md Etapa B 结构）解压出 <root>/extracted/exefs/：迁移到 <root>/exefs/
        char src_exefs[700], dst_exefs[700];
        snprintf(src_exefs, sizeof src_exefs, "%s/extracted/exefs", g_root_dir);
        snprintf(dst_exefs, sizeof dst_exefs, "%s/exefs", g_root_dir);
        struct stat es;
        if (stat(src_exefs, &es) == 0 && S_ISDIR(es.st_mode)) {
            mkdir(dst_exefs, 0777);
            const char *names[] = {"main", "main.npdm", "rtld", "sdk", "subsdk0", "subsdk1"};
            for (const char *nm : names) {
                char sp[700], dp[700];
                snprintf(sp, sizeof sp, "%s/%s", src_exefs, nm);
                snprintf(dp, sizeof dp, "%s/%s", dst_exefs, nm);
                struct stat fs2;
                if (stat(sp, &fs2) == 0) {
                    if (rename(sp, dp) == 0) {
                        OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasBridge", "exefs/%{public}s 已迁移", nm);
                    }
                }
            }
        }
    }
    if (romfs_complete()) {
        write_progress(100);
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasBridge", "romfs: skip（已完整）");
        return true;
    }
    RawFile *rf = nullptr;
    FILE *srcf = open_romfs_source(rm, &rf);
    if (!srcf && !rf) {
        write_progress(-1);
        OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0000, "LasBridge", "romfs.gz: not found");
        return false;
    }
    if (srcf) {  // gzip 末 4 字节 = 原始大小 mod 2^32，不能直接和 4.13GB 比
        uint32_t isz = 0;
        bool ok = false;
        if (fseek(srcf, -4L, SEEK_END) == 0 && fread(&isz, 1, 4, srcf) == 4) {
            ok = (isz == (uint32_t)kRomfsSize);
        }
        fseek(srcf, 0, SEEK_SET);
        if (!ok) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0000, "LasBridge",
                         "gz precheck: ISIZE=%{public}u expect(mod 2^32)=%{public}u -> BAD", isz,
                         (uint32_t)kRomfsSize);
            write_progress(-1);
            fclose(srcf);
            return false;
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasBridge", "gz precheck: ISIZE OK");
    }
    if (g_dest_dir[0]) {
        mkdir(g_dest_dir, 0777);
    }
    FILE *out = fopen(g_dest_file, "wb");
    if (!out) {
        write_progress(-1);
        if (srcf) fclose(srcf);
        if (rf) OH_ResourceManager_CloseRawFile(rf);
        OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0000, "LasBridge", "无法创建 %{public}s", g_dest_file);
        return false;
    }
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    inflateInit2(&zs, 15 + 16);  // gzip
    char *inbuf = (char *)malloc(4 << 20);
    char *outbuf = (char *)malloc(4 << 20);
    long done = 0;
    long last_pct = -1;
    bool ended = false;
    for (;;) {
        int n2 = srcf ? (int)fread(inbuf, 1, 4 << 20, srcf)
                      : OH_ResourceManager_ReadRawFile(rf, inbuf, 4 << 20);
        if (n2 <= 0) break;
        zs.next_in = (Bytef *)inbuf;
        zs.avail_in = n2;
        while (zs.avail_in > 0) {
            zs.next_out = (Bytef *)outbuf;
            zs.avail_out = 4 << 20;
            int rc = inflate(&zs, Z_NO_FLUSH);
            int produced = (4 << 20) - (int)zs.avail_out;
            if (produced > 0) {
                fwrite(outbuf, 1, produced, out);
                done += produced;
                long pct = done * 100 / kRomfsSize;
                if (pct != last_pct) {
                    last_pct = pct;
                    write_progress(pct);
                }
            }
            if (rc == Z_STREAM_END) { ended = true; break; }
            if (rc != Z_OK && rc != Z_BUF_ERROR) {
                OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0000, "LasBridge",
                             "inflate error rc=%{public}d done=%{public}ld", rc, done);
                break;
            }
        }
        if (ended) break;
    }
    inflateEnd(&zs);
    free(inbuf);
    free(outbuf);
    fclose(out);
    if (srcf) fclose(srcf);
    if (rf) OH_ResourceManager_CloseRawFile(rf);
    if (romfs_complete()) {
        write_progress(100);
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasBridge", "romfs 解压完成: %{public}ld", done);
        return true;
    }
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0000, "LasBridge",
                 "romfs 解压不完整: %{public}ld / %{public}ld", done, kRomfsSize);
    write_progress(-1);
    return false;
}

static void *game_thread(void *) {
    constexpr unsigned int kDomain = 0x0000;
    setup_stderr_to_hilog();  // 移植层的 fprintf(stderr)/abort 文字统一进 hilog
    extern void *g_rawManager;
    NativeResourceManager *rm = (NativeResourceManager *)g_rawManager;
    if (g_dest_dir[0]) {
        mkdir(g_dest_dir, 0777);
    }
    // 沙箱里的可写 so 会被链接器拒绝（Invalid argument），清掉历史副本，避免它在正确路径之前被尝试
    if (g_root_dir[0]) {
        char stale[700];
        snprintf(stale, sizeof stale, "%s/lasgame.so", g_root_dir);
        struct stat sst;
        if (lstat(stale, &sst) == 0) unlink(stale);
    }
    // 数据根下的 lasgame.so.id：移植层用它判断游戏代码版本
    if (g_root_dir[0]) {
        RawFile *rf0 = rm ? OH_ResourceManager_OpenRawFile(rm, "data/lasgame.so.id") : nullptr;
        if (rf0) {
            char *b0 = (char *)malloc(64);
            int n0 = OH_ResourceManager_ReadRawFile(rf0, b0, 63);
            if (n0 > 0) {
                char idp[700];
                snprintf(idp, sizeof idp, "%s/lasgame.so.id", g_root_dir);
                FILE *f0 = fopen(idp, "wb");
                if (f0) { fwrite(b0, 1, n0, f0); fclose(f0); }
            }
            free(b0);
            OH_ResourceManager_CloseRawFile(rf0);
        }
    }
    const char *files[] = {"data/exefs/main", "data/exefs/main.npdm", "data/exefs/rtld",
                           "data/exefs/sdk", "data/exefs/subsdk0", "data/exefs/subsdk1"};
    for (const char *rel : files) {
        RawFile *rf = rm ? OH_ResourceManager_OpenRawFile(rm, rel) : nullptr;
        if (!rf) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, 0x0000, "LasBridge", "open %{public}s FAIL", rel);
            continue;
        }
        long sz = OH_ResourceManager_GetRawFileSize(rf);
        char dp[700];
        if (strstr(rel, "exefs/")) {
            // 移植层读 <data root>/exefs，不是 <data root>/extracted/exefs
            char dsub[700];
            snprintf(dsub, sizeof dsub, "%s/exefs", g_root_dir);
            mkdir(dsub, 0777);
            snprintf(dp, sizeof dp, "%s/exefs/%s", g_root_dir, strrchr(rel, '/') + 1);
        } else {
            snprintf(dp, sizeof dp, "%s/%s", g_root_dir, strrchr(rel, '/') + 1);
        }
        FILE *b = fopen(dp, "wb");
        if (!b) { OH_ResourceManager_CloseRawFile(rf); continue; }
        char *buf = (char *)malloc(1 << 20);
        long done = 0;
        for (;;) {
            int n2 = OH_ResourceManager_ReadRawFile(rf, buf, 1 << 20);
            if (n2 <= 0) break;
            fwrite(buf, 1, n2, b);
            done += n2;
        }
        free(buf);
        fclose(b);
        OH_ResourceManager_CloseRawFile(rf);
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasBridge", "%{public}s: %{public}ld/%{public}ld",
                     rel, done, sz);
    }
    // romfs.bin（4.13GB）：完整就跳过，否则从沙箱 romfs.gz 流式解压（HAP 4GB 硬限）
    ensure_romfs_extracted(rm);

    // 移植层的 data root 是「含 extracted/ 的那一级」，不是 extracted 目录本身
    snprintf(g_data_root, sizeof g_data_root, "%s", g_root_dir);
    // 游戏库必须从 HAP 的 libs 目录加载：沙箱里的可写文件会被链接器拒绝（ENOENT）
    snprintf(g_game_lib, sizeof g_game_lib, "%s", kBundledGameLib);
    // 运行时加载移植层（DT_NEEDED 方式失败时错误被 ACE 吞掉；dlopen 的 dlerror 可见）
    dlerror();
    // 文件名与 SONAME 都是 liblasrecomp.so：游戏库 DT_NEEDED 的裸名必须能被解析
    g_h = dlopen("liblasrecomp.so", RTLD_NOW | RTLD_GLOBAL);
    const char *e = dlerror();
    if (!g_h) {
        OH_LOG_Print(LOG_APP, LOG_FATAL, kDomain, "GameEntry", "dlopen lasrecomp: %{public}s", e ? e : "?");
        return nullptr;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, "GameEntry", "lasrecomp loaded, starting SDL_main");
    push_window_size();

    // 移植层的目录约定
    setenv("LARECOMP_DATA_ROOT", g_data_root, 1);
    // 把工作目录设到数据根：相对路径（build/、cache/、截图）都落在这里，便于取用与诊断
    if (g_data_root[0]) { if (chdir(g_data_root) != 0) OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, "GameEntry", "chdir data root failed"); }
    setenv("LARECOMP_GAME_LIB", g_game_lib, 1);
    setenv("LARECOMP_NO_LAUNCHER", "1", 1);  // 第一版无 ImGui 启动器 UI：直接进游戏
    // 游戏字体引擎要 TrueType 轮廓；且必须含中文字形（原 plat 候选表只有拉丁字库）。
    // HarmonyOS_Sans.ttf 只有 342KB 纯拉丁；HarmonyOS_Sans_SC.ttf 20.6MB 才含简体中文/全角标点。
    setenv("LARECOMP_FONT", "/system/fonts/HarmonyOS_Sans_SC.ttf", 1);
    // 只在「渲染器/翻译器版本」变化时清一次 SPIR-V 缓存；原来每次启动都清，导致每次都重编全部 shader（严重卡顿）
    if (g_root_dir[0]) {
        static const char kSpvVer[] = "bcdec7-ff-1";
        char spv[700], ver[700];
        snprintf(spv, sizeof spv, "%s/cache/spirv", g_root_dir);
        snprintf(ver, sizeof ver, "%s/cache/.spvver", g_root_dir);
        char have[64] = "";
        FILE *vf = fopen(ver, "r");
        if (vf) { if (fgets(have, sizeof have, vf) == NULL) have[0] = 0; fclose(vf); }
        if (strncmp(have, kSpvVer, strlen(kSpvVer)) != 0) {
            wipe_tree(spv);
            char cd[700];
            snprintf(cd, sizeof cd, "%s/cache", g_root_dir);
            mkdir(cd, 0777);
            FILE *wv = fopen(ver, "w");
            if (wv) { fprintf(wv, "%s\n", kSpvVer); fclose(wv); }
        }
        // 先关掉 SMAA（Maleoon 上该后处理管线创建失败）；设置文件不存在时写一份最小配置
        char ini[700];
        snprintf(ini, sizeof ini, "%s/larecomp.ini", g_root_dir);
        struct stat ist;
        if (stat(ini, &ist) != 0) {
            FILE *f = fopen(ini, "w");
            if (f) {
                fprintf(f, "# generated by harmony-launcher\nsmaa = 0\nupscaler = bilinear\nvertex_read = copy\ngame_lang = zh-CN\n");
                fclose(f);
            }
        }
        setenv("LARECOMP_SETTINGS", ini, 1);
        // 持续 prof 日志每 2 秒生成大统计串，拖慢主线程；诊断已完成，改由 ArkTS 右上角显示帧率
        setenv("LARECOMP_FPS_INTERVAL", "0.5", 1);
        // 动画分辨率 / 音频缓冲由 larecomp.ini 的 movie_max / audout_ms 决定（apply_display_ini_env）
        setenv("LARECOMP_MOVIE_THREADS", "2", 1);  // 视频解码限 2 线程，给音频混音留 CPU（硬解时无效，软解回退时仍生效）
        // （动态 UBO 已验证不是根因：关闭后仍蓝屏，保留默认开启即可）
        {
            char bd[700];
            snprintf(bd, sizeof bd, "%s/build", g_root_dir);
            mkdir(bd, 0777);
            setenv("LARECOMP_HIDE_FIRST_FRAMES", "0", 1);
            // （IDENTITY 实验已排除；恢复默认 preTransform/旋转）
        }
    }

    // 把同一个 XComponent 窗口从演示三角形渲染线程交还给游戏：先停帧并销毁演示 swapchain，
    // 否则两个 Vulkan 消费者抢同一块 surface，画面会黑/花屏。
    RenderHandOff();

    // 注意：这里不能提前 dlopen 游戏库。移植层 plat_init 要在 0x7000000000 预留 12.8GB guest 区，
    // 提前加载 139MB 的 lasgame.so 会占用这段地址，导致预留失败、随后 memcpy 到 guest 区时 SIGSEGV。

    // 平台层从 XComponent 拿窗口（vkCreateSurfaceOHOS 用）
    void (*set_win)(OHNativeWindow *) = (void (*)(OHNativeWindow *))dlsym(g_h, "letsgo_ohos_set_window");
    if (set_win) set_win(g_win);
    else OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, "GameEntry", "letsgo_ohos_set_window not found");

    int (*main_fn)(int, char **) = (int (*)(int, char **))dlsym(g_h, "SDL_main");
    if (!main_fn) main_fn = (int (*)(int, char **))dlsym(g_h, "main");
    if (!main_fn) {
        OH_LOG_Print(LOG_APP, LOG_FATAL, kDomain, "GameEntry", "SDL_main/main not found: %{public}s", dlerror());
        return nullptr;
    }

    char *argv[] = {(char *)"lasrecomp", g_data_root, (char *)"--play", nullptr};
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasRT", "argv: argc=%{public}d argv1=%{public}s len=%{public}d",
                 3, argv[1], (int)strlen(argv[1]));
    main_fn(3, argv);
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, "GameEntry", "SDL_main returned");
    return nullptr;
}

void game_entry_prepare(void);

static void *prepare_thread(void *) {
    // 只做数据桥 + romfs 解压，不跑 SDL_main
    extern void *g_rawManager;
    NativeResourceManager *rm = (NativeResourceManager *)g_rawManager;
    if (g_dest_dir[0]) {
        mkdir(g_dest_dir, 0777);
    }
    ensure_romfs_extracted(rm);
    return nullptr;
}

// 目录由 ArkTS 传入（Context.filesDir / ApplicationContext.filesDir），不再手拼沙箱路径
void game_entry_set_dirs(const char *haps_files_dir, const char *app_files_dir) {
    if (haps_files_dir && haps_files_dir[0]) {
        snprintf(g_files_dir, sizeof g_files_dir, "%s", haps_files_dir);
    }
    if (app_files_dir && app_files_dir[0]) {
        snprintf(g_app_files_dir, sizeof g_app_files_dir, "%s", app_files_dir);
    } else {
        snprintf(g_app_files_dir, sizeof g_app_files_dir, "%s", g_files_dir);
    }
    if (g_app_files_dir[0]) {
        snprintf(g_root_dir, sizeof g_root_dir, "%s/lasrecomp", g_app_files_dir);
        // 移植层要的是 <数据根>/romfs.bin（和 <数据根>/exefs 同级）
        snprintf(g_dest_dir, sizeof g_dest_dir, "%s", g_root_dir);
        snprintf(g_dest_file, sizeof g_dest_file, "%s/romfs.bin", g_dest_dir);
        char ad[600];
        snprintf(ad, sizeof ad, "%s", g_root_dir);
        mkdir(ad, 0777);
    }
    if (g_files_dir[0]) {
        snprintf(g_prog_path, sizeof g_prog_path, "%s/lasrecomp/.progress", g_files_dir);
        char pd[600];
        snprintf(pd, sizeof pd, "%s/lasrecomp", g_files_dir);
        mkdir(pd, 0777);
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasBridge",
                 "dirs: haps=%{public}s app=%{public}s dest=%{public}s", g_files_dir,
                 g_app_files_dir, g_dest_dir);
}

void game_entry_prepare_dir(const char *haps_files_dir, const char *app_files_dir) {
    game_entry_set_dirs(haps_files_dir, app_files_dir);
    game_entry_prepare();
}

void game_entry_prepare(void) {
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8 << 20);
    pthread_create(&t, &at, prepare_thread, nullptr);
    pthread_attr_destroy(&at);
    pthread_detach(t);
}

void *game_entry_set_rm(napi_env env, napi_value obj) {
    extern void *g_rawManager;
    g_rawManager = OH_ResourceManager_InitNativeResourceManager(env, obj);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasBridge", "rm=%{public}p", g_rawManager);
    return g_rawManager;
}

// 显示设置：larecomp.ini 的 aspect / aspect_fill / movie_max / audout_ms
// -> LARECOMP_WIDE_TEST / LARECOMP_MOVIE_MAX / LARECOMP_AUDOUT_MS，都在游戏线程启动前设置。
static void apply_display_ini_env(void) {
    if (!g_root_dir[0]) return;
    char ini[700];
    snprintf(ini, sizeof ini, "%s/larecomp.ini", g_root_dir);
    FILE *f = fopen(ini, "r");
    if (!f) return;
    char aspect[32] = "game";
    double fill = 0.0;
    int movie_max = 0, audout = 200, vk_prof = 0;
    int collect_on = 0, collect_delay = 10;
    int tex_nocheck = 0;
    int astc_rt = 0;   // 默认关（BC 软解）
    bool has_lang = false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[128];
        if (sscanf(line, " %63[^= ] = %127[^\n]", key, val) != 2) continue;
        for (size_t n = strlen(val); n && (val[n - 1] == '\r' || val[n - 1] == '\n' || val[n - 1] == ' '); n--)
            val[n - 1] = 0;
        if (!strcmp(key, "aspect")) snprintf(aspect, sizeof aspect, "%s", val);
        else if (!strcmp(key, "aspect_fill")) fill = atof(val);
        else if (!strcmp(key, "movie_max")) movie_max = atoi(val);
        else if (!strcmp(key, "audout_ms")) audout = atoi(val);
        else if (!strcmp(key, "vk_prof")) vk_prof = atoi(val);
        else if (!strcmp(key, "collect")) collect_on = atoi(val);
        else if (!strcmp(key, "tex_nocheck")) tex_nocheck = atoi(val);
        else if (!strcmp(key, "astc_rt")) astc_rt = atoi(val);
        else if (!strcmp(key, "collect_delay")) collect_delay = atoi(val);
        else if (!strcmp(key, "game_lang")) has_lang = true;
    }
    fclose(f);
    // 默认游戏语言：简体中文（旧配置没有 game_lang 时补写一次）
    if (!has_lang) {
        FILE *a = fopen(ini, "a");
        if (a) { fputs("game_lang = zh-CN\n", a); fclose(a); }
    }
    if (!strcmp(aspect, "fill") && fill > 1.0 && fill < 3.0) {
        char b[32];
        snprintf(b, sizeof b, "%.4f", fill);
        setenv("LARECOMP_WIDE_TEST", b, 1);
    } else if (!strcmp(aspect, "43")) {
        setenv("LARECOMP_WIDE_TEST", "0.75", 1);
    } else {
        setenv("LARECOMP_WIDE_TEST", "0", 1);  // output_scale 只认 > 0
    }
    if (movie_max >= 480 && movie_max <= 1080) {
        int w = (movie_max * 16 / 9) & ~1;
        char b[32];
        snprintf(b, sizeof b, "%dx%d", w, movie_max);
        setenv("LARECOMP_MOVIE_MAX", b, 1);
    } else {
        setenv("LARECOMP_MOVIE_MAX", "0x0", 1);  // 原生尺寸
    }
    if (audout >= 80 && audout <= 600) {
        char b[32];
        snprintf(b, sizeof b, "%d", audout);
        setenv("LARECOMP_AUDOUT_MS", b, 1);
    } else {
        setenv("LARECOMP_AUDOUT_MS", "200", 1);
    }
    if (vk_prof) setenv("LARECOMP_VK_PROF", "1", 1);
    else unsetenv("LARECOMP_VK_PROF");
    // ★ 采集需要计时器：exec.c 里的 prof_* 计时（gpu_ms / cpu_exec_ms / 各分项）也由
    //   LARECOMP_VK_PROF 控制。采集开启时强制打开，否则 PERF 表里这些列全是 0。
    //   （界面不再提供"帧诊断"开关；这里只是让采集有数据。）
    if (collect_on > 0) setenv("LARECOMP_VK_PROF", "1", 1);
    // 纹理校验开关：界面「纹理校验」关闭 -> 跳过哈希/比较（直接输出）
    if (tex_nocheck > 0) setenv("LARECOMP_TEX_NOCHECK", "1", 1);
    else unsetenv("LARECOMP_TEX_NOCHECK");
    // ASTC 转码开关：astc_rt=0 -> 直接 RGBA8 上传（LARECOMP_NO_RT_ASTC=1）
    if (astc_rt > 0) unsetenv("LARECOMP_NO_RT_ASTC");
    else setenv("LARECOMP_NO_RT_ASTC", "1", 1);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasBridge",
                 "display: aspect=%{public}s wide_test=%{public}s movie_max=%{public}s audout=%{public}s",
                 aspect, getenv("LARECOMP_WIDE_TEST") ? getenv("LARECOMP_WIDE_TEST") : "off",
                 getenv("LARECOMP_MOVIE_MAX") ? getenv("LARECOMP_MOVIE_MAX") : "off",
                 getenv("LARECOMP_AUDOUT_MS") ? getenv("LARECOMP_AUDOUT_MS") : "off");
}

void game_entry_start(const char *game_dir, const char *game_lib) {
    if (game_dir && game_dir[0]) snprintf(g_data_root, sizeof g_data_root, "%s", game_dir);
    else snprintf(g_data_root, sizeof g_data_root, "%s", g_dest_dir);
    if (game_lib && game_lib[0]) snprintf(g_game_lib, sizeof g_game_lib, "%s", game_lib);
    else snprintf(g_game_lib, sizeof g_game_lib, "liblasgame.so");
    apply_display_ini_env();
    pthread_t t;
    {  // main() 有多个 1KB 级栈数组（c[3][1100] 等），OHOS pthread 默认栈不够：显式给 16MB
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 16 << 20);
        pthread_create(&t, &at, game_thread, nullptr);
        pthread_attr_destroy(&at);
    }
    pthread_detach(t);
}

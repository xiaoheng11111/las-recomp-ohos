// NAPI 桥：探针结果交给 ArkTS 显示；同时把 XComponent 句柄交给原生渲染。
//
// 关于 XComponent 解包的时机：
//   ArkTS 里 `import { getVkInfo } from 'libentry.so'` 会先加载本模块，此时 XComponent
//   还没创建，exports 上不会有 __NATIVE_XCOMPONENT_OBJ__。等 XComponent 挂载后，框架才
//   把组件对象塞进 exports，但不会重新调用 Init。
//   所以这里保留两条路：Init 里先试一次，再导出 attachXComponent() 给 onLoad 兜底。
#include <napi/native_api.h>

#include <hilog/log.h>

#include <dlfcn.h>
#include <string>
#include <cstdio>
#include <cstring>

#include "egl_render.h"
#include "hms_probe.h"
#include "render.h"
#include "game_entry.h"
#include <rawfile/raw_file_manager.h>
void *g_rawManager = nullptr;
#include "vk_probe.h"

namespace {
constexpr unsigned int kDomain = 0x0000;
constexpr const char *kTag = "LauncherProbe";
napi_ref g_exportsRef = nullptr;
bool g_attached = false;
}  // namespace

static napi_value GetVkInfo(napi_env env, napi_callback_info info) {
    (void)info;
    std::string json = VkProbeJson();
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "vk probe: %{public}s", json.c_str());

    napi_value result = nullptr;
    napi_create_string_utf8(env, json.c_str(), json.size(), &result);
    return result;
}

// 启动移植层（第一版：数据根与游戏 .so 路径都指向应用沙箱固定位置）
static napi_value PrepareGameDataDir(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {  // rm 必须在这里就拿到，否则 rawfile 兜底路径永远不可用
        extern void *game_entry_set_rm(napi_env, napi_value);
        game_entry_set_rm(env, args[0]);
    }
    char haps[512] = {0};
    char app[512] = {0};
    size_t n = 0;
    if (argc >= 2) napi_get_value_string_utf8(env, args[1], haps, sizeof haps, &n);
    if (argc >= 3) napi_get_value_string_utf8(env, args[2], app, sizeof app, &n);
    if (haps[0] == 0) {
        // 兜底：应用 HAP 级 files 目录（官方沙箱路径）
        snprintf(haps, sizeof haps, "%s", "/data/storage/el2/base/haps/entry/files");
    }
    game_entry_prepare_dir(haps, app);
    napi_value v;
    napi_get_boolean(env, true, &v);
    return v;
}

static napi_value PrepareGameData(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        extern void *game_entry_set_rm(napi_env, napi_value);
        game_entry_set_rm(env, args[0]);
    }
    extern void game_entry_prepare(void);
    game_entry_prepare();  // 只做数据桥+解压，不跑 SDL_main
    napi_value v;
    napi_get_boolean(env, true, &v);
    return v;
}

static napi_value StartGame(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        // rm 对象 → NativeResourceManager（rawfile 读取句柄）
        extern void *game_entry_set_rm(napi_env, napi_value);
        game_entry_set_rm(env, args[0]);
    }
    char haps[512] = {0};
    char app[512] = {0};
    size_t n = 0;
    if (argc >= 2) napi_get_value_string_utf8(env, args[1], haps, sizeof haps, &n);
    if (argc >= 3) napi_get_value_string_utf8(env, args[2], app, sizeof app, &n);
    // 目录以 ArkTS 传入的 Context 为准，不再写死 /data/local/tmp 之类的旧约定
    game_entry_set_dirs(haps, app);
    game_entry_start("", "");
    napi_value ok;
    napi_get_boolean(env, true, &ok);
    return ok;
}

static napi_value GetHmsInfo(napi_env env, napi_callback_info info) {
    (void)info;
    std::string json = HmsProbeJson();
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "hms probe: %{public}s", json.c_str());

    napi_value result = nullptr;
    napi_create_string_utf8(env, json.c_str(), json.size(), &result);
    return result;
}

// 从 exports 上取 XComponent 对象并解包。成功返回 true。
static bool tryAttach(napi_env env, napi_value exports) {
    napi_value xcObj = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &xcObj) != napi_ok ||
        xcObj == nullptr) {
        return false;
    }
    OH_NativeXComponent *component = nullptr;
    if (napi_unwrap(env, xcObj, reinterpret_cast<void **>(&component)) != napi_ok ||
        component == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "XComponent 解包失败");
        return false;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "XComponent 已解包，交给渲染层");
    // 演示三角形渲染会先建 Vulkan 设备并把 0x7000000000 附近的地址用掉，
    // 而移植层要在那里预留 12.8GB guest 区；正式跑游戏时不再启动演示渲染。
    // RenderAttach(component);
    extern void game_entry_attach_xcomp(OH_NativeXComponent *);
    game_entry_attach_xcomp(component);  // 游戏窗口回调（官方 OnSurfaceCreated）
    g_attached = true;
    return true;
}

// ArkTS 在 XComponent 的 onLoad 里调用，此时组件对象一定已挂到 exports 上。
static napi_value AttachXComponent(napi_env env, napi_callback_info info) {
    (void)info;
    if (g_attached) {
        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "重复的 attach 调用，忽略");
        return nullptr;
    }
    napi_value exports = nullptr;
    if (g_exportsRef == nullptr || napi_get_reference_value(env, g_exportsRef, &exports) != napi_ok ||
        exports == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "拿不到模块 exports");
        return nullptr;
    }
    if (!tryAttach(env, exports)) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                     "attach 时 exports 上仍无 XComponent，检查 libraryname 是否写了 'entry'");
    }
    return nullptr;
}

// 虚拟手柄：ArkTS 触控/按键写进 SDL shim 的全局状态，游戏轮询 SDL_GetGamepad* 即可拿到。
// button 用 SDL_GAMEPAD_BUTTON_* 枚举值（0..25），axis 用 SDL_GAMEPAD_AXIS_*（0..5）。
// 这些符号在 liblasrecomp.so（已 RTLD_GLOBAL 提升），用 dlsym 取，避免 libentry 直接链接时 undefined。
extern "C" void letsgo_ohos_pad_button(int button, int down);
extern "C" void letsgo_ohos_pad_axis(int axis, float value);
extern "C" void letsgo_ohos_pad_reset(void);

typedef void (*PadButtonFn)(int, int);
typedef void (*PadAxisFn)(int, float);
typedef void (*PadResetFn)(void);

static void resolvePadFns(PadButtonFn *pb, PadAxisFn *pa, PadResetFn *pr) {
    static PadButtonFn s_pb = nullptr;
    static PadAxisFn s_pa = nullptr;
    static PadResetFn s_pr = nullptr;
    // liblasrecomp.so 可能晚于首次调用才加载，符号会从 null 变成有效：
    // 只要还拿不到就每次重试 dlsym，直到拿到为止。
    if (!s_pb) s_pb = (PadButtonFn)dlsym(RTLD_DEFAULT, "letsgo_ohos_pad_button");
    if (!s_pa) s_pa = (PadAxisFn)dlsym(RTLD_DEFAULT, "letsgo_ohos_pad_axis");
    if (!s_pr) s_pr = (PadResetFn)dlsym(RTLD_DEFAULT, "letsgo_ohos_pad_reset");
    *pb = s_pb;
    *pa = s_pa;
    *pr = s_pr;
}

static napi_value PadButton(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t button = 0, down = 0;
    if (argc >= 1) napi_get_value_int32(env, args[0], &button);
    if (argc >= 2) napi_get_value_int32(env, args[1], &down);
    PadButtonFn pb; PadAxisFn pa; PadResetFn pr;
    resolvePadFns(&pb, &pa, &pr);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "NapiPad", "padButton btn=%{public}d down=%{public}d fn=%{public}p", button, down, pb);
    if (pb) pb(button, down);
    napi_value v;
    napi_get_boolean(env, true, &v);
    return v;
}

static napi_value PadAxis(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t axis = 0;
    double value = 0.0;
    if (argc >= 1) napi_get_value_int32(env, args[0], &axis);
    if (argc >= 2) napi_get_value_double(env, args[1], &value);
    PadButtonFn pb; PadAxisFn pa; PadResetFn pr;
    resolvePadFns(&pb, &pa, &pr);
    if (pa) pa(axis, (float)value);
    napi_value v;
    napi_get_boolean(env, true, &v);
    return v;
}

static napi_value PadReset(napi_env env, napi_callback_info info) {
    (void)info;
    PadButtonFn pb; PadAxisFn pa; PadResetFn pr;
    resolvePadFns(&pb, &pa, &pr);
    if (pr) pr();
    napi_value v;
    napi_get_boolean(env, true, &v);
    return v;
}


// 运行时帧率（liblasrecomp.so 的 g_current_fps / g_frame_ms），ArkTS 右上角显示用
static napi_value GetFps(napi_env env, napi_callback_info info) {
    (void)info;
    static double *pfps = nullptr, *pms = nullptr;
    if (!pfps) pfps = (double *)dlsym(RTLD_DEFAULT, "g_current_fps");
    if (!pms) pms = (double *)dlsym(RTLD_DEFAULT, "g_frame_ms");
    double fps = pfps ? *pfps : 0.0;
    double ms = pms ? *pms : 0.0;
    char buf[64];
    snprintf(buf, sizeof buf, "%d fps | %d ms", (int)(fps + 0.5), (int)(ms + 0.5));
    napi_value r;
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &r);
    return r;
}


// ---- 数据采集：桥接 liblasrecomp.so 导出的 las_collect_start/stop（RTLD_DEFAULT 可见）----
// 为什么走 dlsym：移植层是运行时 dlopen 的，启动器不链接它；但这三个符号在 .so 里是导出（T）。
typedef int (*CollectStartFn)(int);
typedef void (*CollectStopFn)(char *out, size_t out_size);

static napi_value StartCollect(napi_env env, napi_callback_info info) {
    (void)info;
    CollectStartFn f = (CollectStartFn)dlsym(RTLD_DEFAULT, "las_collect_start");
    // 0=已开始；-2=已经在采；-1=没有 LARECOMP_DATA_ROOT；-3=运行时还没加载（游戏没起来）
    int rc = f ? f(0) : -3;
    napi_value r;
    napi_create_int32(env, rc, &r);
    return r;
}

static napi_value StopCollect(napi_env env, napi_callback_info info) {
    (void)info;
    char out[512];
    out[0] = 0;
    CollectStopFn f = (CollectStopFn)dlsym(RTLD_DEFAULT, "las_collect_stop");
    if (f) f(out, sizeof out);
    napi_value r;
    napi_create_string_utf8(env, out, NAPI_AUTO_LENGTH, &r);
    return r;
}


// 软键盘：liblasrecomp 的 swkbd 设置 g_swkbd_state=1 等待；ArkTS 轮询到这里弹原生输入法，再 submit
static napi_value PollSwkbd(napi_env env, napi_callback_info info) {
    (void)info;
    static volatile int *st = nullptr;
    static char *init = nullptr;
    static int *mx = nullptr;
    if (!st) st = (volatile int *)dlsym(RTLD_DEFAULT, "g_swkbd_state");
    if (!init) init = (char *)dlsym(RTLD_DEFAULT, "g_swkbd_initial");
    if (!mx) mx = (int *)dlsym(RTLD_DEFAULT, "g_swkbd_max");
    napi_value r;
    if (!st || *st != 1 || !init || !mx) {
        napi_create_string_utf8(env, "", 0, &r);
        return r;
    }
    char buf[256];
    snprintf(buf, sizeof buf, "{\"initial\":\"%s\",\"max\":%d}", init, *mx);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &r);
    return r;
}

static napi_value SubmitSwkbd(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char text[96] = {0};
    int ok = 0;
    size_t n = 0;
    if (argc >= 1) napi_get_value_string_utf8(env, args[0], text, sizeof text, &n);
    if (argc >= 2) napi_get_value_int32(env, args[1], &ok);
    static char *res = nullptr;
    static int *pok = nullptr;
    static volatile int *st = nullptr;
    if (!res) res = (char *)dlsym(RTLD_DEFAULT, "g_swkbd_result");
    if (!pok) pok = (int *)dlsym(RTLD_DEFAULT, "g_swkbd_ok");
    if (!st) st = (volatile int *)dlsym(RTLD_DEFAULT, "g_swkbd_state");
    if (res) { memcpy(res, text, n < sizeof text ? n : sizeof text - 1); res[n < sizeof text ? n : sizeof text - 1] = 0; }
    if (pok) *pok = ok;
    if (st) *st = 2;  // done
    napi_value v;
    napi_get_boolean(env, true, &v);
    return v;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"getVkInfo", nullptr, GetVkInfo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getHmsInfo", nullptr, GetHmsInfo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"prepareGameDataDir", nullptr, PrepareGameDataDir, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"prepareGameData", nullptr, PrepareGameData, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startGame", nullptr, StartGame, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"attachXComponent", nullptr, AttachXComponent, nullptr, nullptr, nullptr, napi_default,
         nullptr},
        {"padButton", nullptr, PadButton, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"padAxis", nullptr, PadAxis, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"padReset", nullptr, PadReset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getFps", nullptr, GetFps, nullptr, nullptr, nullptr, napi_default, nullptr},
{"startCollect", nullptr, StartCollect, nullptr, nullptr, nullptr, napi_default, nullptr},
{"stopCollect", nullptr, StopCollect, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pollSwkbd", nullptr, PollSwkbd, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"submitSwkbd", nullptr, SubmitSwkbd, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    if (g_exportsRef == nullptr) {
        napi_create_reference(env, exports, 1, &g_exportsRef);
    }

    // 把本库提升到全局符号域：游戏库的 UND（如 __register_atfork）才能解析到这里的实现
    {
        static bool promoted = false;
        if (!promoted) {
            promoted = true;
            void *self = dlopen("libentry.so", RTLD_NOW | RTLD_GLOBAL);
            OH_LOG_Print(LOG_APP, self ? LOG_INFO : LOG_WARN, kDomain, kTag,
                         "libentry global promote=%{public}p err=%{public}s", self,
                         self ? "-" : (dlerror() ? dlerror() : "?"));
        }
    }

    // 若 XComponent 先于模块加载存在（例如 libraryname 触发的那次加载），这里就能直接接上。
    if (!g_attached) {
        if (!tryAttach(env, exports)) {
            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                         "模块加载时尚未挂载 XComponent，等 onLoad 回调 attach");
        }
    }

    return exports;
}
EXTERN_C_END

static napi_module g_module = {
    1,          // nm_version
    0,          // nm_flags
    nullptr,    // nm_filename
    Init,       // nm_register_func
    "entry",    // nm_modname
    nullptr,    // nm_priv
    {nullptr},  // reserved
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void) { napi_module_register(&g_module); }

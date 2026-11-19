// SDL3 -> OHOS 兼容层实现体（第一版：计时/日志/错误真实现，输入事件从
// XComponent 侧 push，音频/手柄/PNG stub，音频后续接 OHOS AudioRenderer）。
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>
#include <dlfcn.h>

extern "C" OHNativeWindow *letsgo_ohos_get_window(void);  // ohos.c（C 链接）
#include <SDL3/SDL_vulkan.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

// ---- 计时 ----
static Uint64 ns_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
}
Uint64 SDL_GetTicksNS(void) { return ns_monotonic(); }
Uint64 SDL_GetTicks(void) { return ns_monotonic() / 1000000ull; }
void SDL_Delay(Uint32 ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
void SDL_DelayNS(Uint64 ns) { struct timespec t = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)}; nanosleep(&t, NULL); }
void SDL_DelayPrecise(Uint64 ns) { SDL_DelayNS(ns); }

// ---- CPU / RAM ----
int SDL_GetNumLogicalCPUCores(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }
int SDL_GetCPUCacheLineSize(void) { return 64; }
const char *SDL_GetCPUModelName(void) { return "OHOS"; }
int SDL_GetSystemRAM(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256]; int kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemTotal: %d", &kb) == 1) break;
    fclose(f);
    return kb / 1024;
}

// ---- 错误 ----
static char errbuf[256] = {0};
const char *SDL_GetError(void) { return errbuf[0] ? errbuf : ""; }
void SDL_SetError(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(errbuf, sizeof errbuf, fmt, ap);
    va_end(ap);
}
void SDL_free(void *p) { free(p); }

// ---- Hint（字符串表）----
#define MAX_HINTS 32
static struct { const char *name, *value; } hints[MAX_HINTS];
static int nhints;
bool SDL_SetHint(const char *name, const char *value) {
    if (!name || !value) return false;
    for (int i = 0; i < nhints; i++)
        if (!strcmp(hints[i].name, name)) { hints[i].value = value; return true; }
    if (nhints < MAX_HINTS) { hints[nhints].name = name; hints[nhints].value = value; nhints++; }
    return true;
}

// ---- 事件队列（XComponent 侧通过 SDL_ShimPushEvent 喂）----
#define EVQ 64
static SDL_Event evq[EVQ];
static int evq_head, evq_tail;
static pthread_mutex_t evq_lock = PTHREAD_MUTEX_INITIALIZER;
bool SDL_PollEvent(SDL_Event *ev) {
    pthread_mutex_lock(&evq_lock);
    bool got = evq_head != evq_tail;
    if (got) { *ev = evq[evq_tail]; evq_tail = (evq_tail + 1) % EVQ; }
    pthread_mutex_unlock(&evq_lock);
    return got;
}
bool SDL_WaitEventTimeout(SDL_Event *ev, Sint64 timeout) {
    Uint64 deadline = ns_monotonic() + (Uint64)(timeout > 0 ? timeout : 0) * 1000000ull;
    for (;;) {
        if (SDL_PollEvent(ev)) return true;
        if (timeout == 0) return false;
        if (timeout > 0 && ns_monotonic() >= deadline) return false;
        SDL_Delay(1);
    }
}
bool SDL_PushEvent(SDL_Event *ev) {
    pthread_mutex_lock(&evq_lock);
    int next = (evq_head + 1) % EVQ;
    bool ok = next != evq_tail;
    if (ok) { evq[evq_head] = *ev; evq_head = next; }
    pthread_mutex_unlock(&evq_lock);
    return ok;
}
// 非 SDL 接口：OHOS 回调线程喂事件用
void SDL_ShimPushEvent(const SDL_Event *ev) { SDL_PushEvent((SDL_Event *)ev); }

// ---- Init ----
int SDL_Init(Uint32 flags) { (void)flags; return 1; }  // SDL3: bool（0=false=失败）
int SDL_InitSubSystem(Uint32 flags) { (void)flags; return 1; }
void SDL_Quit(void) {}

// ---- 窗口（第一版：XComponent 提供 surface，窗口是假句柄）----
struct SDL_Window { Uint32 id; int w, h; };
static Uint32 next_win_id = 1;
SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags) {
    (void)title; (void)flags;
    SDL_Window *win = (SDL_Window *)calloc(1, sizeof *win);
    win->id = next_win_id++; win->w = w; win->h = h;
    return win;
}
void SDL_DestroyWindow(SDL_Window *w) { free(w); }
static int g_ohos_win_w = 0, g_ohos_win_h = 0;
extern "C" void letsgo_ohos_set_window_size(int w, int h) { g_ohos_win_w = w; g_ohos_win_h = h; }
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *ow, int *oh) {
    if (ow) *ow = g_ohos_win_w > 0 ? g_ohos_win_w : (w ? w->w : 0);
    if (oh) *oh = g_ohos_win_h > 0 ? g_ohos_win_h : (w ? w->h : 0);
    return true;
}
float SDL_GetWindowPixelDensity(SDL_Window *w) { (void)w; return 1.0f; }
SDL_Window *SDL_GetWindowFromID(Uint32 id) { (void)id; return nullptr; }
bool SDL_SetWindowFullscreen(SDL_Window *w, bool on) { (void)w; (void)on; return true; }
bool SDL_SetWindowIcon(SDL_Window *w, SDL_Surface *icon) { (void)w; SDL_DestroySurface(icon); return true; }
bool SDL_SetWindowRelativeMouseMode(SDL_Window *w, bool on) { (void)w; (void)on; return true; }
bool SDL_ShowSimpleMessageBox(Uint32 f, const char *t, const char *m, SDL_Window *w) {
    (void)f; (void)w; fprintf(stderr, "[msgbox] %s: %s\n", t, m); return true;
}

// ---- Vulkan surface：真实现接 OHOS（renderthread 走 XComponent 时不需要 SDL 侧）----
bool SDL_Vulkan_CreateSurface(SDL_Window *w, void *vkInstance, void *props, void *surface) {
    (void)w; (void)props;
    OHNativeWindow *win = letsgo_ohos_get_window();
    if (!win) { SDL_SetError("SDL_Vulkan_CreateSurface: no OHNativeWindow"); return false; }
    VkInstance inst = (VkInstance)vkInstance;
    // 官方 OHOS Vulkan 扩展（VK_OHOS_surface）：vkCreateSurfaceOHOS
    auto fn = (PFN_vkCreateSurfaceOHOS)vkGetInstanceProcAddr(inst, "vkCreateSurfaceOHOS");
    if (!fn) fn = (PFN_vkCreateSurfaceOHOS)dlsym(RTLD_DEFAULT, "vkCreateSurfaceOHOS");
    if (!fn) { SDL_SetError("SDL_Vulkan_CreateSurface: vkCreateSurfaceOHOS unavailable"); return false; }
    VkSurfaceCreateInfoOHOS ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
    ci.pNext = nullptr;
    ci.flags = 0;
    ci.window = win;
    VkResult r = fn(inst, &ci, nullptr, (VkSurfaceKHR *)surface);
    if (r != VK_SUCCESS) { SDL_SetError("SDL_Vulkan_CreateSurface: vkCreateSurfaceOHOS failed %d", (int)r); return false; }
    return true;
}
const char *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count) {
    // 官方 OHOS 扩展：VK_OHOS_surface 必须在 instance 创建时启用（否则 vkCreateSurfaceOHOS 返回 -7
    // VK_ERROR_EXTENSION_NOT_PRESENT）
    static const char *exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME, NULL};
    if (count) *count = 2;
    return exts;
}

// ---- Surface / PNG（stub：第一版不做截图）----
SDL_Surface *SDL_CreateSurfaceFrom(int w, int h, int format, void *pixels, int pitch) {
    (void)format;
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof *s);
    if (!s) return nullptr;
    s->w = w; s->h = h; s->pitch = pitch ? pitch : w * 4; s->pixels = pixels;
    return s;
}
void SDL_DestroySurface(SDL_Surface *s) { free(s); }
SDL_Surface *SDL_LoadPNG(const char *path) { (void)path; return nullptr; }
// OHOS: 没有 SDL_image，截图直接写 32bpp BMP（文件名追加 .bmp），供 PC 端查看
bool SDL_SavePNG(SDL_Surface *s, const char *path) {
    if (!s || !s->pixels || !path) return false;
    char bp[1200];
    // 诊断：相对路径写到 $LARECOMP_DATA_ROOT/shot/（应用可写、且 hdc 可读），文件名取 basename
    if (path[0] == '/') snprintf(bp, sizeof bp, "%s.bmp", path);
    else {
        const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
        const char *root = getenv("LARECOMP_DATA_ROOT");
        if (root && root[0]) { char sd[1100]; snprintf(sd, sizeof sd, "%s/shot", root); mkdir(sd, 0777); snprintf(bp, sizeof bp, "%s/%s.bmp", sd, b); }
        else snprintf(bp, sizeof bp, "%s.bmp", path);
    }
    FILE *f = fopen(bp, "wb");
    fprintf(stderr, "[shim-savepng] path=%s -> %s fopen=%p w=%d h=%d\n", path, bp, (void*)f, s->w, s->h);
    if (!f) { SDL_SetError("bmp open failed"); return false; }
    int w = s->w, h = s->h, row = w * 4;
    uint32_t fsz = 54u + (uint32_t)row * (uint32_t)h;
    uint8_t hd[54] = {0};
    hd[0] = 'B'; hd[1] = 'M';
    memcpy(hd + 2, &fsz, 4);
    uint32_t off = 54, isz = 40;
    memcpy(hd + 10, &off, 4); memcpy(hd + 14, &isz, 4);
    memcpy(hd + 18, &w, 4);
    int nh = -h; memcpy(hd + 22, &nh, 4);
    uint16_t planes = 1, bpp = 32;
    memcpy(hd + 26, &planes, 2); memcpy(hd + 28, &bpp, 2);
    uint32_t img = (uint32_t)row * (uint32_t)h;
    memcpy(hd + 34, &img, 4);
    fwrite(hd, 1, 54, f);
    uint8_t *line = (uint8_t *)malloc((size_t)row);
    const uint8_t *p = (const uint8_t *)s->pixels;
    size_t pitch = s->pitch > 0 ? (size_t)s->pitch : (size_t)row;
    for (int y = 0; y < h; y++) {
        const uint8_t *q = p + (size_t)y * pitch;
        for (int x = 0; x < w; x++) {
            line[x * 4 + 0] = q[x * 4 + 2];
            line[x * 4 + 1] = q[x * 4 + 1];
            line[x * 4 + 2] = q[x * 4 + 0];
            line[x * 4 + 3] = q[x * 4 + 3];
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
    return true;
}
bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info) {
    if (!path) return false;
    if (info) memset(info, 0, sizeof *info);  // 官方 SDL3：info 可为 NULL（仅查存在性）
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); if (info) { info->type = 1; info->size = 0; } return true; }
    return false;
}

// ---- Gamepad：单只虚拟手柄，状态由 ArkTS 触控/按键经 letsgo_ohos_pad_* 写入 ----
// 游戏（input.c）两套读法都用：SDL_GetGamepads 发现手柄 + SDL_GetGamepadButton/Axis 轮询。
// 所以这里必须报告 1 只可用手柄并返回真实状态，否则游戏认为“无控制器”，
// 开局的控制器的选择界面就永远选不了（操作无效、无法推进、后续音频不释放）。
#define VGAME_BTN_COUNT 26
#define VGAME_AXIS_COUNT 6
static volatile int vgame_btn[VGAME_BTN_COUNT];
static volatile float vgame_axis[VGAME_AXIS_COUNT];
static const SDL_JoystickID VGAME_ID = 0x0A05;

extern "C" void letsgo_ohos_pad_button(int button, int down) {
    if (button >= 0 && button < VGAME_BTN_COUNT) {
        vgame_btn[button] = down ? 1 : 0;
        // 关键：游戏只有在收到 SDL 事件时才调 recompute_pad() 重新读 SDL_GetGamepadButton。
        // 只写 vgame_btn[] 不 push 事件的话，游戏永远读到"没按键"。这里补推 gamepad 按键事件。
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        ev.gbutton.which = VGAME_ID;
        ev.gbutton.button = button;
        ev.gbutton.down = down ? 1 : 0;
        SDL_PushEvent(&ev);
    }
}
extern "C" void letsgo_ohos_pad_axis(int axis, float value) {
    if (axis >= 0 && axis < VGAME_AXIS_COUNT) {
        vgame_axis[axis] = value;
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        ev.gaxis.windowID = VGAME_ID;
        ev.gaxis.axis = axis;
        ev.gaxis.value = value;
        SDL_PushEvent(&ev);
    }
}
extern "C" void letsgo_ohos_pad_reset(void) {
    for (int i = 0; i < VGAME_BTN_COUNT; i++) vgame_btn[i] = 0;
    for (int i = 0; i < VGAME_AXIS_COUNT; i++) vgame_axis[i] = 0.0f;
}

static SDL_JoystickID vgame_ids[1] = {VGAME_ID};
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) { (void)id; return (SDL_Gamepad *)&vgame_btn[0]; }
void SDL_CloseGamepad(SDL_Gamepad *gp) { (void)gp; }
SDL_JoystickID *SDL_GetGamepads(int *count) {
    if (count) *count = 1;
    return vgame_ids;
}
const char *SDL_GetGamepadName(SDL_Gamepad *gp) { (void)gp; return "OHOS Virtual Gamepad"; }
int SDL_GetGamepadType(SDL_Gamepad *gp) { (void)gp; return 1; }         // SDL_GAMEPAD_TYPE_XBOX360：游戏按 XBOX 布局绑键
int SDL_GetGamepadTypeForID(SDL_JoystickID id) { (void)id; return 1; }
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *gp) { (void)gp; return VGAME_ID; }
float SDL_GetGamepadAxis(SDL_Gamepad *gp, int axis) {
    (void)gp;
    if (axis >= 0 && axis < VGAME_AXIS_COUNT) return vgame_axis[axis];
    return 0.0f;
}
bool SDL_GetGamepadButton(SDL_Gamepad *gp, int button) {
    (void)gp;
    if (button >= 0 && button < VGAME_BTN_COUNT) return vgame_btn[button] != 0;
    return false;
}
bool SDL_AddGamepadMappingsFromFile(const char *path) { (void)path; return false; }
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode sc, int mod, bool keycon) { (void)mod; (void)keycon; return (SDL_Keycode)sc; }
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, int *mod) { (void)mod; return (SDL_Scancode)key; }
const char *SDL_GetScancodeName(SDL_Scancode sc) { static char b[16]; snprintf(b, sizeof b, "sc%d", (int)sc); return b; }
SDL_Scancode SDL_GetScancodeFromName(const char *name) { (void)name; return SDL_SCANCODE_UNKNOWN; }

// ---- Audio：OHOS AudioRenderer 官方实现（回调转发游戏的 cb）----
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiorenderer.h>

#include <vector>
struct SDL_AudioStream {
    OH_AudioRenderer *rend;
    SDL_AudioStreamCallback cb;
    void *userdata;
    std::vector<uint8_t> queued;
    pthread_mutex_t qlock;  // queued 被 audout 线程(Put)和音频回调线程(mix_write)并发访问，必须加锁
    int in_ch;    // 游戏输出声道数（audout 是 6，5.1）
    int out_ch;   // 渲染器声道数（固定 2，手机扬声器支持 best）
    int fmt;      // SDL_AUDIO_S16 / SDL_AUDIO_F32
};

// 把 queued 里的 in_ch 声道 S16 帧下混成 out_ch(2) 声道写入 buf。
// 返回写入字节数（=len 或不足时补零后的 len）。剩余不足一帧的数据留在 queued 里等下次。
static int32_t mix_write(SDL_AudioStream *st, void *buf, int32_t len) {
    uint8_t *out = (uint8_t *)buf;
    pthread_mutex_lock(&st->qlock);
    int32_t filled = 0;
    const int out_ch = st->out_ch > 0 ? st->out_ch : 2;
    const int in_ch = st->in_ch > 0 ? st->in_ch : out_ch;
    const int bytes_per = 2;  // S16
    const int32_t out_frame_bytes = out_ch * bytes_per;
    const int32_t in_frame_bytes = in_ch * bytes_per;
    if (in_ch == out_ch) {
        // 直通
        int32_t n = (int32_t)st->queued.size() < len ? (int32_t)st->queued.size() : len;
        if (n > 0) { memcpy(out, st->queued.data(), n); st->queued.erase(st->queued.begin(), st->queued.begin() + n); filled = n; }
    } else {
        // 下混：out 每帧消费一帧 in
        while (filled + out_frame_bytes <= len && (int32_t)st->queued.size() >= in_frame_bytes) {
            const int16_t *in = (const int16_t *)st->queued.data();
            int16_t *o = (int16_t *)(out + filled);
            if (in_ch == 6) {
                // 顺序 FL FR FC LFE BL BR
                int32_t fl = in[0], fr = in[1], fc = in[2], bl = in[4], br = in[5];
                int32_t l = fl + (fc * 7071 >> 14) + (bl * 7071 >> 14);
                int32_t r = fr + (fc * 7071 >> 14) + (br * 7071 >> 14);
                if (l > 32767) l = 32767; if (l < -32768) l = -32768;
                if (r > 32767) r = 32767; if (r < -32768) r = -32768;
                o[0] = (int16_t)l; o[1] = (int16_t)r;
            } else {
                // 通用：前两声道 + 其它平均叠加
                int32_t l = 0, r = 0;
                for (int c = 0; c < in_ch; c++) {
                    if (c == 0) l += in[c];
                    else if (c == 1) r += in[c];
                    else { l += in[c] / 2; r += in[c] / 2; }
                }
                if (l > 32767) l = 32767; if (l < -32768) l = -32768;
                if (r > 32767) r = 32767; if (r < -32768) r = -32768;
                o[0] = (int16_t)l; o[1] = (int16_t)r;
            }
            st->queued.erase(st->queued.begin(), st->queued.begin() + in_frame_bytes);
            filled += out_frame_bytes;
        }
    }
    if (filled < len) memset(out + filled, 0, len - filled);  // 欠数补零
    pthread_mutex_unlock(&st->qlock);
    return len;
}

static OH_AudioData_Callback_Result ohos_audio_write(OH_AudioRenderer *r, void *userData, void *buf, int32_t len) {
    (void)r;
    SDL_AudioStream *st = (SDL_AudioStream *)userData;
    if (st && st->cb) st->cb(st->userdata, st, len, 0);  // SDL3: (userdata, stream, additional, total)——游戏 cb 内 PutAudioStreamData
    if (!st) { memset(buf, 0, len); return AUDIO_DATA_CALLBACK_RESULT_VALID; }
    static int dbg = 0;
    if (dbg < 12) {
        size_t qsz = st->queued.size();
        size_t n = qsz / 2; if (n > 4096) n = 4096;
        int nz = 0, mx = 0;
        const int16_t *s = (const int16_t *)st->queued.data();
        for (size_t i = 0; i < n; i++) { int v = s[i]; if (v != 0) nz++; int a = v < 0 ? -v : v; if (a > mx) mx = a; }
        fprintf(stderr, "[shim-audio-w] len=%d queued=%zu nonzero=%d/%zu maxabs=%d in_ch=%d\n", len, qsz, nz, n, mx, st->in_ch);
        dbg++;
    }
    mix_write(st, buf, len);
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                           SDL_AudioStreamCallback cb, void *userdata) {
    (void)devid;
    if (!spec) return NULL;
    OH_AudioStreamBuilder *b = NULL;
    if (OH_AudioStreamBuilder_Create(&b, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS || !b) return NULL;
    OH_AudioStreamBuilder_SetSamplingRate(b, spec->freq);
    // 手机扬声器对多声道（尤其 6ch 5.1）的 MUSIC 流支持不稳，Start 会校验失败并推静音。
    // 统一用 stereo 输出，多声道在 write 回调里下混，保证一定能出声。
    // 严格按官方流程：SamplingRate→ChannelCount→SampleFormat→EncodingType→RendererInfo→回调→Generate→Start，
    // 不要设 ChannelLayout（会与 ChannelCount 冲突导致 Start Illegal state）。
    int out_ch = spec->channels > 2 ? 2 : spec->channels;
    OH_AudioStreamBuilder_SetChannelCount(b, out_ch);
    OH_AudioStream_SampleFormat sf = (spec->format == SDL_AUDIO_S16) ? AUDIOSTREAM_SAMPLE_S16LE
                                                                   : AUDIOSTREAM_SAMPLE_F32LE;
    OH_AudioStreamBuilder_SetSampleFormat(b, sf);
    OH_AudioStreamBuilder_SetEncodingType(b, AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetRendererInfo(b, AUDIOSTREAM_USAGE_MUSIC);
    SDL_AudioStream *st = new SDL_AudioStream{NULL, cb, userdata, {}, PTHREAD_MUTEX_INITIALIZER, spec->channels, out_ch, spec->format};
    OH_AudioStreamBuilder_SetRendererWriteDataCallback(b, (OH_AudioRenderer_OnWriteDataCallback)ohos_audio_write, st);
    int gr = (int)OH_AudioStreamBuilder_GenerateRenderer(b, &st->rend);
    fprintf(stderr, "[shim-audio] generate=%d rend=%p in_ch=%d out_ch=%d freq=%d fmt=%d\n", gr, (void*)st->rend, spec->channels, out_ch, spec->freq, spec->format);
    if (!st->rend) { OH_AudioStreamBuilder_Destroy(b); return NULL; }
    OH_AudioStreamBuilder_Destroy(b);
    int sr = (int)OH_AudioRenderer_Start(st->rend);
    fprintf(stderr, "[shim-audio] start=%d\n", sr);
    if (sr != AUDIOSTREAM_SUCCESS) return NULL;
    return st;
}
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec *spec, int *frames) {
    if (spec) { spec->freq = 48000; spec->channels = 2; spec->format = SDL_AUDIO_F32; }
    if (frames) *frames = 0;
    return devid == 0;
}
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *st) { (void)st; return 0; }
float SDL_GetAudioStreamGain(SDL_AudioStream *st) { (void)st; return 1.0f; }
bool SDL_SetAudioStreamGain(SDL_AudioStream *st, float g) { if (st && st->rend) return OH_AudioRenderer_SetVolume(st->rend, g) == AUDIOSTREAM_SUCCESS; return true; }
Uint32 SDL_GetAudioStreamQueued(SDL_AudioStream *st) { if (!st) return 0; pthread_mutex_lock(&st->qlock); Uint32 n = (Uint32)st->queued.size(); pthread_mutex_unlock(&st->qlock); return n; }
bool SDL_PutAudioStreamData(SDL_AudioStream *st, const void *data, int len) { if (st && data && len > 0) { const uint8_t *p = (const uint8_t *)data; pthread_mutex_lock(&st->qlock); st->queued.insert(st->queued.end(), p, p + len); pthread_mutex_unlock(&st->qlock); } return true; }
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *st) { if (st && st->rend) OH_AudioRenderer_Start(st->rend); return true; }
void SDL_DestroyAudioStream(SDL_AudioStream *st) { if (st) { if (st->rend) OH_AudioRenderer_Stop(st->rend); delete st; } }

// 文本输入与 locale（第一版：无输入面板，locale 取 OHOS 环境即可）
void SDL_StartTextInput(SDL_Window *w) { (void)w; }
void SDL_StopTextInput(SDL_Window *w) { (void)w; }
SDL_Locale **SDL_GetPreferredLocales(int *count) {
    static SDL_Locale zh = {"zh", "CN"};
    static SDL_Locale *locs[2] = {&zh, nullptr};
    if (count) *count = 1;
    return locs;
}

#pragma once
#include "SDL_cpuinfo.h"
#include "SDL_timer.h"
#include "SDL_events.h"
#include "SDL_log.h"
#include <stdbool.h>
#include <stddef.h>
#define SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"
#define SDL_HINT_ORIENTATIONS "SDL_ORIENTATIONS"
enum {
       SDL_INIT_VIDEO = 0x20, SDL_INIT_AUDIO = 0x10, SDL_INIT_GAMEPAD = 0x2000, SDL_INIT_EVENTS = 0x4000,
       SDL_MESSAGEBOX_ERROR = 0x10,
       SDL_BUTTON_LEFT = 1, SDL_BUTTON_RIGHT = 2,
       SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK = 0,
       SDL_AUDIO_S16 = 0x8104, SDL_AUDIO_F32 = 0x8120,
       SDL_PIXELFORMAT_RGBA8888 = 0x16462004, SDL_PIXELFORMAT_ABGR8888 = 0x16462005,
       SDL_WINDOW_VULKAN = 0x10000000, SDL_WINDOW_RESIZABLE = 0x20, SDL_WINDOW_HIDDEN = 0x8,
       SDL_WINDOW_HIGH_PIXEL_DENSITY = 0x2000,
       SDL_KMOD_NONE = 0, SDL_KMOD_LSHIFT = 1, SDL_KMOD_RSHIFT = 2, SDL_KMOD_SHIFT = 3,
       SDL_KMOD_LCTRL = 64, SDL_KMOD_RCTRL = 128, SDL_KMOD_CTRL = 192,
       SDL_KMOD_LALT = 256, SDL_KMOD_RALT = 512, SDL_KMOD_ALT = 768,
       SDL_KMOD_GUI = 1024 };
typedef Uint16 SDL_Keymod;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Surface { int flags, w, h, pitch; void *pixels; int refcount; void *reserved; } SDL_Surface;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef Uint32 SDL_AudioDeviceID;
typedef Uint32 SDL_JoystickID;
typedef Uint32 SDL_Keycode;
typedef Uint16 SDL_WindowFlags;
typedef Uint16 SDL_Keymod;
typedef struct SDL_AudioSpec { int format; int channels; int freq; } SDL_AudioSpec;
typedef struct SDL_PathInfo { int type; Uint64 size; } SDL_PathInfo;
typedef void (*SDL_AudioStreamCallback)(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);

#ifdef __cplusplus
extern "C" {
#endif
int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
void SDL_SetError(const char *fmt, ...);
bool SDL_SetHint(const char *name, const char *value);
Uint64 SDL_GetTicks(void);
Uint64 SDL_GetTicksNS(void);
void SDL_Delay(Uint32 ms);
void SDL_DelayNS(Uint64 ns);
void SDL_DelayPrecise(Uint64 ns);
int SDL_GetSystemRAM(void);
int SDL_GetNumLogicalCPUCores(void);
bool SDL_PollEvent(SDL_Event *ev);
bool SDL_WaitEventTimeout(SDL_Event *ev, Sint64 timeout);
bool SDL_PushEvent(SDL_Event *ev);
SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags);
void SDL_DestroyWindow(SDL_Window *w);
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *ow, int *oh);
float SDL_GetWindowPixelDensity(SDL_Window *w);
SDL_Window *SDL_GetWindowFromID(Uint32 id);
bool SDL_SetWindowFullscreen(SDL_Window *w, bool on);
bool SDL_SetWindowIcon(SDL_Window *w, SDL_Surface *icon);
bool SDL_SetWindowRelativeMouseMode(SDL_Window *w, bool on);
bool SDL_Vulkan_CreateSurface(SDL_Window *w, void *vkInstance, void *props, void *surface);
const char *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count);
bool SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *msg, SDL_Window *w);
SDL_Surface *SDL_CreateSurfaceFrom(int w, int h, int format, void *pixels, int pitch);
void SDL_DestroySurface(SDL_Surface *s);
SDL_Surface *SDL_LoadPNG(const char *path);
bool SDL_SavePNG(SDL_Surface *s, const char *path);
bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info);
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id);
void SDL_CloseGamepad(SDL_Gamepad *gp);
SDL_JoystickID *SDL_GetGamepads(int *count);
const char *SDL_GetGamepadName(SDL_Gamepad *gp);
int SDL_GetGamepadType(SDL_Gamepad *gp);
int SDL_GetGamepadTypeForID(SDL_JoystickID id);
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *gp);
float SDL_GetGamepadAxis(SDL_Gamepad *gp, int axis);
bool SDL_GetGamepadButton(SDL_Gamepad *gp, int button);
bool SDL_AddGamepadMappingsFromFile(const char *path);
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode sc, int mod, bool keycon);
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key, int *mod);
const char *SDL_GetScancodeName(SDL_Scancode sc);
SDL_Scancode SDL_GetScancodeFromName(const char *name);
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec,
                                            SDL_AudioStreamCallback cb, void *userdata);
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID devid, SDL_AudioSpec *spec, int *sample_frames);
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *st);
float SDL_GetAudioStreamGain(SDL_AudioStream *st);
bool SDL_SetAudioStreamGain(SDL_AudioStream *st, float gain);
Uint32 SDL_GetAudioStreamQueued(SDL_AudioStream *st);
bool SDL_PutAudioStreamData(SDL_AudioStream *st, const void *data, int len);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *st);
void SDL_DestroyAudioStream(SDL_AudioStream *st);
void SDL_free(void *p);
#ifdef __cplusplus
}
#endif

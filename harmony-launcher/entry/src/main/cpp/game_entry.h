// 移植层入口桥：把 XComponent 的窗口与游戏目录交给 liblasrecomp_ohos.so 的 SDL_main。
#pragma once
#include <native_window/external_window.h>

#ifdef __cplusplus
extern "C" {
#endif

// XComponent onLoad 时调用（窗口就绪后）。
void game_entry_set_window(OHNativeWindow *w);

// 注入 OHNativeWindow（移植层 plat 层用 vkCreateSurfaceOHOS 建交换链）。
void letsgo_ohos_set_window(OHNativeWindow *w);

// 专用线程里启动移植层 main（SDL_main）。返回前线程自终止。
// game_dir: 应用沙箱里的数据根（内含 extracted/）；game_lib: 重编译产物 libgamelib 路径。
void game_entry_start(const char *game_dir, const char *game_lib);

// ArkTS 传入两个沙箱目录：hapsFilesDir（UIAbilityContext.filesDir）、
// appFilesDir（ApplicationContext.filesDir）。数据根固定为 appFilesDir/lasrecomp/extracted。
void game_entry_set_dirs(const char *haps_files_dir, const char *app_files_dir);
void game_entry_prepare_dir(const char *haps_files_dir, const char *app_files_dir);

#ifdef __cplusplus
}
#endif

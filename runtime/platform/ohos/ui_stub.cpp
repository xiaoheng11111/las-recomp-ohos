// 第一版 stub：launcher_lg.mm（ImGui 入口菜单）与 osk.mm（屏幕键盘）暂不编入。
// main.c 无游戏数据时会调 launcher_lg_run：这里打日志后返回，进程随 main 退出。
// 后续：ImGui 后端移植（imgui_impl_sdl3 -> imgui_impl_ohos/xcomponent）后替换。
#include <hilog/log.h>

extern "C" void launcher_lg_run(const char *root) {
    (void)root;
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "LasOhos",
                 "launcher_lg_run stub: no game data at %{public}s (push data then restart)", root);
}

// osk 系列空实现（仅当有引用时链接器需要；现无引用，保险起见提供）
bool osk_is_open(void) { return false; }
void osk_cancel(void) {}
bool ui_text_busy(void) { return false; }
bool ui_input_text_cancelled(void) { return false; }
bool ui_input_text(const char *label, char *buf, unsigned long size, unsigned int flags, const char *hint,
                   const char *title) {
    (void)label; (void)buf; (void)size; (void)flags; (void)hint; (void)title;
    return false;
}

// osk_keys（osk.mm 的屏幕键盘处理；menu.mm 调用）：第一版 no-op
int osk_keys(char *buf, unsigned long cap, int max_chars, bool appearing, float scale, struct ImFont *big, bool upper_first) {
    (void)buf; (void)cap; (void)max_chars; (void)appearing; (void)scale; (void)big; (void)upper_first;
    return 0;
}

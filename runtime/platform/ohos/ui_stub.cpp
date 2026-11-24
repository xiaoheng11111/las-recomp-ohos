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

// menu.mm 的 menu_input_captured：overlay 菜单/改名输入是否独占输入。
// 第一版没有 ImGui 覆盖菜单（LARECOMP_NO_LAUNCHER=1 直接进游戏），恒返回 false。
extern "C" bool menu_input_captured(void) { return false; }

// menu.mm（Dear ImGui overlay）其余导出：OHOS 第一版不编 ImGui 菜单，全部空实现。
// menu_is_open/menu_handle_event：不消费事件（游戏自己处理输入）
// menu_apply_window_requests：不修改窗口（全屏由 ArkTS EntryAbility 控制）
// menu_render：不渲染覆盖层
// menu_test_open：测试钩子，无操作
extern "C" bool menu_is_open(void) { return false; }
extern "C" bool menu_handle_event(const void *e) { (void)e; return false; }
extern "C" void menu_apply_window_requests(void *sdl_window) { (void)sdl_window; }
extern "C" void menu_render(void *cmd, unsigned int target_w, unsigned int target_h) {
    (void)cmd; (void)target_w; (void)target_h;
}
extern "C" void menu_test_open(void) {}
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

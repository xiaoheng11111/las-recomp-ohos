/**
 * 原生探针 + 渲染库。
 */

/** 枚举物理设备并返回判定结果（JSON 字符串）。 */
export const getVkInfo: () => string;

/**
 * 探测 HMS 侧图形加速能力（JSON 字符串）。
 * 覆盖 libxengine.so 的 XEG_* 扩展与 libframegeneration.so 的超帧符号。
 */
export const getHmsInfo: () => string;

/**
 * 绑定 XComponent，必须在 onLoad 里调用。
 * 内部会注册 onSurfaceCreated / Changed / Destroyed 并启动渲染线程。
 */
export const attachXComponent: () => void;

/** 启动移植层 SDL_main（专用线程）。 */
export const prepareGameData: (rm: object) => boolean;
export const prepareGameDataDir: (rm: object, hapsFilesDir: string, appFilesDir: string) => boolean;
export const startGame: (rm: object, hapsFilesDir: string, appFilesDir: string) => boolean;

/** 虚拟手柄：写入 SDL shim 的按钮/轴状态（button=SDL_GAMEPAD_BUTTON_*, axis=SDL_GAMEPAD_AXIS_*）。 */
export const padButton: (button: number, down: number) => boolean;
export const padAxis: (axis: number, value: number) => boolean;
export const padReset: () => boolean;

/** 运行时帧率（如 "30 fps | 33 ms"），游戏运行中右上角显示。 */
export const getFps: () => string;

/** 轮询软键盘请求（返回 JSON 字符串，空串=无请求）。 */
export const pollSwkbd: () => string;
/** 提交软键盘输入（text 为空/ok=0 表示取消）。 */
export const submitSwkbd: (text: string, ok: number) => boolean;

/** 数据采集：开始录制（0=已开始，-2=已在录，-1=无数据根，-3=运行时未加载）。 */
export const startCollect: () => number;
/** 数据采集：停止录制，返回摘要文本。 */
export const stopCollect: () => string;

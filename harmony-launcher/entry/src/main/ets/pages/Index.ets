// 第一步：确认鸿蒙原生进程能不能拿到真 GPU，并把画面真正推到屏幕上。
//
// 这里读的是 OHOS 自己的 libvulkan.so 枚举出来的物理设备，不经过 AOSP 兼容层，
// 所以这个结论和「APK 在鸿蒙上跑得怎样」是两件事。
import { pasteboard, zlib } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { picker } from '@kit.CoreFileKit';
import { promptAction, display } from '@kit.ArkUI';
import { getVkInfo, getHmsInfo, startGame, prepareGameDataDir, attachXComponent, padButton, padAxis, padReset, getFps, pollSwkbd, submitSwkbd, startCollect, stopCollect } from 'libentry.so';
import { vibrator } from '@kit.SensorServiceKit';
import { BusinessError } from '@kit.BasicServicesKit';

// 虚拟按键布局：位置（百分比）+ 是否显示。方向键与 ABXY 永远显示（见 padLocked）。
interface PadCfg {
  x: number;
  y: number;
  on: boolean;
}

interface PadEntry {
  code: string;
  label: string;
}

// romfs.bin 解压后的固定大小（gzip 的 ISIZE 只存低 32 位，不能用这个数直接比较）
const ROMFS_SIZE: number = 4430284852;
const APP_FILES: string = '/data/storage/el2/base/files';  // 兜底：ApplicationContext.filesDir

interface SwkbdReq { initial: string; max: number; }

interface DeviceInfo {
  index: number;
  name: string;
  deviceType: string;
  vendorName: string;
  driverID: number;
  driverName: string;
  driverString: string;
  apiVersion: string;
  hasGraphicsQueue: boolean;
  verdict: string;
  reason: string;
  deviceExtCount: number;
  fastPath: string;
  ext: Record<string, boolean>;
  limits: Record<string, number>;
}

interface ProbeResult {
  ok: boolean;
  instanceVersion: string;
  deviceCount: number;
  devices: DeviceInfo[];
  verdict: string;
  reason: string;
  deviceName: string;
  instanceLayerCount: number;
  instanceLayers: string[];
  instanceExtCount: number;
  hasOhosSurface: boolean;
  error?: string;
}

// 决定移植层走哪条提交路径的四个关键扩展
const KEY_EXTS: string[] = ['descriptorBuffer', 'pushDescriptor', 'descriptorIndexing', 'multiDraw'];

// 鸿蒙 HMS 侧图形加速能力的探测结果
interface HmsLib {
  lib: string;
  loaded: boolean;
  error?: string;
  extensions?: string[];
  hasEnumerate?: boolean;
  enumerateResult?: number;
  found?: number;
  total?: number;
}

interface HmsResult {
  ok: boolean;
  xengine: HmsLib;
  framegeneration: HmsLib;
  gamePerformance: HmsLib;
  iGraphicsCore: HmsLib;
  frameFlow: HmsLib;
}

@Entry
@Component
struct Index {
  @State stickX: number = 0;  // 摇杆头偏移（官方触点跟随）
  @State stickY: number = 0;
  @State stickActive: boolean = false;  // 摇杆是否按下（按下才显示圆盘，松手消失）
  @State baseX: number = 200;  // 摇杆底座位置
  @State baseY: number = 800;
  @State overlayName: string = '';  // '' | 'pad' | 'display' | 'about'（全屏卡片漂浮层）
  @State padMode: string = 'stick';  // stick=摇杆方向（默认，官方触点跟随方案）/ dpad=十字键方向
  @State padLayout: string = '';     // 虚拟键布局：code:x%,y%,on ; 空 = 全部默认
  // 显示设置：写入 larecomp.ini，下次「开始游戏」生效
  @State dsRes: number = 0;              // render_height：0=自动，720/1080/1440/2160
  @State dsDynamic: boolean = false;     // resolution=dynamic
  @State dsUpscaler: string = 'bilinear'; // bilinear | fsr1（移植层默认双线性）
  @State dsSmaa: boolean = false;
  @State dsTex: number = 100;            // 材质精度 0..100（纹理各向异性 + mip 偏置）
  @State dsFps60: boolean = false;
  @State dsRenderThread: boolean = true;
  @State dsVertexRead: string = 'copy';  // auto | copy | direct（移植层默认 copy）
  @State dsVkProf: boolean = false;      // LARECOMP_VK_PROF：帧时间诊断日志
  @State dsCollect: boolean = false;     // 数据采集闸门（larecomp.ini: collect=1）：开启后游戏内录制按钮可用
  @State dsTexCheck: boolean = true;     // 纹理校验：开=哈希/比较（默认）；关=不校验直接输出
  @State dsAstcRt: boolean = false;       // ASTC 转码（实测 BC 软解更快，默认关）：开=BC解→ASTC上传（默认）；关=BC解→RGBA直传
  @State dsCollectDelay: number = 10;    // 保留键位兼容（当前不再使用，录制改为按钮手动触发）
  @State recOn: boolean = false;         // 游戏内录制中
  @State recSec: number = 0;             // 已录制秒数
  private recTimer: number = -1;
  @State padEdit: boolean = false;       // 全屏拖拽编辑布局
  @State padEditMode: number = 0;        // 0=拖拽模式（不能隐藏/恢复） 1=隐藏恢复模式（不能拖）
  @State dbgTouch: string = '';          // 拖拽调试读数（确认单位口径用）
  // 拖拽用轻量状态：只改被拖那一个键的 position，避免每次 Move 重建整个布局字符串。
  @State dragCode: string = '';
  @State dragOn: boolean = false;
  @State dragX: number = 0;              // 被拖键的左上角（vp）
  @State dragY: number = 0;
  private grabDX: number = 0;            // 按下时手指相对按钮左上角的偏移（vp）
  private grabDY: number = 0;
  private startBX: number = 0;           // 拖拽开始时按钮左上角（vp）
  private startBY: number = 0;
  private pressAt: number = 0;
  private padSilent: boolean = false;    // 拖动中不写盘，松手再写
  private editW: number = 0;
  private editH: number = 0;
  @State dsAspect: string = 'game';      // game | fill | 43
  @State dsFill: number = 1.245;
  @State dsMovieRes: number = 0;         // movie_max：0=原生，720/540
  @State dsAudoutMs: number = 200;       // audout_ms：音频缓冲
  @State dsMovieAudio: boolean = true;
  @State dsVolume: number = 100;
  @State dsReverb: boolean = true;
  @State dsLang: string = 'zh-CN';   // 游戏语言（默认简体中文）
  @State pressedCodes: string[] = [];  // 当前按下的虚拟按键 code 列表（用于按下反馈）
  @State dataState: string = 'empty';
  @State fpsText: string = '';  // 右上角帧率
  @State swkbdVisible: boolean = false;  // 软键盘浮层
  @State swkbdText: string = '';
  @State swkbdMax: number = 12;
  private swkbdTimer: number = -1;
  private fpsTimer: number = -1;  // empty|importing|unpacking|ready|running
  @State progress: number = 0;         // 0-100
  @State dataHint: string = '';
  @State verdict: string = '';
  @State deviceName: string = '';
  @State reason: string = '';
  @State raw: string = '';
  @State showRaw: boolean = false;
  @State fastPath: string = '';
  @State extFlags: Record<string, boolean> = {};
  @State layerCount: number = 0;
  @State hmsXeg: string = '';
  @State hmsXegExts: string = '';
  @State hmsFg: string = '';
  @State hmsFgDetail: string = '';
  @State hmsRaw: string = '';
  @State copyHint: string = '';
  @State importHint: string = '';
  @State rtLog: string = '';
  private logTimer: number = -1;

  // 沙箱目录一律从 Context 取，不手拼 /data/storage/... 之类的固定路径
  private hapsFilesDir(): string {
    return getContext(this).filesDir;
  }

  private appFilesDir(): string {
    try {
      return getContext(this).getApplicationContext().filesDir;
    } catch (e) {
      return APP_FILES;
    }
  }

  private iniPath(): string {
    return this.appFilesDir() + '/lasrecomp/larecomp.ini';
  }

  // 只改一个键，保留 ini 里其它内容（语言 / 采集 / 按键绑定都不动）
  private iniWrite(key: string, val: string): void {
    try {
      let txt = '';
      try { txt = fs.readTextSync(this.iniPath()); } catch (e) { /* 新建 */ }
      const lines: string[] = (txt.length > 0) ? txt.split('\n') : [];
      const out: string[] = [];
      let done = false;
      for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        if (line.length === 0) continue;
        const eq = line.indexOf(' = ');
        if (eq >= 0 && line.substring(0, eq).trim() === key) {
          out.push(key + ' = ' + val);
          done = true;
        } else {
          out.push(line);
        }
      }
      if (!done) out.push(key + ' = ' + val);
      try { fs.mkdirSync(this.appFilesDir() + '/lasrecomp'); } catch (e) { /* 已存在 */ }
      const file = fs.openSync(this.iniPath(),
        fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      try {
        fs.writeSync(file.fd, out.join('\n') + '\n');
      } finally {
        fs.closeSync(file);
      }
    } catch (e) {
      promptAction.showToast({ message: '保存失败，请检查数据目录' });
    }
  }

  // ---- 虚拟按键布局：默认位置（百分比）----
  // 初始布局按"网格对齐"设计，不再七扭八歪：
  //   左上角一排：L / ZL        顶部偏左：－(select)   顶部偏右：+(start)   右上角一排：ZR / R
  //   左下十字键：↑←→↓ 成菱形    右下 ABXY 成菱形
  private padDefX(code: string): number {
    if (code === 'up' || code === 'down') return 12;
    if (code === 'left') return 4;
    if (code === 'right') return 20;
    if (code === 'x' || code === 'a') return 82;
    if (code === 'y') return 74;
    if (code === 'b') return 90;
    if (code === 'l') return 2;
    if (code === 'zl') return 9;
    if (code === 'select') return 26;
    if (code === 'start') return 62;
    if (code === 'zr') return 79;
    if (code === 'r') return 86;
    return 50;
  }

  private padDefY(code: string): number {
    if (code === 'up' || code === 'x') return 64;
    if (code === 'down' || code === 'a') return 84;
    if (code === 'left' || code === 'right' || code === 'y' || code === 'b') return 74;
    if (code === 'l' || code === 'zl' || code === 'r' || code === 'zr'
        || code === 'start' || code === 'select') return 5;
    return 50;
  }

  // 基础操作键：一律不允许删除。
  // 方向（上下左右 + 摇杆那套）删了不能走路；ABXY 删了不能确认/取消/交互。
  private padLocked(code: string): boolean {
    return code === 'up' || code === 'down' || code === 'left' || code === 'right'
      || code === 'a' || code === 'b' || code === 'x' || code === 'y';
  }

  private padCfg(code: string): PadCfg {
    let x = this.padDefX(code);
    let y = this.padDefY(code);
    let on = true;
    const parts = this.padLayout.length > 0 ? this.padLayout.split(';') : [];
    for (let i = 0; i < parts.length; i++) {
      const t = parts[i];
      if (t.length === 0) continue;
      const f = t.split(':');
      if (f.length !== 2 || f[0] !== code) continue;
      const v = f[1].split(',');
      if (v.length !== 3) continue;
      const px = parseFloat(v[0]);
      const py = parseFloat(v[1]);
      if (!isNaN(px)) x = px;
      if (!isNaN(py)) y = py;
      on = (v[2] === '1');
    }
    if (this.padLocked(code)) on = true;   // 锁定的键永不被隐藏
    const r: PadCfg = { x: x, y: y, on: on };
    return r;
  }

  private padSet(code: string, x: number, y: number, on: boolean): void {
    const px = Math.max(0, Math.min(94, x));
    const py = Math.max(0, Math.min(94, y));
    const keep = this.padLocked(code) ? true : on;
    const parts = this.padLayout.length > 0 ? this.padLayout.split(';') : [];
    const out: string[] = [];
    let done = false;
    for (let i = 0; i < parts.length; i++) {
      const t = parts[i];
      if (t.length === 0) continue;
      const f = t.split(':');
      if (f.length === 2 && f[0] === code) {
        out.push(code + ':' + px.toFixed(1) + ',' + py.toFixed(1) + ',' + (keep ? '1' : '0'));
        done = true;
      } else {
        out.push(t);
      }
    }
    if (!done) out.push(code + ':' + px.toFixed(1) + ',' + py.toFixed(1) + ',' + (keep ? '1' : '0'));
    this.padLayout = out.join(';');
    if (!this.padSilent) {
      this.iniWrite('pad_layout', this.padLayout);
      this.iniWrite('pad_mode', this.padMode);
    }
  }

  private padNudge(code: string, dx: number, dy: number): void {
    const c = this.padCfg(code);
    this.padSet(code, c.x + dx, c.y + dy, c.on);
  }

  private padToggleVisible(code: string): void {
    if (this.padLocked(code)) return;
    const c = this.padCfg(code);
    this.padSet(code, c.x, c.y, !c.on);
  }

  private padResetAll(): void {
    this.padLayout = '';
    this.iniWrite('pad_layout', '');
    promptAction.showToast({ message: '虚拟按键已恢复默认位置' });
  }

  // 「操作方式」面板里可编辑的虚拟键清单（顺序即面板顺序）
  private padEntries(): PadEntry[] {
    const list: PadEntry[] = [
      { code: 'up', label: '↑' }, { code: 'left', label: '←' },
      { code: 'right', label: '→' }, { code: 'down', label: '↓' },
      { code: 'a', label: 'A' }, { code: 'b', label: 'B' },
      { code: 'x', label: 'X' }, { code: 'y', label: 'Y' },
      { code: 'l', label: 'L' }, { code: 'zl', label: 'ZL' },
      { code: 'r', label: 'R' }, { code: 'zr', label: 'ZR' },
      { code: 'start', label: '+' }, { code: 'select', label: '－' }
    ];
    return list;
  }

  // 两个可见按钮是否重叠（按 52vp 的圆形外接方框估算，单位换算成百分比）
  private padOverlap(a: PadCfg, b: PadCfg): boolean {
    if (!a.on || !b.on) return false;
    const sw: number = this.editW > 0 ? 52 / this.editW * 100 : 0;
    const sh: number = this.editH > 0 ? 52 / this.editH * 100 : 0;
    return Math.abs(a.x - b.x) < sw && Math.abs(a.y - b.y) < sh;
  }

  // 编辑器渲染用的坐标：正在拖的那个键用 dragX/dragY（vp），其余用配置的百分比换算成 vp。
  private padDrawX(code: string): number {
    if (this.dragOn && this.dragCode === code) return this.dragX;
    return this.padCfg(code).x * this.editW / 100;
  }

  private padDrawY(code: string): number {
    if (this.dragOn && this.dragCode === code) return this.dragY;
    return this.padCfg(code).y * this.editH / 100;
  }

  private padHasConflict(code: string): boolean {
    const c = this.padCfg(code);
    if (!c.on) return false;
    const all = this.padEntries();
    for (let i = 0; i < all.length; i++) {
      if (all[i].code === code) continue;
      if (this.padOverlap(c, this.padCfg(all[i].code))) return true;
    }
    return false;
  }

  private padAnyConflict(): boolean {
    const all = this.padEntries();
    for (let i = 0; i < all.length; i++) {
      if (this.padHasConflict(all[i].code)) return true;
    }
    return false;
  }

  private padAdvancedInfoCount(): number {
    return this.padAnyConflict() ? 1 : 0;
  }

  // 打开全屏拖拽编辑器
  private padOpenEditor(): void {
    try {
      const d = display.getDefaultDisplaySync();
      // 应用锁横屏：长边=宽、短边=高。统一换算成 vp，和 .position({x:'%'}) 的口径一致。
      const wpx: number = Math.max(d.width, d.height);
      const hpx: number = Math.min(d.width, d.height);
      this.editW = px2vp(wpx) > 0 ? px2vp(wpx) : 1;
      this.editH = px2vp(hpx) > 0 ? px2vp(hpx) : 1;
    } catch (e) {
      this.editW = 1;
      this.editH = 1;
    }
    this.overlayName = '';
    this.padEditMode = 0;
    this.padEdit = true;
  }

  private padSaveEditor(): void {
    this.padSilent = false;
    this.iniWrite('pad_mode', this.padMode);
    this.iniWrite('pad_layout', this.padLayout);
    this.padEdit = false;
    promptAction.showToast({ message: '按钮布局已保存，下次启动继续沿用' });
  }

  // 编辑器里的触摸：
  //   模式 0（拖拽）：按住 350ms 后拖动，松手固定并写盘；点一下不改变显隐。
  //   模式 1（隐藏/恢复）：点一下按钮就切换显隐，绝不能拖动。
  private onEditTouch(e: TouchEvent, code: string): void {
    if (this.padEditMode === 1) {
      if (e.type === TouchType.Down) {
        this.padToggleVisible(code);
        if (!this.padLocked(code)) {
          promptAction.showToast({ message: this.padCfg(code).on ? '已显示' : '已隐藏' });
        }
      }
      return;
    }
    const t: TouchObject = e.touches.length > 0 ? e.touches[0] : e.changedTouches[0];
    const tx: number = px2vp(t.windowX);
    const ty: number = px2vp(t.windowY);
    if (e.type === TouchType.Down) {
      this.dragCode = code;
      this.dragOn = false;
      this.pressAt = Date.now();
      // 记录"手指相对按钮左上角"的偏移：这样按下去不会把按钮吸到手指中心（不会"初始到一个位置"）
      const c = this.padCfg(code);
      this.grabDX = tx - c.x * this.editW / 100;
      this.grabDY = ty - c.y * this.editH / 100;
    } else if (e.type === TouchType.Move) {
      if (this.dragCode !== code) return;
      if (!this.dragOn) {
        if (Date.now() - this.pressAt < 200) return;
        this.dragOn = true;
      }
      // ★ 只改被拖这一个键的 position（一个数字属性），不重建布局字符串、不写盘 —— 这才跟手
      this.dragX = Math.max(0, Math.min(this.editW - 52, tx - this.grabDX));
      this.dragY = Math.max(0, Math.min(this.editH - 52, ty - this.grabDY));
      this.dbgTouch = 'touch=' + tx.toFixed(0) + ',' + ty.toFixed(0)
        + '  edit=' + this.editW.toFixed(0) + ',' + this.editH.toFixed(0)
        + '  btn=' + this.dragX.toFixed(0) + ',' + this.dragY.toFixed(0);
    } else {
      if (this.dragCode === code && this.dragOn) {
        // 松手才提交：vp 坐标换成百分比，写一次 ini
        this.padSet(code, this.dragX / this.editW * 100, this.dragY / this.editH * 100, this.padCfg(code).on);
      }
      this.dragCode = '';
      this.dragOn = false;
    }
  }

  private loadPadSettings(): void {
    this.padMode = 'stick';
    this.padLayout = '';
    try {
      const txt = fs.readTextSync(this.iniPath());
      const lines = txt.split('\n');
      for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const eq = line.indexOf(' = ');
        if (eq < 0) continue;
        const k = line.substring(0, eq).trim();
        const v = line.substring(eq + 3).trim();
        if (k === 'pad_mode') this.padMode = (v === 'dpad') ? 'dpad' : 'stick';
        else if (k === 'pad_layout') this.padLayout = v;
      }
    } catch (e) {
      // 配置尚未生成
    }
  }

  // 读取 larecomp.ini；缺失的键用移植层默认值
  private loadDisplaySettings(): void {
    this.dsRes = 0;
    this.dsDynamic = false;
    this.dsUpscaler = 'bilinear';
    this.dsSmaa = false;
    this.dsTex = 100;
    this.dsFps60 = false;
    this.dsRenderThread = true;
    this.dsVertexRead = 'copy';
    this.dsVkProf = false;
    this.dsCollect = false;
    this.dsCollectDelay = 10;
    this.dsTexCheck = true;
    this.dsAstcRt = false;
    this.dsAspect = 'game';
    this.dsMovieRes = 0;
    this.dsAudoutMs = 200;
    this.dsMovieAudio = true;
    this.dsVolume = 100;
    this.dsReverb = true;
    try {
      const d = display.getDefaultDisplaySync();
      const longSide = Math.max(d.width, d.height);
      const shortSide = Math.min(d.width, d.height);
      if (shortSide > 0) {
        this.dsFill = (longSide / shortSide) / (16 / 9);
      }
    } catch (e) {
      // 拿不到屏幕尺寸时用默认填充比例
    }
    try {
      const txt = fs.readTextSync(this.iniPath());
      const lines = txt.split('\n');
      for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const eq = line.indexOf(' = ');
        if (eq < 0) continue;
        const k = line.substring(0, eq).trim();
        const v = line.substring(eq + 3).trim();
        if (k === 'render_height') {
          const n = parseInt(v, 10);
          this.dsRes = isNaN(n) ? 0 : n;
        } else if (k === 'resolution') {
          this.dsDynamic = (v === 'dynamic');
        } else if (k === 'upscaler') {
          this.dsUpscaler = (v === 'bilinear') ? 'bilinear' : 'fsr1';
        } else if (k === 'smaa') {
          this.dsSmaa = (v !== '0');
        } else if (k === 'tex_quality') {
          const n = parseInt(v, 10);
          this.dsTex = (n >= 0 && n <= 100) ? n : 100;
        } else if (k === 'fps') {
          this.dsFps60 = (parseInt(v, 10) === 60);
        } else if (k === 'render_thread') {
          this.dsRenderThread = (v !== '0');
        } else if (k === 'vertex_read') {
          this.dsVertexRead = (v === 'copy' || v === 'direct') ? v : 'auto';
        } else if (k === 'vk_prof') {
          this.dsVkProf = (v !== '0');
        } else if (k === 'tex_nocheck') {
          this.dsTexCheck = (parseInt(v, 10) === 0);
        } else if (k === 'astc_rt') {
          this.dsAstcRt = (parseInt(v, 10) !== 0);
        } else if (k === 'collect') {
          this.dsCollect = (parseInt(v, 10) > 0);
        } else if (k === 'collect_delay') {
          const n = parseInt(v, 10);
          this.dsCollectDelay = (n >= 1 && n <= 120) ? n : 10;
        } else if (k === 'aspect') {
          this.dsAspect = (v === 'fill' || v === '43') ? v : 'game';
        } else if (k === 'aspect_fill') {
          const f = parseFloat(v);
          if (f > 1.0 && f < 3.0) this.dsFill = f;
        } else if (k === 'movie_audio') {
          this.dsMovieAudio = (v !== '0');
        } else if (k === 'volume') {
          const f = parseFloat(v);
          if (f >= 0 && f <= 2) this.dsVolume = Math.round(f * 100);
        } else if (k === 'reverb') {
          this.dsReverb = (v !== '0');
        } else if (k === 'movie_max') {
          const n = parseInt(v, 10);
          this.dsMovieRes = (n === 720 || n === 540) ? n : 0;
        } else if (k === 'audout_ms') {
          const n = parseInt(v, 10);
          this.dsAudoutMs = (n >= 80 && n <= 600) ? n : 200;
        }
      }
    } catch (e) {
      // 配置文件尚未生成：使用默认值
    }
    if (this.dsRes !== 0) this.dsDynamic = false;
  }

  // 只改显示相关键，保留 larecomp.ini 里的其它内容（mod / 语言 / 按键绑定）
  private saveDisplaySettings(): void {
    const updates: Record<string, string> = {
      'render_height': this.dsDynamic ? '0' : this.dsRes.toString(),
      'resolution': this.dsDynamic ? 'dynamic' : 'match',
      'upscaler': this.dsUpscaler,
      'smaa': this.dsSmaa ? '1' : '0',
      'tex_quality': this.dsTex.toString(),
      'fps': this.dsFps60 ? '60' : '30',
      'render_thread': this.dsRenderThread ? '1' : '0',
      'vertex_read': this.dsVertexRead,
      'tex_nocheck': this.dsTexCheck ? '0' : '1',
      'astc_rt': this.dsAstcRt ? '1' : '0',
      'collect': this.dsCollect ? '1' : '0',
      'collect_delay': this.dsCollectDelay.toString(),
      'aspect': this.dsAspect,
      'aspect_fill': this.dsFill.toFixed(4),
      'movie_max': this.dsMovieRes.toString(),
      'audout_ms': this.dsAudoutMs.toString(),
      'movie_audio': this.dsMovieAudio ? '1' : '0',
      'volume': (this.dsVolume / 100).toFixed(2),
      'reverb': this.dsReverb ? '1' : '0'
    };
    const keys: string[] = ['render_height', 'resolution', 'upscaler', 'smaa', 'tex_quality', 'fps', 'render_thread',
                            'vertex_read', 'tex_nocheck', 'astc_rt', 'collect', 'collect_delay',
                            'aspect', 'aspect_fill', 'movie_max', 'audout_ms',
                            'movie_audio', 'volume', 'reverb'];
    try {
      let txt = '';
      try { txt = fs.readTextSync(this.iniPath()); } catch (e) { /* 新建 */ }
      const lines: string[] = (txt.length > 0) ? txt.split('\n') : [];
      const seen: Record<string, boolean> = {};
      for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const eq = line.indexOf(' = ');
        if (eq < 0) continue;
        const k = line.substring(0, eq).trim();
        if (keys.indexOf(k) >= 0) {
          lines[i] = k + ' = ' + updates[k];
          seen[k] = true;
        }
      }
      for (let i = 0; i < keys.length; i++) {
        const k = keys[i];
        if (!seen[k]) lines.push(k + ' = ' + updates[k]);
      }
      let out = lines.join('\n');
      if (!out.endsWith('\n')) out += '\n';
      try { fs.mkdirSync(this.appFilesDir() + '/lasrecomp'); } catch (e) { /* 已存在 */ }
      const file = fs.openSync(this.iniPath(),
        fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      try {
        fs.writeSync(file.fd, out);
      } finally {
        fs.closeSync(file);
      }
      promptAction.showToast({ message: '显示设置已保存，下次开始游戏生效' });
    } catch (e) {
      promptAction.showToast({ message: '保存失败，请检查数据目录' });
    }
  }

  private loadGameSettings(): void {
    this.dsLang = 'zh-CN';
    try {
      const txt = fs.readTextSync(this.iniPath());
      const lines = txt.split('\n');
      for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const eq = line.indexOf(' = ');
        if (eq < 0) continue;
        const k = line.substring(0, eq).trim();
        const v = line.substring(eq + 3).trim();
        if (k === 'game_lang') this.dsLang = v.length > 0 ? v : 'zh-CN';
      }
    } catch (e) {
      // 配置文件尚未生成：使用默认值
    }
  }

  private saveGameSettings(): void {
    const updates: Record<string, string> = {
      'game_lang': this.dsLang
    };
    const keys: string[] = ['game_lang'];
    try {
      let txt = '';
      try { txt = fs.readTextSync(this.iniPath()); } catch (e) { /* 新建 */ }
      const lines: string[] = (txt.length > 0) ? txt.split('\n') : [];
      const seen: Record<string, boolean> = {};
      for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const eq = line.indexOf(' = ');
        if (eq < 0) continue;
        const k = line.substring(0, eq).trim();
        if (keys.indexOf(k) >= 0) { lines[i] = k + ' = ' + updates[k]; seen[k] = true; }
      }
      for (let i = 0; i < keys.length; i++) {
        const k = keys[i];
        if (!seen[k]) lines.push(k + ' = ' + updates[k]);
      }
      try { fs.mkdirSync(this.appFilesDir() + '/lasrecomp'); } catch (e) { /* 已存在 */ }
      let out = lines.join('\n');
      if (!out.endsWith('\n')) out += '\n';
      const file = fs.openSync(this.iniPath(), fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      try { fs.writeSync(file.fd, out); } finally { fs.closeSync(file); }
      promptAction.showToast({ message: '游戏设置已保存，下次开始游戏生效' });
    } catch (e) {
      promptAction.showToast({ message: '保存失败，请检查数据目录' });
    }
  }

  // ---- 每项设置下方的一行说明（会随当前选择变化）----------------------------------
  private resHint(res: number, dyn: boolean): string {
    if (dyn) return '动态：按帧率自动升降分辨率，可能反复重建目标，稳定性一般';
    if (res === 720) return '720p：GPU 压力最小，掉帧时优先选它（性能最好）';
    if (res === 1080) return '1080p：清晰度和性能的平衡点（推荐）';
    if (res === 1440) return '1440p：更清晰，GPU 压力明显变大';
    if (res === 2160) return '4K：最清晰，也最卡，一般不建议';
    return '自动：按屏幕实际大小渲染（默认）';
  }

  private movieResHint(v: number): string {
    if (v === 540) return '540p：动画上传最省，动画卡顿严重时选它';
    if (v === 720) return '720p：动画清晰度和性能平衡（推荐）';
    return '原生：画面最清晰，但每帧纹理上传最重';
  }

  private upscalerHint(v: string): string {
    return v === 'fsr1'
      ? 'FSR1：画面更锐利，但多一次全屏后处理，更吃 GPU'
      : '双线性：最省 GPU、兼容性最好（性能优先）';
  }

  private smaaHint(on: boolean): string {
    return on ? 'SMAA：边缘更平滑，但多几个全屏 pass，更吃性能' : '关闭：性能最好（推荐）';
  }

  private aspectHint(v: string): string {
    if (v === 'fill') return '拉伸填充：铺满整屏，会横向拉伸；不增加性能开销';
    if (v === '43') return '4:3：画面压到 4:3，左右留黑边；不增加性能开销';
    return '原生 16:9：保持游戏比例，上下留黑边';
  }

  private texHint(q: number): string {
    if (q >= 100) return '100%：最清晰（各向异性 16x，无 mip 偏置）';
    if (q >= 70) return '较高：约 12–16x 各向异性，GPU 压力略增';
    if (q >= 40) return '中等：约 7–9x 各向异性 + 轻微 mip 偏置，性能和清晰度平衡';
    if (q >= 10) return '较低：约 2–4x 各向异性 + 明显 mip 偏置，省纹理带宽但画面变糊';
    return '0%：1x 各向异性 + 最大 mip 偏置，最省 GPU，画面最糊';
  }

  private fpsHint(on: boolean): string {
    return on
      ? '60fps：世界与菜单按 60 帧运行；战斗、捕捉等场景仍按游戏原始的 30 帧。这是游戏本身的设计（它的动画/逻辑按 33.3ms 步进），不是漏洞或渲染异常。'
      : '30fps：游戏原生帧率，最稳、最省电（推荐）。';
  }

  private renderThreadHint(on: boolean): string {
    return on ? '开启：渲染在独立线程提交，通常更顺（推荐）' : '关闭：游戏线程直接提交，调试用，容易掉帧';
  }

  private vertexReadHint(v: string): string {
    if (v === 'direct') return '直读：GPU 直接读 guest 内存，少一次拷贝，但兼容性风险高';
    if (v === 'copy') return '拷贝：最稳（默认）；多一次拷贝，但兼容性最好';
    return '自动：集成 GPU 直读、独显拷贝；本机建议先试拷贝更稳';
  }

  private vkProfHint(on: boolean): string {
    return on ? '开启：hilog 打印 [hitch] 帧时间，仅排查用，平时关掉' : '关闭：不产生诊断日志';
  }

  private movieAudioHint(on: boolean): string {
    return on ? '开启：动画有声音，会多一个音频解码线程' : '关闭：动画静音，省一点 CPU';
  }

  private audoutHint(ms: number): string {
    if (ms >= 300) return '300ms：更抗断续，但延迟更大（声音卡时先试它）';
    if (ms <= 100) return '100ms：延迟最低，但容易 underrun';
    return '200ms：默认平衡';
  }

  private reverbHint(on: boolean): string {
    return on ? '开启：有房间混响效果，略占 CPU' : '关闭：省一点 CPU，声音更干';
  }

  // 一键套用对帧率最友好的组合；画面比例/音量保持不动
  private applyPerfProfile(): void {
    this.dsRes = 720;
    this.dsDynamic = false;
    this.dsMovieRes = 540;
    this.dsUpscaler = 'bilinear';
    this.dsSmaa = false;
    this.dsTex = 60;
    this.dsFps60 = false;
    this.dsRenderThread = true;
    this.dsVertexRead = 'copy';
    this.dsVkProf = false;
    this.dsMovieAudio = true;
    this.dsReverb = false;
    this.dsAudoutMs = 300;
    promptAction.showToast({ message: '已套用性能优先设置，点「保存并关闭」生效' });
  }

  // 轮询移植层日志（stderr 重定向后落到沙箱文件），显示在界面上
  private startSwkbdWatch(): void {
    if (this.swkbdTimer >= 0) return;
    this.swkbdTimer = setInterval(() => {
      try {
        const js: string = pollSwkbd();
        if (js.length > 0 && !this.swkbdVisible) {
          const r = JSON.parse(js) as SwkbdReq;
          this.swkbdText = r.initial;
          this.swkbdMax = r.max > 0 ? r.max : 12;
          this.swkbdVisible = true;
        }
      } catch (e) { /* ignore */ }
    }, 250);
  }

  private closeSwkbd(ok: boolean): void {
    this.swkbdVisible = false;
    try { submitSwkbd(ok ? this.swkbdText : '', ok ? 1 : 0); } catch (e) { /* ignore */ }
  }

  private startFpsWatch(): void {
    if (this.fpsTimer >= 0) return;
    this.fpsTimer = setInterval(() => {
      try { this.fpsText = getFps(); } catch (e) { /* 未就绪 */ }
    }, 500);
  }

  private startLogWatch(): void {
    if (this.logTimer >= 0) return;
    this.logTimer = setInterval(() => {
      try {
        const p = this.hapsFilesDir() + '/lasrecomp_rt.log';
        fs.stat(p).then((st) => {
          if (st.size > 0) {
            fs.readText(p).then((t) => {
              this.rtLog = t.length > 3000 ? '...' + t.substring(t.length - 3000) : t;
            }).catch(() => {});
          }
        }).catch(() => {});
      } catch (e) {
        // 日志文件尚未生成
      }
    }, 1000);
  }

  // 递归拷贝目录（GB 级资源，回调分片做，避免阻塞 UI）
  private async copyDir(srcUri: string, dstPath: string): Promise<number> {
    let count = 0;
    try {
      const isDir = (await fs.stat(srcUri)).isDirectory();
      if (!isDir) {
        await fs.copyFile(srcUri, dstPath + '/' + srcUri.split('/').pop());
        return 1;
      }
      await fs.mkdir(dstPath, true).catch(() => {});
      const items = await fs.listFile(srcUri);
      for (const it of items) {
        const s2 = srcUri + '/' + it;
        const d2 = dstPath + '/' + it;
        const st = await fs.stat(s2);
        if (st.isDirectory()) count += await this.copyDir(s2, d2);
        else { await fs.copyFile(s2, d2); count++; }
      }
    } catch (err) {
      this.importHint = '拷贝失败: ' + (err as Error).message;
    }
    return count;
  }

  // 手机上公共「下载」目录不可直读（getUserDownloadDir 仅 PC/平板可用），
  // 所以走系统文件选择器：拿到 uri 的临时读权限后流式拷进沙箱。
  private async pickAndCopyRomfs(destPath: string): Promise<boolean> {
    try {
      const p = new picker.DocumentViewPicker(getContext(this));
      const opts: picker.DocumentSelectOptions = { maxSelectNumber: 1 };
      const uris: string[] = await p.select(opts);
      if (uris === undefined || uris.length === 0) {
        this.importHint = '未选择文件';
        return false;
      }
      this.dataHint = '正在拷贝数据包，请保持应用在前台…';
      const src = await fs.open(uris[0], fs.OpenMode.READ_ONLY);
      const dst = await fs.open(destPath, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      const buf = new ArrayBuffer(8 * 1024 * 1024);
      let total = 0;
      for (;;) {
        const n = await fs.read(src.fd, buf);
        if (n <= 0) break;
        await fs.write(dst.fd, buf, { length: n });
        total += n;
        this.dataHint = '正在拷贝数据包 ' + (total / 1073741824).toFixed(2) + ' GB';
      }
      await fs.close(src);
      await fs.close(dst);
      return fs.accessSync(destPath);
    } catch (err) {
      this.importHint = '导入失败: ' + (err as Error).message;
      return false;
    }
  }

  private async importGameData(): Promise<void> {
    this.dataState = 'importing';
    this.progress = 0;
    this.dataHint = '正在检查数据包…';
    const haps = this.hapsFilesDir();
    const app = this.appFilesDir();
    const progPath = haps + '/lasrecomp/.progress';
    const hadData = this.romfsReady();
    try { fs.unlinkSync(progPath); } catch (e) { /* 首次无文件 */ }
    try {
      // 已有完整数据时先确认：native 会用新包原子覆盖旧 romfs.bin/exefs，存档与缓存保留
      if (hadData) {
        try {
          const res = await promptAction.showDialog({
            title: '覆盖现有数据？',
            message: '新数据包会覆盖 romfs.bin 与 exefs；存档 save/ 和缓存会保留。',
            buttons: [
              { text: '取消', color: '#666D75' },
              { text: '覆盖导入', color: '#E53935' }
            ]
          });
          if (res.index !== 1) {
            this.dataState = 'ready';
            this.progress = 100;
            this.dataHint = '游戏数据已就绪，点击「开始游戏」';
            return;
          }
        } catch (e) {
          // 弹窗失败：按原逻辑继续导入
        }
      }
      this.progress = -1;
      // 导入按钮是显式操作：即使已有完整数据也允许重新导入，native 会用新解压出的
      // extracted/romfs.bin 覆盖旧文件（支持导入转码后的数据包）
      const pickedPath = await this.pickFileToSandbox(app);
      if (!pickedPath) {
        if (hadData) {
          this.dataState = 'ready';
          this.progress = 100;
          this.dataHint = '游戏数据已就绪，点击「开始游戏」';
        } else {
          this.dataState = 'empty';
          this.dataHint = '未选择数据包。点「导入数据」选择游戏数据 zip 或 romfs.gz';
        }
        return;
      }

      // ③ 判断是 zip 还是 gzip：读文件头魔数
      const isZip = this.isZipFile(pickedPath);
      if (isZip) {
        // zip：解压到数据根，得到 <root>/extracted/{exefs/,romfs.bin}
        this.dataState = 'unpacking';
        this.dataHint = '正在解压游戏数据包（较大，请保持应用在前台）…';
        this.progress = -1;  // zlib 解压没有进度回调：转圈，完成后再显示 100
        const root = app + '/lasrecomp';
        try { fs.mkdirSync(root); } catch (e) { /* 已存在 */ }
        let unzipTimer = -1;
        try {
          // zlib 没有进度回调：轮询 extracted/romfs.bin 的写入大小，给出真实百分比
          unzipTimer = setInterval(() => {
            try {
              const st = fs.statSync(root + '/extracted/romfs.bin');
              if (st.size > 0) {
                const pct = Math.min(99, Math.floor(st.size * 100 / ROMFS_SIZE));
                if (pct > this.progress || this.progress < 0) {
                  this.progress = pct;
                  this.dataHint = '正在解压游戏数据 ' + pct + '%';
                }
              }
            } catch (e) {
              // romfs.bin 还没出现（先解压 exefs 等小文件）
            }
          }, 500);
          await zlib.decompressFile(pickedPath, root, {});
        } catch (err) {
          this.dataState = hadData ? 'ready' : 'empty';
          this.progress = hadData ? 100 : 0;
          this.dataHint = hadData ? '导入失败，旧数据仍可用：' + (err as Error).message
                                  : '解压失败：' + (err as Error).message;
          return;
        } finally {
          if (unzipTimer >= 0) clearInterval(unzipTimer);
        }
        try { fs.unlinkSync(pickedPath); } catch (e) { /* 清理 zip */ }
        // 解压完成：调 native 迁移 extracted/→数据根（异步线程），等 extracted/romfs.bin 被移走才算完成
        this.dataState = 'unpacking';
        this.dataHint = '正在写入数据（迁移 extracted/）…';
        this.progress = 99;
        prepareGameDataDir(getContext(this).resourceManager, haps, app);
        let wait = 0;
        const t2 = setInterval(() => {
          wait++;
          if (this.importDone()) {
            clearInterval(t2);
            this.finalizeCheck();
            return;
          }
          if (wait >= 120) {  // 约 60 秒仍没就位
            clearInterval(t2);
            this.finalizeCheck();
          }
        }, 500);
        return;
      }

      // ④ romfs.gz：走 native 的 gzip 流式解压（带进度）
      this.dataState = 'unpacking';
      this.dataHint = '正在校验数据包完整性…';
      prepareGameDataDir(getContext(this).resourceManager, haps, app);
      let stall = 0;
      let lastPct = -1;
      const timer = setInterval(() => {
        try {
          const txt = fs.readTextSync(progPath);
          const pct = parseInt(txt, 10);
          if (isNaN(pct)) return;
          if (pct < 0) {
            clearInterval(timer);
            this.dataState = 'empty';
            this.dataHint = '数据包校验失败（文件损坏或不完整）。请重新选择完整的 romfs.gz';
            return;
          }
          if (pct === lastPct) { stall++; } else { lastPct = pct; stall = 0; }
          this.progress = Math.min(100, pct);
          this.dataHint = pct >= 100 ? '正在校验解压结果…' : ('正在解压游戏数据 ' + pct + '%');
          if (pct >= 100) {
            clearInterval(timer);
            this.finalizeCheck();
            return;
          }
          if (stall >= 25) {
            clearInterval(timer);
            this.dataState = 'empty';
            this.dataHint = '数据处理无响应，请重新导入';
          }
        } catch (e) { /* 进度文件尚未生成 */ }
      }, 800);
    } catch (err) {
      this.dataState = 'empty';
      this.dataHint = '数据准备失败：' + (err as Error).message;
    }
  }

  private romfsReady(): boolean {
    try {
      return fs.statSync(this.romfsFile()).size === ROMFS_SIZE;
    } catch (e) {
      return false;
    }
  }

  private importDone(): boolean {
    // extracted/romfs.bin 还在 = native 迁移还没完成；旧 romfs.bin 的尺寸不能当完成依据
    try {
      fs.statSync(this.appFilesDir() + '/lasrecomp/extracted/romfs.bin');
      return false;
    } catch (e) {
      // 已被 rename 到数据根
    }
    return this.romfsReady();
  }

  private finalizeCheck(): void {
    if (this.importDone()) {
      this.dataState = 'ready';
      this.progress = 100;
      this.dataHint = '游戏数据已就绪，点击「开始游戏」';
      return;
    }
    if (this.romfsReady()) {  // 迁移失败但旧数据完整：可用，明确提示未完成
      this.dataState = 'ready';
      this.progress = 100;
      this.dataHint = '导入未完成，旧数据仍可用；可重试导入';
      return;
    }
    this.dataState = 'empty';
    this.dataHint = '数据迁移未完成，请重新导入';
  }

  // 读文件头 4 字节判断是否 zip（PK\x03\x04）
  private isZipFile(path: string): boolean {
    try {
      const f = fs.openSync(path, fs.OpenMode.READ_ONLY);
      const buf = new ArrayBuffer(4);
      fs.readSync(f.fd, buf);
      fs.closeSync(f);
      const b = new Uint8Array(buf);
      return b[0] === 0x50 && b[1] === 0x4b && b[2] === 0x03 && b[3] === 0x04;
    } catch (e) {
      return false;
    }
  }

  // 用系统文件选择器选一个文件并拷到沙箱：返回目标路径（失败返回空串）
  private async pickFileToSandbox(app: string): Promise<string> {
    try {
      const p = new picker.DocumentViewPicker(getContext(this));
      const opts: picker.DocumentSelectOptions = { maxSelectNumber: 1 };
      const uris: string[] = await p.select(opts);
      if (uris === undefined || uris.length === 0) {
        this.importHint = '未选择文件';
        return '';
      }
      const dstPath = app + '/lasrecomp/import_data.bin';
      try { fs.mkdirSync(app + '/lasrecomp'); } catch (e) { /* 已存在 */ }
      const src = await fs.open(uris[0], fs.OpenMode.READ_ONLY);
      const dst = await fs.open(dstPath, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      const buf = new ArrayBuffer(8 * 1024 * 1024);
      let total = 0;
      for (;;) {
        const n = await fs.read(src.fd, buf);
        if (n <= 0) break;
        await fs.write(dst.fd, buf, { length: n });
        total += n;
        this.progress = -1;  // 拷贝阶段用不确定进度（转圈），不要卡在 5%
        this.dataHint = '正在拷贝数据包 ' + (total / 1073741824).toFixed(2) + ' GB';
      }
      await fs.close(src);
      await fs.close(dst);
      return dstPath;
    } catch (err) {
      this.importHint = '导入失败: ' + (err as Error).message;
      return '';
    }
  }

  statusShort(): string {
    if (this.dataState === 'empty') return '未导入';
    if (this.dataState === 'importing') return '导入中';
    if (this.dataState === 'unpacking') return '解压中';
    if (this.dataState === 'verifying') return '校验中';
    if (this.dataState === 'failed') return '校验失败';
    if (this.dataState === 'ready') return '就绪';
    return '未导入';
  }

  statusText(): string {
    if (this.dataHint.length > 0 && this.dataState !== 'ready' && this.dataState !== 'running') {
      return this.dataHint;  // 具体错误/进度提示优先
    }
    if (this.dataState === 'empty') return '未导入游戏数据，请点击「导入数据」';
    if (this.dataState === 'importing') return '正在导入游戏数据… ' + this.progress + '%';
    if (this.dataState === 'unpacking') return '正在解压游戏数据… ' + this.progress + '%';
    if (this.dataState === 'verifying') return '正在校验数据完整性…';
    if (this.dataState === 'failed') return '数据校验失败，请重新导入';
    if (this.dataState === 'ready') return '游戏数据已就绪，点击「开始游戏」';
    return '';
  }

  statusColor(): string {
    if (this.dataState === 'ready') return '#3FB950';
    if (this.dataState === 'failed') return '#F85149';
    if (this.dataState === 'importing' || this.dataState === 'unpacking' || this.dataState === 'verifying') return '#58A6FF';
    return '#FFD866';
  }

  loadTitle(): string {
    if (this.dataState === 'importing') return '正在导入游戏数据';
    if (this.dataState === 'unpacking') return '正在解压游戏数据';
    return '正在校验数据完整性';
  }

  overlayTitle(): string {
    if (this.overlayName === 'pad') return '配置游戏操作方式';
    if (this.overlayName === 'display') return '显示设置';
    if (this.overlayName === 'game') return '游戏设置';
    return '关于';
  }

  showPadDialog(): void {
    // 官方 ActionSheet：两种按键体系
    try {
      // 官方 ActionMenuOptions: { title?, buttons: [{text, color}] }
      promptAction.showActionMenu({
        title: '配置游戏操作方式（当前：' + (this.padMode === 'stick' ? '摇杆方向' : '十字键方向') + '）',
        buttons: [
          { text: '摇杆方向（默认）', color: '#58A6FF' },
          { text: '十字键方向', color: '#58A6FF' },
        ],
      }).then((res) => {
        if (res.index === 0) this.padMode = 'stick';
        if (res.index === 1) this.padMode = 'dpad';
      }).catch(() => {});
    } catch (e) { /* 忽略 */ }
  }

  showDisplayDialog(): void {
    try {
      promptAction.showDialog({
        title: '显示设置（预留）',
        message: '图形后端：Vulkan\n设备：NVIDIA RTX 3060\n分辨率：自适应\n帧率上限：60\n\n详细选项（画质/分辨率/滤镜）将在游戏跑通后开放',
        buttons: [{ text: '知道了', color: '#58A6FF' }],
      });
    } catch (e) { /* 忽略 */ }
  }

  showAboutDialog(): void {
    try {
      promptAction.showDialog({
        title: '关于',
        message: 'LAS 启动器（HarmonyOS 版）\nLet\'s Go 重编译运行时\n\n游戏函数 12.3 万 · Vulkan 渲染\n仅供学习交流',
        buttons: [{ text: '知道了', color: '#58A6FF' }],
      });
    } catch (e) { /* 忽略 */ }
  }

  onStickTouch(e: TouchEvent): void {
    if (e.type === TouchType.Up || e.type === TouchType.Cancel) {
      this.stickX = 0;
      this.stickY = 0;
      this.stickActive = false;   // 松手：摇杆圆盘消失
      padAxis(0, 0);  // SDL_GAMEPAD_AXIS_LEFTX
      padAxis(2, 0);  // SDL_GAMEPAD_AXIS_LEFTY
      return;
    }
    if (e.touches.length === 0) return;  // 抬起时 touches 可能为空，先判空再取
    const t = e.touches[0];
    if (e.type === TouchType.Down) {
      this.baseX = t.x;
      this.baseY = t.y;
      this.stickActive = true;  // 按下：在触点位置出现摇杆圆盘
    }
    const dx = t.x - this.baseX, dy = t.y - this.baseY;
    const max = 60;
    const len = Math.sqrt(dx * dx + dy * dy);
    const k = len > max ? max / len : 1;
    this.stickX = dx * k;
    this.stickY = dy * k;
    const nx = (dx * k) / max;
    const ny = (dy * k) / max;
    const near = Math.max(-1, Math.min(1, nx));
    const nearY = Math.max(-1, Math.min(1, ny));
    padAxis(0, near * 32767);   // LEFTX
    padAxis(2, nearY * 32767);  // LEFTY（屏幕向下为正，游戏内 ly=-axis 恰好对应屏幕方向）
  }

  // 全屏编辑器里的按键：长按拖动、右上角 X 隐藏/恢复（锁定的键没有 X）
  @Builder
  padEditBtn(label: string, code: string) {
    Stack() {
      Text(label)
        .fontSize(18)
        .fontColor(this.padCfg(code).on ? '#E6FFFFFF' : '#8B949E')
    }
    .width(52).height(52).borderRadius(26)
    .backgroundColor(this.padCfg(code).on ? '#40FFFFFF' : '#1A000000')
    .border({
      width: this.padHasConflict(code) ? 2 : 1,
      color: this.padHasConflict(code) ? '#FF4D4F'
        : (this.padCfg(code).on ? '#60FFFFFF' : '#8B949E'),
      radius: 26
    })
    .position({ x: this.padDrawX(code), y: this.padDrawY(code) })
    .hitTestBehavior(HitTestMode.Block)
    .onClick(() => {
      if (this.padEditMode === 1) {
        this.buzz();
        this.padToggleVisible(code);
      }
    })
    .gesture(
      GestureGroup(GestureMode.Sequence,
        LongPressGesture({ repeat: false, duration: 200 })
          .onAction(() => {
            if (this.padEditMode === 1) return;
            this.buzz();
            // 起点用配置坐标换算（不要用 padDrawX：那时 dragOn 还没置位）
            this.startBX = this.padCfg(code).x * this.editW / 100;
            this.startBY = this.padCfg(code).y * this.editH / 100;
            this.dragX = this.startBX;
            this.dragY = this.startBY;
            this.dragCode = code;
            this.dragOn = true;
          }),
        PanGesture({ fingers: 1, direction: PanDirection.All, distance: 1 })
          .onActionUpdate((e: GestureEvent) => {
            if (this.padEditMode === 1 || !this.dragOn) return;
            // GestureEvent.offsetX/offsetY 的单位是 vp，和 position 一致 —— 不再做 px2vp 换算
            this.dragX = Math.max(0, Math.min(this.editW - 52, this.startBX + e.offsetX));
            this.dragY = Math.max(0, Math.min(this.editH - 52, this.startBY + e.offsetY));
            this.dbgTouch = 'd=' + e.offsetX.toFixed(0) + ',' + e.offsetY.toFixed(0)
              + '  btn=' + this.dragX.toFixed(0) + ',' + this.dragY.toFixed(0)
              + '  edit=' + this.editW.toFixed(0) + ',' + this.editH.toFixed(0);
          })
          .onActionEnd(() => {
            if (this.padEditMode === 1 || !this.dragOn) return;
            this.padSet(code, this.dragX / this.editW * 100, this.dragY / this.editH * 100, this.padCfg(code).on);
            this.dragCode = '';
            this.dragOn = false;
          })
          .onActionCancel(() => {
            this.dragCode = '';
            this.dragOn = false;
          })
      )
    )
  }

  @Builder
  padBtn(label: string, code: string) {
    // 用 Stack 自绘按键（不用 Button：Button 内部点击手势会吞掉 onTouch 的 Up，
    // 导致按住后无法抬起 → 游戏侧一直读到按下）。
    // 按下时高亮（背景更亮、边框变蓝），松手复原——明确反馈按键是否生效。
    // 位置与显隐来自 pad_layout（在「操作方式」里配置），默认值见 padDefX/padDefY。
    if (this.padCfg(code).on) {
      Stack() {
        Text(label)
          .fontSize(18)
          .fontColor(this.pressedCodes.includes(code) ? '#FFFFFF' : '#E6FFFFFF')
      }
      .width(52)
      .height(52)
      .borderRadius(26)
      .backgroundColor(this.pressedCodes.includes(code) ? '#B358A6FF' : '#40FFFFFF')
      .border({
        width: this.pressedCodes.includes(code) ? 2 : 1,
        color: this.pressedCodes.includes(code) ? '#FF58A6FF' : '#60FFFFFF',
        radius: 26
      })
      .position({ x: this.padCfg(code).x + '%', y: this.padCfg(code).y + '%' })
      .hitTestBehavior(HitTestMode.Block)
      .onTouch((e: TouchEvent) => {
        if (e.type === TouchType.Down) {
          this.buzz();
          if (!this.pressedCodes.includes(code)) { this.pressedCodes.push(code); }
          this.sendPadDown(code);
        } else if (e.type === TouchType.Up || e.type === TouchType.Cancel) {
          this.pressedCodes = this.pressedCodes.filter((c: string) => c !== code);
          this.sendPadUp(code);
        }
      })
    }
  }

  // 虚拟手柄注入：code → SDL_GAMEPAD_BUTTON_* 索引（Switch/Xbox 布局）
  // south=A(0) east=B(1) west=X(2) north=Y(3) back(4) guide(5) start(6)
  // LEFT_STICK=7 RIGHT_STICK=8 L_SHOULDER=9 R_SHOULDER=10 DPAD_UP=11 DOWN=12 LEFT=13 RIGHT=14
  private padBtnIndex(code: string): number {
    // Switch 布局：A 在右(EAST=1)、B 在下(SOUTH=0)、X 在上(NORTH=3)、Y 在左(WEST=2)
    if (code === 'a') return 1;
    if (code === 'b') return 0;
    if (code === 'x') return 3;
    if (code === 'y') return 2;
    if (code === 'select') return 4;
    if (code === 'start') return 6;
    if (code === 'lstick') return 7;
    if (code === 'l') return 9;
    if (code === 'r') return 10;
    if (code === 'up') return 11;
    if (code === 'down') return 12;
    if (code === 'left') return 13;
    if (code === 'right') return 14;
    return -1;
  }

  // Switch 的 ZL/ZR 在 SDL 里走扳机轴（LEFT_TRIGGER=4 / RIGHT_TRIGGER=5）
  private padTriggerAxis(code: string): number {
    if (code === 'zl') return 4;
    if (code === 'zr') return 5;
    return -1;
  }

  sendPadDown(code: string): void {
    const trig = this.padTriggerAxis(code);
    if (trig >= 0) { padAxis(trig, 1.0); return; }
    const idx = this.padBtnIndex(code);
    if (idx >= 0) padButton(idx, 1);
  }

  sendPadUp(code: string): void {
    const trig = this.padTriggerAxis(code);
    if (trig >= 0) { padAxis(trig, 0.0); return; }
    const idx = this.padBtnIndex(code);
    if (idx >= 0) padButton(idx, 0);
  }

  sendPad(code: string): void {
    // 单次点击：按下并立即抬起
    this.sendPadDown(code);
    this.sendPadUp(code);
  }

  private runProbe(): void {
    try {
      const json: string = getVkInfo();
      this.raw = json;
      const r = JSON.parse(json) as ProbeResult;
      if (!r.ok) {
        this.verdict = 'FAIL';
        this.reason = r.error ?? '原生调用失败';
        this.deviceName = '';
        return;
      }
      this.verdict = r.verdict;
      this.deviceName = r.deviceName;
      this.reason = r.reason;
      this.layerCount = r.instanceLayerCount ?? 0;
      if (r.devices && r.devices.length > 0) {
        this.fastPath = r.devices[0].fastPath ?? '';
        this.extFlags = r.devices[0].ext ?? {};
      }
    } catch (err) {
      this.verdict = 'ERROR';
      this.reason = '调用原生库异常: ' + JSON.stringify(err);
    }
  }

  // 探测 HMS 侧图形加速：XEngine（libxengine.so）与超帧（libframegeneration.so）
  private runHmsProbe(): void {
    try {
      const json: string = getHmsInfo();
      this.hmsRaw = json;
      const h = JSON.parse(json) as HmsResult;

      this.hmsXeg = h.xengine.loaded ? '已加载' : '未加载';
      const exts: string[] = h.xengine.extensions ?? [];
      if (exts.length > 0) {
        this.hmsXegExts = exts.join(' · ');
      } else {
        this.hmsXegExts = h.xengine.error ?? '库在，但枚举不到扩展';
      }

      this.hmsFg = h.framegeneration.loaded ? '已加载' : '未加载';
      if (h.framegeneration.loaded) {
        const f: number = h.framegeneration.found ?? 0;
        const t: number = h.framegeneration.total ?? 0;
        this.hmsFgDetail = f.toString() + '/' + t.toString() + ' 个符号可用';
      } else {
        this.hmsFgDetail = h.framegeneration.error ?? '';
      }
    } catch (err) {
      this.hmsXeg = '探测异常';
      this.hmsFg = '探测异常';
      this.hmsXegExts = JSON.stringify(err);
      console.error('HmsProbe 异常: ' + JSON.stringify(err));
    }
  }

  // 一键复制两份原始 JSON（Vulkan 探针 + HMS 探测），方便直接贴回对话
  private copyRaw(): void {
    try {
      const text: string = this.raw + '\n' + this.hmsRaw;
      const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      pasteboard.getSystemPasteboard().setData(data);
      this.copyHint = '已复制';
    } catch (err) {
      this.copyHint = '复制失败';
    }
  }

  private verdictColor(): string {
    if (this.verdict === 'HARDWARE') {
      return '#3FB950';
    }
    if (this.verdict === 'SOFTWARE') {
      return '#F85149';
    }
    return '#D29922';
  }

  private extText(k: string): string {
    return (this.extFlags[k] === true ? '支持 ' : '无 ') + k;
  }

  // romfs.bin 现在直接在数据根下；旧版本的 extracted/ 布局也认（native 启动时会迁移）
  private romfsFile(): string {
    const app = this.appFilesDir();
    const cur = app + '/lasrecomp/romfs.bin';
    try {
      if (fs.statSync(cur).size === ROMFS_SIZE) return cur;
    } catch (e) { /* 不存在 */ }
    const legacy = app + '/lasrecomp/extracted/romfs.bin';
    try {
      if (fs.statSync(legacy).size === ROMFS_SIZE) return legacy;
    } catch (e) { /* 不存在 */ }
    return cur;
  }


  // romfs.gz 既可能是文件，也可能是「解压/文件管理器放出来的一层目录」（里面再放 romfs.gz）
  private resolveRomfsGz(baseDir: string): string {
    const p: string = baseDir + '/romfs.gz';
    try {
      const st = fs.statSync(p);
      if (st.isDirectory()) {
        const inner: string = p + '/romfs.gz';
        try {
          if (fs.statSync(inner).isFile()) return inner;
        } catch (e) {
          // 目录里没有同名文件：继续找最大的文件
        }
        let best: string = '';
        let bestSize: number = 0;
        try {
          const names: string[] = fs.listFileSync(p);
          for (const n of names) {
            const fp: string = p + '/' + n;
            try {
              const s2 = fs.statSync(fp);
              if (s2.isFile() && s2.size > bestSize) {
                bestSize = s2.size;
                best = fp;
              }
            } catch (e) {
              // 跳过不可读项
            }
          }
        } catch (e) {
          // 目录不可读
        }
        return best;
      }
    } catch (e) {
      // 不存在：按文件路径返回
    }
    return p;
  }

  checkDataReady(): void {
    try {
      const st = fs.statSync(this.romfsFile());
      if (st.size === ROMFS_SIZE) {
        this.dataState = 'ready';
        this.progress = 100;
        this.dataHint = '游戏数据已就绪，点击「开始游戏」';
      } else {
        this.dataState = 'empty';
        this.dataHint = '请先导入游戏数据包（romfs.gz）';
      }
    } catch (e) {
      this.dataState = 'empty';
      this.dataHint = '请先导入游戏数据包（romfs.gz）';
    }
  }

  // 按下反馈：30ms 短振。错误码 14600101 = Device operation failed：
  // 官方可能原因包括「触觉管理开关关闭 / 设备被占用 / hdi 异常」。系统键盘能振说明开关是开的，
  // 更可能是三方应用用 'touch' 场景被拦，所以这里按 usage 依次回退，直到有一个成功。
  private lastBuzz: number = 0;
  private buzzErrShown: boolean = false;
  private buzzBest: number = 0;          // 上次成功的 usage 下标，下次优先用它

  private buzzList(): string[] {
    return ['physicalFeedback', 'notification', 'communication', 'touch', 'unknown'];
  }

  private buzz(): void {
    const now: number = Date.now();
    if (now - this.lastBuzz < 80) return;
    this.lastBuzz = now;
    this.buzzTry(this.buzzBest, 0);
  }

  private buzzTry(idx: number, depth: number): void {
    const list: string[] = this.buzzList();
    if (depth >= list.length) {
      if (!this.buzzErrShown) {
        this.buzzErrShown = true;
        promptAction.showToast({ message: '振动不可用：所有场景都被系统拒绝（14600101）' });
      }
      return;
    }
    const i: number = (idx + depth) % list.length;
    const u: vibrator.Usage = list[i] as vibrator.Usage;
    vibrator.startVibration({ type: 'time', duration: 30 }, { id: 0, usage: u })
      .then(() => {
        this.buzzBest = i;
      })
      .catch((err: BusinessError) => {
        this.buzzTry(idx, depth + 1);
      });
  }

  private recFmt(): string {
    const m: number = Math.floor(this.recSec / 60);
    const s: number = this.recSec % 60;
    return (m < 10 ? '0' : '') + m.toString() + ':' + (s < 10 ? '0' : '') + s.toString();
  }

  // 游戏内录制按钮（R 下方）：点一下开始，再点一下停止。
  // 真正干活的是移植层的 las_collect_start/stop（已导出），这里只做闸门与计时显示。
  private toggleRec(): void {
    if (this.recOn) {
      try {
        stopCollect();   // 返回值是给日志看的英文摘要，这里不给用户看
      } catch (e) {
        // 运行时没加载时忽略
      }
      promptAction.showToast({ message: '已停止录制，数据已保存到 collect 目录' });
      this.recOn = false;
      this.recSec = 0;
      if (this.recTimer >= 0) { clearInterval(this.recTimer); this.recTimer = -1; }
      return;
    }
    try {
      const rc: number = startCollect();
      if (rc === 0 || rc === -2) {
        this.recOn = true;
        this.recSec = 0;
        if (this.recTimer < 0) {
          this.recTimer = setInterval(() => { this.recSec = this.recSec + 1; }, 1000);
        }
        promptAction.showToast({ message: rc === 0 ? '开始录制采集数据' : '已经在录制中' });
      } else if (rc === -3) {
        promptAction.showToast({ message: '游戏还没启动，进游戏后再点' });
      } else {
        promptAction.showToast({ message: '录制启动失败（错误 ' + rc.toString() + '）' });
      }
    } catch (e) {
      promptAction.showToast({ message: '录制不可用：运行时尚未加载' });
    }
  }

  aboutToAppear(): void {
    this.checkDataReady();
    this.runProbe();
    this.runHmsProbe();
    // 启动时就把上次保存的配置读进界面：改一次、保存一次，之后每次启动都沿用，
    // 不用再进设置面板重设一遍。移植层本身也从 larecomp.ini 读，两边口径一致。
    this.loadDisplaySettings();
    this.loadGameSettings();
    this.loadPadSettings();
  }

build() {
    Stack() {
      // ① 游戏画面（running 时可见，其他时候被封面盖住）
      XComponent({ id: 'lasSurface', type: XComponentType.SURFACE, libraryname: 'entry' })
        .width('100%')
        .height('100%')
        .hitTestBehavior(HitTestMode.None)
        .onLoad(() => { attachXComponent(); })

      // ② 封面主页（非 running）：全屏封面 + 底部按钮排
      if (this.dataState !== 'running') {
        Image($r('app.media.pokemon_cover'))
          .width('100%')
          .height('100%')
          .objectFit(ImageFit.Cover)
          .position({ x: 0, y: 0 })
        Text('＋ / － 同时按住：退出菜单（当前构建会崩溃，请用系统手势退出）')
          .fontSize(10).fontColor('#B0FFFFFF')
          .backgroundColor('#70000000').borderRadius(6).padding({ left: 8, right: 8, top: 3, bottom: 3 })
          .position({ x: '22%', y: '3%' })

        Column({ space: 8 }) {
          // 开始游戏上方状态提示（未就绪时常显；就绪显示绿字）
          Text(this.statusText())
            .fontSize(12)
            .fontColor(this.statusColor())
            .backgroundColor('#B0000000')
            .borderRadius(4)
            .padding({ left: 10, right: 10, top: 4, bottom: 4 })

          // 五个按钮（除导入/开始外，均打开全屏卡片漂浮层）
          Row({ space: 6 }) {
            // 例外1：开始游戏（未就绪灰、就绪绿；点击直接进游戏）
            Button() {
              Row({ space: 6 }) {
                Text('开始游戏').fontSize(13).fontWeight(FontWeight.Medium)
                  .fontColor('#FFFFFF')
              }
            }
              .height(52)
              .layoutWeight(1.8)
              .borderRadius(26)
              .backgroundColor(this.dataState === 'ready' ? '#E53935' : '#9E9E9E')
              .border({ width: 2, color: '#FFFFFF' })
              .shadow({ radius: 8, color: '#40000000', offsetY: 2 })
              .enabled(this.dataState === 'ready')
              .onClick(() => {
                this.dataState = 'running';
                this.startLogWatch();
                this.startFpsWatch();
                this.startSwkbdWatch();
                padReset();
                startGame(getContext(this).resourceManager, this.hapsFilesDir(), this.appFilesDir());
              })
            // 例外2：导入游戏数据包（触发王者式加载层，不建漂浮层）
            Button() {
              Row({ space: 4 }) {
                Text('导入数据').fontSize(11).fontColor('#C62828')
              }
            }
              .height(52)
              .layoutWeight(1)
              .borderRadius(26)
              .backgroundColor('#F7F7F7')
              .border({ width: 1, color: '#E0E0E0' })
              .shadow({ radius: 6, color: '#30000000', offsetY: 2 })
              .enabled(this.dataState !== 'importing' && this.dataState !== 'unpacking' && this.dataState !== 'verifying')
              .onClick(() => { this.importGameData(); })
            // 漂浮层：操作方式
            Button() {
              Row({ space: 4 }) {
                Text('操作方式').fontSize(11).fontColor('#C62828')
              }
            }
              .height(52)
              .layoutWeight(1)
              .borderRadius(26)
              .backgroundColor('#F7F7F7')
              .border({ width: 1, color: '#E0E0E0' })
              .shadow({ radius: 6, color: '#30000000', offsetY: 2 })
              .onClick(() => { this.overlayName = 'pad'; })
            // 漂浮层：显示设置
            Button() {
              Row({ space: 4 }) {
                Text('显示设置').fontSize(11).fontColor('#C62828')
              }
            }
              .height(52)
              .layoutWeight(1)
              .borderRadius(26)
              .backgroundColor('#F7F7F7')
              .border({ width: 1, color: '#E0E0E0' })
              .shadow({ radius: 6, color: '#30000000', offsetY: 2 })
              .onClick(() => { this.loadDisplaySettings(); this.overlayName = 'display'; })
            // 漂浮层：游戏设置（语言 / 跳过选柄）
            Button() {
              Row({ space: 4 }) {
                Text('游戏设置').fontSize(11).fontColor('#C62828')
              }
            }
              .height(52)
              .layoutWeight(1)
              .borderRadius(26)
              .backgroundColor('#F7F7F7')
              .border({ width: 1, color: '#E0E0E0' })
              .shadow({ radius: 6, color: '#30000000', offsetY: 2 })
              .onClick(() => { this.loadGameSettings(); this.overlayName = 'game'; })
            // 漂浮层：关于
            Button() {
              Row({ space: 4 }) {
                Text('关于').fontSize(11).fontColor('#C62828')
              }
            }
              .height(52)
              .layoutWeight(1)
              .borderRadius(26)
              .backgroundColor('#F7F7F7')
              .border({ width: 1, color: '#E0E0E0' })
              .shadow({ radius: 6, color: '#30000000', offsetY: 2 })
              .onClick(() => { this.overlayName = 'about'; })
          }
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.End)
        .alignItems(HorizontalAlign.Start)
        .padding(16)
      }

      // ③ 王者式全屏加载层（导入/解压/校验——不建漂浮层，直接覆盖）
      if (this.dataState === 'importing' || this.dataState === 'unpacking' || this.dataState === 'verifying') {
        Column({ space: 14 }) {
          Text(this.loadTitle())
            .fontSize(18)
            .fontWeight(FontWeight.Medium)
            .fontColor('#FFFFFF')
          Text(this.dataHint)
            .fontSize(13)
            .fontColor('#8B949E')
          if (this.progress < 0) {
            LoadingProgress().width(48).height(48).color('#E53935')
            Text('正在处理，请保持应用在前台…')
              .fontSize(11).fontColor('#8B949E')
          } else {
            Progress({ value: this.progress, total: 100, type: ProgressType.Linear })
              .width(420)
              .height(8)
              .color('#E53935')
            Text(this.progress + '%')
              .fontSize(28)
              .fontWeight(FontWeight.Bold)
              .fontColor('#3FB950')
          }
          Text('请勿关闭应用，处理完成后「开始游戏」将变为可用')
            .fontSize(11)
            .fontColor('#666D75')
            .margin({ top: 8 })
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.Center)
        .backgroundColor('#E6000000')
      }

      // ④ 全屏卡片漂浮层（操作方式 / 显示设置 / 关于——各自独立）
      if (this.overlayName !== '') {
        Column({ space: 0 }) {
          Row() {
            Text(this.overlayTitle())
              .fontSize(18)
              .fontWeight(FontWeight.Medium)
              .fontColor('#FFFFFF')
              .layoutWeight(1)
            Button('✕')
              .fontSize(16)
              .width(40)
              .height(40)
              .backgroundColor('#E53935')
              .fontColor('#FFFFFF')
              .onClick(() => {
                if (this.overlayName === 'display') this.saveDisplaySettings();
                if (this.overlayName === 'game') this.saveGameSettings();
                this.overlayName = '';
              })
          }
          .width('100%')
          .padding({ bottom: 12 })

          Divider().color('#30363D')

          // 漂浮层内容（每个按钮独立配置）
          if (this.overlayName === 'pad') {
            Column({ space: 10 }) {
              Row({ space: 6 }) {
                Button('摇杆方向')
                  .fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                  .backgroundColor(this.padMode === 'stick' ? '#E53935' : '#30363D')
                  .onClick(() => { this.padMode = 'stick'; this.iniWrite('pad_mode', 'stick'); })
                Button('十字键方向')
                  .fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                  .backgroundColor(this.padMode === 'dpad' ? '#E53935' : '#30363D')
                  .onClick(() => { this.padMode = 'dpad'; this.iniWrite('pad_mode', 'dpad'); })
              }
              .width('100%')
              Text('编辑该示例调整当前的按钮布局')
                .fontSize(12).fontColor('#C9D1D9').width('100%')
              Button('编辑按钮布局')
                .fontSize(12).height(34).width('100%').fontColor('#FFFFFF').backgroundColor('#3B4CCA')
                .onClick(() => { this.padOpenEditor(); })
              Button('恢复默认布局')
                .fontSize(10).height(30).width('100%').fontColor('#FFFFFF').backgroundColor('#30363D')
                .onClick(() => { this.padResetAll(); })
              Text('进入后两种模式：① 拖拽模式（长按拖动、松手固定，底部「保存按钮布局」退出）② 点顶部「隐藏 / 恢复」进入显隐模式，此时点按钮即切换、不能拖，点绿色「保存」结束。')
                .fontSize(9).fontColor('#7D8590').width('100%')
              Text('方向键（↑←→↓ / 摇杆两套）与 ABXY 不可隐藏——删了连走路和确认都做不到。')
                .fontSize(9).fontColor('#D29922').width('100%')
            }
            .width('100%')
            .padding({ top: 12 })
          } else if (this.overlayName === 'display') {
            Scroll() {
              Column({ space: 6 }) {
                Text('图形后端').fontSize(11).fontColor('#8B949E').width('100%')
                Text('Vulkan · ' + (this.deviceName.length > 0 ? this.deviceName : '当前设备 GPU'))
                  .fontSize(12).fontColor('#58A6FF').width('100%')
                Button('套用性能优先设置')
                  .fontSize(12).height(36).width('100%').fontColor('#FFFFFF').backgroundColor('#3B4CCA')
                  .onClick(() => { this.applyPerfProfile(); })
                Text('改成 720p / 动画540p / 双线性 / SMAA关 / 30fps / 渲染线程开 / 顶点拷贝 / 音频300ms；画面比例和音量不变')
                  .fontSize(10).fontColor('#666D75').width('100%')

                Text('分辨率').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('自动').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor((!this.dsDynamic && this.dsRes === 0) ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsDynamic = false; this.dsRes = 0; })
                  Button('720p').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor((!this.dsDynamic && this.dsRes === 720) ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsDynamic = false; this.dsRes = 720; })
                  Button('1080p').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor((!this.dsDynamic && this.dsRes === 1080) ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsDynamic = false; this.dsRes = 1080; })
                }
                .width('100%')
                Row({ space: 6 }) {
                  Button('1440p').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor((!this.dsDynamic && this.dsRes === 1440) ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsDynamic = false; this.dsRes = 1440; })
                  Button('4K').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor((!this.dsDynamic && this.dsRes === 2160) ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsDynamic = false; this.dsRes = 2160; })
                  Button('动态').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsDynamic ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsDynamic = true; this.dsRes = 0; })
                }
                .width('100%')
                Text(this.resHint(this.dsRes, this.dsDynamic))
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Text('动画分辨率').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('原生').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsMovieRes === 0 ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsMovieRes = 0; })
                  Button('720p').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsMovieRes === 720 ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsMovieRes = 720; })
                  Button('540p').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsMovieRes === 540 ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsMovieRes = 540; })
                }
                .width('100%')
                Text(this.movieResHint(this.dsMovieRes))
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Text('滤镜 / 上采样').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('双线性').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsUpscaler === 'bilinear' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsUpscaler = 'bilinear'; })
                  Button('锐化（FSR1）').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsUpscaler === 'fsr1' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsUpscaler = 'fsr1'; })
                }
                .width('100%')
                Text(this.upscalerHint(this.dsUpscaler))
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Text('抗锯齿').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('关闭').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(!this.dsSmaa ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsSmaa = false; })
                  Button('SMAA').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsSmaa ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsSmaa = true; })
                }
                .width('100%')
                Text(this.smaaHint(this.dsSmaa))
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Text('画面比例').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('原生 16:9').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsAspect === 'game' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsAspect = 'game'; })
                  Button('拉伸填充').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsAspect === 'fill' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsAspect = 'fill'; })
                  Button('4:3').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsAspect === '43' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsAspect = '43'; })
                }
                .width('100%')
                Text(this.aspectHint(this.dsAspect))
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Row() {
                  Text('帧率').fontSize(11).fontColor('#8B949E')
                  Blank()
                  Text(this.dsFps60 ? '60 fps' : '30 fps')
                    .fontSize(10).fontColor(this.dsFps60 ? '#3FB950' : '#8B949E')
                  Toggle({ type: ToggleType.Switch, isOn: this.dsFps60 })
                    .selectedColor('#E53935')
                    .width(44).height(24)
                    .margin({ left: 8 })
                    .onChange((on: boolean) => { this.dsFps60 = on; })
                }
                .width('100%')
                Text(this.fpsHint(this.dsFps60))
                  .fontSize(9).fontColor(this.dsFps60 ? '#7D8590' : '#7D8590').width('100%')
                if (this.dsFps60) {
                  Text('说明：只在世界和菜单生效，战斗/捕捉保持 30 帧 —— 游戏自身设计，请勿当作异常反馈。')
                    .fontSize(9).fontColor('#D29922').width('100%')
                }

                Text('渲染线程').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('开启').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsRenderThread ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsRenderThread = true; })
                  Button('关闭').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(!this.dsRenderThread ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsRenderThread = false; })
                }
                .width('100%')
                Text(this.renderThreadHint(this.dsRenderThread))
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Text('顶点读取').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('自动').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsVertexRead === 'auto' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsVertexRead = 'auto'; })
                  Button('拷贝').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsVertexRead === 'copy' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsVertexRead = 'copy'; })
                  Button('直读').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsVertexRead === 'direct' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsVertexRead = 'direct'; })
                }
                .width('100%')
                Text(this.vertexReadHint(this.dsVertexRead))
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Row() {
                  Text('ASTC 转码').fontSize(11).fontColor('#8B949E')
                  Blank()
                  Text(this.dsAstcRt ? '开（BC→ASTC）' : '关（BC→RGBA）')
                    .fontSize(10).fontColor(this.dsAstcRt ? '#8B949E' : '#D29922')
                  Toggle({ type: ToggleType.Switch, isOn: this.dsAstcRt })
                    .selectedColor('#E53935')
                    .width(44).height(24)
                    .margin({ left: 8 })
                    .onChange((on: boolean) => { this.dsAstcRt = on; })
                }
                .width('100%')
                Text(this.dsAstcRt
                  ? '开：解码后再转成 ASTC 上传（省显存/带宽，多一次编码，默认）。'
                  : '关：解码后直接上传 RGBA8（少一层 CPU，但纹理体积大 2~8 倍）。')
                  .fontSize(9).fontColor(this.dsAstcRt ? '#7D8590' : '#D29922').width('100%')

                Row() {
                  Text('纹理校验').fontSize(11).fontColor('#8B949E')
                  Blank()
                  Text(this.dsTexCheck ? '开（哈希/比较）' : '关（直接输出）')
                    .fontSize(10).fontColor(this.dsTexCheck ? '#8B949E' : '#D29922')
                  Toggle({ type: ToggleType.Switch, isOn: this.dsTexCheck })
                    .selectedColor('#E53935')
                    .width(44).height(24)
                    .margin({ left: 8 })
                    .onChange((on: boolean) => { this.dsTexCheck = on; })
                }
                .width('100%')
                Text(this.dsTexCheck
                  ? '开：每帧检测动态纹理内容是否变化（默认，正确但吃 CPU）。'
                  : '关：跳过哈希与比较，动态纹理每帧直接重传（更快，但可能更吃带宽）。')
                  .fontSize(9).fontColor(this.dsTexCheck ? '#7D8590' : '#D29922').width('100%')

                Row() {
                  Text('数据采集').fontSize(11).fontColor('#8B949E')
                  Blank()
                  Text(this.dsCollect ? '开（游戏内可录制）' : '关')
                    .fontSize(10).fontColor(this.dsCollect ? '#3FB950' : '#8B949E')
                  Toggle({ type: ToggleType.Switch, isOn: this.dsCollect })
                    .selectedColor('#E53935')
                    .width(44).height(24)
                    .margin({ left: 8 })
                    .onChange((on: boolean) => { this.dsCollect = on; })
                }
                .width('100%')
                Text('打开后：游戏里 R 键下方出现「录制」按钮，点它开始计时、再点一次停止。每次录制新建 collect/<时间>/（perf.csv / frame.csv）。关掉则不出现按钮、也不产生数据。')
                  .fontSize(9).fontColor(this.dsCollect ? '#3FB950' : '#7D8590').width('100%')

                Text('SMAA / FSR1 在部分 Maleoon 驱动上可能创建失败；保守使用「双线性 + 关闭 SMAA」。')
                  .fontSize(10).fontColor('#666D75').width('100%')

                Divider().color('#30363D').width('100%').margin({ top: 4, bottom: 4 })
                Text('音频').fontSize(13).fontColor('#FFFFFF').width('100%')
                Text('动画音频').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('开启').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsMovieAudio ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsMovieAudio = true; })
                  Button('关闭').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(!this.dsMovieAudio ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsMovieAudio = false; })
                }
                .width('100%')
                Text(this.movieAudioHint(this.dsMovieAudio))
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Text('主音量').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 8 }) {
                  Slider({ value: this.dsVolume, min: 0, max: 100, step: 5, style: SliderStyle.OutSet })
                    .layoutWeight(1)
                    .selectedColor('#E53935')
                    .onChange((value: number, mode: SliderChangeMode) => { this.dsVolume = Math.round(value); })
                  Text(this.dsVolume.toString() + '%')
                    .fontSize(12).fontColor('#E6FFFFFF').width(42).textAlign(TextAlign.End)
                }
                .width('100%')
                Text('主音量：影响游戏和动画音量；100% 为原始音量')
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Text('音频缓冲').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('100ms').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsAudoutMs === 100 ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsAudoutMs = 100; })
                  Button('200ms').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsAudoutMs === 200 ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsAudoutMs = 200; })
                  Button('300ms').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsAudoutMs === 300 ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsAudoutMs = 300; })
                }
                .width('100%')
                Text(this.audoutHint(this.dsAudoutMs))
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Text('混响').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('开启').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsReverb ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsReverb = true; })
                  Button('关闭').fontSize(10).height(30).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(!this.dsReverb ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsReverb = false; })
                }
                .width('100%')
                Text(this.reverbHint(this.dsReverb))
                  .fontSize(9).fontColor('#7D8590').width('100%')
                Text('输出通道：OHAudio（系统默认设备，当前唯一）')
                  .fontSize(10).fontColor('#666D75').width('100%')

                Text('设置写入 larecomp.ini，下次「开始游戏」生效')
                  .fontSize(10).fontColor('#666D75').width('100%').margin({ top: 4 })
                Button('保存并关闭')
                  .fontSize(13).height(38).width('100%').fontColor('#FFFFFF').backgroundColor('#E53935')
                  .onClick(() => { this.saveDisplaySettings(); this.overlayName = ''; })
              }
              .width('100%')
              .padding({ top: 12, bottom: 4 })
            }
            .height(360)
            .width('100%')
            .scrollBar(BarState.Off)
          } else if (this.overlayName === 'game') {
            Scroll() {
              Column({ space: 6 }) {
                Text('游戏语言').fontSize(11).fontColor('#8B949E').width('100%')
                Row({ space: 6 }) {
                  Button('English').fontSize(10).height(36).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsLang === 'en-US' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsLang = 'en-US'; })
                  Button('Español').fontSize(10).height(36).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsLang === 'es' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsLang = 'es'; })
                  Button('Français').fontSize(10).height(36).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsLang === 'fr' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsLang = 'fr'; })
                }
                .width('100%')
                Row({ space: 6 }) {
                  Button('Deutsch').fontSize(10).height(36).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsLang === 'de' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsLang = 'de'; })
                  Button('Italiano').fontSize(10).height(36).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsLang === 'it' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsLang = 'it'; })
                  Button('日本語').fontSize(10).height(36).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsLang === 'ja' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsLang = 'ja'; })
                }
                .width('100%')
                Row({ space: 6 }) {
                  Button('한국어').fontSize(10).height(36).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsLang === 'ko' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsLang = 'ko'; })
                  Button('简体中文').fontSize(10).height(36).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsLang === 'zh-CN' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsLang = 'zh-CN'; })
                  Button('繁體中文').fontSize(10).height(36).layoutWeight(1).fontColor('#FFFFFF')
                    .backgroundColor(this.dsLang === 'zh-TW' ? '#E53935' : '#30363D')
                    .onClick(() => { this.dsLang = 'zh-TW'; })
                }
                .width('100%')
                Text('游戏内文本按此语言加载，需重新开始游戏生效（游戏本体是否响应取决于游戏资源）')
                  .fontSize(9).fontColor('#7D8590').width('100%')

                Text('设置写入 larecomp.ini，下次「开始游戏」生效')
                  .fontSize(10).fontColor('#666D75').width('100%').margin({ top: 4 })
                Button('保存并关闭')
                  .fontSize(13).height(38).width('100%').fontColor('#FFFFFF').backgroundColor('#E53935')
                  .onClick(() => { this.saveGameSettings(); this.overlayName = ''; })
              }
              .width('100%')
              .padding({ top: 12, bottom: 4 })
            }
            .height(360)
            .width('100%')
            .scrollBar(BarState.Off)
          } else if (this.overlayName === 'about') {
            Column({ space: 12 }) {
              Text('LAS 启动器（HarmonyOS 版）').fontSize(15).fontColor('#FFFFFF').width('100%')
              Text('Let\'s Go 重编译运行时').fontSize(13).fontColor('#8B949E').width('100%')
              Text('游戏函数 123,612 · Vulkan 渲染').fontSize(13).fontColor('#8B949E').width('100%')
              Text('仅供学习交流，请支持正版').fontSize(12).fontColor('#666D75').width('100%').margin({ top: 8 })
            }
            .width('100%')
            .padding({ top: 16 })
          }
        }
        .width('78%')
        .padding(20)
        .backgroundColor('#F0161B22')
        .borderRadius(14)
        .border({ width: 1, color: '#30363D' })
        .position({ x: '11%', y: '14%' })
      }

      // ④b 全屏按钮布局编辑器（半黑遮罩，像进了游戏）
      if (this.padEdit) {
        Stack() {
          Column()
            .width('100%')
            .height('100%')
            .backgroundColor('#B0000000')
          // 所有按键（隐藏的也画出来，红边 + 绿色 X，点 X 即恢复）
          // 摇杆模式下方向不需要设置：左半屏任意位置按下就出现摇杆（王者式触点跟随），
          // 所以这里不画十字键；只有切到「十字键方向」才需要摆这 4 个键。
          if (this.padMode === 'dpad') {
            this.padEditBtn('↑', 'up')
            this.padEditBtn('←', 'left')
            this.padEditBtn('→', 'right')
            this.padEditBtn('↓', 'down')
          }
          this.padEditBtn('X', 'x')
          this.padEditBtn('Y', 'y')
          this.padEditBtn('B', 'b')
          this.padEditBtn('A', 'a')
          this.padEditBtn('L', 'l')
          this.padEditBtn('ZL', 'zl')
          this.padEditBtn('R', 'r')
          this.padEditBtn('ZR', 'zr')
          this.padEditBtn('+', 'start')
          this.padEditBtn('－', 'select')
          // 顶部中间：模式切换按钮。
          //  模式 0（拖拽）：蓝底「隐藏 / 恢复」→ 点它进入隐藏恢复模式
          //  模式 1（隐藏恢复）：绿底「保存」→ 点它保存显隐结果并回到拖拽模式
          Row({ space: 10 }) {
            Button(this.padEditMode === 1 ? '保存' : '隐藏 / 恢复')
              .fontSize(12).height(34).width(150).fontColor('#FFFFFF')
              .backgroundColor(this.padEditMode === 1 ? '#2EA043' : '#3B4CCA')
              .onClick(() => {
                if (this.padEditMode === 1) {
                  this.iniWrite('pad_mode', this.padMode);
                  this.iniWrite('pad_layout', this.padLayout);
                  this.padEditMode = 0;
                  promptAction.showToast({ message: '显示/隐藏已保存，回到拖拽模式' });
                } else {
                  this.padEditMode = 1;
                }
              })
          }
          .width('100%')
          .justifyContent(FlexAlign.Center)
          .hitTestBehavior(HitTestMode.None)   // 只让中间的按钮接收触摸，别挡住顶部那排 L/ZL/R/ZR/+/-（否则它们拖不动）
          .position({ x: 0, y: '2%' })

          Text(this.padEditMode === 1
            ? '隐藏/恢复模式：点一下按钮即可切换；此模式不能拖动。点上方绿色「保存」完成。'
            : '拖拽模式：长按 0.2 秒拖动，松手固定；此模式不能隐藏/恢复。点上方「隐藏 / 恢复」切换。')
            .fontSize(11).fontColor('#B0FFFFFF')
            .position({ x: '14%', y: '8%' })
            .hitTestBehavior(HitTestMode.None)
          Text(this.padMode === 'stick'
            ? '摇杆方向：方向键不需要设置（左半屏任意位置按下即出现摇杆）'
            : '十字键方向：可拖动 ↑ ← → ↓ 调整位置')
            .fontSize(10).fontColor('#8B949E')
            .position({ x: '26%', y: '12%' })
            .hitTestBehavior(HitTestMode.None)

          Text(this.dbgTouch)
            .fontSize(9).fontColor('#58A6FF')
            .position({ x: '2%', y: '93%' })
            .hitTestBehavior(HitTestMode.None)
          // 拖拽模式下才显示「保存按钮布局」；进入隐藏恢复模式时它暂时消失。
          if (this.padEditMode === 0) {
            Row() {
              Button('保存按钮布局')
                .fontSize(13).height(40).width(180)
                .fontColor('#FFFFFF')
                .backgroundColor(this.padAnyConflict() ? '#484F58' : '#E53935')
                .enabled(!this.padAnyConflict())
                .onClick(() => { this.padSaveEditor(); })
            }
            .width('100%')
            .justifyContent(FlexAlign.Center)
            .hitTestBehavior(HitTestMode.None)   // 同上：不能挡住底部的 A / ↓ 键
            .position({ x: 0, y: '90%' })
            if (this.padAnyConflict()) {
              Text('按钮重叠了（红圈标出），请先分开再保存')
                .fontSize(10).fontColor('#FF4D4F')
                .position({ x: '38%', y: '96%' })
                .hitTestBehavior(HitTestMode.None)
            }
          }
        }
        .width('100%')
        .height('100%')
        .onAreaChange((oldArea: Area, newArea: Area) => {
          // 用编辑器自身实测尺寸（vp），比 display 推算可靠：容器可能被内边距/安全区裁过
          const w: number = Number(newArea.width);
          const h: number = Number(newArea.height);
          if (w > 0 && h > 0) {
            this.editW = w;
            this.editH = h;
          }
        })
      }
      if (this.dataState === 'running') {
        // 右上角帧率
        Text(this.fpsText)
          .fontSize(13).fontColor('#B0FFFFFF').backgroundColor('#70000000')
          .borderRadius(6).padding({ left: 8, right: 8, top: 3, bottom: 3 })
          .position({ x: '86%', y: '12%' })
        if (this.padMode === 'stick') {
          Rect()
            .width('35%').height('100%')
            .fill(Color.Transparent)
            .position({ x: 0, y: 0 })
            .onTouch((e: TouchEvent) => this.onStickTouch(e))
          if (this.stickActive) {
            Stack() {
              Circle({ width: 120, height: 120 }).fill('#33FFFFFF')
              Circle({ width: 48, height: 48 }).fill('#AAFFFFFF')
                .position({ x: 36 + this.stickX, y: 36 + this.stickY })
            }
            .position({ x: this.baseX - 60, y: this.baseY - 60 })
            .hitTestBehavior(HitTestMode.None)
          }
        } else {
          // 十字键（左下）：▲上 / ◀ ● ▶ / ▼下
          this.padBtn('↑', 'up')
          this.padBtn('←', 'left')
          this.padBtn('→', 'right')
          this.padBtn('↓', 'down')
        }
        // ABXY（右下）：X上 / Y左 B右 / A下
        this.padBtn('X', 'x')
        this.padBtn('Y', 'y')
        this.padBtn('B', 'b')
        this.padBtn('A', 'a')
        // 肩键（上角）
        this.padBtn('L', 'l')
        this.padBtn('ZL', 'zl')
        this.padBtn('R', 'r')
        this.padBtn('ZR', 'zr')
        // 录制按钮：R 正下方。开启「数据采集」闸门后才出现；点一下开始计时，再点一下停止。
        if (this.dsCollect) {
          Stack() {
            Text(this.recOn ? ('REC ' + this.recFmt()) : '录制')
              .fontSize(this.recOn ? 12 : 13)
              .fontColor('#FFFFFF')
          }
          .height(30)
          .constraintSize({ minWidth: 64 })
          .padding({ left: 10, right: 10 })
          .borderRadius(15)
          .backgroundColor(this.recOn ? '#E53935' : '#70000000')
          .border({ width: 1, color: this.recOn ? '#FF8A80' : '#60FFFFFF', radius: 15 })
          .position({ x: '84%', y: '19%' })
          .hitTestBehavior(HitTestMode.Block)
          .onClick(() => { this.toggleRec(); })
        }
        // 菜单键（上中）
        this.padBtn('+', 'start')
        this.padBtn('－', 'select')
        // 退出提示（不挡触控）
        Text('＋/－ 退出菜单会崩溃，请用系统手势退出')
          .fontSize(10).fontColor('#80FFFFFF')
          .position({ x: '35%', y: '2%' })
          .hitTestBehavior(HitTestMode.None)
        // ⑥ 软键盘浮层：游戏取名请求 → 弹鸿蒙原生输入法
        if (this.swkbdVisible) {
          Column({ space: 14 }) {
            Text('输入名字').fontSize(16).fontColor('#FFFFFF')
            TextInput({ text: this.swkbdText, placeholder: '请输入' })
              .maxLength(this.swkbdMax)
              .defaultFocus(true)
              .width('100%')
              .height(44)
              .onChange((v: string) => { this.swkbdText = v; })
              .onSubmit(() => this.closeSwkbd(true))
            Row({ space: 20 }) {
              Button('确定').layoutWeight(1).backgroundColor('#E53935')
                .onClick(() => this.closeSwkbd(true))
              Button('取消').layoutWeight(1).backgroundColor('#30363D')
                .onClick(() => this.closeSwkbd(false))
            }.width('100%')
          }
          .width('56%')
          .padding(20)
          .backgroundColor('#F0161B22')
          .borderRadius(14)
          .border({ width: 1, color: '#58A6FF' })
          .position({ x: '22%', y: '28%' })
        }
      }
    }
    .width('100%')
    .height('100%')
  }
}

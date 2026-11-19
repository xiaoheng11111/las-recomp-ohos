import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

const DOMAIN = 0x0000;
const TAG = 'LasLauncher';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, 'content loaded');
    });
    // 沉浸式全屏：隐藏状态栏和底部导航条（含“小艺”入口），游戏画面铺满整屏
    try {
      const win = windowStage.getMainWindowSync();
      const noBars: Array<'status' | 'navigation'> = [];
      win.setWindowLayoutFullScreen(true).catch((e: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'layoutFullScreen failed: %{public}s', JSON.stringify(e));
      });
      win.setWindowSystemBarEnable(noBars).catch((e: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'hideSystemBars failed: %{public}s', JSON.stringify(e));
      });
      win.setImmersiveModeEnabledState(true);
      win.setWindowKeepScreenOn(true).catch((e: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'keepScreenOn failed: %{public}s', JSON.stringify(e));
      });
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'fullscreen setup failed: %{public}s', JSON.stringify(e));
    }
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onWindowStageDestroy');
  }

  onForeground(): void {}

  onBackground(): void {}

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onDestroy');
  }
}

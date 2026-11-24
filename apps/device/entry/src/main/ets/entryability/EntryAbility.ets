import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import native from 'libdeskmate_device.so';

const DOMAIN: number = 0x3210;
const TAG: string = 'DeskMateAbility';

export default class EntryAbility extends UIAbility {
  onCreate(_want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
    this.context.getApplicationContext().setColorMode(
      ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (error) => {
      if (error.code !== 0) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s',
          JSON.stringify(error));
        return;
      }
      // Immersive full screen: the extended display must not be letterboxed by
      // system bars.
      windowStage.getMainWindow().then((mainWindow: window.Window) => {
        mainWindow.setWindowLayoutFullScreen(true);
        mainWindow.setWindowSystemBarEnable([]);
        mainWindow.setWindowKeepScreenOn(true);
      }).catch((error: Error) => {
        hilog.error(DOMAIN, TAG, 'window setup failed: %{public}s',
          error.message);
      });
    });
  }

  onForeground(): void {
    native.noteLifecycle('foreground');
  }

  // AOA re-enumeration moves the Ability to the background. The native worker
  // keeps the session alive; nothing here tears it down.
  onBackground(): void {
    native.noteLifecycle('background');
  }

  onDestroy(): void {
    native.noteLifecycle('destroy');
    native.stopSession();
  }
}

import { deviceInfo } from '@kit.BasicServicesKit';
import { UIContext, window } from '@kit.ArkUI';

export interface SafeAreaInsets {
  top: number;
  bottom: number;
  left: number;
  right: number;
}

export function isPhoneDevice(): boolean {
  return deviceInfo.deviceType === 'phone' || deviceInfo.deviceType === 'default';
}

export function preferredOrientationForPage(isPhone: boolean, inDisplay: boolean): window.Orientation {
  if (!isPhone) {
    return window.Orientation.UNSPECIFIED;
  }
  return inDisplay ? window.Orientation.AUTO_ROTATION_LANDSCAPE : window.Orientation.PORTRAIT;
}

function safePxToVp(uiContext: UIContext, px: number): number {
  if (px <= 0 || !Number.isFinite(px)) {
    return 0;
  }
  const vp: number = uiContext.px2vp(px);
  return Number.isFinite(vp) && vp > 0 ? vp : 0;
}

export function readSafeAreaInsets(appWindow: window.Window, uiContext: UIContext): SafeAreaInsets {
  let top: number = 0;
  let bottom: number = 0;
  let left: number = 0;
  let right: number = 0;

  try {
    const systemArea: window.AvoidArea = appWindow.getWindowAvoidArea(window.AvoidAreaType.TYPE_SYSTEM);
    top = Math.max(top, safePxToVp(uiContext, systemArea.topRect.height));
    bottom = Math.max(bottom, safePxToVp(uiContext, systemArea.bottomRect.height));
  } catch (error) {
    // Some window states temporarily disallow avoid-area queries.
  }

  try {
    const navArea: window.AvoidArea =
      appWindow.getWindowAvoidArea(window.AvoidAreaType.TYPE_NAVIGATION_INDICATOR);
    bottom = Math.max(bottom, safePxToVp(uiContext, navArea.bottomRect.height));
  } catch (error) {
    // Navigation indicator may not exist on all form factors.
  }

  try {
    const cutoutArea: window.AvoidArea = appWindow.getWindowAvoidArea(window.AvoidAreaType.TYPE_CUTOUT);
    top = Math.max(top, safePxToVp(uiContext, cutoutArea.topRect.height));
    left = Math.max(left, safePxToVp(uiContext, cutoutArea.leftRect.width));
    right = Math.max(right, safePxToVp(uiContext, cutoutArea.rightRect.width));
  } catch (error) {
    // Cutout info is optional.
  }

  return { top, bottom, left, right };
}

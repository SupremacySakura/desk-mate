import { deviceInfo } from '@kit.BasicServicesKit';
import { display, UIContext, window } from '@kit.ArkUI';

export interface SafeAreaInsets {
  top: number;
  bottom: number;
  left: number;
  right: number;
}

export type SafeAreaMode = 'auto' | 'symmetric' | 'fullscreen';

export interface DisplayViewport {
  leftVp: number;
  topVp: number;
  widthVp: number;
  heightVp: number;
  safeLeftVp: number;
  safeRightVp: number;
  safeTopVp: number;
  safeBottomVp: number;
}

export interface DisplayMetricsPx {
  widthPx: number;
  heightPx: number;
  refreshHz: number;
}

const DEFAULT_PHONE_CORNER_RADIUS_VP: number = 28;
const MIN_PHONE_CORNER_RADIUS_VP: number = 24;
const MAX_PHONE_CORNER_RADIUS_VP: number = 56;

export function normalizeSafeAreaMode(value: string): SafeAreaMode {
  if (value === 'symmetric' || value === 'fullscreen') {
    return value;
  }
  return 'auto';
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
    bottom = Math.max(bottom, safePxToVp(uiContext, cutoutArea.bottomRect.height));
    left = Math.max(left, safePxToVp(uiContext, cutoutArea.leftRect.width));
    right = Math.max(right, safePxToVp(uiContext, cutoutArea.rightRect.width));
  } catch (error) {
    // Cutout info is optional.
  }

  return { top, bottom, left, right };
}

export function readCutoutInsets(appWindow: window.Window, uiContext: UIContext): SafeAreaInsets {
  let top: number = 0;
  let bottom: number = 0;
  let left: number = 0;
  let right: number = 0;

  try {
    const cutoutArea: window.AvoidArea = appWindow.getWindowAvoidArea(window.AvoidAreaType.TYPE_CUTOUT);
    top = Math.max(top, safePxToVp(uiContext, cutoutArea.topRect.height));
    bottom = Math.max(bottom, safePxToVp(uiContext, cutoutArea.bottomRect.height));
    left = Math.max(left, safePxToVp(uiContext, cutoutArea.leftRect.width));
    right = Math.max(right, safePxToVp(uiContext, cutoutArea.rightRect.width));
  } catch (error) {
    // Cutout info is optional.
  }

  return { top, bottom, left, right };
}

export function readRoundedCornerRadiusPx(info: display.Display, isPhone: boolean): number {
  let maxRadiusPx: number = 0;
  try {
    const corners: Array<display.RoundedCorner> = info.getRoundedCorner();
    for (let i: number = 0; i < corners.length; i++) {
      const r: number = corners[i].radius;
      if (Number.isFinite(r) && r > maxRadiusPx) {
        maxRadiusPx = r;
      }
    }
  } catch (error) {
    // Devices without rounded-corner configuration fall back cleanly below.
  }
  if (maxRadiusPx > 0) {
    return Math.round(maxRadiusPx);
  }
  if (!isPhone) {
    return 0;
  }
  const density: number = info.densityPixels > 0 ? info.densityPixels : 3;
  return Math.round(DEFAULT_PHONE_CORNER_RADIUS_VP * density);
}

export function readRoundedCornerRadiusVp(uiContext: UIContext, isPhone: boolean): number {
  if (!isPhone) {
    return 0;
  }
  try {
    const info: display.Display = display.getDefaultDisplaySync();
    const radiusPx: number = readRoundedCornerRadiusPx(info, isPhone);
    const radiusVp: number = safePxToVp(uiContext, radiusPx);
    if (radiusVp > 0) {
      return Math.min(MAX_PHONE_CORNER_RADIUS_VP, Math.max(MIN_PHONE_CORNER_RADIUS_VP, radiusVp));
    }
  } catch (error) {
    // Ignore transient display lookup failures.
  }
  return DEFAULT_PHONE_CORNER_RADIUS_VP;
}

function parseResolutionDimensions(resolution: string): Array<number> {
  if (!resolution || resolution.length === 0) {
    return [0, 0];
  }
  const atIndex: number = resolution.indexOf('@');
  const dimsPart: string = atIndex >= 0 ? resolution.substring(0, atIndex) : resolution;
  const xIndex: number = dimsPart.indexOf('x');
  if (xIndex <= 0) {
    return [0, 0];
  }
  const w: number = Number(dimsPart.substring(0, xIndex));
  const h: number = Number(dimsPart.substring(xIndex + 1));
  if (!Number.isFinite(w) || !Number.isFinite(h) || w <= 0 || h <= 0) {
    return [0, 0];
  }
  return [w, h];
}

export function computeDisplayViewport(
  windowWidthVp: number,
  windowHeightVp: number,
  cutout: SafeAreaInsets,
  cornerRadiusVp: number,
  isPhone: boolean,
  mode: SafeAreaMode,
  activeResolution: string
): DisplayViewport {
  const safeWinW: number = windowWidthVp > 1 ? windowWidthVp : 1;
  const safeWinH: number = windowHeightVp > 1 ? windowHeightVp : 1;

  const cornerSafeVp: number = isPhone ? Math.round(cornerRadiusVp * 1.05) + 2 : 0;
  let safeLeft: number = Math.max(cutout.left > 0 ? cutout.left + 4 : 0, cornerSafeVp);
  let safeRight: number = Math.max(cutout.right > 0 ? cutout.right + 4 : 0, cornerSafeVp);
  let safeTop: number = cutout.top > 0 ? cutout.top + 2 : 0;
  let safeBottom: number = cutout.bottom > 0 ? cutout.bottom + 2 : 0;

  if (mode === 'symmetric') {
    const horiz: number = Math.max(safeLeft, safeRight);
    const vert: number = Math.max(safeTop, safeBottom);
    safeLeft = horiz;
    safeRight = horiz;
    safeTop = vert;
    safeBottom = vert;
  }

  if (mode === 'fullscreen') {
    return {
      leftVp: 0,
      topVp: 0,
      widthVp: safeWinW,
      heightVp: safeWinH,
      safeLeftVp: safeLeft,
      safeRightVp: safeRight,
      safeTopVp: safeTop,
      safeBottomVp: safeBottom
    };
  }

  // Clamp total insets so we never invert or collapse the viewport on tiny split windows.
  const maxHoriz: number = Math.max(0, safeWinW - 120);
  if (safeLeft + safeRight > maxHoriz && safeLeft + safeRight > 0) {
    const scale: number = maxHoriz / (safeLeft + safeRight);
    safeLeft = Math.floor(safeLeft * scale);
    safeRight = Math.floor(safeRight * scale);
  }
  const maxVert: number = Math.max(0, safeWinH - 120);
  if (safeTop + safeBottom > maxVert && safeTop + safeBottom > 0) {
    const scale: number = maxVert / (safeTop + safeBottom);
    safeTop = Math.floor(safeTop * scale);
    safeBottom = Math.floor(safeBottom * scale);
  }

  const availW: number = Math.max(1, safeWinW - safeLeft - safeRight);
  const availH: number = Math.max(1, safeWinH - safeTop - safeBottom);

  const parsedDims: Array<number> = parseResolutionDimensions(activeResolution);
  const streamW: number = parsedDims[0];
  const streamH: number = parsedDims[1];

  let viewW: number = availW;
  let viewH: number = availH;
  let leftVp: number = safeLeft;
  let topVp: number = safeTop;

  if (streamW > 0 && streamH > 0) {
    const streamAspect: number = streamW / streamH;
    const availAspect: number = availW / availH;
    const aspectDiffRatio: number = Math.abs(availAspect - streamAspect) / streamAspect;
    if (aspectDiffRatio > 0.018) {
      if (streamAspect > availAspect) {
        viewW = availW;
        viewH = availW / streamAspect;
        leftVp = safeLeft;
        topVp = safeTop + (availH - viewH) / 2;
      } else {
        viewH = availH;
        viewW = availH * streamAspect;
        leftVp = safeLeft + (availW - viewW) / 2;
        topVp = safeTop;
      }
    }
  }

  return {
    leftVp,
    topVp,
    widthVp: Math.max(1, viewW),
    heightVp: Math.max(1, viewH),
    safeLeftVp: safeLeft,
    safeRightVp: safeRight,
    safeTopVp: safeTop,
    safeBottomVp: safeBottom
  };
}

export function computeLandscapeDisplayMetricsPx(
  info: display.Display,
  cutoutInfo: display.CutoutInfo | undefined,
  isPhone: boolean,
  mode: SafeAreaMode
): DisplayMetricsPx {
  const rawW: number = info.width;
  const rawH: number = info.height;
  const landscapeW: number = Math.max(rawW, rawH);
  const landscapeH: number = Math.min(rawW, rawH);
  const refreshHz: number = info.refreshRate > 0 ? Math.round(info.refreshRate) : 60;

  if (mode === 'fullscreen') {
    return {
      widthPx: landscapeW & ~1,
      heightPx: landscapeH & ~1,
      refreshHz
    };
  }

  const density: number = info.densityPixels > 0 ? info.densityPixels : 3;
  const cornerRadiusPx: number = isPhone ? readRoundedCornerRadiusPx(info, true) : 0;
  const cornerSafePx: number = isPhone ? Math.round(cornerRadiusPx * 1.05 + 2 * density) : 0;

  let curLeft: number = 0;
  let curRight: number = 0;
  let curTop: number = 0;
  let curBottom: number = 0;

  if (cutoutInfo !== undefined) {
    const rects: Array<display.Rect> = cutoutInfo.boundingRects;
    for (let i: number = 0; i < rects.length; i++) {
      const r: display.Rect = rects[i];
      if (r.width <= 0 || r.height <= 0) {
        continue;
      }
      // Assign each punch-hole/notch rect to its single closest edge so a
      // portrait top-corner hole only counts as a horizontal side inset in landscape.
      const distTop: number = Math.max(0, r.top);
      const distBottom: number = Math.max(0, rawH - (r.top + r.height));
      const distLeft: number = Math.max(0, r.left);
      const distRight: number = Math.max(0, rawW - (r.left + r.width));
      const minDist: number = Math.min(distTop, distBottom, distLeft, distRight);
      if (minDist === distTop) {
        curTop = Math.max(curTop, r.top + r.height);
      } else if (minDist === distBottom) {
        curBottom = Math.max(curBottom, rawH - r.top);
      } else if (minDist === distLeft) {
        curLeft = Math.max(curLeft, r.left + r.width);
      } else {
        curRight = Math.max(curRight, rawW - r.left);
      }
    }

    const wf: display.WaterfallDisplayAreaRects = cutoutInfo.waterfallDisplayAreaRects;
    curLeft = Math.max(curLeft, wf.left.width);
    curRight = Math.max(curRight, wf.right.width);
    curTop = Math.max(curTop, wf.top.height);
    curBottom = Math.max(curBottom, wf.bottom.height);
  }

  // Project portrait cutouts (top/bottom edges) to landscape side edges.
  const isCurrentlyPortrait: boolean = rawW < rawH;
  const cutLeftPx: number = isCurrentlyPortrait ? curTop : curLeft;
  const cutRightPx: number = isCurrentlyPortrait ? curBottom : curRight;
  const cutTopPx: number = isCurrentlyPortrait ? curLeft : curTop;
  const cutBottomPx: number = isCurrentlyPortrait ? curRight : curBottom;

  const pad4Px: number = Math.round(4 * density);
  const pad2Px: number = Math.round(2 * density);
  let leftPx: number = Math.max(cutLeftPx > 0 ? cutLeftPx + pad4Px : 0, cornerSafePx);
  let rightPx: number = Math.max(cutRightPx > 0 ? cutRightPx + pad4Px : 0, cornerSafePx);
  let topPx: number = cutTopPx > 0 ? cutTopPx + pad2Px : 0;
  let bottomPx: number = cutBottomPx > 0 ? cutBottomPx + pad2Px : 0;

  if (mode === 'symmetric') {
    const horizPx: number = Math.max(leftPx, rightPx);
    const vertPx: number = Math.max(topPx, bottomPx);
    leftPx = horizPx;
    rightPx = horizPx;
    topPx = vertPx;
    bottomPx = vertPx;
  }

  const widthPx: number = Math.max(640, (landscapeW - leftPx - rightPx) & ~7);
  const heightPx: number = Math.max(480, (landscapeH - topPx - bottomPx) & ~7);
  return { widthPx, heightPx, refreshHz };
}

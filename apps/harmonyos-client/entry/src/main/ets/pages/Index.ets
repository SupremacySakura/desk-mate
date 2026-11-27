import { AccessoryLink, LinkSnapshot } from '../deskmate/AccessoryLink';
import {
  isPhoneDevice,
  preferredOrientationForPage,
  readSafeAreaInsets,
  SafeAreaInsets
} from '../deskmate/DeviceLayout';
import {
  APP_DISPLAY_NAME,
  CONTACT_EMAIL,
  DEVELOPER_NAME,
  FALLBACK_BUNDLE_NAME,
  FALLBACK_VERSION_CODE,
  FALLBACK_VERSION_NAME,
  PRIVACY_CONSENT_HIGHLIGHTS,
  PRIVACY_ONE_LINE_COMMITMENT,
  PRIVACY_POLICY_SECTIONS,
  PRIVACY_UPDATED_DATE,
  PrivacySection
} from '../deskmate/PrivacyContent';
import native from 'libdeskmate_device.so';
import { bundleManager } from '@kit.AbilityKit';
import { display, UIContext, window } from '@kit.ArkUI';

PersistentStorage.persistProp('deskMateShowFps', false);
PersistentStorage.persistProp('deskMateCodec', 'auto');
PersistentStorage.persistProp('deskMatePrivacyAccepted', false);

interface EngineSnapshot {
  session: string;
  failure: string;
  profile: string;
  codec: string;
  resolution: string;
  preferredCodec: string;
  rxBytes: number;
  lifecycle: string;
  surface: string;
  framesReceived: number;
  framesDropped: number;
  decoderRendered: number;
  decoderSubmitted: number;
  decoderDropped: number;
  decoderOpens: number;
  inputSent: number;
  inputRejected: number;
  error: string;
}

interface CodecOption {
  value: string;
  title: string;
  subtitle: string;
}

const CODEC_OPTIONS: Array<CodecOption> = [
  { value: 'auto', title: '自动', subtitle: '优先 HEVC' },
  { value: 'hevc', title: 'HEVC', subtitle: 'H.265' },
  { value: 'h264', title: 'H.264', subtitle: 'AVC' }
];

interface DiagnosticRow {
  label: string;
  value: string;
}

type Page = 'home' | 'settings' | 'diagnostics' | 'about' | 'display';

/**
 * Quiet Squircle wired dual-display schematic (Host + USB-C AOA + HarmonyOS tablet/phone).
 */
@Component
struct WiredLinkSchematic {
  @Prop @Watch('redraw') linked: boolean = false;
  @Prop @Watch('redraw') streaming: boolean = false;
  @Prop @Watch('redraw') fps: number = 0;
  @Prop @Watch('redraw') codecText: string = '';
  @Prop @Watch('redraw') resolutionText: string = '';
  @Prop compact: boolean = false;
  @Prop @Watch('redraw') isPhone: boolean = false;
  @StorageProp('deskMateDarkMode') @Watch('redraw') darkMode: boolean = false;

  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings);

  private redraw(): void {
    this.drawDiagram();
  }

  private drawRoundedRect(
    x: number,
    y: number,
    w: number,
    h: number,
    r: number
  ): void {
    this.ctx.beginPath();
    this.ctx.moveTo(x + r, y);
    this.ctx.lineTo(x + w - r, y);
    this.ctx.arcTo(x + w, y, x + w, y + r, r);
    this.ctx.lineTo(x + w, y + h - r);
    this.ctx.arcTo(x + w, y + h, x + w - r, y + h, r);
    this.ctx.lineTo(x + r, y + h);
    this.ctx.arcTo(x, y + h, x, y + h - r, r);
    this.ctx.lineTo(x, y + r);
    this.ctx.arcTo(x, y, x + r, y, r);
    this.ctx.closePath();
  }

  private drawDiagram(): void {
    const w: number = this.ctx.width;
    const h: number = this.ctx.height;
    if (w <= 10 || h <= 10) {
      return;
    }

    this.ctx.clearRect(0, 0, w, h);

    const accentColor: string = this.darkMode ? '#4F8BFF' : '#2563EB';
    const accentSoft: string = this.darkMode ? '#18253D' : '#EFF6FF';
    const positiveColor: string = this.darkMode ? '#34D399' : '#15803D';
    const inkColor: string = this.darkMode ? '#F4F4F5' : '#18181B';
    const inkSecondary: string = this.darkMode ? '#A1A1AA' : '#5A5A60';
    const inkMuted: string = this.darkMode ? '#71717A' : '#8B8B92';
    const surfaceColor: string = this.darkMode ? '#181A1E' : '#FFFFFF';
    const subtleColor: string = this.darkMode ? '#212429' : '#ECECE8';
    const hairlineColor: string = this.darkMode ? '#2A2D33' : '#E2E1DC';
    const activeColor: string = this.streaming
      ? accentColor
      : (this.linked ? positiveColor : inkMuted);

    // Subtle dot grid inside schematic canvas
    const step: number = 16;
    this.ctx.fillStyle = hairlineColor;
    for (let gx: number = 14; gx < w - 10; gx += step) {
      for (let gy: number = 14; gy < h - 10; gy += step) {
        this.ctx.beginPath();
        this.ctx.arc(gx, gy, 0.9, 0, Math.PI * 2);
        this.ctx.fill();
      }
    }

    const cy: number = h * 0.5;
    const macW: number = Math.min(w * (this.compact ? 0.34 : 0.33), this.compact ? 98 : 122);
    const macH: number = macW * 0.64;
    const macX: number = w * (this.compact ? 0.08 : 0.12);
    const macY: number = cy - macH * 0.54;

    const padW: number = Math.min(w * (this.compact ? 0.36 : 0.35), this.compact ? 106 : 132);
    const padH: number = this.isPhone ? padW * 0.58 : padW * 0.68;
    const padX: number = w * (this.compact ? 0.92 : 0.88) - padW;
    const padY: number = cy - padH * 0.46;

    // Smooth USB-C link connector
    const busStartX: number = macX + macW;
    const busStartY: number = cy;
    const busEndX: number = padX;
    const busEndY: number = cy;
    const busMidX: number = (busStartX + busEndX) * 0.5;

    this.ctx.strokeStyle = this.linked ? activeColor : hairlineColor;
    this.ctx.lineWidth = this.linked ? 1.8 : 1.2;
    this.ctx.beginPath();
    this.ctx.moveTo(busStartX, busStartY);
    this.ctx.lineTo(busEndX, busEndY);
    this.ctx.stroke();

    // Central USB-C pill badge
    const pillW: number = 42;
    const pillH: number = 18;
    this.drawRoundedRect(busMidX - pillW * 0.5, cy - pillH * 0.5, pillW, pillH, 9);
    this.ctx.fillStyle = this.linked ? accentSoft : subtleColor;
    this.ctx.fill();
    this.ctx.strokeStyle = this.linked ? activeColor : hairlineColor;
    this.ctx.lineWidth = 1.0;
    this.ctx.stroke();

    this.ctx.fillStyle = this.linked ? activeColor : inkMuted;
    this.ctx.font = '600 9px monospace';
    this.ctx.textAlign = 'center';
    this.ctx.fillText('USB-C', busMidX, cy + 3.2);

    // 1. Primary Mac display tile
    this.drawRoundedRect(macX, macY, macW, macH, 10);
    this.ctx.fillStyle = surfaceColor;
    this.ctx.fill();
    this.ctx.strokeStyle = hairlineColor;
    this.ctx.lineWidth = 1.2;
    this.ctx.stroke();

    const macCenterX: number = macX + macW * 0.5;
    const standBottom: number = macY + macH + 9;
    this.ctx.strokeStyle = inkMuted;
    this.ctx.lineWidth = 1.2;
    this.ctx.beginPath();
    this.ctx.moveTo(macCenterX, macY + macH);
    this.ctx.lineTo(macCenterX, standBottom);
    this.ctx.moveTo(macCenterX - macW * 0.16, standBottom);
    this.ctx.lineTo(macCenterX + macW * 0.16, standBottom);
    this.ctx.stroke();

    this.ctx.fillStyle = inkColor;
    this.ctx.font = this.compact ? '500 10.5px sans-serif' : '600 11.5px sans-serif';
    this.ctx.textAlign = 'center';
    this.ctx.fillText('电脑主屏', macCenterX, macY + macH * 0.48);

    this.ctx.fillStyle = inkMuted;
    this.ctx.font = '400 9.5px monospace';
    this.ctx.fillText('HOST', macCenterX, macY + macH * 0.72);

    // 2. HarmonyOS Tablet / Phone extended display tile
    const cornerRadius: number = this.isPhone ? 12 : 10;
    this.drawRoundedRect(padX, padY, padW, padH, cornerRadius);
    this.ctx.fillStyle = this.streaming ? accentSoft : surfaceColor;
    this.ctx.fill();
    this.ctx.strokeStyle = this.linked ? activeColor : hairlineColor;
    this.ctx.lineWidth = this.linked ? 1.5 : 1.2;
    this.ctx.stroke();

    // Active status dot in top-right of device screen
    this.ctx.fillStyle = activeColor;
    this.ctx.beginPath();
    this.ctx.arc(padX + padW - 10, padY + 10, 2.8, 0, Math.PI * 2);
    this.ctx.fill();

    const padCenterX: number = padX + padW * 0.5;
    this.ctx.fillStyle = this.streaming ? accentColor : inkColor;
    this.ctx.font = this.compact ? '600 10.5px sans-serif' : '600 11.5px sans-serif';
    this.ctx.textAlign = 'center';
    this.ctx.fillText(
      this.streaming && this.fps > 0 ? `${this.fps} FPS` : (this.isPhone ? '手机副屏' : '扩展副屏'),
      padCenterX,
      padY + padH * 0.48
    );

    this.ctx.fillStyle = this.linked ? inkSecondary : inkMuted;
    this.ctx.font = '400 9.5px monospace';
    const subText: string = this.streaming
      ? (this.codecText.length > 0 ? this.codecText : 'LIVE')
      : (this.linked ? 'USB READY' : 'STANDBY');
    this.ctx.fillText(subText, padCenterX, padY + padH * 0.72);
  }

  build() {
    Column({ space: 0 }) {
      Row({ space: 8 }) {
        Text('有线链路拓扑')
          .fontSize(12.5)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.dm_ink'))
          .layoutWeight(1)

        Row({ space: 5 }) {
          Row()
            .width(6)
            .height(6)
            .borderRadius(3)
            .backgroundColor(this.streaming
              ? $r('app.color.dm_accent')
              : (this.linked ? $r('app.color.dm_positive') : $r('app.color.dm_ink_muted')))
          Text(this.streaming ? '推流中' : (this.linked ? 'USB 已连接' : '待连接'))
            .fontSize(11)
            .fontWeight(FontWeight.Medium)
            .fontColor(this.streaming
              ? $r('app.color.dm_accent')
              : (this.linked ? $r('app.color.dm_positive') : $r('app.color.dm_ink_secondary')))
        }
        .padding({ left: 8, right: 8, top: 3, bottom: 3 })
        .backgroundColor(this.streaming
          ? $r('app.color.dm_accent_soft')
          : (this.linked ? $r('app.color.dm_positive_soft') : $r('app.color.dm_subtle')))
        .border({
          width: 0.8,
          color: this.streaming
            ? $r('app.color.dm_accent_border')
            : (this.linked ? $r('app.color.dm_positive_border') : $r('app.color.dm_hairline'))
        })
        .borderRadius(999)
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 11, bottom: 10 })

      Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8)

      Canvas(this.ctx)
        .width('100%')
        .height(this.compact ? (this.isPhone ? 152 : 172) : 210)
        .backgroundColor($r('app.color.dm_canvas'))
        .onReady(() => this.drawDiagram())
        .onAreaChange(() => this.drawDiagram())

      Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8)

      Row({ space: 8 }) {
        Text('USB-C AOA 直连')
          .fontSize(11)
          .fontColor($r('app.color.dm_ink_muted'))
        Text(this.resolutionText.length > 0
          ? `${this.codecText || 'HEVC'} · ${this.resolutionText}`
          : (this.linked ? '正在同步分辨率' : '等待数据线握手'))
          .fontSize(11)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.dm_ink_secondary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .layoutWeight(1)
          .textAlign(TextAlign.End)
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 10, bottom: 10 })
      .backgroundColor($r('app.color.dm_surface'))
    }
    .width('100%')
    .backgroundColor($r('app.color.dm_surface'))
    .border({ width: 0.8, color: $r('app.color.dm_hairline') })
    .borderRadius(16)
    .clip(true)
  }
}

@Entry
@Component
struct Index {
  @State private link: LinkSnapshot =
    { state: 'searching', message: 'starting', accessoryFd: -1 };
  @State private engine: string = '{}';
  @State private page: Page = 'home';
  @State private frameReady: boolean = false;
  @State private controlsExpanded: boolean = false;
  @StorageLink('deskMateShowFps') private showFps: boolean = false;
  @StorageLink('deskMateCodec') private preferredCodec: string = 'auto';
  @StorageLink('deskMatePrivacyAccepted') private privacyAccepted: boolean = false;
  @State private privacyDeclined: boolean = false;
  @State private showConsentFullPolicy: boolean = false;
  @State private appVersionName: string = FALLBACK_VERSION_NAME;
  @State private appVersionCode: number = FALLBACK_VERSION_CODE;
  @State private appBundleName: string = FALLBACK_BUNDLE_NAME;
  @State private fps: number = 0;
  @State private bitrateMbps: number = 0;
  @State private activeCodec: string = '';
  @State private activeResolution: string = '';
  @State private droppedFrames: number = 0;
  @State private codecChangedWhileConnected: boolean = false;
  private readonly isPhone: boolean = isPhoneDevice();
  @State private windowWidthVp: number = isPhoneDevice() ? 390 : 1024;
  @State private windowHeightVp: number = isPhoneDevice() ? 844 : 768;
  @State private safeTopVp: number = 0;
  @State private safeBottomVp: number = 0;
  @State private safeLeftVp: number = 0;
  @State private safeRightVp: number = 0;
  private mainWindow: window.Window | undefined = undefined;
  private lastAppliedOrientation: window.Orientation | undefined = undefined;
  private readonly handleWindowSizeChange = (size: window.Size): void => {
    try {
      const uiContext: UIContext = this.getUIContext();
      if (size.width > 0 && size.height > 0) {
        this.windowWidthVp = uiContext.px2vp(size.width);
        this.windowHeightVp = uiContext.px2vp(size.height);
      }
    } catch (error) {
      // Ignore transient UIContext lookup errors during teardown.
    }
    if (this.mainWindow !== undefined) {
      this.refreshSafeAreas(this.mainWindow);
    }
  };
  private readonly handleAvoidAreaChange = (_options: window.AvoidAreaOptions): void => {
    if (this.mainWindow !== undefined) {
      this.refreshSafeAreas(this.mainWindow);
    }
  };

  private accessoryLink: AccessoryLink =
    new AccessoryLink((snapshot: LinkSnapshot) => {
      if (snapshot.state !== 'connected' && this.link.state === 'connected') {
        this.frameReady = false;
        this.enteredThisLink = false;
        this.fps = 0;
        this.bitrateMbps = 0;
        this.activeCodec = '';
        this.activeResolution = '';
        this.droppedFrames = 0;
        this.codecChangedWhileConnected = false;
        this.lastRendered = 0;
        this.lastRxBytes = 0;
        this.lastSampleAt = 0;
        this.engine = '{}';
        this.syncTouchCapture();
        if (this.page === 'display') {
          this.navigate('home');
        }
      }
      this.link = snapshot;
    });
  private refreshTimer: number = -1;
  private lastRendered: number = 0;
  private lastRxBytes: number = 0;
  private lastSampleAt: number = 0;
  private enteredThisLink: boolean = false;
  private touchCaptureActive: boolean = false;

  private syncTouchCapture(): void {
    const active: boolean =
      this.page === 'display' && this.link.state === 'connected' && this.frameReady;
    if (active !== this.touchCaptureActive) {
      this.touchCaptureActive = active;
      native.setTouchEnabled(active);
    }
  }

  private isCompactLayout(): boolean {
    return this.windowWidthVp < 600;
  }

  private deviceLabel(): string {
    return this.isPhone ? '手机' : '平板';
  }

  private shortStatusLabel(): string {
    if (this.frameReady) {
      return '推流中';
    }
    if (this.link.state === 'connected') {
      return 'USB 已连';
    }
    if (this.link.state === 'authorizing') {
      return '待授权';
    }
    if (this.link.state === 'error') {
      return '异常';
    }
    return '待机';
  }

  private refreshSafeAreas(appWindow: window.Window): void {
    try {
      const insets: SafeAreaInsets = readSafeAreaInsets(appWindow, this.getUIContext());
      this.safeTopVp = insets.top;
      this.safeBottomVp = insets.bottom;
      this.safeLeftVp = insets.left;
      this.safeRightVp = insets.right;
    } catch (error) {
      // Ignore if UIContext is not attached yet.
    }
  }

  private updateWindowMetrics(appWindow: window.Window): void {
    try {
      const props: window.WindowProperties = appWindow.getWindowProperties();
      const uiContext: UIContext = this.getUIContext();
      if (props.windowRect.width > 0 && props.windowRect.height > 0) {
        this.windowWidthVp = uiContext.px2vp(props.windowRect.width);
        this.windowHeightVp = uiContext.px2vp(props.windowRect.height);
      }
    } catch (error) {
      // Keep default fallback dimensions if windowRect query fails.
    }
    this.refreshSafeAreas(appWindow);
  }

  private applyWindowPresentation(appWindow: window.Window): void {
    const bars: Array<'status' | 'navigation'> = this.page === 'display' ? [] : ['status', 'navigation'];
    appWindow.setWindowSystemBarEnable(bars).catch(() => {});
    if (this.isPhone) {
      const target: window.Orientation = preferredOrientationForPage(this.isPhone, this.page === 'display');
      if (target !== this.lastAppliedOrientation) {
        this.lastAppliedOrientation = target;
        appWindow.setPreferredOrientation(target).catch(() => {});
      }
    }
  }

  private syncWindowPresentation(): void {
    if (this.mainWindow !== undefined) {
      this.applyWindowPresentation(this.mainWindow);
      return;
    }
    window.getLastWindow(this.getUIContext().getHostContext()).then((appWindow: window.Window) => {
      this.mainWindow = appWindow;
      this.applyWindowPresentation(appWindow);
    }).catch(() => {});
  }

  private reportDisplayMetrics(): void {
    try {
      const info: display.Display = display.getDefaultDisplaySync();
      if (info.width > 0 && info.height > 0) {
        const landscapeWidth: number = Math.max(info.width, info.height);
        const landscapeHeight: number = Math.min(info.width, info.height);
        const refreshHz: number = info.refreshRate > 0 ? Math.round(info.refreshRate) : 60;
        native.setDisplayMetrics(landscapeWidth, landscapeHeight, refreshHz);
      }
    } catch (error) {
      // Fallback presets inside native DeviceEngine are used if display query is unavailable.
    }
  }

  private selectCodec(codec: string): void {
    if (this.preferredCodec === codec) {
      return;
    }
    this.preferredCodec = codec;
    native.setPreferredCodec(codec);
    if (this.link.state === 'connected') {
      this.codecChangedWhileConnected = true;
    }
  }

  private loadBundleMetadata(): void {
    try {
      const info: bundleManager.BundleInfo =
        bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT);
      if (info.versionName && info.versionName.length > 0) {
        this.appVersionName = info.versionName;
      }
      if (info.versionCode > 0) {
        this.appVersionCode = info.versionCode;
      }
      if (info.name && info.name.length > 0) {
        this.appBundleName = info.name;
      }
    } catch (error) {
      // Keep fallback metadata if bundleManager query is unavailable.
    }
  }

  private startAccessoryServices(): void {
    this.accessoryLink.start();
    if (this.refreshTimer >= 0) {
      return;
    }
    this.refreshTimer = setInterval(() => {
      const json: string = native.snapshotJson();
      if (json !== this.engine) {
        this.engine = json;
      }
      const parsed: EngineSnapshot | undefined = this.parseEngine();
      if (this.link.state !== 'connected' || parsed === undefined) {
        return;
      }
      this.updateTelemetry(parsed);
      if (parsed.session === 'streaming' && parsed.surface === 'attached' && parsed.decoderRendered > 0) {
        this.frameReady = true;
        if (!this.enteredThisLink) {
          this.enteredThisLink = true;
          this.navigate('display');
        }
      } else if (parsed.session !== 'streaming' && parsed.session !== 'paused') {
        this.frameReady = false;
      }
      this.syncTouchCapture();
    }, 250);
  }

  private stopAccessoryServices(): void {
    if (this.refreshTimer >= 0) {
      clearInterval(this.refreshTimer);
      this.refreshTimer = -1;
    }
    this.accessoryLink.stop();
  }

  private acceptPrivacyPolicy(): void {
    this.privacyDeclined = false;
    this.showConsentFullPolicy = false;
    this.privacyAccepted = true;
    this.startAccessoryServices();
  }

  private declinePrivacyPolicy(): void {
    this.privacyDeclined = true;
    this.stopAccessoryServices();
  }

  aboutToAppear(): void {
    this.loadBundleMetadata();
    this.reportDisplayMetrics();
    native.setPreferredCodec(this.preferredCodec);
    window.getLastWindow(this.getUIContext().getHostContext()).then((appWindow: window.Window) => {
      this.mainWindow = appWindow;
      this.updateWindowMetrics(appWindow);
      appWindow.on('windowSizeChange', this.handleWindowSizeChange);
      appWindow.on('avoidAreaChange', this.handleAvoidAreaChange);
      this.applyWindowPresentation(appWindow);
    }).catch(() => {});
    if (this.privacyAccepted) {
      this.startAccessoryServices();
    }
  }

  aboutToDisappear(): void {
    if (this.mainWindow !== undefined) {
      try {
        this.mainWindow.off('windowSizeChange', this.handleWindowSizeChange);
        this.mainWindow.off('avoidAreaChange', this.handleAvoidAreaChange);
      } catch (error) {
        // Window may already be destroyed.
      }
      this.mainWindow = undefined;
    }
    if (this.touchCaptureActive) {
      this.touchCaptureActive = false;
      native.setTouchEnabled(false);
    }
    this.stopAccessoryServices();
  }

  private parseEngine(): EngineSnapshot | undefined {
    if (this.engine === '{}') {
      return undefined;
    }
    try {
      return JSON.parse(this.engine) as EngineSnapshot;
    } catch (error) {
      return undefined;
    }
  }

  private updateTelemetry(parsed: EngineSnapshot | undefined): void {
    if (parsed === undefined) {
      this.fps = 0;
      this.bitrateMbps = 0;
      this.activeCodec = '';
      this.activeResolution = '';
      this.droppedFrames = 0;
      return;
    }
    this.activeCodec = parsed.codec || '';
    this.activeResolution = parsed.resolution || '';
    this.droppedFrames = (parsed.framesDropped || 0) + (parsed.decoderDropped || 0);

    const now: number = Date.now();
    const rendered: number = parsed.decoderRendered || 0;
    const rxBytes: number = parsed.rxBytes || 0;
    if (this.lastSampleAt === 0 || rendered < this.lastRendered || rxBytes < this.lastRxBytes) {
      this.lastRendered = rendered;
      this.lastRxBytes = rxBytes;
      this.lastSampleAt = now;
      return;
    }
    const elapsedMs: number = now - this.lastSampleAt;
    if (elapsedMs < 450) {
      return;
    }
    const frameDelta: number = rendered - this.lastRendered;
    const byteDelta: number = rxBytes - this.lastRxBytes;
    this.fps = Math.round(frameDelta * 1000 / elapsedMs);
    this.bitrateMbps = (byteDelta * 8) / (elapsedMs * 1000);
    this.lastRendered = rendered;
    this.lastRxBytes = rxBytes;
    this.lastSampleAt = now;
  }

  private navigate(page: Page): void {
    this.controlsExpanded = false;
    this.page = page;
    this.syncTouchCapture();
    this.syncWindowPresentation();
  }

  onBackPress(): boolean {
    if (!this.privacyAccepted) {
      if (this.showConsentFullPolicy) {
        this.showConsentFullPolicy = false;
        return true;
      }
      return false;
    }
    if (this.page === 'diagnostics' || this.page === 'about') {
      this.navigate('settings');
      return true;
    }
    if (this.page !== 'home') {
      this.navigate('home');
      return true;
    }
    return false;
  }

  private statusTitle(): string {
    if (this.link.state === 'error') {
      return this.link.message.includes('permission') ? '需要 USB 配件授权' : '暂时无法连接';
    }
    if (this.link.state === 'authorizing') {
      return '等待 USB 配件授权';
    }
    if (this.link.state === 'connected') {
      return this.frameReady ? '扩展桌面已就绪' : '正在准备画面';
    }
    return '连接数据线即可扩展';
  }

  private statusDetail(): string {
    if (this.link.state === 'error') {
      return this.link.message.includes('permission')
        ? '点按下方重试，并在系统提示弹窗中允许 USB 配件连接。'
        : '请检查电脑端 DeskMate 状态与 USB 数据线连接后重试。';
    }
    if (this.link.state === 'authorizing') {
      return '请在系统提示弹窗中点击允许，建立高速 USB 配件通信通道。';
    }
    if (this.link.state === 'connected') {
      return this.frameReady
        ? '已建立低延迟有线扩展桌面，可随时进入全屏画面与反向触控。'
        : 'USB 已连接，正在协商编码格式与虚拟显示器首帧画面。';
    }
    if (this.isPhone) {
      return '连接电脑后将自动匹配手机比例创建虚拟扩展屏，出画后自动切换横屏，支持低延迟画面与反向触控。';
    }
    return '连接电脑后将自动匹配平板比例创建虚拟扩展屏，支持低延迟画面与反向触控。';
  }

  private badgeFgColor(): Resource {
    if (this.frameReady) {
      return $r('app.color.dm_accent');
    }
    if (this.link.state === 'connected') {
      return $r('app.color.dm_positive');
    }
    if (this.link.state === 'authorizing') {
      return $r('app.color.dm_warning');
    }
    if (this.link.state === 'error') {
      return $r('app.color.dm_danger');
    }
    return $r('app.color.dm_ink_secondary');
  }

  private badgeBgColor(): Resource {
    if (this.frameReady) {
      return $r('app.color.dm_accent_soft');
    }
    if (this.link.state === 'connected') {
      return $r('app.color.dm_positive_soft');
    }
    if (this.link.state === 'authorizing') {
      return $r('app.color.dm_warning_soft');
    }
    if (this.link.state === 'error') {
      return $r('app.color.dm_danger_soft');
    }
    return $r('app.color.dm_subtle');
  }

  private badgeBorderColor(): Resource {
    if (this.frameReady) {
      return $r('app.color.dm_accent_border');
    }
    if (this.link.state === 'connected') {
      return $r('app.color.dm_positive_border');
    }
    if (this.link.state === 'authorizing') {
      return $r('app.color.dm_warning_border');
    }
    if (this.link.state === 'error') {
      return $r('app.color.dm_danger_border');
    }
    return $r('app.color.dm_hairline');
  }

  private statusDotColor(): Resource {
    if (this.frameReady) {
      return $r('app.color.dm_accent');
    }
    if (this.link.state === 'connected') {
      return $r('app.color.dm_positive');
    }
    if (this.link.state === 'authorizing') {
      return $r('app.color.dm_warning');
    }
    if (this.link.state === 'error') {
      return $r('app.color.dm_danger');
    }
    return $r('app.color.dm_ink_muted');
  }

  private codecDisplayName(value: string): string {
    if (value === 'hevc') {
      return 'HEVC (H.265)';
    }
    if (value === 'h264') {
      return 'H.264 (AVC)';
    }
    return '自动（优先 HEVC）';
  }

  private diagnosticRows(): Array<DiagnosticRow> {
    const parsed: EngineSnapshot | undefined = this.parseEngine();
    if (parsed === undefined) {
      return [
        { label: '连接状态', value: this.statusTitle() },
        { label: 'USB 链路', value: this.link.message },
        { label: '偏好编码', value: this.codecDisplayName(this.preferredCodec) },
        { label: '会话快照', value: '暂无会话数据' }
      ];
    }
    const dropped: number = (parsed.framesDropped || 0) + (parsed.decoderDropped || 0);
    return [
      { label: '连接状态', value: this.statusTitle() },
      { label: 'USB 链路', value: this.link.message },
      { label: '会话阶段', value: parsed.session },
      { label: '协商编码', value: `${parsed.codec || '未协商'}（偏好：${this.codecDisplayName(parsed.preferredCodec || this.preferredCodec)}）` },
      { label: '输出分辨率', value: parsed.resolution || parsed.profile || '未协商' },
      { label: '实时性能', value: `${this.fps} FPS · ${this.bitrateMbps.toFixed(1)} Mbps` },
      { label: '已渲染帧', value: `${parsed.decoderRendered} 帧` },
      { label: '丢帧统计', value: `${dropped} 帧` },
      { label: '触控转发', value: `已发送 ${parsed.inputSent} · 丢弃 ${parsed.inputRejected}` },
      { label: '失败原因', value: parsed.failure.length > 0 ? parsed.failure : '无' },
      { label: '错误详情', value: parsed.error.length > 0 ? parsed.error : '无' }
    ];
  }

  @Builder
  private sectionHeader(title: string) {
    Text(title)
      .fontSize(13)
      .fontWeight(FontWeight.Medium)
      .fontColor($r('app.color.dm_ink'))
      .width('100%')
      .margin({ top: 4, bottom: 2 })
  }

  @Builder
  private primaryButton(label: string, onTap: () => void, fullWidth: boolean) {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 8 }) {
        Text(label)
          .fontSize(13.5)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.dm_accent_fg'))
        Text('→')
          .fontSize(13.5)
          .fontColor($r('app.color.dm_accent_fg'))
      }
      .width(fullWidth ? '100%' : 'auto')
      .justifyContent(FlexAlign.Center)
      .padding({
        left: fullWidth ? 18 : 22,
        right: fullWidth ? 18 : 22,
        top: fullWidth ? 12 : 10,
        bottom: fullWidth ? 12 : 10
      })
    }
    .width(fullWidth ? '100%' : 'auto')
    .backgroundColor($r('app.color.dm_accent'))
    .borderRadius(999)
    .onClick(onTap)
  }

  @Builder
  private stepRow(num: string, text: string) {
    Row({ space: 12 }) {
      Text(num)
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.dm_accent'))
        .width(18)
      Text(text)
        .fontSize(13)
        .fontColor($r('app.color.dm_ink_secondary'))
        .layoutWeight(1)
    }
    .width('100%')
    .padding({ top: 10, bottom: 10, left: 14, right: 14 })
  }

  @Builder
  private headerTab(target: Page, label: string) {
    Button({ type: ButtonType.Normal }) {
      Text(label)
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.page === target ? $r('app.color.dm_ink') : $r('app.color.dm_ink_secondary'))
        .padding({ left: 14, right: 14, top: 6, bottom: 6 })
    }
    .backgroundColor(this.page === target ? $r('app.color.dm_surface') : Color.Transparent)
    .borderRadius(999)
    .onClick(() => this.navigate(target))
  }

  @Builder
  private bottomTab(target: Page, label: string) {
    Button({ type: ButtonType.Normal }) {
      Text(label)
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.page === target ? $r('app.color.dm_accent') : $r('app.color.dm_ink_secondary'))
        .width('100%')
        .textAlign(TextAlign.Center)
        .padding({ top: 8, bottom: 8 })
    }
    .layoutWeight(1)
    .backgroundColor(this.page === target ? $r('app.color.dm_accent_soft') : Color.Transparent)
    .borderRadius(999)
    .onClick(() => this.navigate(target))
  }

  @Builder
  private connectionGuideCard() {
    Column() {
      this.stepRow('1', '确保电脑端的 DeskMate 已启动运行')
      Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8)
      this.stepRow('2', `使用高速 USB-C 数据线连接电脑与${this.deviceLabel()}`)
      Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8)
      this.stepRow('3', '系统弹出访问确认时点击允许 USB 配件连接')
    }
    .width('100%')
    .backgroundColor($r('app.color.dm_surface'))
    .border({ width: 0.8, color: $r('app.color.dm_hairline') })
    .borderRadius(16)
    .clip(true)
  }

  @Builder
  private liveSpecCard() {
    Column({ space: 0 }) {
      Row({ space: 12 }) {
        Text('显示规格')
          .fontSize(12)
          .fontColor($r('app.color.dm_ink_muted'))
          .width(68)
        Text(`${this.activeCodec || this.codecDisplayName(this.preferredCodec)} · ${this.activeResolution || '协商中'}`)
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.dm_ink'))
          .layoutWeight(1)
      }
      .width('100%')
      .padding({ left: 14, right: 14, top: 11, bottom: 11 })

      if (this.frameReady) {
        Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8)
        Row({ space: 12 }) {
          Text('实时性能')
            .fontSize(12)
            .fontColor($r('app.color.dm_accent'))
            .width(68)
          Text(`${this.fps} FPS · ${this.bitrateMbps.toFixed(1)} Mbps · 触控就绪`)
            .fontSize(13)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_ink'))
            .layoutWeight(1)
        }
        .width('100%')
        .padding({ left: 14, right: 14, top: 11, bottom: 11 })
      }
    }
    .width('100%')
    .backgroundColor($r('app.color.dm_surface'))
    .border({ width: 0.8, color: $r('app.color.dm_hairline') })
    .borderRadius(16)
    .clip(true)
  }

  @Builder
  private homeContent() {
    if (this.isCompactLayout()) {
      Column({ space: 14 }) {
        WiredLinkSchematic({
          linked: this.link.state === 'connected',
          streaming: this.frameReady,
          fps: this.fps,
          codecText: this.activeCodec,
          resolutionText: this.activeResolution,
          compact: true,
          isPhone: this.isPhone
        })

        Column({ space: 6 }) {
          Text(this.statusTitle())
            .fontSize(20)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_ink'))

          Text(this.statusDetail())
            .fontSize(13)
            .lineHeight(19.5)
            .fontColor($r('app.color.dm_ink_secondary'))
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)

        if (this.link.state === 'error') {
          this.primaryButton('重试 USB 连接', () => this.accessoryLink.retry(), true)
        } else if (this.frameReady) {
          this.primaryButton(this.isPhone ? '进入横屏扩展桌面' : '进入扩展桌面', () => this.navigate('display'), true)
        }

        if (this.link.state === 'searching' || this.link.state === 'authorizing') {
          this.connectionGuideCard()
        } else if (this.link.state === 'connected') {
          this.liveSpecCard()
        }
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 14, bottom: 20 })
      .alignItems(HorizontalAlign.Start)
    } else {
      Row({ space: 24 }) {
        Column({ space: 12 }) {
          WiredLinkSchematic({
            linked: this.link.state === 'connected',
            streaming: this.frameReady,
            fps: this.fps,
            codecText: this.activeCodec,
            resolutionText: this.activeResolution,
            compact: false,
            isPhone: this.isPhone
          })
        }
        .width('44%')
        .alignItems(HorizontalAlign.Start)

        Column({ space: 14 }) {
          Text(this.statusTitle())
            .fontSize(24)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_ink'))

          Text(this.statusDetail())
            .fontSize(13.5)
            .lineHeight(20)
            .fontColor($r('app.color.dm_ink_secondary'))

          if (this.link.state === 'searching' || this.link.state === 'authorizing') {
            this.connectionGuideCard()
          } else if (this.link.state === 'connected') {
            this.liveSpecCard()
          }

          if (this.link.state === 'error') {
            Row() {
              this.primaryButton('重试 USB 连接', () => this.accessoryLink.retry(), false)
            }.margin({ top: 2 })
          } else if (this.frameReady) {
            Row() {
              this.primaryButton('进入扩展桌面', () => this.navigate('display'), false)
            }.margin({ top: 2 })
          }

          Row({ space: 10 }) {
            Button('编码与浮窗设置', { type: ButtonType.Normal })
              .height(36)
              .padding({ left: 14, right: 14 })
              .fontSize(12.5)
              .fontColor($r('app.color.dm_ink'))
              .backgroundColor($r('app.color.dm_surface'))
              .border({ width: 0.8, color: $r('app.color.dm_hairline') })
              .borderRadius(11)
              .onClick(() => this.navigate('settings'))

            Button('查看连接诊断', { type: ButtonType.Normal })
              .height(36)
              .padding({ left: 14, right: 14 })
              .fontSize(12.5)
              .fontColor($r('app.color.dm_accent'))
              .backgroundColor($r('app.color.dm_accent_soft'))
              .border({ width: 0.8, color: $r('app.color.dm_accent_border') })
              .borderRadius(11)
              .onClick(() => this.navigate('diagnostics'))
          }.margin({ top: 2 })
        }
        .width('56%')
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .constraintSize({ maxWidth: 1000 })
      .padding({ left: 28, right: 28, top: 20, bottom: 20 })
      .alignItems(VerticalAlign.Center)
    }
  }

  @Builder
  private settingsFields(compact: boolean) {
    Column({ space: 14 }) {
      Text('系统设置')
        .fontSize(compact ? 19 : 20)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.dm_ink'))
        .width('100%')

      if (compact) {
        Row({ space: 10 }) {
          Text('当前编码与规格')
            .fontSize(12)
            .fontColor($r('app.color.dm_ink_muted'))
          Blank()
          Text(this.activeCodec.length > 0
            ? `${this.activeCodec}${this.activeResolution.length > 0 ? ' · ' + this.activeResolution : ''}`
            : this.codecDisplayName(this.preferredCodec))
            .fontSize(12.5)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_ink'))
        }
        .width('100%')
        .padding({ left: 14, right: 14, top: 12, bottom: 12 })
        .backgroundColor($r('app.color.dm_surface'))
        .border({ width: 0.8, color: $r('app.color.dm_hairline') })
        .borderRadius(16)
      }

      this.sectionHeader('画面与性能浮窗')

      Row({ space: 12 }) {
        Column({ space: 3 }) {
          Text('性能参数浮窗')
            .fontSize(14)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_ink'))
          Text('全屏投屏时在右上角显示帧率、码率、编码格式与丢帧统计')
            .fontSize(12)
            .fontColor($r('app.color.dm_ink_secondary'))
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: this.showFps })
          .selectedColor($r('app.color.dm_accent'))
          .accessibilityText('性能参数浮窗')
          .onChange((enabled: boolean) => {
            this.showFps = enabled;
          })
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 12, bottom: 12 })
      .backgroundColor($r('app.color.dm_surface'))
      .border({ width: 0.8, color: $r('app.color.dm_hairline') })
      .borderRadius(16)

      if (this.isPhone) {
        Column({ space: 3 }) {
          Text('横竖屏自动切换')
            .fontSize(14)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_ink'))
          Text('连接前保持竖屏便于配置，进入扩展画面后自动切换横屏并支持左右翻转')
            .fontSize(12)
            .lineHeight(18)
            .fontColor($r('app.color.dm_ink_secondary'))
        }
        .width('100%')
        .padding({ left: 16, right: 16, top: 12, bottom: 12 })
        .backgroundColor($r('app.color.dm_surface'))
        .border({ width: 0.8, color: $r('app.color.dm_hairline') })
        .borderRadius(16)
        .alignItems(HorizontalAlign.Start)
      }

      this.sectionHeader('视频编码格式')

      Text(this.activeCodec.length > 0
        ? `当前协商：${this.activeCodec}${this.activeResolution.length > 0 ? ' · ' + this.activeResolution : ''}`
        : '选择传输编码偏好，默认自动优先使用 HEVC 硬件解码')
        .fontSize(12)
        .fontColor($r('app.color.dm_ink_secondary'))
        .width('100%')

      Row({ space: compact ? 8 : 10 }) {
        ForEach(CODEC_OPTIONS, (option: CodecOption) => {
          Button({ type: ButtonType.Normal }) {
            Column({ space: 4 }) {
              Text(option.title)
                .fontSize(compact ? 13 : 14)
                .fontWeight(FontWeight.Medium)
                .fontColor(this.preferredCodec === option.value ? $r('app.color.dm_accent') : $r('app.color.dm_ink'))
              Text(option.subtitle)
                .fontSize(compact ? 10.5 : 11)
                .fontColor(this.preferredCodec === option.value ? $r('app.color.dm_accent') : $r('app.color.dm_ink_muted'))
            }
            .width('100%')
            .padding({ top: 11, bottom: 11, left: compact ? 6 : 10, right: compact ? 6 : 10 })
            .backgroundColor(this.preferredCodec === option.value ? $r('app.color.dm_accent_soft') : $r('app.color.dm_surface'))
            .border({
              width: 0.8,
              color: this.preferredCodec === option.value ? $r('app.color.dm_accent_border') : $r('app.color.dm_hairline')
            })
            .borderRadius(11)
          }
          .layoutWeight(1)
          .backgroundColor(Color.Transparent)
          .onClick(() => this.selectCodec(option.value))
        }, (option: CodecOption) => `${option.value}:${this.preferredCodec === option.value}`)
      }.width('100%')

      if (this.codecChangedWhileConnected && this.link.state === 'connected') {
        Row({ space: 12 }) {
          Text('编码偏好已更新，重新连接 USB 会话后生效')
            .fontSize(12)
            .fontColor($r('app.color.dm_accent'))
            .layoutWeight(1)
          Button('立即重连', { type: ButtonType.Normal })
            .height(32)
            .padding({ left: 14, right: 14 })
            .fontSize(12)
            .fontColor($r('app.color.dm_accent_fg'))
            .backgroundColor($r('app.color.dm_accent'))
            .borderRadius(999)
            .onClick(() => {
              this.codecChangedWhileConnected = false;
              this.accessoryLink.reconnect();
            })
        }
        .width('100%')
        .padding({ left: 14, right: 14, top: 10, bottom: 10 })
        .backgroundColor($r('app.color.dm_accent_soft'))
        .border({ width: 0.8, color: $r('app.color.dm_accent_border') })
        .borderRadius(14)
      }

      this.sectionHeader('连接诊断')

      Button({ type: ButtonType.Normal }) {
        Row({ space: 12 }) {
          Column({ space: 2 }) {
            Text('查看连接与解码统计')
              .fontSize(14)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.dm_ink'))
            Text('检视 USB 状态、协商参数、码率与帧率统计')
              .fontSize(12)
              .fontColor($r('app.color.dm_ink_secondary'))
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)

          Text('查看 →')
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_accent'))
        }
        .width('100%')
        .padding({ top: 12, bottom: 12, left: 16, right: 16 })
        .backgroundColor($r('app.color.dm_surface'))
        .border({ width: 0.8, color: $r('app.color.dm_hairline') })
        .borderRadius(16)
      }
      .width('100%')
      .backgroundColor(Color.Transparent)
      .onClick(() => this.navigate('diagnostics'))

      this.sectionHeader('关于与隐私')

      Button({ type: ButtonType.Normal }) {
        Row({ space: 12 }) {
          Column({ space: 2 }) {
            Text('关于 DeskMate 与隐私说明')
              .fontSize(14)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.dm_ink'))
            Text(`版本 v${this.appVersionName} · 本地 USB 传输，不联网、不收集个人信息`)
              .fontSize(12)
              .fontColor($r('app.color.dm_ink_secondary'))
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)

          Text('查看 →')
            .fontSize(12)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_accent'))
        }
        .width('100%')
        .padding({ top: 12, bottom: 12, left: 16, right: 16 })
        .backgroundColor($r('app.color.dm_surface'))
        .border({ width: 0.8, color: $r('app.color.dm_hairline') })
        .borderRadius(16)
      }
      .width('100%')
      .backgroundColor(Color.Transparent)
      .onClick(() => this.navigate('about'))
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  private settingsContent() {
    if (this.isCompactLayout()) {
      Column() {
        this.settingsFields(true)
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 14, bottom: 20 })
      .alignItems(HorizontalAlign.Start)
    } else {
      Row({ space: 24 }) {
        Column({ space: 10 }) {
          WiredLinkSchematic({
            linked: this.link.state === 'connected',
            streaming: this.frameReady,
            fps: this.fps,
            codecText: this.activeCodec,
            resolutionText: this.activeResolution,
            compact: true,
            isPhone: this.isPhone
          })

          Column({ space: 4 }) {
            Text('当前编码偏好')
              .fontSize(11)
              .fontColor($r('app.color.dm_ink_muted'))
            Text(this.activeCodec.length > 0
              ? `${this.activeCodec}${this.activeResolution.length > 0 ? ' · ' + this.activeResolution : ''}`
              : this.codecDisplayName(this.preferredCodec))
              .fontSize(13)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.dm_ink'))
          }
          .width('100%')
          .padding(14)
          .backgroundColor($r('app.color.dm_surface'))
          .border({ width: 0.8, color: $r('app.color.dm_hairline') })
          .borderRadius(16)
          .alignItems(HorizontalAlign.Start)
        }
        .width('35%')
        .alignItems(HorizontalAlign.Start)

        Column() {
          this.settingsFields(false)
        }
        .width('65%')
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .constraintSize({ maxWidth: 1000 })
      .padding({ left: 28, right: 28, top: 18, bottom: 24 })
      .alignItems(VerticalAlign.Top)
    }
  }

  @Builder
  private diagnosticsRowsCard(compact: boolean) {
    Column() {
      ForEach(this.diagnosticRows(), (row: DiagnosticRow, index: number) => {
        Column() {
          if (index > 0) {
            Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8)
          }
          Row({ space: 12 }) {
            Text(row.label)
              .fontSize(compact ? 12 : 12.5)
              .fontColor($r('app.color.dm_ink_muted'))
              .width(compact ? 76 : 96)
            Text(row.value)
              .fontSize(compact ? 12 : 12.5)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.dm_ink'))
              .layoutWeight(1)
          }.width('100%').padding({ top: 10, bottom: 10, left: 14, right: 14 })
        }.width('100%')
      }, (row: DiagnosticRow) => `${row.label}:${row.value}`)
    }
    .width('100%')
    .backgroundColor($r('app.color.dm_surface'))
    .border({ width: 0.8, color: $r('app.color.dm_hairline') })
    .borderRadius(16)
    .clip(true)
  }

  @Builder
  private reconnectUsbButton(height: number) {
    Button('重新握手 USB 连接', { type: ButtonType.Normal })
      .width('100%')
      .height(height)
      .fontSize(12.5)
      .fontWeight(FontWeight.Medium)
      .fontColor($r('app.color.dm_accent'))
      .backgroundColor($r('app.color.dm_accent_soft'))
      .border({ width: 0.8, color: $r('app.color.dm_accent_border') })
      .borderRadius(11)
      .onClick(() => {
        if (this.link.state === 'connected') {
          this.accessoryLink.reconnect();
        } else {
          this.accessoryLink.retry();
        }
      })
  }

  @Builder
  private diagnosticsContent() {
    if (this.isCompactLayout()) {
      Column({ space: 12 }) {
        Text('连接与解码诊断')
          .fontSize(19)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.dm_ink'))
          .width('100%')

        this.diagnosticsRowsCard(true)

        this.reconnectUsbButton(42)
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 14, bottom: 20 })
      .alignItems(HorizontalAlign.Start)
    } else {
      Row({ space: 24 }) {
        Column({ space: 10 }) {
          WiredLinkSchematic({
            linked: this.link.state === 'connected',
            streaming: this.frameReady,
            fps: this.fps,
            codecText: this.activeCodec,
            resolutionText: this.activeResolution,
            compact: true,
            isPhone: this.isPhone
          })

          this.reconnectUsbButton(38)
        }
        .width('35%')
        .alignItems(HorizontalAlign.Start)

        Column({ space: 12 }) {
          Text('连接与解码诊断')
            .fontSize(20)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_ink'))
            .width('100%')

          this.diagnosticsRowsCard(false)

          Row({ space: 10 }) {
            Button('← 返回系统设置', { type: ButtonType.Normal })
              .height(36)
              .padding({ left: 14, right: 14 })
              .fontSize(12)
              .fontColor($r('app.color.dm_ink_secondary'))
              .backgroundColor($r('app.color.dm_surface'))
              .border({ width: 0.8, color: $r('app.color.dm_hairline') })
              .borderRadius(11)
              .onClick(() => this.navigate('settings'))

            Button('返回主页', { type: ButtonType.Normal })
              .height(36)
              .padding({ left: 14, right: 14 })
              .fontSize(12)
              .fontColor($r('app.color.dm_accent'))
              .backgroundColor($r('app.color.dm_accent_soft'))
              .border({ width: 0.8, color: $r('app.color.dm_accent_border') })
              .borderRadius(11)
              .onClick(() => this.navigate('home'))
          }
        }
        .width('65%')
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .constraintSize({ maxWidth: 1000 })
      .padding({ left: 28, right: 28, top: 18, bottom: 24 })
      .alignItems(VerticalAlign.Top)
    }
  }

  private aboutInfoRows(): Array<DiagnosticRow> {
    return [
      { label: '应用名称', value: APP_DISPLAY_NAME },
      { label: '当前版本', value: `v${this.appVersionName} (${this.appVersionCode})` },
      { label: '应用包名', value: this.appBundleName },
      { label: '支持设备', value: 'HarmonyOS 手机 / 平板' },
      { label: '开发者', value: DEVELOPER_NAME },
      { label: '联系邮箱', value: CONTACT_EMAIL },
      { label: '联网与权限', value: '无网络权限 · 仅使用本地 USB 配件接口' }
    ];
  }

  @Builder
  private aboutIdentityCard() {
    Row({ space: 14 }) {
      Image($r('app.media.app_icon'))
        .width(48)
        .height(48)
        .borderRadius(12)
        .objectFit(ImageFit.Cover)

      Column({ space: 3 }) {
        Text(APP_DISPLAY_NAME)
          .fontSize(16)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.dm_ink'))
        Text(`版本 v${this.appVersionName} (${this.appVersionCode})`)
          .fontSize(12)
          .fontColor($r('app.color.dm_ink_secondary'))
        Text(this.appBundleName)
          .fontSize(11)
          .fontColor($r('app.color.dm_ink_muted'))
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)
    }
    .width('100%')
    .padding(14)
    .backgroundColor($r('app.color.dm_surface'))
    .border({ width: 0.8, color: $r('app.color.dm_hairline') })
    .borderRadius(16)
  }

  @Builder
  private aboutMetadataCard(compact: boolean) {
    Column() {
      ForEach(this.aboutInfoRows(), (row: DiagnosticRow, index: number) => {
        Column() {
          if (index > 0) {
            Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8)
          }
          Row({ space: 12 }) {
            Text(row.label)
              .fontSize(compact ? 12 : 12.5)
              .fontColor($r('app.color.dm_ink_muted'))
              .width(compact ? 74 : 84)
            Text(row.value)
              .fontSize(compact ? 12 : 12.5)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.dm_ink'))
              .layoutWeight(1)
          }
          .width('100%')
          .padding({ top: 10, bottom: 10, left: 14, right: 14 })
        }.width('100%')
      }, (row: DiagnosticRow) => `${row.label}:${row.value}`)
    }
    .width('100%')
    .backgroundColor($r('app.color.dm_surface'))
    .border({ width: 0.8, color: $r('app.color.dm_hairline') })
    .borderRadius(16)
    .clip(true)
  }

  @Builder
  private aboutCommitmentCard() {
    Column({ space: 6 }) {
      Text('本地隐私承诺')
        .fontSize(12.5)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.dm_positive'))
      Text(PRIVACY_ONE_LINE_COMMITMENT)
        .fontSize(13)
        .lineHeight(19.5)
        .fontColor($r('app.color.dm_ink'))
    }
    .width('100%')
    .padding(14)
    .backgroundColor($r('app.color.dm_positive_soft'))
    .border({ width: 0.8, color: $r('app.color.dm_positive_border') })
    .borderRadius(16)
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  private aboutPrivacyPolicyCard() {
    Column({ space: 10 }) {
      Row({ space: 8 }) {
        Text('隐私政策全文')
          .fontSize(14)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.dm_ink'))
          .layoutWeight(1)
        Text(`更新：${PRIVACY_UPDATED_DATE}`)
          .fontSize(11)
          .fontColor($r('app.color.dm_ink_muted'))
      }
      .width('100%')

      Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8)

      ForEach(PRIVACY_POLICY_SECTIONS, (section: PrivacySection, index: number) => {
        Column({ space: 4 }) {
          if (index > 0) {
            Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8).margin({ bottom: 6 })
          }
          Text(section.title)
            .fontSize(12.5)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_ink'))
          Text(section.body)
            .fontSize(12)
            .lineHeight(18)
            .fontColor($r('app.color.dm_ink_secondary'))
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)
      }, (section: PrivacySection, index: number) => `${index}:${section.title}`)
    }
    .width('100%')
    .padding(16)
    .backgroundColor($r('app.color.dm_surface'))
    .border({ width: 0.8, color: $r('app.color.dm_hairline') })
    .borderRadius(16)
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  private aboutContent() {
    if (this.isCompactLayout()) {
      Column({ space: 12 }) {
        Text('关于与隐私')
          .fontSize(19)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.dm_ink'))
          .width('100%')

        this.aboutIdentityCard()
        this.aboutCommitmentCard()
        this.aboutMetadataCard(true)
        this.aboutPrivacyPolicyCard()
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 14, bottom: 20 })
      .alignItems(HorizontalAlign.Start)
    } else {
      Row({ space: 24 }) {
        Column({ space: 12 }) {
          this.aboutIdentityCard()
          this.aboutMetadataCard(false)

          Row({ space: 10 }) {
            Button('← 返回系统设置', { type: ButtonType.Normal })
              .height(36)
              .padding({ left: 14, right: 14 })
              .fontSize(12)
              .fontColor($r('app.color.dm_ink_secondary'))
              .backgroundColor($r('app.color.dm_surface'))
              .border({ width: 0.8, color: $r('app.color.dm_hairline') })
              .borderRadius(11)
              .onClick(() => this.navigate('settings'))

            Button('返回主页', { type: ButtonType.Normal })
              .height(36)
              .padding({ left: 14, right: 14 })
              .fontSize(12)
              .fontColor($r('app.color.dm_accent'))
              .backgroundColor($r('app.color.dm_accent_soft'))
              .border({ width: 0.8, color: $r('app.color.dm_accent_border') })
              .borderRadius(11)
              .onClick(() => this.navigate('home'))
          }
        }
        .width('36%')
        .alignItems(HorizontalAlign.Start)

        Column({ space: 12 }) {
          Text('关于与隐私')
            .fontSize(20)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_ink'))
            .width('100%')

          this.aboutCommitmentCard()
          this.aboutPrivacyPolicyCard()
        }
        .width('64%')
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .constraintSize({ maxWidth: 1000 })
      .padding({ left: 28, right: 28, top: 18, bottom: 24 })
      .alignItems(VerticalAlign.Top)
    }
  }

  @Builder
  private privacyConsentGate() {
    Column() {
      Column({ space: 0 }) {
        Row({ space: 12 }) {
          Image($r('app.media.app_icon'))
            .width(44)
            .height(44)
            .borderRadius(10)
            .objectFit(ImageFit.Cover)

          Column({ space: 2 }) {
            Text('欢迎使用 DeskMate')
              .fontSize(17)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.dm_ink'))
            Text('隐私保护提示')
              .fontSize(12)
              .fontColor($r('app.color.dm_ink_secondary'))
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)
        }
        .width('100%')
        .padding({ left: 20, right: 20, top: 18, bottom: 14 })

        Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8)

        Scroll() {
          Column({ space: 12 }) {
            Row({ space: 6 }) {
              Row()
                .width(6)
                .height(6)
                .borderRadius(3)
                .backgroundColor($r('app.color.dm_positive'))
              Text('不联网 · 不收集个人信息 · 本地 USB 直连')
                .fontSize(11.5)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.dm_positive'))
            }
            .padding({ left: 10, right: 10, top: 5, bottom: 5 })
            .backgroundColor($r('app.color.dm_positive_soft'))
            .border({ width: 0.8, color: $r('app.color.dm_positive_border') })
            .borderRadius(999)

            Text('DeskMate 需配合电脑端主机通过 USB-C 数据线建立本地扩展屏会话。在您点击“同意并继续”之前，应用不会枚举 USB 配件或请求 USB 配件授权。')
              .fontSize(13)
              .lineHeight(19.5)
              .fontColor($r('app.color.dm_ink_secondary'))

            Column({ space: 8 }) {
              ForEach(PRIVACY_CONSENT_HIGHLIGHTS, (item: PrivacySection, index: number) => {
                Column({ space: 3 }) {
                  Text(item.title)
                    .fontSize(12.5)
                    .fontWeight(FontWeight.Medium)
                    .fontColor($r('app.color.dm_ink'))
                  Text(item.body)
                    .fontSize(12)
                    .lineHeight(17.5)
                    .fontColor($r('app.color.dm_ink_secondary'))
                }
                .width('100%')
                .padding(11)
                .backgroundColor($r('app.color.dm_canvas'))
                .border({ width: 0.8, color: $r('app.color.dm_soft_hairline') })
                .borderRadius(12)
                .alignItems(HorizontalAlign.Start)
              }, (item: PrivacySection, index: number) => `${index}:${item.title}`)
            }
            .width('100%')

            Button(this.showConsentFullPolicy ? '收起完整隐私政策' : '查看完整隐私政策', { type: ButtonType.Normal })
              .height(34)
              .padding({ left: 12, right: 12 })
              .fontSize(12)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.dm_accent'))
              .backgroundColor($r('app.color.dm_accent_soft'))
              .border({ width: 0.8, color: $r('app.color.dm_accent_border') })
              .borderRadius(999)
              .onClick(() => {
                this.showConsentFullPolicy = !this.showConsentFullPolicy;
              })

            if (this.showConsentFullPolicy) {
              this.aboutPrivacyPolicyCard()
            }

            if (this.privacyDeclined) {
              Column({ space: 4 }) {
                Text('未同意隐私保护提示')
                  .fontSize(12.5)
                  .fontWeight(FontWeight.Medium)
                  .fontColor($r('app.color.dm_warning'))
                Text('当前不会枚举 USB 配件，也无法建立有线扩展屏连接。您可以随时点击下方“同意并继续”启用，或退出本应用。')
                  .fontSize(12)
                  .lineHeight(17.5)
                  .fontColor($r('app.color.dm_ink_secondary'))
              }
              .width('100%')
              .padding(12)
              .backgroundColor($r('app.color.dm_warning_soft'))
              .border({ width: 0.8, color: $r('app.color.dm_warning_border') })
              .borderRadius(12)
              .alignItems(HorizontalAlign.Start)
            }
          }
          .width('100%')
          .padding({ left: 20, right: 20, top: 14, bottom: 14 })
          .alignItems(HorizontalAlign.Start)
        }
        .width('100%')
        .constraintSize({ maxHeight: Math.max(220, this.windowHeightVp - this.safeTopVp - this.safeBottomVp - 240) })

        Divider().color($r('app.color.dm_soft_hairline')).strokeWidth(0.8)

        Row({ space: 10 }) {
          Button('不同意', { type: ButtonType.Normal })
            .layoutWeight(1)
            .height(42)
            .fontSize(13.5)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_ink_secondary'))
            .backgroundColor($r('app.color.dm_subtle'))
            .border({ width: 0.8, color: $r('app.color.dm_hairline') })
            .borderRadius(999)
            .onClick(() => this.declinePrivacyPolicy())

          Button('同意并继续', { type: ButtonType.Normal })
            .layoutWeight(1.4)
            .height(42)
            .fontSize(13.5)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.dm_accent_fg'))
            .backgroundColor($r('app.color.dm_accent'))
            .borderRadius(999)
            .onClick(() => this.acceptPrivacyPolicy())
        }
        .width('100%')
        .padding({ left: 20, right: 20, top: 14, bottom: 16 })
      }
      .width('100%')
      .constraintSize({ maxWidth: 540 })
      .backgroundColor($r('app.color.dm_surface'))
      .border({ width: 0.8, color: $r('app.color.dm_hairline') })
      .borderRadius(20)
      .clip(true)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
    .padding({
      left: 16,
      right: 16,
      top: Math.max(16, this.safeTopVp + 8),
      bottom: Math.max(16, this.safeBottomVp + 8)
    })
    .backgroundColor($r('app.color.dm_canvas'))
    .zIndex(2)
  }

  @Builder
  private shell() {
    Column() {
      // Top navigation header
      if (this.isCompactLayout()) {
        Row({ space: 12 }) {
          if (this.page !== 'home') {
            Button({ type: ButtonType.Normal }) {
              Row({ space: 5 }) {
                Text('←')
                  .fontSize(13)
                  .fontColor($r('app.color.dm_accent'))
                Text('返回')
                  .fontSize(12.5)
                  .fontColor($r('app.color.dm_ink'))
              }
            }
            .height(32)
            .padding({ left: 12, right: 12 })
            .backgroundColor($r('app.color.dm_subtle'))
            .border({ width: 0.8, color: $r('app.color.dm_hairline') })
            .borderRadius(999)
            .onClick(() => this.onBackPress())
          } else {
            Text('DeskMate')
              .fontSize(15)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.dm_ink'))
          }

          Blank()

          Row({ space: 6 }) {
            Row()
              .width(6)
              .height(6)
              .borderRadius(3)
              .backgroundColor(this.statusDotColor())
            Text(this.shortStatusLabel())
              .fontSize(11)
              .fontWeight(FontWeight.Medium)
              .fontColor(this.badgeFgColor())
          }
          .padding({ left: 10, right: 10, top: 4, bottom: 4 })
          .backgroundColor(this.badgeBgColor())
          .border({ width: 0.8, color: this.badgeBorderColor() })
          .borderRadius(999)
        }
        .width('100%')
        .padding({ left: 16, right: 16, top: Math.max(this.safeTopVp + 6, 14), bottom: 10 })
        .backgroundColor($r('app.color.dm_surface'))
      } else {
        Row({ space: 16 }) {
          if (this.page !== 'home') {
            Button({ type: ButtonType.Normal }) {
              Row({ space: 5 }) {
                Text('←')
                  .fontSize(13)
                  .fontColor($r('app.color.dm_accent'))
                Text('返回')
                  .fontSize(12.5)
                  .fontColor($r('app.color.dm_ink'))
              }
            }
            .height(34)
            .padding({ left: 13, right: 13 })
            .backgroundColor($r('app.color.dm_subtle'))
            .border({ width: 0.8, color: $r('app.color.dm_hairline') })
            .borderRadius(999)
            .onClick(() => this.onBackPress())
          } else {
            Row({ space: 10 }) {
              Text('DeskMate')
                .fontSize(15)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.dm_ink'))

              Row({ space: 6 }) {
                Row()
                  .width(6)
                  .height(6)
                  .borderRadius(3)
                  .backgroundColor(this.statusDotColor())
                Text(this.shortStatusLabel())
                  .fontSize(11)
                  .fontWeight(FontWeight.Medium)
                  .fontColor(this.badgeFgColor())
              }
              .padding({ left: 10, right: 10, top: 4, bottom: 4 })
              .backgroundColor(this.badgeBgColor())
              .border({ width: 0.8, color: this.badgeBorderColor() })
              .borderRadius(999)
            }
          }

          Blank()

          Row({ space: 2 }) {
            this.headerTab('home', '连接')
            this.headerTab('settings', '设置')
            this.headerTab('diagnostics', '诊断')
          }
          .padding(3)
          .backgroundColor($r('app.color.dm_subtle'))
          .border({ width: 0.8, color: $r('app.color.dm_soft_hairline') })
          .borderRadius(999)
        }
        .width('100%')
        .padding({ left: 28, right: 28, top: Math.max(this.safeTopVp + 8, 22), bottom: 10 })
        .backgroundColor($r('app.color.dm_surface'))
      }

      Divider().color($r('app.color.dm_hairline')).strokeWidth(0.8)

      Scroll() {
        Column() {
          if (this.page === 'home') {
            this.homeContent()
          } else if (this.page === 'settings') {
            this.settingsContent()
          } else if (this.page === 'about') {
            this.aboutContent()
          } else {
            this.diagnosticsContent()
          }
        }
        .width('100%')
      }
      .width('100%')
      .layoutWeight(1)
      .align(!this.isCompactLayout() && this.page === 'home' ? Alignment.Center : Alignment.Top)

      Divider().color($r('app.color.dm_hairline')).strokeWidth(0.8)

      if (this.isCompactLayout()) {
        Row({ space: 6 }) {
          this.bottomTab('home', '连接')
          this.bottomTab('settings', '设置')
          this.bottomTab('diagnostics', '诊断')
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 6, bottom: Math.max(8, this.safeBottomVp) })
        .backgroundColor($r('app.color.dm_surface'))
      } else {
        Row() {
          Text('USB-C AOA 硬件直连 · 自动 HiDPI 虚拟屏 · 多点与双指触控')
            .fontSize(11)
            .fontColor($r('app.color.dm_ink_muted'))
          Blank()
          Row({ space: 6 }) {
            Row()
              .width(6)
              .height(6)
              .borderRadius(3)
              .backgroundColor(this.statusDotColor())
            Text(`${this.activeCodec.length > 0 ? this.activeCodec : this.preferredCodec.toUpperCase()} · ${this.activeResolution.length > 0 ? this.activeResolution : '待机'}`)
              .fontSize(11)
              .fontColor($r('app.color.dm_ink_secondary'))
          }
        }
        .width('100%')
        .constraintSize({ minHeight: 32 + this.safeBottomVp })
        .padding({ left: 28, right: 28, top: 6, bottom: Math.max(6, this.safeBottomVp) })
        .backgroundColor($r('app.color.dm_surface'))
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.dm_canvas'))
    .zIndex(1)
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      XComponent({
        id: 'deskmate_surface',
        type: XComponentType.SURFACE,
        libraryname: 'deskmate_device'
      })
        .width('100%')
        .height('100%')
        .backgroundColor('#000000')

      if (!this.privacyAccepted) {
        this.privacyConsentGate()
      } else if (this.page !== 'display') {
        this.shell()
      } else {
        Row({ space: 8 }) {
          if (this.controlsExpanded) {
            Button('← 返回主页', { type: ButtonType.Normal })
              .height(this.isPhone ? 34 : 38)
              .padding({ left: this.isPhone ? 12 : 14, right: this.isPhone ? 12 : 14 })
              .fontSize(this.isPhone ? 12 : 12.5)
              .fontWeight(FontWeight.Medium)
              .fontColor('#F4F4F5')
              .backgroundColor('#DB181A1E')
              .border({ width: 0.8, color: '#26FFFFFF' })
              .borderRadius(999)
              .onClick(() => this.navigate('home'))
          }
          Button({ type: ButtonType.Normal }) {
            SymbolGlyph(this.controlsExpanded ? $r('sys.symbol.chevron_up') : $r('sys.symbol.chevron_down'))
              .fontSize(this.isPhone ? 16 : 17).fontColor(['#F4F4F5'])
          }
          .width(this.isPhone ? 34 : 38)
          .height(this.isPhone ? 34 : 38)
          .borderRadius(999)
          .backgroundColor('#DB181A1E')
          .border({ width: 0.8, color: '#26FFFFFF' })
          .accessibilityText(this.controlsExpanded ? '收起显示工具栏' : '展开显示工具栏')
          .onClick(() => { this.controlsExpanded = !this.controlsExpanded; })

          Blank()

          if (this.showFps) {
            Row({ space: this.isPhone ? 7 : 10 }) {
              Row()
                .width(6)
                .height(6)
                .borderRadius(3)
                .backgroundColor('#4F8BFF')
              Row({ space: 4 }) {
                Text(`${this.fps}`)
                  .fontSize(this.isPhone ? 11.5 : 13)
                  .fontWeight(FontWeight.Bold)
                  .fontColor('#4F8BFF')
                Text('FPS')
                  .fontSize(this.isPhone ? 10 : 10.5)
                  .fontColor('#A1A1AA')
              }
              Divider().vertical(true).height(12).color('#3F3F46')
              Row({ space: 4 }) {
                Text(`${this.bitrateMbps.toFixed(1)}`)
                  .fontSize(this.isPhone ? 11.5 : 13)
                  .fontWeight(FontWeight.Bold)
                  .fontColor('#F4F4F5')
                Text('Mbps')
                  .fontSize(this.isPhone ? 10 : 10.5)
                  .fontColor('#A1A1AA')
              }
              Divider().vertical(true).height(12).color('#3F3F46')
              Text(this.activeCodec.length > 0 ? this.activeCodec : '--')
                .fontSize(this.isPhone ? 11 : 12)
                .fontWeight(FontWeight.Medium)
                .fontColor('#F4F4F5')
              if (this.activeResolution.length > 0) {
                Divider().vertical(true).height(12).color('#3F3F46')
                Text(this.activeResolution)
                  .fontSize(this.isPhone ? 11 : 12)
                  .fontColor('#A1A1AA')
              }
              if (this.droppedFrames > 0) {
                Divider().vertical(true).height(12).color('#3F3F46')
                Text(`丢帧 ${this.droppedFrames}`)
                  .fontSize(this.isPhone ? 11 : 12)
                  .fontColor('#F87171')
              }
            }
            .padding({
              left: this.isPhone ? 10 : 14,
              right: this.isPhone ? 10 : 14,
              top: this.isPhone ? 6 : 7,
              bottom: this.isPhone ? 6 : 7
            })
            .backgroundColor('#DB181A1E')
            .borderRadius(999)
            .border({ width: 0.8, color: '#26FFFFFF' })
            .hitTestBehavior(HitTestMode.None)
          }
        }
        .width('100%')
        .padding({ left: Math.max(14, this.safeLeftVp + 8), right: Math.max(14, this.safeRightVp + 8), top: 12 })
        .hitTestBehavior(HitTestMode.Transparent)
        .zIndex(2)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}

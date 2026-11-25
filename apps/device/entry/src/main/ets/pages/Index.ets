import { AccessoryLink, LinkSnapshot } from '../deskmate/AccessoryLink';
import {
  isPhoneDevice,
  preferredOrientationForPage,
  readSafeAreaInsets,
  SafeAreaInsets
} from '../deskmate/DeviceLayout';
import native from 'libdeskmate_device.so';
import { display, UIContext, window } from '@kit.ArkUI';

PersistentStorage.persistProp('deskMateShowFps', false);
PersistentStorage.persistProp('deskMateCodec', 'auto');

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

type Page = 'home' | 'settings' | 'diagnostics' | 'display';

/**
 * Architectural wired dual-display schematic (Mac host + USB-C AOA data bus + HarmonyOS tablet).
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

    const activeColor: string = this.streaming
      ? '#9D193D'
      : (this.linked ? '#B9975B' : '#8C858E');
    const inkColor: string = '#232026';
    const hairlineColor: string = '#DCD6CC';

    // Subtle corner calibration brackets
    const inset: number = this.compact ? 9 : 12;
    const tick: number = this.compact ? 6 : 7;
    this.ctx.strokeStyle = hairlineColor;
    this.ctx.lineWidth = 1.0;
    this.ctx.beginPath();
    this.ctx.moveTo(inset, inset + tick);
    this.ctx.lineTo(inset, inset);
    this.ctx.lineTo(inset + tick, inset);

    this.ctx.moveTo(w - inset - tick, inset);
    this.ctx.lineTo(w - inset, inset);
    this.ctx.lineTo(w - inset, inset + tick);

    this.ctx.moveTo(inset, h - inset - tick);
    this.ctx.lineTo(inset, h - inset);
    this.ctx.lineTo(inset + tick, h - inset);

    this.ctx.moveTo(w - inset - tick, h - inset);
    this.ctx.lineTo(w - inset, h - inset);
    this.ctx.lineTo(w - inset, h - inset - tick);
    this.ctx.stroke();

    // Subtle center horizontal drafting guide
    const cy: number = h * 0.5;
    this.ctx.save();
    this.ctx.setLineDash([3, 4]);
    this.ctx.strokeStyle = '#EAE4DD';
    this.ctx.lineWidth = 1.0;
    this.ctx.beginPath();
    this.ctx.moveTo(this.compact ? 16 : 22, cy);
    this.ctx.lineTo(w - (this.compact ? 16 : 22), cy);
    this.ctx.stroke();
    this.ctx.restore();

    const macW: number = Math.min(w * (this.compact ? 0.36 : 0.34), this.compact ? 96 : 118);
    const macH: number = macW * 0.64;
    const macX: number = w * (this.compact ? 0.08 : 0.14);
    const macY: number = cy - macH * 0.56;

    const padW: number = Math.min(w * (this.compact ? 0.38 : 0.36), this.compact ? 104 : 128);
    const padH: number = this.isPhone ? padW * 0.56 : padW * 0.68;
    const padX: number = w * (this.compact ? 0.92 : 0.86) - padW;
    const padY: number = cy - padH * 0.44;

    // Stepped USB-C AOA data bus connecting Mac bottom-right to Tablet top/left edge
    const busStartX: number = macX + macW;
    const busStartY: number = macY + macH * 0.52;
    const busEndX: number = padX;
    const busEndY: number = padY + padH * 0.52;
    const busMidX: number = (busStartX + busEndX) * 0.5;

    this.ctx.strokeStyle = this.linked ? '#9D193D' : '#CFC8BC';
    this.ctx.lineWidth = this.linked ? 1.8 : 1.1;
    this.ctx.beginPath();
    this.ctx.moveTo(busStartX, busStartY);
    this.ctx.lineTo(busMidX, busStartY);
    this.ctx.lineTo(busMidX, busEndY);
    this.ctx.lineTo(busEndX, busEndY);
    this.ctx.stroke();

    // Bus terminal square pads & central signal node
    this.ctx.fillStyle = activeColor;
    this.ctx.fillRect(busStartX - 2, busStartY - 2, 4, 4);
    this.ctx.fillRect(busEndX - 2, busEndY - 2, 4, 4);
    this.ctx.fillRect(busMidX - 3, (busStartY + busEndY) * 0.5 - 3, 6, 6);

    // 1. Primary Mac display frame
    this.drawRoundedRect(macX, macY, macW, macH, 3);
    this.ctx.fillStyle = '#FFFFFF';
    this.ctx.fill();
    this.ctx.strokeStyle = inkColor;
    this.ctx.lineWidth = 1.25;
    this.ctx.stroke();

    // Top menu bar inside Mac screen
    this.ctx.fillStyle = '#F2EDE4';
    this.ctx.fillRect(macX + 1, macY + 1, macW - 2, 6);

    // Stand foot below Mac screen
    const macCenterX: number = macX + macW * 0.5;
    const standBottom: number = macY + macH + 9;
    this.ctx.strokeStyle = '#767079';
    this.ctx.lineWidth = 1.2;
    this.ctx.beginPath();
    this.ctx.moveTo(macCenterX, macY + macH);
    this.ctx.lineTo(macCenterX, standBottom);
    this.ctx.moveTo(macCenterX - macW * 0.16, standBottom);
    this.ctx.lineTo(macCenterX + macW * 0.16, standBottom);
    this.ctx.stroke();

    this.ctx.fillStyle = inkColor;
    this.ctx.font = this.compact ? '500 10px sans-serif' : '500 11px sans-serif';
    this.ctx.textAlign = 'center';
    this.ctx.fillText('MAC 主屏', macCenterX, macY + macH * 0.54);

    this.ctx.fillStyle = '#8C858E';
    this.ctx.font = '400 9px monospace';
    this.ctx.fillText('HOST', macCenterX, macY + macH * 0.74);

    // 2. HarmonyOS Tablet / Phone extended display frame
    const cornerRadius: number = this.isPhone ? 5 : 3.5;
    this.drawRoundedRect(padX, padY, padW, padH, cornerRadius);
    this.ctx.fillStyle = this.streaming
      ? '#F7EAEF'
      : (this.linked ? '#FAF7F2' : '#FFFFFF');
    this.ctx.fill();
    this.ctx.strokeStyle = this.linked ? '#9D193D' : '#5A545E';
    this.ctx.lineWidth = this.linked ? 1.65 : 1.15;
    this.ctx.stroke();

    // Inner display outline
    this.drawRoundedRect(padX + 4, padY + 4, padW - 8, padH - 8, this.isPhone ? 3 : 2);
    this.ctx.strokeStyle = this.streaming ? '#D9B6C2' : '#EAE4DD';
    this.ctx.lineWidth = 0.8;
    this.ctx.stroke();

    if (this.isPhone) {
      // Subtle front-camera pill on left bezel to indicate landscape phone orientation
      this.ctx.fillStyle = this.streaming ? '#D9B6C2' : '#DCD6CC';
      this.ctx.fillRect(padX + 2.5, padY + padH * 0.36, 1.8, padH * 0.28);
    }

    // Active status square dot in top-right of device screen
    this.ctx.fillStyle = activeColor;
    this.ctx.fillRect(padX + padW - 11, padY + 7, 4, 4);

    const padCenterX: number = padX + padW * 0.5;
    this.ctx.fillStyle = this.streaming ? '#9D193D' : inkColor;
    this.ctx.font = this.compact ? '600 10px sans-serif' : '600 11px sans-serif';
    this.ctx.textAlign = 'center';
    this.ctx.fillText(
      this.streaming && this.fps > 0 ? `${this.fps} FPS` : (this.isPhone ? '手机副屏' : '扩展副屏'),
      padCenterX,
      padY + padH * 0.52
    );

    this.ctx.fillStyle = this.linked ? '#767079' : '#A49EA6';
    this.ctx.font = '400 9px monospace';
    const subText: string = this.streaming
      ? (this.codecText.length > 0 ? this.codecText : 'LIVE')
      : (this.linked ? 'USB READY' : 'STANDBY');
    this.ctx.fillText(subText, padCenterX, padY + padH * 0.74);
  }

  build() {
    Column({ space: 0 }) {
      // Card top spec bar
      Row({ space: 8 }) {
        Row({ space: 6 }) {
          Row()
            .width(5)
            .height(5)
            .backgroundColor(this.streaming ? '#9D193D' : (this.linked ? '#B9975B' : '#8C858E'))
          Text('TOPOLOGY // 副屏链路拓扑')
            .fontSize(9.5)
            .fontWeight(FontWeight.Medium)
            .letterSpacing(1.0)
            .fontColor('#8C858E')
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .layoutWeight(1)
        }.layoutWeight(1)
        Text(this.streaming ? '推流中' : (this.linked ? 'USB 已连接' : '待连接'))
          .fontSize(10)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.streaming ? '#9D193D' : (this.linked ? '#7D602B' : '#767079'))
          .padding({ left: 7, right: 7, top: 2, bottom: 2 })
          .backgroundColor(this.streaming ? '#F7EAEF' : '#FAF7F2')
          .border({ width: 0.7, color: this.streaming ? '#D9B6C2' : '#DCD6CC' })
          .borderRadius(2)
      }
      .width('100%')
      .padding({ left: 14, right: 14, top: 10, bottom: 8 })

      Divider().color('#EAE4DD').strokeWidth(0.75)

      Canvas(this.ctx)
        .width('100%')
        .height(this.compact ? (this.isPhone ? 148 : 170) : 216)
        .backgroundColor('#FCFBF8')
        .onReady(() => this.drawDiagram())
        .onAreaChange(() => this.drawDiagram())

      Divider().color('#EAE4DD').strokeWidth(0.75)

      Row({ space: 8 }) {
        Text('USB · AOA 硬件直连')
          .fontSize(10.5)
          .fontColor('#8C858E')
        Text(this.resolutionText.length > 0
          ? `${this.codecText || 'HEVC'} · ${this.resolutionText}`
          : (this.linked ? '正在同步分辨率' : '等待数据线握手'))
          .fontSize(10.5)
          .fontWeight(FontWeight.Medium)
          .fontColor('#5A545E')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .layoutWeight(1)
          .textAlign(TextAlign.End)
      }
      .width('100%')
      .padding({ left: 14, right: 14, top: 9, bottom: 10 })
      .backgroundColor('#FFFFFF')
    }
    .width('100%')
    .backgroundColor('#FFFFFF')
    .border({ width: 0.8, color: '#DCD6CC' })
    .borderRadius(4)
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

  aboutToAppear(): void {
    this.reportDisplayMetrics();
    native.setPreferredCodec(this.preferredCodec);
    window.getLastWindow(this.getUIContext().getHostContext()).then((appWindow: window.Window) => {
      this.mainWindow = appWindow;
      this.updateWindowMetrics(appWindow);
      appWindow.on('windowSizeChange', this.handleWindowSizeChange);
      appWindow.on('avoidAreaChange', this.handleAvoidAreaChange);
      this.applyWindowPresentation(appWindow);
    }).catch(() => {});
    this.accessoryLink.start();
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
    if (this.refreshTimer >= 0) {
      clearInterval(this.refreshTimer);
      this.refreshTimer = -1;
    }
    this.accessoryLink.stop();
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
    if (this.page === 'diagnostics') {
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
      return this.frameReady ? '扩展显示运行中' : '正在准备画面';
    }
    return '等待 USB 有线接入';
  }

  private statusDetail(): string {
    if (this.link.state === 'error') {
      return this.link.message.includes('permission')
        ? '点按下方重试，并在系统提示弹窗中允许 USB 配件连接。'
        : '请检查 Mac 端 DeskMate 状态与 USB 数据线连接后重试。';
    }
    if (this.link.state === 'authorizing') {
      return '请在系统提示弹窗中点击允许，建立高速 USB 配件通信通道。';
    }
    if (this.link.state === 'connected') {
      return this.frameReady
        ? '扩展桌面已建立并保持实时推流，可随时进入全屏扩展画面。'
        : 'USB 已连接，正在协商编码格式与虚拟显示器首帧画面。';
    }
    if (this.isPhone) {
      return '连接 Mac 后将自动匹配手机比例创建虚拟扩展屏，进入连接后自动横屏，支持低延迟画面与反向触控。';
    }
    return '连接 Mac 后将自动匹配平板比例创建虚拟扩展屏，支持低延迟画面与反向触控。';
  }

  private statusEyebrow(): string {
    if (this.link.state === 'connected') {
      return this.frameReady ? 'STREAMING // 画面就绪' : 'USB LINKED // 已连接';
    }
    if (this.link.state === 'error') {
      return 'FAULT // 连接异常';
    }
    if (this.link.state === 'authorizing') {
      return 'AUTH // 等待配件授权';
    }
    return 'STANDBY // 等待 USB 接入';
  }

  private statusAccent(): string {
    if (this.frameReady) {
      return '#2D7D5B';
    }
    if (this.link.state === 'error' || this.link.state === 'connected') {
      return '#9D193D';
    }
    if (this.link.state === 'authorizing') {
      return '#A8641E';
    }
    return '#8C858E';
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
  private sectionHeader(en: string, zh: string) {
    Row({ space: 10 }) {
      Row({ space: 6 }) {
        Text(en)
          .fontSize(10.5)
          .fontWeight(FontWeight.Medium)
          .letterSpacing(1.4)
          .fontColor('#9D193D')
        Text('/')
          .fontSize(10.5)
          .fontColor('#DCD6CC')
        Text(zh)
          .fontSize(11.5)
          .fontWeight(FontWeight.Medium)
          .fontColor('#232026')
      }
      Divider()
        .color('#DCD6CC')
        .strokeWidth(0.8)
        .layoutWeight(1)
    }.width('100%').margin({ top: 4, bottom: 2 })
  }

  @Builder
  private primaryButton(label: string, onTap: () => void, fullWidth: boolean) {
    Button({ type: ButtonType.Normal }) {
      Column() {
        Row().width('100%').height(1.2).backgroundColor('#75B9975B')
        Row({ space: 10 }) {
          Text(label)
            .fontSize(13.5)
            .fontWeight(FontWeight.Medium)
            .letterSpacing(0.6)
            .fontColor('#FFFFFF')
          Text('→')
            .fontSize(13.5)
            .fontColor('#FFFFFF')
        }
        .width(fullWidth ? '100%' : 'auto')
        .justifyContent(FlexAlign.Center)
        .padding({
          left: fullWidth ? 18 : 20,
          right: fullWidth ? 18 : 20,
          top: fullWidth ? 12 : 10,
          bottom: fullWidth ? 13 : 11
        })
      }
      .width(fullWidth ? '100%' : 'auto')
    }
    .width(fullWidth ? '100%' : 'auto')
    .backgroundColor('#9D193D')
    .border({ width: 0.8, color: '#7E1230' })
    .borderRadius(3)
    .onClick(onTap)
  }

  @Builder
  private stepRow(num: string, text: string) {
    Row({ space: 12 }) {
      Text(num)
        .fontSize(11)
        .fontWeight(FontWeight.Medium)
        .fontColor('#9D193D')
        .width(22)
      Text(text)
        .fontSize(13.5)
        .fontColor('#4D4752')
        .layoutWeight(1)
    }
    .width('100%')
    .padding({ top: 9, bottom: 9, left: 12, right: 12 })
  }

  @Builder
  private headerTab(target: Page, en: string, zh: string) {
    Button({ type: ButtonType.Normal }) {
      Column({ space: 5 }) {
        Row({ space: 6 }) {
          Row()
            .width(4)
            .height(4)
            .backgroundColor(this.page === target ? '#9D193D' : Color.Transparent)
            .border({ width: 0.8, color: this.page === target ? '#9D193D' : '#8C858E' })
          Text(`${en} / ${zh}`)
            .fontSize(11.5)
            .fontWeight(this.page === target ? FontWeight.Medium : FontWeight.Normal)
            .fontColor(this.page === target ? '#9D193D' : '#767079')
        }
        Row()
          .width('100%')
          .height(1.5)
          .backgroundColor(this.page === target ? '#9D193D' : Color.Transparent)
      }.padding({ top: 6, bottom: 2, left: 6, right: 6 })
    }
    .backgroundColor(Color.Transparent)
    .onClick(() => this.navigate(target))
  }

  @Builder
  private bottomTab(target: Page, en: string, zh: string) {
    Button({ type: ButtonType.Normal }) {
      Column({ space: 3 }) {
        Row()
          .width(26)
          .height(2)
          .backgroundColor(this.page === target ? '#9D193D' : Color.Transparent)
        Row({ space: 4 }) {
          Row()
            .width(4)
            .height(4)
            .backgroundColor(this.page === target ? '#9D193D' : Color.Transparent)
            .border({ width: 0.8, color: this.page === target ? '#9D193D' : '#8C858E' })
          Text(en)
            .fontSize(9)
            .fontWeight(FontWeight.Medium)
            .letterSpacing(1.0)
            .fontColor(this.page === target ? '#9D193D' : '#8C858E')
        }
        Text(zh)
          .fontSize(11.5)
          .fontWeight(this.page === target ? FontWeight.Medium : FontWeight.Normal)
          .fontColor(this.page === target ? '#232026' : '#767079')
      }
      .width('100%')
      .padding({ top: 6, bottom: 8, left: 4, right: 4 })
      .alignItems(HorizontalAlign.Center)
    }
    .layoutWeight(1)
    .backgroundColor(Color.Transparent)
    .onClick(() => this.navigate(target))
  }

  @Builder
  private connectionGuideCard() {
    Column() {
      this.stepRow('01', '确保 Mac 上的 DeskMate 已启动运行')
      Divider().color('#EAE4DD').strokeWidth(0.75)
      this.stepRow('02', `使用高速 USB 数据线连接 Mac 与${this.deviceLabel()}`)
      Divider().color('#EAE4DD').strokeWidth(0.75)
      this.stepRow('03', '弹出询问时点击允许访问 USB 配件')
    }
    .width('100%')
    .backgroundColor('#FFFFFF')
    .border({ width: 0.8, color: '#DCD6CC' })
    .borderRadius(4)
  }

  @Builder
  private liveSpecCard() {
    Column({ space: 8 }) {
      Row({ space: 12 }) {
        Text('显示规格')
          .fontSize(11.5)
          .fontColor('#8C858E')
          .width(64)
        Text(`${this.activeCodec || this.codecDisplayName(this.preferredCodec)} · ${this.activeResolution || '协商中'}`)
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor('#232026')
          .layoutWeight(1)
      }.width('100%')

      if (this.frameReady) {
        Divider().color('#EAE4DD').strokeWidth(0.75)
        Row({ space: 12 }) {
          Text('实时性能')
            .fontSize(11.5)
            .fontColor('#9D193D')
            .width(64)
          Text(`${this.fps} FPS · ${this.bitrateMbps.toFixed(1)} Mbps · 触控就绪`)
            .fontSize(13)
            .fontWeight(FontWeight.Medium)
            .fontColor('#232026')
            .layoutWeight(1)
        }.width('100%')
      }
    }
    .width('100%')
    .padding(12)
    .backgroundColor('#FFFFFF')
    .border({ width: 0.8, color: '#DCD6CC' })
    .borderRadius(4)
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

        Column({ space: 8 }) {
          Row({ space: 7 }) {
            Row()
              .width(5)
              .height(5)
              .backgroundColor(this.statusAccent())
            Text(this.statusEyebrow())
              .fontSize(10)
              .fontWeight(FontWeight.Medium)
              .letterSpacing(1.3)
              .fontColor(this.statusAccent())
          }

          Text(this.statusTitle())
            .fontSize(22)
            .fontWeight(FontWeight.Regular)
            .fontColor('#232026')

          Text(this.statusDetail())
            .fontSize(13)
            .lineHeight(19.5)
            .fontColor('#5A545E')
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
      Row({ space: 28 }) {
        // Left architectural wired dual-display schematic
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
        .width('43%')
        .alignItems(HorizontalAlign.Start)

        // Right editorial column
        Column({ space: 14 }) {
          Row({ space: 7 }) {
            Row()
              .width(5)
              .height(5)
              .backgroundColor(this.statusAccent())
            Text(this.statusEyebrow())
              .fontSize(10.5)
              .fontWeight(FontWeight.Medium)
              .letterSpacing(1.4)
              .fontColor(this.statusAccent())
          }

          Text(this.statusTitle())
            .fontSize(26)
            .fontWeight(FontWeight.Regular)
            .fontColor('#232026')

          Text(this.statusDetail())
            .fontSize(13.5)
            .lineHeight(20)
            .fontColor('#5A545E')

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

          Row({ space: 12 }) {
            Button('编码与浮窗设置', { type: ButtonType.Normal })
              .height(34)
              .padding({ left: 12, right: 12 })
              .fontSize(12)
              .fontColor('#232026')
              .backgroundColor('#FFFFFF')
              .border({ width: 0.8, color: '#DCD6CC' })
              .borderRadius(3)
              .onClick(() => this.navigate('settings'))

            Button('查看连接诊断', { type: ButtonType.Normal })
              .height(34)
              .padding({ left: 12, right: 12 })
              .fontSize(12)
              .fontColor('#9D193D')
              .backgroundColor('#F7EAEF')
              .border({ width: 0.8, color: '#D9B6C2' })
              .borderRadius(3)
              .onClick(() => this.navigate('diagnostics'))
          }.margin({ top: 2 })
        }
        .width('57%')
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .constraintSize({ maxWidth: 1040 })
      .padding({ left: 28, right: 28, top: 18, bottom: 18 })
      .alignItems(VerticalAlign.Center)
    }
  }

  @Builder
  private settingsFields(compact: boolean) {
    Column({ space: 14 }) {
      Column({ space: 4 }) {
        Text('SETTINGS // 显示与解码配置')
          .fontSize(10)
          .fontWeight(FontWeight.Medium)
          .letterSpacing(1.4)
          .fontColor('#9D193D')
        Text('系统设置')
          .fontSize(compact ? 20 : 21)
          .fontWeight(FontWeight.Regular)
          .fontColor('#232026')
      }.width('100%').alignItems(HorizontalAlign.Start)

      if (compact) {
        Row({ space: 10 }) {
          Text('当前编码与规格')
            .fontSize(11.5)
            .fontColor('#8C858E')
          Blank()
          Text(this.activeCodec.length > 0
            ? `${this.activeCodec}${this.activeResolution.length > 0 ? ' · ' + this.activeResolution : ''}`
            : this.codecDisplayName(this.preferredCodec))
            .fontSize(12.5)
            .fontWeight(FontWeight.Medium)
            .fontColor('#232026')
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 10, bottom: 10 })
        .backgroundColor('#FFFFFF')
        .border({ width: 0.8, color: '#DCD6CC' })
        .borderRadius(4)
      }

      this.sectionHeader('DISPLAY', '画面与性能浮窗')

      Row({ space: 12 }) {
        Column({ space: 3 }) {
          Text('性能参数浮窗')
            .fontSize(14.5)
            .fontWeight(FontWeight.Medium)
            .fontColor('#232026')
          Text('全屏投屏时在右上角显示帧率、码率、编码格式与丢帧统计')
            .fontSize(12)
            .fontColor('#767079')
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: this.showFps })
          .selectedColor('#9D193D')
          .accessibilityText('性能参数浮窗')
          .onChange((enabled: boolean) => {
            this.showFps = enabled;
          })
      }
      .width('100%')
      .padding({ left: 14, right: 14, top: 10, bottom: 10 })
      .backgroundColor('#FFFFFF')
      .border({ width: 0.8, color: '#DCD6CC' })
      .borderRadius(4)

      if (this.isPhone) {
        Column({ space: 3 }) {
          Text('横竖屏自动切换')
            .fontSize(14)
            .fontWeight(FontWeight.Medium)
            .fontColor('#232026')
          Text('连接前保持竖屏便于配置，进入扩展画面后自动切换横屏并支持左右翻转')
            .fontSize(12)
            .lineHeight(18)
            .fontColor('#767079')
        }
        .width('100%')
        .padding({ left: 14, right: 14, top: 10, bottom: 10 })
        .backgroundColor('#FFFFFF')
        .border({ width: 0.8, color: '#DCD6CC' })
        .borderRadius(4)
        .alignItems(HorizontalAlign.Start)
      }

      this.sectionHeader('CODEC', '视频编码格式')

      Text(this.activeCodec.length > 0
        ? `当前协商：${this.activeCodec}${this.activeResolution.length > 0 ? ' · ' + this.activeResolution : ''}`
        : '选择传输编码偏好，默认自动优先使用 HEVC 硬件解码')
        .fontSize(12)
        .fontColor('#767079')
        .width('100%')

      Row({ space: compact ? 8 : 10 }) {
        ForEach(CODEC_OPTIONS, (option: CodecOption) => {
          Button({ type: ButtonType.Normal }) {
            Column({ space: 4 }) {
              Row({ space: compact ? 4 : 6 }) {
                Row()
                  .width(4)
                  .height(4)
                  .backgroundColor(this.preferredCodec === option.value ? '#9D193D' : Color.Transparent)
                  .border({ width: 0.8, color: this.preferredCodec === option.value ? '#9D193D' : '#8C858E' })
                Text(option.title)
                  .fontSize(compact ? 13 : 14)
                  .fontWeight(this.preferredCodec === option.value ? FontWeight.Medium : FontWeight.Normal)
                  .fontColor(this.preferredCodec === option.value ? '#9D193D' : '#232026')
              }
              Text(option.subtitle)
                .fontSize(compact ? 10.5 : 11)
                .fontColor(this.preferredCodec === option.value ? '#9D193D' : '#767079')
            }
            .width('100%')
            .padding({ top: 10, bottom: 10, left: compact ? 6 : 10, right: compact ? 6 : 10 })
            .backgroundColor(this.preferredCodec === option.value ? '#F7EAEF' : '#FFFFFF')
            .border({
              width: 0.8,
              color: this.preferredCodec === option.value ? '#D9B6C2' : '#DCD6CC'
            })
            .borderRadius(3)
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
            .fontColor('#9D193D')
            .layoutWeight(1)
          Button('立即重连', { type: ButtonType.Normal })
            .height(30)
            .padding({ left: 12, right: 12 })
            .fontSize(11.5)
            .fontColor(Color.White)
            .backgroundColor('#9D193D')
            .borderRadius(3)
            .onClick(() => {
              this.codecChangedWhileConnected = false;
              this.accessoryLink.reconnect();
            })
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 8, bottom: 8 })
        .backgroundColor('#F7EAEF')
        .border({ width: 0.8, color: '#D9B6C2' })
        .borderRadius(3)
      }

      this.sectionHeader('DIAGNOSTICS', '实时诊断')

      Button({ type: ButtonType.Normal }) {
        Row({ space: 12 }) {
          Column({ space: 2 }) {
            Text('查看连接与解码统计')
              .fontSize(14)
              .fontWeight(FontWeight.Medium)
              .fontColor('#232026')
            Text('检视 USB 状态、协商参数、码率与帧率统计')
              .fontSize(12)
              .fontColor('#767079')
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)

          Text('查看 →')
            .fontSize(11.5)
            .fontWeight(FontWeight.Medium)
            .fontColor('#9D193D')
        }
        .width('100%')
        .padding({ top: 10, bottom: 10, left: 14, right: 14 })
        .backgroundColor('#FFFFFF')
        .border({ width: 0.8, color: '#DCD6CC' })
        .borderRadius(4)
      }
      .width('100%')
      .backgroundColor(Color.Transparent)
      .onClick(() => this.navigate('diagnostics'))
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
      Row({ space: 26 }) {
        // Left wired schematic card
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
              .fontSize(10.5)
              .fontColor('#8C858E')
            Text(this.activeCodec.length > 0
              ? `${this.activeCodec}${this.activeResolution.length > 0 ? ' · ' + this.activeResolution : ''}`
              : this.codecDisplayName(this.preferredCodec))
              .fontSize(12.5)
              .fontWeight(FontWeight.Medium)
              .fontColor('#232026')
          }
          .width('100%')
          .padding(12)
          .backgroundColor('#FFFFFF')
          .border({ width: 0.8, color: '#DCD6CC' })
          .borderRadius(4)
          .alignItems(HorizontalAlign.Start)
        }
        .width('34%')
        .alignItems(HorizontalAlign.Start)

        // Right settings column
        Column() {
          this.settingsFields(false)
        }
        .width('66%')
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .constraintSize({ maxWidth: 1040 })
      .padding({ left: 28, right: 28, top: 16, bottom: 24 })
      .alignItems(VerticalAlign.Top)
    }
  }

  @Builder
  private diagnosticsRowsCard(compact: boolean) {
    Column() {
      ForEach(this.diagnosticRows(), (row: DiagnosticRow, index: number) => {
        Column() {
          if (index > 0) {
            Divider().color('#EAE4DD').strokeWidth(0.75)
          }
          Row({ space: 12 }) {
            Text(row.label)
              .fontSize(compact ? 12 : 12.5)
              .fontColor('#767079')
              .width(compact ? 76 : 96)
            Text(row.value)
              .fontSize(compact ? 12 : 12.5)
              .fontWeight(FontWeight.Medium)
              .fontColor('#232026')
              .layoutWeight(1)
          }.width('100%').padding({ top: 8, bottom: 8, left: 12, right: 12 })
        }.width('100%')
      }, (row: DiagnosticRow) => `${row.label}:${row.value}`)
    }
    .width('100%')
    .backgroundColor('#FFFFFF')
    .border({ width: 0.8, color: '#DCD6CC' })
    .borderRadius(4)
  }

  @Builder
  private reconnectUsbButton(height: number) {
    Button('重新握手 USB 连接', { type: ButtonType.Normal })
      .width('100%')
      .height(height)
      .fontSize(12)
      .fontWeight(FontWeight.Medium)
      .fontColor('#9D193D')
      .backgroundColor('#F7EAEF')
      .border({ width: 0.8, color: '#D9B6C2' })
      .borderRadius(3)
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
        Column({ space: 4 }) {
          Text('DIAGNOSTICS // 会话与传输摘要')
            .fontSize(10)
            .fontWeight(FontWeight.Medium)
            .letterSpacing(1.4)
            .fontColor('#9D193D')
          Text('连接与解码诊断')
            .fontSize(20)
            .fontWeight(FontWeight.Regular)
            .fontColor('#232026')
        }.width('100%').alignItems(HorizontalAlign.Start)

        Divider().color('#DCD6CC').strokeWidth(0.8)

        this.diagnosticsRowsCard(true)

        this.reconnectUsbButton(40)
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 14, bottom: 20 })
      .alignItems(HorizontalAlign.Start)
    } else {
      Row({ space: 26 }) {
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

          this.reconnectUsbButton(34)
        }
        .width('34%')
        .alignItems(HorizontalAlign.Start)

        Column({ space: 12 }) {
          Column({ space: 4 }) {
            Text('DIAGNOSTICS // 会话与传输摘要')
              .fontSize(10)
              .fontWeight(FontWeight.Medium)
              .letterSpacing(1.4)
              .fontColor('#9D193D')
            Text('连接与解码诊断')
              .fontSize(21)
              .fontWeight(FontWeight.Regular)
              .fontColor('#232026')
          }.width('100%').alignItems(HorizontalAlign.Start)

          Divider().color('#DCD6CC').strokeWidth(0.8)

          this.diagnosticsRowsCard(false)

          Row({ space: 12 }) {
            Button('← 返回系统设置', { type: ButtonType.Normal })
              .height(34)
              .padding({ left: 12, right: 12 })
              .fontSize(12)
              .fontColor('#5A545E')
              .backgroundColor('#FFFFFF')
              .border({ width: 0.8, color: '#DCD6CC' })
              .borderRadius(3)
              .onClick(() => this.navigate('settings'))

            Button('返回主页', { type: ButtonType.Normal })
              .height(34)
              .padding({ left: 12, right: 12 })
              .fontSize(12)
              .fontColor('#9D193D')
              .backgroundColor('#F7EAEF')
              .border({ width: 0.8, color: '#D9B6C2' })
              .borderRadius(3)
              .onClick(() => this.navigate('home'))
          }
        }
        .width('66%')
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .constraintSize({ maxWidth: 1040 })
      .padding({ left: 28, right: 28, top: 16, bottom: 24 })
      .alignItems(VerticalAlign.Top)
    }
  }

  @Builder
  private shell() {
    Column() {
      // Top navigation header
      if (this.isCompactLayout()) {
        Row({ space: 12 }) {
          if (this.page !== 'home') {
            Button({ type: ButtonType.Normal }) {
              Row({ space: 6 }) {
                Text('←')
                  .fontSize(13)
                  .fontColor('#9D193D')
                Text('返回')
                  .fontSize(12)
                  .fontColor('#232026')
              }
            }
            .height(32)
            .padding({ left: 11, right: 11 })
            .backgroundColor('#FFFFFF')
            .border({ width: 0.8, color: '#DCD6CC' })
            .borderRadius(3)
            .onClick(() => this.onBackPress())
          } else {
            Row({ space: 8 }) {
              Row()
                .width(5)
                .height(5)
                .backgroundColor(this.statusAccent())
              Text('DESKMATE')
                .fontSize(13.5)
                .fontWeight(FontWeight.Medium)
                .letterSpacing(1.8)
                .fontColor('#232026')
            }
          }

          Blank()

          Row({ space: 6 }) {
            Row()
              .width(4)
              .height(4)
              .backgroundColor(this.statusAccent())
            Text(this.shortStatusLabel())
              .fontSize(10.5)
              .fontWeight(FontWeight.Medium)
              .fontColor(this.frameReady || this.link.state === 'error' ? '#9D193D' : '#5A545E')
          }
          .padding({ left: 9, right: 9, top: 4, bottom: 4 })
          .backgroundColor(this.frameReady || this.link.state === 'error' ? '#F7EAEF' : '#FAF7F2')
          .border({ width: 0.8, color: this.frameReady || this.link.state === 'error' ? '#D9B6C2' : '#DCD6CC' })
          .borderRadius(3)
        }
        .width('100%')
        .padding({ left: 16, right: 16, top: Math.max(this.safeTopVp + 6, 14), bottom: 10 })
        .backgroundColor('#CCFFFFFF')
      } else {
        Row({ space: 16 }) {
          if (this.page !== 'home') {
            Button({ type: ButtonType.Normal }) {
              Row({ space: 6 }) {
                Text('←')
                  .fontSize(13)
                  .fontColor('#9D193D')
                Text('返回')
                  .fontSize(12)
                  .fontColor('#232026')
              }
            }
            .height(34)
            .padding({ left: 12, right: 12 })
            .backgroundColor('#FFFFFF')
            .border({ width: 0.8, color: '#DCD6CC' })
            .borderRadius(3)
            .onClick(() => this.onBackPress())
          } else {
            Row({ space: 8 }) {
              Row()
                .width(5)
                .height(5)
                .backgroundColor(this.statusAccent())
              Text('DESKMATE')
                .fontSize(14)
                .fontWeight(FontWeight.Medium)
                .letterSpacing(2.0)
                .fontColor('#232026')
              Text('// 扩展屏终端')
                .fontSize(11)
                .fontColor('#8C858E')
            }
          }

          Blank()

          Row({ space: 8 }) {
            this.headerTab('home', 'DISPLAY', '扩展显示')
            this.headerTab('settings', 'SETTINGS', '系统设置')
            this.headerTab('diagnostics', 'DIAGNOSTICS', '连接诊断')
          }
        }
        .width('100%')
        .padding({ left: 28, right: 28, top: Math.max(this.safeTopVp + 8, 28), bottom: 8 })
        .backgroundColor('#CCFFFFFF')
      }

      Divider().color('#DCD6CC').strokeWidth(0.8)

      Scroll() {
        Column() {
          if (this.page === 'home') {
            this.homeContent()
          } else if (this.page === 'settings') {
            this.settingsContent()
          } else {
            this.diagnosticsContent()
          }
        }
        .width('100%')
      }
      .width('100%')
      .layoutWeight(1)
      .align(!this.isCompactLayout() && this.page === 'home' ? Alignment.Center : Alignment.Top)

      Divider().color('#DCD6CC').strokeWidth(0.8)

      // Bottom navigation (compact) or whisper status bar (tablet)
      if (this.isCompactLayout()) {
        Row({ space: 0 }) {
          this.bottomTab('home', 'DISPLAY', '扩展显示')
          this.bottomTab('settings', 'SETTINGS', '系统设置')
          this.bottomTab('diagnostics', 'DIAG', '连接诊断')
        }
        .width('100%')
        .padding({ left: 8, right: 8, bottom: this.safeBottomVp })
        .backgroundColor('#F2FFFFFF')
      } else {
        Row() {
          Text('USB · AOA 硬件直连  //  自动 HiDPI 虚拟屏  //  多点与双指触控')
            .fontSize(10.5)
            .fontColor('#8C858E')
          Blank()
          Row({ space: 6 }) {
            Row()
              .width(4)
              .height(4)
              .backgroundColor(this.statusAccent())
            Text(`${this.activeCodec.length > 0 ? this.activeCodec : this.preferredCodec.toUpperCase()} · ${this.activeResolution.length > 0 ? this.activeResolution : '待机'}`)
              .fontSize(10.5)
              .fontColor('#5A545E')
          }
        }
        .width('100%')
        .constraintSize({ minHeight: 30 + this.safeBottomVp })
        .padding({ left: 28, right: 28, top: 6, bottom: Math.max(6, this.safeBottomVp) })
        .backgroundColor('#CCFFFFFF')
      }
    }
    .width('100%')
    .height('100%')
    .radialGradient({
      center: ['45%', '42%'],
      radius: 680,
      colors: [['#FFFFFF', 0.0], ['#FCFBF8', 0.58], ['#F4F0E8', 1.0]]
    })
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

      if (this.page !== 'display') {
        this.shell()
      } else {
        Row({ space: 8 }) {
          if (this.controlsExpanded) {
            Button('← 返回主页', { type: ButtonType.Normal })
              .height(this.isPhone ? 34 : 40)
              .padding({ left: this.isPhone ? 11 : 14, right: this.isPhone ? 11 : 14 })
              .fontSize(this.isPhone ? 12 : 12.5)
              .fontWeight(FontWeight.Medium)
              .fontColor('#232026')
              .backgroundColor('#F5FCFBF8')
              .border({ width: 0.8, color: '#DCD6CC' })
              .borderRadius(4)
              .onClick(() => this.navigate('home'))
          }
          Button({ type: ButtonType.Normal }) {
            SymbolGlyph(this.controlsExpanded ? $r('sys.symbol.chevron_up') : $r('sys.symbol.chevron_down'))
              .fontSize(this.isPhone ? 16 : 18).fontColor(['#FCFBF8'])
          }
          .width(this.isPhone ? 34 : 40)
          .height(this.isPhone ? 34 : 40)
          .borderRadius(4)
          .backgroundColor('#D918161B')
          .border({ width: 0.8, color: '#5A5059' })
          .accessibilityText(this.controlsExpanded ? '收起显示工具栏' : '展开显示工具栏')
          .onClick(() => { this.controlsExpanded = !this.controlsExpanded; })

          Blank()

          if (this.showFps) {
            Row({ space: this.isPhone ? 7 : 10 }) {
              Row()
                .width(5)
                .height(5)
                .backgroundColor('#9D193D')
              Row({ space: 4 }) {
                Text(`${this.fps}`)
                  .fontSize(this.isPhone ? 11.5 : 13)
                  .fontWeight(FontWeight.Bold)
                  .fontColor('#8BF2C4')
                Text('FPS')
                  .fontSize(this.isPhone ? 10 : 10.5)
                  .fontColor('#D6D1CC')
              }
              Divider().vertical(true).height(12).color('#4B444E')
              Row({ space: 4 }) {
                Text(`${this.bitrateMbps.toFixed(1)}`)
                  .fontSize(this.isPhone ? 11.5 : 13)
                  .fontWeight(FontWeight.Bold)
                  .fontColor('#FCFBF8')
                Text('Mbps')
                  .fontSize(this.isPhone ? 10 : 10.5)
                  .fontColor('#D6D1CC')
              }
              Divider().vertical(true).height(12).color('#4B444E')
              Text(this.activeCodec.length > 0 ? this.activeCodec : '--')
                .fontSize(this.isPhone ? 11 : 12)
                .fontWeight(FontWeight.Medium)
                .fontColor('#E6C994')
              if (this.activeResolution.length > 0) {
                Divider().vertical(true).height(12).color('#4B444E')
                Text(this.activeResolution)
                  .fontSize(this.isPhone ? 11 : 12)
                  .fontColor('#E2DDD6')
              }
              if (this.droppedFrames > 0) {
                Divider().vertical(true).height(12).color('#4B444E')
                Text(`丢帧 ${this.droppedFrames}`)
                  .fontSize(this.isPhone ? 11 : 12)
                  .fontColor('#F8B4B4')
              }
            }
            .padding({
              left: this.isPhone ? 9 : 12,
              right: this.isPhone ? 9 : 12,
              top: this.isPhone ? 5 : 7,
              bottom: this.isPhone ? 5 : 7
            })
            .backgroundColor('#D918161B')
            .borderRadius(4)
            .border({ width: 0.8, color: '#5A5059' })
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

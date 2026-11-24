import { AccessoryLink, LinkSnapshot } from '../deskmate/AccessoryLink';
import native from 'libdeskmate_device.so';
import { window } from '@kit.ArkUI';

PersistentStorage.persistProp('deskMateShowFps', false);

const KIND_TOUCH_DOWN: number = 1;
const KIND_TOUCH_MOVE: number = 2;
const KIND_TOUCH_UP: number = 3;
const KIND_TOUCH_CANCEL: number = 4;
const KIND_SCROLL: number = 5;
const FLAG_PRIMARY: number = 0x0001;
const SCROLL_THRESHOLD: number = 0.0005;

interface EngineSnapshot {
  session: string;
  failure: string;
  profile: string;
  lifecycle: string;
  surface: string;
  framesReceived: number;
  framesDropped: number;
  decoderRendered: number;
  decoderSubmitted: number;
  decoderOpens: number;
  inputSent: number;
  inputRejected: number;
  error: string;
}

interface NormalizedPoint {
  x: number;
  y: number;
}

type Page = 'home' | 'settings' | 'diagnostics' | 'display';

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
  @State private fps: number = 0;

  private accessoryLink: AccessoryLink =
    new AccessoryLink((snapshot: LinkSnapshot) => {
      if (snapshot.state !== 'connected' && this.link.state === 'connected') {
        this.frameReady = false;
        this.enteredThisLink = false;
        this.fps = 0;
        this.lastRendered = 0;
        this.lastRenderedAt = 0;
        this.engine = '{}';
        this.scrolling = false;
        this.pointerDown = false;
        if (this.page === 'display') {
          this.navigate('home');
        }
      }
      this.link = snapshot;
    });
  private refreshTimer: number = -1;
  private surfaceWidth: number = 1;
  private surfaceHeight: number = 1;
  private lastRendered: number = 0;
  private lastRenderedAt: number = 0;
  private enteredThisLink: boolean = false;
  private pointerDown: boolean = false;

  // One pointer drives the host cursor; a second pointer switches the gesture to
  // scroll and cancels the cursor drag so no button is left pressed.
  private scrolling: boolean = false;
  private lastX: number = 0;
  private lastY: number = 0;
  private lastTwoFingerY: number = 0;

  aboutToAppear(): void {
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
      this.updateFps(parsed);
      if (parsed.session === 'streaming' && parsed.surface === 'attached' && parsed.decoderRendered > 0) {
        this.frameReady = true;
        if (!this.enteredThisLink) {
          this.enteredThisLink = true;
          this.navigate('display');
        }
      } else if (parsed.session !== 'streaming' && parsed.session !== 'paused') {
        this.frameReady = false;
      }
    }, 250);
  }

  aboutToDisappear(): void {
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

  private updateFps(parsed: EngineSnapshot | undefined): void {
    if (parsed === undefined) {
      this.fps = 0;
      return;
    }
    const now: number = Date.now();
    const rendered: number = parsed.decoderRendered;
    if (this.lastRenderedAt > 0 && now > this.lastRenderedAt &&
      rendered >= this.lastRendered) {
      const delta: number = rendered - this.lastRendered;
      this.fps = Math.round(delta * 1000 / (now - this.lastRenderedAt));
    } else {
      this.fps = 0;
    }
    this.lastRendered = rendered;
    this.lastRenderedAt = now;
  }

  private normalize(x: number, y: number): NormalizedPoint {
    const width: number = this.surfaceWidth > 0 ? this.surfaceWidth : 1;
    const height: number = this.surfaceHeight > 0 ? this.surfaceHeight : 1;
    const point: NormalizedPoint = {
      x: Math.min(1, Math.max(0, x / width)),
      y: Math.min(1, Math.max(0, y / height))
    };
    return point;
  }

  private averageY(event: TouchEvent): number {
    const height: number = this.surfaceHeight > 0 ? this.surfaceHeight : 1;
    let total: number = 0;
    for (const touch of event.touches) {
      total += touch.y;
    }
    const count: number = event.touches.length;
    return count > 0 ? (total / count) / height : 0;
  }

  private onTouchEvent(event: TouchEvent): void {
    if (this.page !== 'display' || this.link.state !== 'connected' || !this.frameReady) {
      return;
    }
    const count: number = event.touches.length;
    if (event.type === TouchType.Down) {
      if (count >= 2) {
        if (!this.scrolling) {
          this.scrolling = true;
          native.sendInput(KIND_TOUCH_CANCEL, 0, 0, this.lastX, this.lastY,
            0, 0, 0);
          this.pointerDown = false;
        }
        this.lastTwoFingerY = this.averageY(event);
        return;
      }
      if (count === 0) {
        return;
      }
      const first: TouchObject = event.touches[0];
      const point: NormalizedPoint = this.normalize(first.x, first.y);
      this.lastX = point.x;
      this.lastY = point.y;
      native.sendInput(KIND_TOUCH_DOWN, 0, FLAG_PRIMARY, point.x, point.y,
        0, 0, 0);
      this.pointerDown = true;
      return;
    }

    if (event.type === TouchType.Move) {
      if (this.scrolling || count >= 2) {
        const average: number = this.averageY(event);
        const delta: number = average - this.lastTwoFingerY;
        this.lastTwoFingerY = average;
        if (Math.abs(delta) > SCROLL_THRESHOLD) {
          native.sendInput(KIND_SCROLL, 0, 0, 0.5, 0.5, 0, delta, 0);
        }
        return;
      }
      if (count === 0 || !this.pointerDown) {
        return;
      }
      const first: TouchObject = event.touches[0];
      const point: NormalizedPoint = this.normalize(first.x, first.y);
      this.lastX = point.x;
      this.lastY = point.y;
      native.sendInput(KIND_TOUCH_MOVE, 0, FLAG_PRIMARY, point.x, point.y,
        0, 0, 0);
      return;
    }

    if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
      if (this.scrolling) {
        if (count <= 1) {
          this.scrolling = false;
        }
        return;
      }
      if (this.pointerDown) {
        this.pointerDown = false;
        const kind: number = event.type === TouchType.Cancel ? KIND_TOUCH_CANCEL : KIND_TOUCH_UP;
        native.sendInput(kind, 0, 0, this.lastX, this.lastY, 0, 0, 0);
      }
    }
  }

  private navigate(page: Page): void {
    if (this.page === 'display' && page !== 'display') {
      if (this.pointerDown) {
        native.sendInput(KIND_TOUCH_CANCEL, 0, 0, this.lastX, this.lastY, 0, 0, 0);
      }
      this.pointerDown = false;
      this.scrolling = false;
    }
    this.controlsExpanded = false;
    this.page = page;
    window.getLastWindow(this.getUIContext().getHostContext()).then((appWindow: window.Window) => {
      const bars: Array<'status' | 'navigation'> = this.page === 'display' ? [] : ['status', 'navigation'];
      return appWindow.setWindowSystemBarEnable(bars);
    }).catch(() => {});
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
      return this.frameReady ? '扩展显示仍在运行' : '正在准备画面';
    }
    return '等待 USB 连接';
  }

  private statusDetail(): string {
    if (this.link.state === 'error') {
      return this.link.message.includes('permission')
        ? '点按重试，并在系统提示中允许 USB 配件连接。'
        : '检查 Mac 上的发送状态和 USB 数据线，然后重试。';
    }
    if (this.link.state === 'authorizing') {
      return '请在系统提示中允许 USB 配件连接。';
    }
    if (this.link.state === 'connected') {
      return this.frameReady ? '当前连接已保留，可以随时返回画面。' : '已连接 USB，正在与 Mac 建立会话并等待画面。';
    }
    return 'Mac 上的扩展桌面将在这里显示。';
  }

  private engineDetail(): string {
    const parsed: EngineSnapshot | undefined = this.parseEngine();
    if (parsed === undefined) {
      return '暂无会话数据';
    }
    return `会话：${parsed.session}\n码流：${parsed.profile || '未协商'}\n已输出帧：${parsed.decoderRendered}\n${parsed.failure}\n${parsed.error}`;
  }

  @Builder
  private step(number: string, title: string) {
    Divider().color('#DCE3EB')
    Row({ space: 12 }) {
      Text(number).fontSize(13).fontWeight(FontWeight.Medium)
        .fontColor('#315385').width(26).height(26).textAlign(TextAlign.Center)
        .backgroundColor('#E6EDF8').borderRadius(7)
      Text(title).fontSize(15).fontColor('#202936').layoutWeight(1)
    }.width('100%').padding({ top: 9, bottom: 9 })
  }

  @Builder
  private homeContent() {
    Column({ space: 8 }) {
      Image($r('app.media.usb_link')).width('86%').height(126).objectFit(ImageFit.Contain)
        .accessibilityText('用 USB 数据线连接 Mac 与鸿蒙平板')
      Row() {
        Text('Mac').layoutWeight(1).textAlign(TextAlign.Center).fontSize(13).fontColor('#506073')
        Blank().width('24%')
        Text('鸿蒙平板').layoutWeight(1).textAlign(TextAlign.Center).fontSize(13).fontColor('#506073')
      }.width('86%').margin({ bottom: 8 })
      if (this.link.state === 'connected') {
        Text(this.frameReady ? '画面可用' : 'USB 已连接').fontSize(13).fontWeight(FontWeight.Medium)
          .fontColor(this.frameReady ? '#17634C' : '#315385')
          .backgroundColor(this.frameReady ? '#E5F2EC' : '#E8EFFE')
          .borderRadius(6).padding({ left: 9, right: 9, top: 4, bottom: 4 })
      }
      Text(this.statusTitle()).fontSize(32).fontWeight(FontWeight.Bold).fontColor('#202936')
        .textAlign(TextAlign.Center)
      Text(this.statusDetail()).fontSize(16).fontColor('#506073').textAlign(TextAlign.Center)
      if (this.link.state === 'searching') {
        Column() {
          this.step('1', '打开 Mac 上的 DeskMate')
          this.step('2', '用 USB 数据线连接两台设备')
          this.step('3', '出现系统提示时，允许 USB 配件连接')
        }.width('100%').constraintSize({ maxWidth: 370 }).margin({ top: 16 })
      } else if (this.link.state === 'error') {
        Button('重试').backgroundColor('#2152BF').height(48).onClick(() => this.accessoryLink.retry())
          .margin({ top: 12 })
      } else if (this.frameReady) {
        Button('返回画面').backgroundColor('#2152BF').height(48).onClick(() => this.navigate('display'))
          .margin({ top: 12 })
      }
    }.width('100%').constraintSize({ maxWidth: 540 }).padding(20)
  }

  @Builder
  private settingsContent() {
    Column({ space: 10 }) {
      Text('画面').fontSize(14).fontColor('#506073').width('100%')
      Row({ space: 12 }) {
        Column({ space: 4 }) {
          Text('全屏显示帧率').fontSize(16).fontWeight(FontWeight.Medium).fontColor('#202936')
          Text('显示平板侧解码输出的帧率').fontSize(13).fontColor('#506073')
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        Toggle({ type: ToggleType.Switch, isOn: this.showFps })
          .selectedColor('#2152BF').accessibilityText('全屏显示帧率')
          .onChange((enabled: boolean) => { this.showFps = enabled; })
      }.width('100%').padding(16).backgroundColor(Color.White).borderRadius(12)
      Text('连接').fontSize(14).fontColor('#506073').width('100%').margin({ top: 12 })
      Button({ type: ButtonType.Normal }) {
        Row({ space: 12 }) {
          Column({ space: 4 }) {
            Text('连接诊断').fontSize(16).fontWeight(FontWeight.Medium).fontColor('#202936')
            Text('查看 USB、会话和画面状态').fontSize(13).fontColor('#506073')
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(17).fontColor(['#506073'])
        }.width('100%').padding(16)
      }.width('100%').backgroundColor(Color.White).borderRadius(12)
        .onClick(() => this.navigate('diagnostics'))
    }.width('100%').constraintSize({ maxWidth: 720 }).padding({ top: 24, left: 24, right: 24, bottom: 24 })
  }

  @Builder
  private shell() {
    Column() {
      Row({ space: 8 }) {
        if (this.page !== 'home') {
          Button({ type: ButtonType.Normal }) {
            SymbolGlyph($r('sys.symbol.chevron_left')).fontSize(20).fontColor(['#2152BF'])
          }.width(48).height(48).backgroundColor('#E8EFFE').borderRadius(10)
            .accessibilityText('返回').onClick(() => this.onBackPress())
        } else {
          SymbolGlyph($r('sys.symbol.rectangle_on_rectangle')).fontSize(22).fontColor(['#2152BF'])
        }
        Text(this.page === 'home' ? 'DeskMate' : this.page === 'settings' ? '设置' : '连接诊断')
          .fontSize(22).fontWeight(FontWeight.Bold).fontColor('#202936').layoutWeight(1)
        if (this.page === 'home') {
          Button('设置', { type: ButtonType.Normal }).height(48).padding({ left: 16, right: 16 })
            .fontColor('#314763').backgroundColor(Color.White).borderRadius(10)
            .onClick(() => this.navigate('settings'))
        }
      }.width('100%').padding({ left: 24, right: 24, top: 36, bottom: 8 })
      Scroll() {
        Column() {
          if (this.page === 'home') {
            this.homeContent()
          } else if (this.page === 'settings') {
            this.settingsContent()
          } else {
            Text(`${this.statusTitle()}\nUSB：${this.link.message}\n${this.engineDetail()}`)
              .fontSize(15).fontColor('#202936').lineHeight(25)
              .width('100%').constraintSize({ maxWidth: 650 }).padding(22)
              .backgroundColor(Color.White).borderRadius(12)
          }
        }.width('100%').padding({ bottom: 24 })
      }.width('100%').layoutWeight(1)
        .align(this.page === 'home' ? Alignment.Center : Alignment.Top)
    }.width('100%').height('100%').backgroundColor('#F4F7FB').zIndex(1)
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
        .onAreaChange((oldArea: Area, newArea: Area) => {
          this.surfaceWidth = Number(newArea.width);
          this.surfaceHeight = Number(newArea.height);
        })
        .onTouch((event: TouchEvent) => this.onTouchEvent(event))

      if (this.page !== 'display') {
        this.shell()
      } else {
        Row({ space: 8 }) {
          if (this.controlsExpanded) {
            Button('返回首页').height(48).fontColor('#233A5E').backgroundColor('#F4F7FC')
              .onClick(() => this.navigate('home'))
          }
          Button({ type: ButtonType.Normal }) {
            SymbolGlyph(this.controlsExpanded ? $r('sys.symbol.chevron_up') : $r('sys.symbol.chevron_down'))
              .fontSize(18).fontColor([Color.White])
          }.width(48).height(48).borderRadius(12).backgroundColor('#B322344D')
            .accessibilityText(this.controlsExpanded ? '收起显示工具栏' : '展开显示工具栏')
            .onClick(() => { this.controlsExpanded = !this.controlsExpanded; })
          Blank()
          if (this.showFps) {
            Text(`${this.fps} 帧/秒`).fontSize(13).fontColor(Color.White)
              .backgroundColor('#B322344D').padding(8).borderRadius(7)
              .hitTestBehavior(HitTestMode.None)
          }
        }
        .width('100%')
        .padding({ left: 14, right: 14, top: 12 })
        .hitTestBehavior(HitTestMode.Transparent)
        .zIndex(2)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}

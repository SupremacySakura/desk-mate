import { AccessoryLink, LinkSnapshot } from '../deskmate/AccessoryLink';
import native from 'libdeskmate_device.so';

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

@Entry
@Component
struct Index {
  @State private link: LinkSnapshot =
    { state: 'searching', message: 'starting', accessoryFd: -1 };
  @State private engine: string = '{}';
  @State private overlayDim: boolean = false;
  @State private fps: number = 0;

  private accessoryLink: AccessoryLink =
    new AccessoryLink((snapshot: LinkSnapshot) => {
      this.link = snapshot;
    });
  private refreshTimer: number = -1;
  private surfaceWidth: number = 1;
  private surfaceHeight: number = 1;
  private lastRendered: number = 0;
  private lastRenderedAt: number = 0;

  // One pointer drives the host cursor; a second pointer switches the gesture to
  // scroll and cancels the cursor drag so no button is left pressed.
  private scrolling: boolean = false;
  private lastX: number = 0;
  private lastY: number = 0;
  private lastTwoFingerY: number = 0;

  aboutToAppear(): void {
    this.accessoryLink.start();
    this.refreshTimer = setInterval(() => {
      this.engine = native.snapshotJson();
      const parsed: EngineSnapshot | undefined = this.parseEngine();
      this.overlayDim = parsed !== undefined && parsed.session === 'streaming';
      this.updateFps(parsed);
    }, 500);
  }

  aboutToDisappear(): void {
    if (this.refreshTimer >= 0) {
      clearInterval(this.refreshTimer);
      this.refreshTimer = -1;
    }
    this.accessoryLink.stop();
  }

  private parseEngine(): EngineSnapshot | undefined {
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
    if (this.link.state !== 'connected') {
      return;
    }
    const count: number = event.touches.length;
    if (event.type === TouchType.Down) {
      if (count >= 2) {
        if (!this.scrolling) {
          this.scrolling = true;
          native.sendInput(KIND_TOUCH_CANCEL, 0, 0, this.lastX, this.lastY,
            0, 0, 0);
        }
        this.lastTwoFingerY = this.averageY(event);
        return;
      }
      const first: TouchObject = event.touches[0];
      const point: NormalizedPoint = this.normalize(first.x, first.y);
      this.lastX = point.x;
      this.lastY = point.y;
      native.sendInput(KIND_TOUCH_DOWN, 0, FLAG_PRIMARY, point.x, point.y,
        0, 0, 0);
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
      if (count === 0) {
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
      native.sendInput(KIND_TOUCH_UP, 0, 0, this.lastX, this.lastY, 0, 0, 0);
    }
  }

  private statusColor(): string {
    if (this.link.state === 'connected') {
      return '#3FB950';
    }
    if (this.link.state === 'error') {
      return '#F85149';
    }
    return '#D29922';
  }

  private statusText(): string {
    if (this.link.state !== 'connected') {
      return this.link.message;
    }
    const parsed: EngineSnapshot | undefined = this.parseEngine();
    if (parsed === undefined) {
      return 'connected';
    }
    if (parsed.failure.length > 0) {
      return `${parsed.session} - ${parsed.failure}`;
    }
    const profile: string =
      parsed.profile.length > 0 ? parsed.profile : 'negotiating';
    const rate: string = parsed.session === 'streaming' ? ` - ${this.fps} fps` : '';
    return `${parsed.session} - ${profile}${rate}`;
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

      Column() {
        Row({ space: 8 }) {
          Circle({ width: 8, height: 8 })
            .fill(this.statusColor())
          Text(this.statusText())
            .fontSize(12)
            .fontColor('#E6EDF3')
            .layoutWeight(1)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 6, bottom: 6 })
        .backgroundColor('#B3000000')

        Blank()
      }
      .width('100%')
      .height('100%')
      .hitTestBehavior(HitTestMode.None)
      .opacity(this.overlayDim ? 0.0 : 1.0)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
    .onAreaChange((oldArea: Area, newArea: Area) => {
      this.surfaceWidth = Number(newArea.width);
      this.surfaceHeight = Number(newArea.height);
    })
    .onTouch((event: TouchEvent) => {
      this.onTouchEvent(event);
    })
  }
}

import { AccessoryProbe, WakeMode } from '../probe/AccessoryProbe';

@Entry
@Component
struct Index {
  private probe: AccessoryProbe = new AccessoryProbe();
  @State accessoryInfo: string = 'not authorized';
  @State nativeResult: string = '';
  @State stats: string = '{}';
  @State logs: string[] = [];

  private append(message: string): void {
    const timestamp: string = new Date().toISOString();
    const line: string = `${timestamp} ${message}`;
    this.logs = [line, ...this.logs].slice(0, 80);
  }

  private async openAccessory(): Promise<void> {
    try {
      this.accessoryInfo = await this.probe.authorizeAndOpen();
      this.append('accessory opened');
    } catch (error) {
      this.accessoryInfo = String(error);
      this.append(this.accessoryInfo);
    }
  }

  private async start(mode: WakeMode): Promise<void> {
    this.nativeResult = this.probe.start(mode);
    this.append(`start ${mode}: ${this.nativeResult}`);
    this.refreshStats();
  }

  private async stopAndClose(mode: WakeMode): Promise<void> {
    try {
      this.nativeResult = this.probe.stopAndClose(mode);
      this.append(`stop ${mode}: ${this.nativeResult}`);
      this.accessoryInfo = this.probe.nativeRunning() ? 'handle retained' : 'handle closed';
    } catch (error) {
      this.nativeResult = String(error);
      this.append(this.nativeResult);
    }
    this.refreshStats();
  }

  private beginOwnerClose(): void {
    this.nativeResult = this.probe.beginOwnerCloseStop();
    this.append(`owner-close begin: ${this.nativeResult}`);
  }

  private closeOwnerHandle(): void {
    try {
      this.nativeResult = this.probe.closeOwnerHandle();
      this.accessoryInfo = 'owner handle closed, native join pending';
      this.append(this.nativeResult);
    } catch (error) {
      this.nativeResult = String(error);
      this.append(this.nativeResult);
    }
  }

  private finishOwnerCloseJoin(): void {
    this.nativeResult = this.probe.finishOwnerCloseJoin();
    this.append(`owner-close join: ${this.nativeResult}`);
    this.refreshStats();
  }

  private refreshStats(): void {
    this.stats = this.probe.stats();
  }

  private closeIdle(): void {
    this.nativeResult = this.probe.closeIdleHandle();
    this.append(this.nativeResult);
    this.accessoryInfo = 'not authorized';
    this.refreshStats();
  }

  build() {
    Column({ space: 10 }) {
      Text('DeskMate Gate 1 AOA Probe')
        .fontSize(24)
        .fontWeight(FontWeight.Bold)
        .width('100%')

      Text('ArkTS owns USBAccessoryHandle. C++ borrows accessoryFd and never closes it.')
        .fontSize(13)
        .fontColor('#486581')
        .width('100%')

      Text(`Accessory: ${this.accessoryInfo}`)
        .fontSize(14)
        .width('100%')
        .padding(10)
        .backgroundColor('#E6F1FF')
        .borderRadius(8)

      Row({ space: 8 }) {
        Button('1. 授权并打开')
          .onClick(() => { this.openAccessory(); })
        Button('关闭空闲句柄')
          .onClick(() => { this.closeIdle(); })
      }
      .width('100%')

      Divider()

      Text('poll/eventfd 分支')
        .fontSize(16)
        .fontWeight(FontWeight.Medium)
        .width('100%')
      Row({ space: 8 }) {
        Button('启动 eventfd').onClick(() => { this.start(WakeMode.EVENTFD); })
        Button('停止并 join').onClick(() => { this.stopAndClose(WakeMode.EVENTFD); })
      }
      .width('100%')

      Text('无 SA_RESTART 的定向 SIGUSR1')
        .fontSize(16)
        .fontWeight(FontWeight.Medium)
        .width('100%')
      Row({ space: 8 }) {
        Button('启动 signal').onClick(() => { this.start(WakeMode.SIGNAL); })
        Button('信号唤醒并 join').onClick(() => { this.stopAndClose(WakeMode.SIGNAL); })
      }
      .width('100%')

      Text('官方 closeAccessory 唤醒（必须按顺序）')
        .fontSize(16)
        .fontWeight(FontWeight.Medium)
        .width('100%')
      Row({ space: 8 }) {
        Button('启动 owner-close').onClick(() => { this.start(WakeMode.OWNER_CLOSE); })
      }
      .width('100%')
      Row({ space: 8 }) {
        Button('A. 拒绝新工作').onClick(() => { this.beginOwnerClose(); })
        Button('B. closeAccessory').onClick(() => { this.closeOwnerHandle(); })
        Button('C. join').onClick(() => { this.finishOwnerCloseJoin(); })
      }
      .width('100%')

      Row({ space: 8 }) {
        Button('刷新统计').onClick(() => { this.refreshStats(); })
        Text(this.probe.nativeRunning() ? 'native running' : 'native idle')
          .fontSize(13)
          .fontColor('#486581')
      }
      .width('100%')

      Text('Last native result')
        .fontSize(16)
        .fontWeight(FontWeight.Medium)
        .width('100%')
      Text(this.nativeResult)
        .fontSize(12)
        .fontFamily('monospace')
        .width('100%')
        .padding(8)
        .backgroundColor('#F0F4F8')
        .borderRadius(6)

      Text('Native stats')
        .fontSize(16)
        .fontWeight(FontWeight.Medium)
        .width('100%')
      Text(this.stats)
        .fontSize(11)
        .fontFamily('monospace')
        .width('100%')
        .padding(8)
        .backgroundColor('#F0F4F8')
        .borderRadius(6)

      Text('Lifecycle / event log')
        .fontSize(16)
        .fontWeight(FontWeight.Medium)
        .width('100%')
      Scroll() {
        Column({ space: 4 }) {
          ForEach(this.logs, (line: string) => {
            Text(line)
              .fontSize(11)
              .fontFamily('monospace')
              .width('100%')
          }, (line: string) => line)
        }
      }
      .layoutWeight(1)
      .width('100%')
      .padding(8)
      .backgroundColor('#F8FAFC')
      .borderRadius(8)
    }
    .padding(16)
    .width('100%')
    .height('100%')
    .backgroundColor('#FFFFFF')
  }
}

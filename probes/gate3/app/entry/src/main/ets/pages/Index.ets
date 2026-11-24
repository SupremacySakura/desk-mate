import { common } from '@kit.AbilityKit';
import deviceInfo from '@ohos.deviceInfo';
import probe from 'libentry.so';

interface DeviceSnapshot {
  productModel: string;
  osFullName: string;
  sdkApiVersion: number;
  abiList: string;
  displayVersion: string;
  buildType: string;
  incrementalVersion: string;
}

@Entry
@Component
struct Index {
  @State private snapshot: string = '{}';
  @State private streamStatus: string = 'stream not loaded';
  @State private actionStatus: string = 'waiting for Surface';
  @State private suiteActive: boolean = false;
  @State private manualFrozen: boolean = false;
  private refreshTimer: number = -1;

  aboutToAppear(): void {
    this.publishDeviceInfo();
    this.loadTestStream();
    this.refreshTimer = setInterval(() => {
      this.refreshSnapshot();
    }, 500);
  }

  aboutToDisappear(): void {
    if (this.refreshTimer >= 0) {
      clearInterval(this.refreshTimer);
      this.refreshTimer = -1;
    }
  }

  private publishDeviceInfo(): void {
    try {
      const info: DeviceSnapshot = {
        productModel: deviceInfo.productModel,
        osFullName: deviceInfo.osFullName,
        sdkApiVersion: deviceInfo.sdkApiVersion,
        abiList: deviceInfo.abiList,
        displayVersion: deviceInfo.displayVersion,
        buildType: deviceInfo.buildType,
        incrementalVersion: deviceInfo.incrementalVersion
      };
      if (!probe.setDeviceInfo(JSON.stringify(info))) {
        this.actionStatus = 'device info rejected by native probe';
      }
    } catch (error) {
      this.actionStatus = 'device info failed: ' + JSON.stringify(error);
    }
  }

  private loadTestStream(): void {
    try {
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const bytes: Uint8Array = context.resourceManager.getRawFileContentSync('gate3-testsrc.h264');
      const accepted = probe.loadStream(bytes);
      this.streamStatus = accepted
        ? `Annex-B loaded (${bytes.byteLength} bytes)`
        : `Annex-B rejected (${bytes.byteLength} bytes)`;
    } catch (error) {
      this.streamStatus = 'stream load failed: ' + JSON.stringify(error);
    }
  }

  private refreshSnapshot(): void {
    try {
      const next = probe.snapshotJson();
      if (next.length > 0) {
        this.snapshot = next;
      }
    } catch (error) {
      this.snapshot = JSON.stringify(error);
    }
  }

  build() {
    Column({ space: 10 }) {
      Row() {
        Column({ space: 2 }) {
          Text('DeskMate Gate 3')
            .fontSize(22)
            .fontWeight(FontWeight.Bold)
            .fontColor('#E6EDF3')
          Text(this.streamStatus)
            .fontSize(12)
            .fontColor('#8B949E')
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Text(this.suiteActive ? 'SUITE ACTIVE' : 'IDLE')
          .fontSize(11)
          .fontColor(this.suiteActive ? '#F2CC60' : '#7EE787')
      }
      .width('100%')

      Stack({ alignContent: Alignment.TopStart }) {
        XComponent({
          id: 'deskmate_gate3_surface',
          type: XComponentType.SURFACE,
          libraryname: 'entry'
        })
          .width('100%')
          .height(300)
          .backgroundColor('#000000')

        Text('Surface output - no CPU pixel readback')
          .fontSize(11)
          .fontColor('#8B949E')
          .backgroundColor('#99000000')
          .padding({ left: 8, right: 8, top: 4, bottom: 4 })
      }
      .width('100%')
      .height(300)
      .border({ width: 1, color: '#30363D' })

      Text(this.actionStatus)
        .fontSize(12)
        .fontColor('#C9D1D9')
        .width('100%')

      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
        Button('Run recovery suite')
          .enabled(!this.suiteActive)
          .margin({ right: 8, bottom: 8 })
          .onClick(() => {
            this.suiteActive = probe.runSuite();
            this.actionStatus = this.suiteActive
              ? 'suite accepted on control worker'
              : 'suite rejected: load stream and Surface first';
          })

        Button('Freeze for LGF check')
          .enabled(!this.manualFrozen)
          .margin({ right: 8, bottom: 8 })
          .onClick(() => {
            const accepted = probe.beginFreeze();
            this.manualFrozen = accepted;
            this.actionStatus = accepted
              ? 'output frozen; inspect whether the last frame remains visible'
              : 'freeze rejected';
          })

        Button('Mark LGF stable')
          .enabled(this.manualFrozen)
          .margin({ right: 8, bottom: 8 })
          .onClick(() => {
            const accepted = probe.recordLastGoodFrame(true);
            this.actionStatus = accepted
              ? 'operator marked Last Good Frame stable during freeze'
              : 'Last Good Frame mark rejected';
          })

        Button('Resume after freeze')
          .enabled(this.manualFrozen)
          .margin({ right: 8, bottom: 8 })
          .onClick(() => {
            const accepted = probe.resumeAfterFreeze();
            if (accepted) {
              this.manualFrozen = false;
              this.actionStatus = 'resume accepted; watch for recovery output';
            } else {
              this.actionStatus = 'resume rejected';
            }
          })
      }
      .width('100%')

      Row() {
        Text('Raw evidence')
          .fontSize(13)
          .fontWeight(FontWeight.Medium)
          .fontColor('#E6EDF3')
          .layoutWeight(1)
        Text(`${this.snapshot.length} chars`)
          .fontSize(11)
          .fontColor('#8B949E')
      }
      .width('100%')

      Scroll() {
        Text(this.snapshot)
          .fontSize(11)
          .fontFamily('monospace')
          .fontColor('#C9D1D9')
          .width('100%')
          .padding(8)
          .textAlign(TextAlign.Start)
      }
      .width('100%')
      .layoutWeight(1)
      .backgroundColor('#0D1117')
      .border({ width: 1, color: '#30363D' })
      .scrollBar(BarState.Auto)
    }
    .width('100%')
    .height('100%')
    .padding(16)
    .backgroundColor('#111318')
  }
}

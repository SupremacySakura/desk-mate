import { BusinessError, usbManager } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import probe from 'libgate1_probe.so';

const DOMAIN: number = 0x3201;
const TAG: string = 'Gate1Device';

export enum WakeMode {
  EVENTFD = 'eventfd',
  SIGNAL = 'signal',
  OWNER_CLOSE = 'owner-close'
}

export class AccessoryProbe {
  private handle: usbManager.USBAccessoryHandle | undefined = undefined;
  private activeMode: WakeMode | undefined = undefined;
  private running: boolean = false;

  private log(level: string, message: string): void {
    if (level === 'error') {
      hilog.error(DOMAIN, TAG, '%{public}s', message);
    } else {
      hilog.info(DOMAIN, TAG, '%{public}s', message);
    }
  }

  async authorizeAndOpen(): Promise<string> {
    if (this.handle !== undefined) {
      return `already open: fd=${this.handle.accessoryFd}`;
    }
    try {
      const accessories: Array<Readonly<usbManager.USBAccessory>> =
        usbManager.getAccessoryList();
      if (accessories.length === 0) {
        throw new Error('no AOA accessory in getAccessoryList()');
      }
      const accessory: usbManager.USBAccessory = accessories[0];
      const granted: boolean = await usbManager.requestAccessoryRight(accessory);
      this.log('info', `requestAccessoryRight=${granted}`);
      if (!granted) {
        throw new Error('accessory authorization denied');
      }
      const opened: usbManager.USBAccessoryHandle = usbManager.openAccessory(accessory);
      if (opened.accessoryFd <= 0) {
        throw new Error(`invalid accessoryFd=${opened.accessoryFd}`);
      }
      this.handle = opened;
      const summary: string =
        `manufacturer=${accessory.manufacturer}; product=${accessory.product}; ` +
        `description=${accessory.description}; version=${accessory.version}; ` +
        `serial=${accessory.serialNumber}; fd=${opened.accessoryFd}`;
      this.log('info', `openAccessory ok: ${summary}`);
      return summary;
    } catch (error) {
      const failure = error as BusinessError;
      const message = `openAccessory failed: code=${failure.code} message=${failure.message}`;
      this.log('error', message);
      throw new Error(message);
    }
  }

  start(mode: WakeMode): string {
    if (this.running) {
      return 'probe is already running';
    }
    if (this.handle === undefined) {
      return 'open accessory before starting';
    }
    const result: string = probe.startProbe(this.handle.accessoryFd, mode);
    this.running = probe.isRunning();
    this.activeMode = this.running ? mode : undefined;
    this.log('info', `start ${mode}: ${result}`);
    return result;
  }

  stopAndClose(mode: WakeMode): string {
    if (mode === WakeMode.OWNER_CLOSE) {
      return 'owner-close requires beginOwnerCloseStop -> closeAccessory -> finishOwnerCloseJoin';
    }
    if (this.handle === undefined) {
      return 'no handle';
    }
    let result: string = '';
    try {
      result = probe.stopAndJoinProbe(mode);
      this.log('info', `stop ${mode}: ${result}`);
      if (result.indexOf('"ok":true') < 0) {
        return result;
      }
      usbManager.closeAccessory(this.handle);
      this.log('info', `closeAccessory after joined ${mode}`);
      this.handle = undefined;
      this.activeMode = undefined;
      this.running = false;
      return result;
    } catch (error) {
      const failure = error as BusinessError;
      const message = `stopAndClose failed: code=${failure.code} message=${failure.message}`;
      this.log('error', message);
      throw new Error(message);
    }
  }

  beginOwnerCloseStop(): string {
    if (this.handle === undefined) {
      return 'no handle';
    }
    this.activeMode = WakeMode.OWNER_CLOSE;
    const result: string = probe.beginOwnerCloseStop();
    this.log('info', `owner-close begin: ${result}`);
    return result;
  }

  closeOwnerHandle(): string {
    if (this.handle === undefined) {
      return 'no handle';
    }
    const fd: number = this.handle.accessoryFd;
    try {
      usbManager.closeAccessory(this.handle);
      this.handle = undefined;
      this.log('info', `owner closeAccessory invoked for fd=${fd}`);
      return `closeAccessory called for fd=${fd}`;
    } catch (error) {
      const failure = error as BusinessError;
      const message = `closeAccessory failed: code=${failure.code} message=${failure.message}`;
      this.log('error', message);
      throw new Error(message);
    }
  }

  finishOwnerCloseJoin(): string {
    const result: string = probe.finishOwnerCloseJoin();
    this.log('info', `owner-close join: ${result}`);
    if (result.indexOf('"ok":true') >= 0) {
      this.activeMode = undefined;
      this.running = false;
    }
    return result;
  }

  closeIdleHandle(): string {
    if (this.handle === undefined) {
      return 'no handle';
    }
    if (probe.isRunning()) {
      return 'refusing to close while native worker is running';
    }
    usbManager.closeAccessory(this.handle);
    this.handle = undefined;
    this.activeMode = undefined;
    this.running = false;
    return 'idle handle closed';
  }

  stats(): string {
    return probe.getStatsJson();
  }

  nativeRunning(): boolean {
    return probe.isRunning();
  }
}

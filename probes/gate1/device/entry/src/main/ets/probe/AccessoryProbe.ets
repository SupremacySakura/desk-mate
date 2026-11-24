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
  private openingPromise: Promise<string> | undefined = undefined;
  private accessoryGeneration: number = 0;
  private destroyedForAbility: boolean = false;

  private log(level: string, message: string): void {
    if (level === 'error') {
      hilog.error(DOMAIN, TAG, '%{public}s', message);
    } else {
      hilog.info(DOMAIN, TAG, '%{public}s', message);
    }
  }

  resetForAbility(): void {
    ++this.accessoryGeneration;
    this.destroyedForAbility = false;
    this.openingPromise = undefined;
    this.handle = undefined;
    this.activeMode = undefined;
    this.running = false;
  }

  async authorizeAndOpen(): Promise<string> {
    if (this.destroyedForAbility) {
      throw new Error('accessory probe belongs to a destroyed ability');
    }
    if (this.handle !== undefined) {
      return `already open: fd=${this.handle.accessoryFd}`;
    }
    if (this.openingPromise !== undefined) {
      return this.openingPromise;
    }

    const generation: number = this.accessoryGeneration;
    const opening: Promise<string> = this.performOpen(generation);
    this.openingPromise = opening;
    try {
      return await opening;
    } finally {
      if (this.openingPromise === opening) {
        this.openingPromise = undefined;
      }
    }
  }

  private isStaleOpen(generation: number): boolean {
    return this.destroyedForAbility || generation !== this.accessoryGeneration;
  }

  private closeRejectedOpen(opened: usbManager.USBAccessoryHandle): void {
    try {
      usbManager.closeAccessory(opened);
    } catch (error) {
      const failure = error as BusinessError;
      this.log('error',
        `closeAccessory after rejected open failed: code=${failure.code} message=${failure.message}`);
    }
  }

  private async performOpen(generation: number): Promise<string> {
    try {
      const accessories: Array<Readonly<usbManager.USBAccessory>> =
        usbManager.getAccessoryList();
      if (accessories.length === 0) {
        throw new Error('no AOA accessory in getAccessoryList()');
      }
      const accessory: usbManager.USBAccessory = accessories[0];
      const granted: boolean = await usbManager.requestAccessoryRight(accessory);
      this.log('info', `requestAccessoryRight=${granted}`);
      if (this.isStaleOpen(generation)) {
        throw new Error('ability destroyed while accessory authorization was pending');
      }
      if (!granted) {
        throw new Error('accessory authorization denied');
      }
      const opened: usbManager.USBAccessoryHandle = usbManager.openAccessory(accessory);
      if (opened.accessoryFd <= 0) {
        this.closeRejectedOpen(opened);
        throw new Error(`invalid accessoryFd=${opened.accessoryFd}`);
      }
      if (this.isStaleOpen(generation)) {
        this.closeRejectedOpen(opened);
        throw new Error('ability destroyed while accessory open was pending');
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
    if (this.destroyedForAbility) {
      return 'accessory probe belongs to a destroyed ability';
    }
    if (this.openingPromise !== undefined) {
      return 'accessory open is still pending';
    }
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

  shutdownForDestroy(): string {
    this.destroyedForAbility = true;
    ++this.accessoryGeneration;
    try {
      if (probe.isRunning()) {
        if (this.activeMode === WakeMode.OWNER_CLOSE) {
          const begin: string = probe.beginOwnerCloseStop();
          this.log('info', `onDestroy owner-close begin: ${begin}`);
          if (begin.indexOf('"ok":true') < 0) {
            return begin;
          }
          if (this.handle !== undefined) {
            usbManager.closeAccessory(this.handle);
            this.handle = undefined;
          }
          const joined: string = probe.finishOwnerCloseJoin();
          this.log('info', `onDestroy owner-close join: ${joined}`);
          if (joined.indexOf('"ok":true') >= 0) {
            this.activeMode = undefined;
            this.running = false;
          }
          return joined;
        }

        if (this.activeMode === undefined) {
          return '{"ok":false,"message":"native worker is running without an active mode"}';
        }
        const stopped: string = probe.stopAndJoinProbe(this.activeMode);
        this.log('info', `onDestroy stop ${this.activeMode}: ${stopped}`);
        if (stopped.indexOf('"ok":true') < 0) {
          return stopped;
        }
        this.running = false;
        this.activeMode = undefined;
      }

      if (this.handle !== undefined) {
        usbManager.closeAccessory(this.handle);
        this.handle = undefined;
        this.log('info', 'onDestroy closed ArkTS-owned accessory handle');
      }
      this.activeMode = undefined;
      this.running = false;
      return '{"ok":true,"message":"worker joined; ArkTS closed accessory handle"}';
    } catch (error) {
      const failure = error as BusinessError;
      const message = `onDestroy cleanup failed: code=${failure.code} message=${failure.message}`;
      this.log('error', message);
      return `{"ok":false,"message":"${message}"}`;
    }
  }

  stats(): string {
    return probe.getStatsJson();
  }

  nativeRunning(): boolean {
    return probe.isRunning();
  }
}

export const gate1Probe: AccessoryProbe = new AccessoryProbe();

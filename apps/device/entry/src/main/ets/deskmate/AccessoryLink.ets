import { BusinessError, usbManager } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import native from 'libdeskmate_device.so';

const DOMAIN: number = 0x3210;
const TAG: string = 'DeskMateLink';

export type LinkState = 'searching' | 'authorizing' | 'connected' | 'error';

export interface LinkSnapshot {
  state: LinkState;
  message: string;
  accessoryFd: number;
}

// Owns the USBAccessoryHandle. Native only ever borrows accessoryFd; every close
// happens here (ADR 0004). Closing is also what wakes the blocked poll() with
// POLLHUP on this platform, so it must always be followed by native stopSession.
export class AccessoryLink {
  private handle: usbManager.USBAccessoryHandle | undefined = undefined;
  private pollTimer: number = -1;
  private busy: boolean = false;
  private disposed: boolean = false;
  private state: LinkState = 'searching';
  private message: string = 'searching for accessory';
  private onUpdate: (snapshot: LinkSnapshot) => void;

  constructor(onUpdate: (snapshot: LinkSnapshot) => void) {
    this.onUpdate = onUpdate;
  }

  start(): void {
    if (this.pollTimer >= 0) {
      return;
    }
    this.pollTimer = setInterval(() => {
      this.tick();
    }, 1000);
    this.tick();
  }

  stop(): void {
    this.disposed = true;
    if (this.pollTimer >= 0) {
      clearInterval(this.pollTimer);
      this.pollTimer = -1;
    }
    this.closeAccessory();
  }

  snapshot(): LinkSnapshot {
    return {
      state: this.state,
      message: this.message,
      accessoryFd: this.handle === undefined ? -1 : this.handle.accessoryFd
    };
  }

  private publish(state: LinkState, message: string): void {
    this.state = state;
    this.message = message;
    this.onUpdate(this.snapshot());
  }

  private closeAccessory(): void {
    const opened: usbManager.USBAccessoryHandle | undefined = this.handle;
    this.handle = undefined;
    if (opened === undefined) {
      return;
    }
    // Stop the worker first so it is waiting on poll() when the descriptor is
    // closed; closeAccessory is what produces POLLHUP.
    native.stopSession();
    try {
      usbManager.closeAccessory(opened);
      hilog.info(DOMAIN, TAG, 'accessory closed');
    } catch (error) {
      const failure = error as BusinessError;
      hilog.error(DOMAIN, TAG, 'closeAccessory failed: %{public}s',
        `${failure.code} ${failure.message}`);
    }
  }

  private tick(): void {
    if (this.disposed || this.busy) {
      return;
    }
    if (this.handle !== undefined) {
      return;
    }
    this.busy = true;
    this.authorizeAndOpen().finally(() => {
      this.busy = false;
    });
  }

  private async authorizeAndOpen(): Promise<void> {
    let accessories: Array<Readonly<usbManager.USBAccessory>> = [];
    try {
      accessories = usbManager.getAccessoryList();
    } catch (error) {
      const failure = error as BusinessError;
      this.publish('error', `getAccessoryList failed: ${failure.code}`);
      return;
    }
    if (accessories.length === 0) {
      this.publish('searching', 'waiting for USB accessory');
      return;
    }

    const accessory: Readonly<usbManager.USBAccessory> = accessories[0];
    this.publish('authorizing', 'requesting accessory permission');
    let granted = false;
    try {
      granted = await usbManager.requestAccessoryRight(accessory);
    } catch (error) {
      const failure = error as BusinessError;
      this.publish('error', `requestAccessoryRight failed: ${failure.code}`);
      return;
    }
    if (this.disposed) {
      return;
    }
    if (!granted) {
      this.publish('error', 'accessory permission denied');
      return;
    }

    let opened: usbManager.USBAccessoryHandle;
    try {
      opened = usbManager.openAccessory(accessory);
    } catch (error) {
      const failure = error as BusinessError;
      this.publish('error', `openAccessory failed: ${failure.code}`);
      return;
    }
    if (opened.accessoryFd <= 0) {
      try {
        usbManager.closeAccessory(opened);
      } catch (error) {
        const failure = error as BusinessError;
        hilog.error(DOMAIN, TAG, 'close rejected open: %{public}s',
          `${failure.code}`);
      }
      this.publish('error', `invalid accessoryFd=${opened.accessoryFd}`);
      return;
    }
    if (this.disposed) {
      usbManager.closeAccessory(opened);
      return;
    }

    this.handle = opened;
    const started: boolean = native.startSession(opened.accessoryFd);
    if (!started) {
      this.closeAccessory();
      this.publish('error', 'native session rejected the descriptor');
      return;
    }
    this.publish('connected', `accessory fd=${opened.accessoryFd}`);
    hilog.info(DOMAIN, TAG, 'session started fd=%{public}d', opened.accessoryFd);
  }
}

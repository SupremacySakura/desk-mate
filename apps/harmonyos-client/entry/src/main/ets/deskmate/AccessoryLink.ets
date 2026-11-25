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
  private retryRequired: boolean = false;
  private state: LinkState = 'searching';
  private message: string = 'waiting for USB accessory';
  private onUpdate: (snapshot: LinkSnapshot) => void;

  constructor(onUpdate: (snapshot: LinkSnapshot) => void) {
    this.onUpdate = onUpdate;
  }

  start(): void {
    if (this.pollTimer >= 0) {
      return;
    }
    this.disposed = false;
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

  retry(): void {
    this.retryRequired = false;
    this.tick();
  }

  reconnect(): void {
    this.closeAccessory();
    this.retryRequired = false;
    this.publish('searching', 'reconnecting USB session');
    this.tick();
  }

  private fail(message: string): void {
    if (this.disposed) {
      return;
    }
    this.retryRequired = true;
    this.publish('error', message);
  }

  snapshot(): LinkSnapshot {
    return {
      state: this.state,
      message: this.message,
      accessoryFd: this.handle === undefined ? -1 : this.handle.accessoryFd
    };
  }

  private publish(state: LinkState, message: string): void {
    if (this.state === state && this.message === message) {
      return;
    }
    this.state = state;
    this.message = message;
    this.onUpdate(this.snapshot());
  }

  private closeAccessory(): void {
    const opened: usbManager.USBAccessoryHandle | undefined = this.handle;
    this.handle = undefined;
    if (opened === undefined) {
      native.requestStop();
      return;
    }
    // Order matters on this platform: the worker blocks in poll() and only the
    // official closeAccessory() wakes it (POLLHUP). requestStop() is
    // non-blocking, so it must come first, then the close, then the join.
    native.requestStop();
    try {
      usbManager.closeAccessory(opened);
      hilog.info(DOMAIN, TAG, 'accessory closed');
    } catch (error) {
      const failure = error as BusinessError;
      hilog.error(DOMAIN, TAG, 'closeAccessory failed: %{public}s',
        `${failure.code} ${failure.message}`);
    }
    native.stopSession();
  }

  private tick(): void {
    if (this.disposed || this.busy) {
      return;
    }
    if (this.handle !== undefined) {
      try {
        if (usbManager.getAccessoryList().length === 0) {
          this.closeAccessory();
          this.retryRequired = false;
          this.publish('searching', 'waiting for USB accessory');
          return;
        }
      } catch (error) {
        const failure = error as BusinessError;
        hilog.warn(DOMAIN, TAG, 'getAccessoryList failed during session: %{public}d', failure.code);
      }
      // The worker exits by itself when the accessory hangs up or the rebuild
      // budget is spent. Drop the stale handle and let the user retry.
      if (!native.isRunning()) {
        hilog.warn(DOMAIN, TAG, 'native worker exited');
        this.closeAccessory();
        this.fail('USB session ended');
      }
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
      if (!this.retryRequired) {
        this.fail(`getAccessoryList failed: ${failure.code}`);
      }
      return;
    }
    if (accessories.length === 0) {
      this.retryRequired = false;
      this.publish('searching', 'waiting for USB accessory');
      return;
    }
    if (this.retryRequired) {
      return;
    }

    const accessory: Readonly<usbManager.USBAccessory> = accessories[0];
    this.publish('authorizing', 'requesting accessory permission');
    let granted = false;
    try {
      granted = await usbManager.requestAccessoryRight(accessory);
    } catch (error) {
      const failure = error as BusinessError;
      this.fail(`requestAccessoryRight failed: ${failure.code}`);
      return;
    }
    if (this.disposed) {
      return;
    }
    if (!granted) {
      this.fail('accessory permission denied');
      return;
    }

    let opened: usbManager.USBAccessoryHandle;
    try {
      opened = usbManager.openAccessory(accessory);
    } catch (error) {
      const failure = error as BusinessError;
      this.fail(`openAccessory failed: ${failure.code}`);
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
      this.fail(`invalid accessoryFd=${opened.accessoryFd}`);
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
      this.fail('native session rejected the descriptor');
      return;
    }
    this.publish('connected', `accessory fd=${opened.accessoryFd}`);
    hilog.info(DOMAIN, TAG, 'session started fd=%{public}d', opened.accessoryFd);
  }
}

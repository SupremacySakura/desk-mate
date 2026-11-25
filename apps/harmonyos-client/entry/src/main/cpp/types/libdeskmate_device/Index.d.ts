export const startSession: (accessoryFd: number) => boolean;
export const stopSession: () => boolean;
export const requestStop: () => boolean;
export const isRunning: () => boolean;
export const surfaceCreated: (surfaceId: string) => boolean;
export const surfaceDestroyed: (surfaceId: string) => boolean;
export const sendInput: (
  kind: number,
  pointerId: number,
  flags: number,
  x: number,
  y: number,
  dx: number,
  dy: number,
  keycode: number
) => boolean;
export const noteLifecycle: (event: string) => boolean;
export const setPreferredCodec: (codec: string) => boolean;
export const setDisplayMetrics: (width: number, height: number, refreshHz: number) => boolean;
export const setTouchEnabled: (enabled: boolean) => boolean;
export const snapshotJson: () => string;

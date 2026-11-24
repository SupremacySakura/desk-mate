export const startProbe: (fd: number, mode: string) => string;
export const stopAndJoinProbe: (mode: string) => string;
export const beginOwnerCloseStop: () => string;
export const finishOwnerCloseJoin: () => string;
export const getStatsJson: () => string;
export const noteLifecycle: (event: string) => void;
export const isRunning: () => boolean;

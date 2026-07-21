// Device model for the console switcher.
//
// Only the Jetson currently has a live WebRTC stream (mediamtx path `jetsona`
// + the input bridge). The PC is reachable for power control over Wake-on-LAN
// — the Jetson sits on its LAN and emits the magic packet — but has no capture
// pipeline yet, so its tab renders a placeholder instead of a video surface.

import { Cpu, Monitor, type LucideIcon } from 'lucide-react';

export type DeviceId = 'pc' | 'jetson';

/** Coarse health of a device, driving every colour in the UI. */
export type DeviceState = 'online' | 'sleep' | 'offline' | 'unknown';

export type DeviceMeta = {
  id: DeviceId;
  label: string;
  /** Shown in the switcher instead of the label, which made the pill lopsided. */
  icon: LucideIcon;
  /** Whether this device has a WebRTC screen to attach to. */
  hasStream: boolean;
  /** Whether the power button can act on it (only the PC answers Wake-on-LAN). */
  canPower: boolean;
};

export const DEVICES: DeviceMeta[] = [
  { id: 'pc', label: 'PC', icon: Monitor, hasStream: false, canPower: true },
  { id: 'jetson', label: 'Jetson Nano', icon: Cpu, hasStream: true, canPower: false },
];

export const STATE_LABEL: Record<DeviceState, string> = {
  online: 'Trực tuyến',
  sleep: 'Đang khởi động',
  offline: 'Ngoại tuyến',
  unknown: 'Đang kiểm tra',
};

/** Tailwind classes per state, kept in one place so the dot, ring and text agree. */
export const STATE_STYLE: Record<DeviceState, { dot: string; text: string; ring: string }> = {
  online: {
    dot: 'bg-state-online shadow-[0_0_10px_var(--color-state-online)]',
    text: 'text-state-online',
    ring: 'ring-state-online/40',
  },
  sleep: {
    dot: 'bg-state-sleep shadow-[0_0_10px_var(--color-state-sleep)]',
    text: 'text-state-sleep',
    ring: 'ring-state-sleep/40',
  },
  offline: {
    dot: 'bg-state-offline shadow-[0_0_10px_var(--color-state-offline)]',
    text: 'text-state-offline',
    ring: 'ring-state-offline/40',
  },
  unknown: {
    dot: 'bg-white/40',
    text: 'text-white/60',
    ring: 'ring-white/20',
  },
};

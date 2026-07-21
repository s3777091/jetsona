'use client';

// Segmented control under the screen that switches which machine the console
// is attached to. The active pill slides between segments rather than snapping,
// which keeps the switch legible when the screen behind it also swaps.

import { DEVICES, STATE_STYLE, type DeviceId, type DeviceState } from '@/lib/devices';
import { cn } from '@/lib/utils';

export default function DeviceTabs({
  value,
  onChange,
  states,
}: {
  value: DeviceId;
  onChange: (id: DeviceId) => void;
  states: Record<DeviceId, DeviceState>;
}) {
  const index = DEVICES.findIndex((device) => device.id === value);

  return (
    <div
      role="tablist"
      aria-label="Chọn thiết bị"
      className="pointer-events-auto relative flex rounded-full border border-white/10 bg-white/8 p-1 shadow-2xl shadow-black/40 backdrop-blur-2xl"
    >
      {/* Sliding indicator, sized as a fraction of the track. */}
      <span
        aria-hidden
        className="absolute inset-y-1 left-1 rounded-full bg-white/22 transition-transform duration-300 ease-[cubic-bezier(0.22,1,0.36,1)]"
        style={{
          width: `calc((100% - 0.5rem) / ${DEVICES.length})`,
          transform: `translateX(${index * 100}%)`,
        }}
      />

      {DEVICES.map((device) => {
        const selected = device.id === value;
        return (
          <button
            key={device.id}
            role="tab"
            type="button"
            aria-selected={selected}
            onClick={() => onChange(device.id)}
            className={cn(
              'relative z-10 flex items-center gap-2 rounded-full px-5 py-1.5 text-sm transition-colors duration-200',
              'outline-none focus-visible:ring-2 focus-visible:ring-white/70',
              selected ? 'font-semibold text-white' : 'font-medium text-white/60 hover:text-white/90',
            )}
          >
            <span
              className={cn('size-1.5 shrink-0 rounded-full', STATE_STYLE[states[device.id]].dot)}
              aria-hidden
            />
            {device.label}
          </button>
        );
      })}
    </div>
  );
}

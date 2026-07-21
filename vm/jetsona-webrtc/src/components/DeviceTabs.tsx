'use client';

// Segmented control under the screen that switches which machine the console
// is attached to.
//
// Icon-only by design: with text labels the two segments had different widths
// ("PC" vs "Jetson Nano"), so the sliding indicator — which assumes equal
// fractions of the track — never lined up with the active one. Equal-width
// icon buttons make the indicator exact, and keep the pill compact.

import { DEVICES, STATE_LABEL, STATE_STYLE, type DeviceId, type DeviceState } from '@/lib/devices';
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
      className="relative flex rounded-full border border-white/10 bg-white/8 p-1 shadow-2xl shadow-black/40 backdrop-blur-2xl"
    >
      {/* Sliding indicator, sized as an exact fraction of the track. */}
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
        const state = states[device.id];
        const Icon = device.icon;
        return (
          <button
            key={device.id}
            role="tab"
            type="button"
            aria-selected={selected}
            aria-label={`${device.label} · ${STATE_LABEL[state]}`}
            title={`${device.label} · ${STATE_LABEL[state]}`}
            onClick={() => onChange(device.id)}
            className={cn(
              'group relative z-10 grid size-11 place-items-center rounded-full transition-colors duration-200',
              'outline-none focus-visible:ring-2 focus-visible:ring-white/70',
              selected ? 'text-white' : 'text-white/55 hover:text-white/90',
            )}
          >
            <Icon className="size-[19px]" aria-hidden />

            {/* Status dot, badged on the icon so the pill stays compact. */}
            <span
              aria-hidden
              className={cn(
                'absolute right-2 top-2 size-1.5 rounded-full ring-2 ring-black/40',
                STATE_STYLE[state].dot,
              )}
            />

            {/* Label on hover, since the text no longer sits in the pill. */}
            <span className="pointer-events-none absolute bottom-[calc(100%+0.6rem)] left-1/2 z-20 -translate-x-1/2 whitespace-nowrap rounded-lg border border-white/10 bg-black/80 px-2.5 py-1.5 text-xs font-medium text-white opacity-0 backdrop-blur-md transition-opacity duration-150 group-hover:opacity-100 group-focus-visible:opacity-100">
              {device.label}
            </span>
          </button>
        );
      })}
    </div>
  );
}

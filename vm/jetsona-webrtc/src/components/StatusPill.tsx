'use client';

// Connection/health readout pinned to the top-left of the screen surface.
// Colour is the primary signal (green online / amber waking / red offline);
// the text is the detail.

import { STATE_LABEL, STATE_STYLE, type DeviceState } from '@/lib/devices';
import { cn } from '@/lib/utils';

export default function StatusPill({
  state,
  label,
  detail,
}: {
  state: DeviceState;
  label?: string;
  detail?: string;
}) {
  const style = STATE_STYLE[state];
  return (
    <div
      className={cn(
        'pointer-events-none flex items-center gap-2.5 rounded-full border border-white/10 bg-black/55 py-1.5 pl-3 pr-4',
        'text-[13px] leading-none text-white shadow-lg shadow-black/30 ring-1 backdrop-blur-xl',
        style.ring,
      )}
    >
      <span className="relative grid size-2.5 place-items-center">
        <span className={cn('size-2 rounded-full', style.dot)} />
        {state === 'sleep' && (
          <span className={cn('absolute inset-0 animate-ping rounded-full opacity-60', style.dot)} />
        )}
      </span>
      <span className={cn('font-medium', style.text)}>{label ?? STATE_LABEL[state]}</span>
      {detail && <span className="max-w-[16rem] truncate text-white/50">· {detail}</span>}
    </div>
  );
}

'use client';

// The floating icon rail on the left of the console (visionOS-style pill).
//
// Actions are declared by the parent so the dock stays presentational; it only
// owns hover/focus affordances and the tooltip.

import type { LucideIcon } from 'lucide-react';
import { cn } from '@/lib/utils';

export type DockAction = {
  id: string;
  label: string;
  icon: LucideIcon;
  onSelect?: () => void;
  /** Rendered dimmed and inert. May be temporary (wrong tab) or permanent. */
  disabled?: boolean;
  /** Marks a not-yet-wired entry, so only those are tagged "sắp có". */
  placeholder?: boolean;
  /** Spins the icon while a matching action is in flight (e.g. reconnecting). */
  busy?: boolean;
  /** Highlights the button while its mode is engaged (e.g. fullscreen on). */
  active?: boolean;
};

export default function DeviceDock({ actions }: { actions: DockAction[] }) {
  return (
    <nav
      aria-label="Điều khiển thiết bị"
      className="pointer-events-auto flex flex-col items-center gap-1.5 rounded-full border border-white/10 bg-white/8 p-2 shadow-2xl shadow-black/40 backdrop-blur-2xl"
    >
      {actions.map(({ id, label, icon: Icon, onSelect, disabled, placeholder, busy, active }) => (
        <button
          key={id}
          type="button"
          onClick={onSelect}
          disabled={disabled || busy}
          title={label}
          aria-label={label}
          className={cn(
            'group relative grid size-10 place-items-center rounded-full transition-all duration-200 outline-none',
            'focus-visible:ring-2 focus-visible:ring-white/70',
            disabled
              ? 'cursor-not-allowed text-white/25'
              : 'text-white/75 hover:bg-white/15 hover:text-white active:scale-90',
            active && 'bg-white/20 text-white',
          )}
        >
          <Icon className={cn('size-[18px]', busy && 'animate-spin')} aria-hidden />

          {/* Tooltip. Pointer-events-none so it never blocks the button. */}
          <span
            className={cn(
              'pointer-events-none absolute left-[calc(100%+0.75rem)] z-20 whitespace-nowrap rounded-lg',
              'border border-white/10 bg-black/80 px-2.5 py-1.5 text-xs font-medium text-white',
              'opacity-0 backdrop-blur-md transition-opacity duration-150',
              'group-hover:opacity-100 group-focus-visible:opacity-100',
            )}
          >
            {label}
            {placeholder && <span className="ml-1.5 text-white/40">(sắp có)</span>}
          </span>
        </button>
      ))}
    </nav>
  );
}

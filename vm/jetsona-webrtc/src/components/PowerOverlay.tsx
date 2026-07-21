'use client';

// Blocking overlay shown while a Wake-on-LAN boot is in flight. It blurs the
// screen behind it so the stale frame does not read as live, and narrates the
// boot as it progresses. Failures are surfaced as toasts by the parent, not
// here — this overlay only ever shows forward progress.

import { cn } from '@/lib/utils';

export default function PowerOverlay({
  open,
  title,
  detail,
  done,
}: {
  open: boolean;
  title: string;
  detail?: string;
  /** Switches the dots for a settled check-mark on the success beat. */
  done?: boolean;
}) {
  if (!open) return null;

  return (
    <div
      role="status"
      aria-live="polite"
      className="animate-fade-in absolute inset-0 z-30 grid place-items-center rounded-[inherit] bg-black/45 backdrop-blur-xl"
    >
      <div className="flex flex-col items-center gap-5 px-8 text-center">
        {done ? (
          <div className="grid size-12 place-items-center rounded-full bg-state-online/15 ring-1 ring-state-online/40">
            <svg viewBox="0 0 24 24" className="size-6 text-state-online" aria-hidden>
              <path
                d="M5 13l4 4L19 7"
                fill="none"
                stroke="currentColor"
                strokeWidth="2.5"
                strokeLinecap="round"
                strokeLinejoin="round"
              />
            </svg>
          </div>
        ) : (
          <div className="flex h-12 items-center gap-2">
            {[0, 1, 2].map((i) => (
              <span
                key={i}
                className="animate-loading-dot size-2.5 rounded-full bg-white"
                style={{ animationDelay: `${i * 0.16}s` }}
              />
            ))}
          </div>
        )}

        <div className="space-y-1.5">
          <p className={cn('text-base font-semibold text-white', done && 'text-state-online')}>
            {title}
          </p>
          {detail && <p className="text-sm text-white/55">{detail}</p>}
        </div>
      </div>
    </div>
  );
}

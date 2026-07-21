'use client';

// Stand-in surface for the PC tab.
//
// The PC has no capture pipeline yet — only Wake-on-LAN power control — so
// instead of an empty black rectangle this renders a framed still with the
// current power state. The photo is decorative: if it fails to load (offline
// browser, blocked third party) the gradient underneath carries the panel on
// its own, so nothing ever reads as broken.
//
// Powering on lives on the dock's power button, not here: two power controls on
// one screen read as two different actions.

import { useState } from 'react';
import { Monitor, Power } from 'lucide-react';
import { STATE_STYLE, type DeviceState } from '@/lib/devices';
import { cn } from '@/lib/utils';

const STILL =
  'https://images.unsplash.com/photo-1517336714731-489689fd1ca8?auto=format&fit=crop&w=1600&q=70';

export default function PcPanel({ state, detail }: { state: DeviceState; detail?: string }) {
  const [showStill, setShowStill] = useState(true);
  const style = STATE_STYLE[state];

  return (
    <div className="relative size-full overflow-hidden bg-[radial-gradient(120%_120%_at_50%_0%,#1b1533_0%,#07060f_65%)]">
      {showStill && (
        // eslint-disable-next-line @next/next/no-img-element -- decorative, no layout shift, unoptimized on purpose
        <img
          src={STILL}
          alt=""
          aria-hidden
          onError={() => setShowStill(false)}
          className="absolute inset-0 size-full scale-105 object-cover opacity-20 blur-[2px] saturate-50"
        />
      )}
      <div className="absolute inset-0 bg-gradient-to-b from-black/20 via-black/45 to-black/80" />

      <div className="relative grid size-full place-items-center p-8">
        <div className="flex max-w-md flex-col items-center gap-5 text-center">
          <div
            className={cn(
              'grid size-16 place-items-center rounded-2xl bg-white/6 ring-1 backdrop-blur-sm',
              style.ring,
            )}
          >
            <Monitor className={cn('size-7', style.text)} aria-hidden />
          </div>

          <div className="space-y-1.5">
            <h2 className="text-xl font-semibold text-white">PC</h2>
            <p className={cn('text-sm font-medium', style.text)}>
              {state === 'online'
                ? 'Máy đang bật'
                : state === 'sleep'
                  ? 'Đang khởi động…'
                  : state === 'offline'
                    ? 'Máy đang tắt'
                    : 'Đang kiểm tra trạng thái…'}
            </p>
            {detail && <p className="text-xs text-white/45">{detail}</p>}
          </div>

          {state === 'online' ? (
            <p className="text-sm leading-relaxed text-white/45">
              Chưa có luồng hình cho PC. Hình ảnh sẽ hiện ở đây khi luồng capture được cấu hình.
            </p>
          ) : (
            <p className="flex items-center gap-2 text-sm leading-relaxed text-white/45">
              Bấm
              <span className="inline-flex items-center gap-1.5 rounded-full border border-white/15 bg-white/10 px-2.5 py-1 text-white/80">
                <Power className="size-3.5" aria-hidden />
                nguồn
              </span>
              ở thanh bên trái để bật máy.
            </p>
          )}
        </div>
      </div>
    </div>
  );
}

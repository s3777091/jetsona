'use client';

// Stand-in surface for the PC tab.
//
// The PC has no capture pipeline yet — only Wake-on-LAN power control — so
// instead of an empty black rectangle this renders a framed still with the
// current power state. The photo is decorative: if it fails to load (offline
// browser, blocked third party) the gradient underneath carries the panel on
// its own, so nothing ever reads as broken.

import { useState } from 'react';
import { MonitorSmartphone, Power } from 'lucide-react';
import { STATE_STYLE, type DeviceState } from '@/lib/devices';
import { cn } from '@/lib/utils';

const STILL =
  'https://images.unsplash.com/photo-1517336714731-489689fd1ca8?auto=format&fit=crop&w=1600&q=70';

export default function PcPanel({
  state,
  detail,
  onWake,
  waking,
}: {
  state: DeviceState;
  detail?: string;
  onWake: () => void;
  waking: boolean;
}) {
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
            <MonitorSmartphone className={cn('size-7', style.text)} aria-hidden />
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

          <p className="text-sm leading-relaxed text-white/45">
            {state === 'online'
              ? 'Chưa có luồng hình cho PC. Hình ảnh sẽ hiện ở đây khi luồng capture được cấu hình.'
              : 'Bấm bật máy để gửi Wake-on-LAN qua Jetson tới PC trong cùng mạng LAN.'}
          </p>

          {state !== 'online' && (
            <button
              type="button"
              onClick={onWake}
              disabled={waking}
              className={cn(
                'mt-1 flex items-center gap-2 rounded-full border border-white/15 bg-white/10 px-5 py-2.5',
                'text-sm font-semibold text-white shadow-lg shadow-black/30 backdrop-blur-xl transition-all duration-200',
                'outline-none hover:bg-white/18 focus-visible:ring-2 focus-visible:ring-white/70 active:scale-95',
                'disabled:cursor-not-allowed disabled:opacity-50 disabled:active:scale-100',
              )}
            >
              <Power className="size-4" aria-hidden />
              {waking ? 'Đang bật…' : 'Bật máy'}
            </button>
          )}
        </div>
      </div>
    </div>
  );
}

'use client';

// Console shell: one framed screen, a device switcher beneath it and an icon
// dock beside it.
//
// It owns everything the individual surfaces should not: which device is
// attached, Wake-on-LAN polling and the boot narration, fullscreen, and the
// toast stack. `RemoteScreen` (Jetson) and `PcPanel` (PC) are swapped inside
// the same frame so the chrome never re-mounts.

import { useCallback, useEffect, useRef, useState } from 'react';
import {
  Gamepad2,
  LogOut,
  Maximize2,
  Minimize2,
  Power,
  RotateCw,
  Settings,
  Undo2,
} from 'lucide-react';

import DeviceDock, { type DockAction } from '@/components/DeviceDock';
import DeviceTabs from '@/components/DeviceTabs';
import PcPanel from '@/components/PcPanel';
import PowerOverlay from '@/components/PowerOverlay';
import RemoteScreen, { type LinkStatus, type RemoteScreenHandle } from '@/components/RemoteScreen';
import StatusPill from '@/components/StatusPill';
import { Toaster, useToasts } from '@/components/ui/toast';
import { STATE_LABEL, type DeviceId, type DeviceState } from '@/lib/devices';
import { cn } from '@/lib/utils';

/** How long to wait for the PC to answer pings before calling the boot failed. */
const WAKE_TIMEOUT_MS = 90_000;

// Vertical room reserved outside the screen for the switcher that straddles its
// bottom edge, so the frame is height-bound on short/wide windows instead of
// overflowing past the viewport. Half of this lands below the frame and must
// clear the switcher's 2rem offset plus its ~2.25rem height.
const CHROME_RESERVE = '11rem';

const SCREEN_SIZE = {
  width: `min(92vw, (100dvh - ${CHROME_RESERVE}) * 5 / 3)`,
  aspectRatio: '5 / 3',
} as const;

type WakeOverlay = { open: boolean; title: string; detail?: string; done: boolean };

const WAKE_IDLE: WakeOverlay = { open: false, title: '', done: false };

export default function RemoteConsole({ inputToken }: { inputToken: string }) {
  const [device, setDevice] = useState<DeviceId>('jetson');
  const { toasts, push, dismiss } = useToasts();

  // --- Jetson link -----------------------------------------------------------
  const screenRef = useRef<RemoteScreenHandle>(null);
  const [link, setLink] = useState<LinkStatus>({ state: 'unknown' });
  const onLink = useCallback((status: LinkStatus) => setLink(status), []);

  // --- PC power --------------------------------------------------------------
  const [pcOnline, setPcOnline] = useState<boolean | null>(null);
  const [pcError, setPcError] = useState<string | null>(null);
  const [waking, setWaking] = useState(false);
  const [wake, setWake] = useState<WakeOverlay>(WAKE_IDLE);

  const pollPc = useCallback(async () => {
    try {
      const res = await fetch('/api/wol/status', { cache: 'no-store' });
      const data = await res.json().catch(() => null);
      if (!res.ok || !data?.ok) throw new Error(data?.error || `lỗi ${res.status}`);
      setPcError(null);
      setPcOnline(Boolean(data.online));
    } catch (error) {
      setPcError((error as Error).message);
    }
  }, []);

  useEffect(() => {
    void pollPc();
    const id = setInterval(pollPc, waking ? 1500 : 4000);
    return () => clearInterval(id);
  }, [pollPc, waking]);

  // The PC answered — settle the overlay on a success beat before hiding it.
  useEffect(() => {
    if (!waking || !pcOnline) return;
    setWaking(false);
    setWake({ open: true, title: 'Khởi động thành công', detail: 'PC đã sẵn sàng', done: true });
    push('success', 'PC đã bật', 'Máy đã phản hồi trên mạng LAN.');
    const id = setTimeout(() => setWake(WAKE_IDLE), 1900);
    return () => clearTimeout(id);
  }, [waking, pcOnline, push]);

  // …or it never did.
  useEffect(() => {
    if (!waking) return;
    const id = setTimeout(() => {
      setWaking(false);
      setWake(WAKE_IDLE);
      push(
        'error',
        'PC chưa phản hồi',
        'Đã quá 90 giây kể từ khi gửi magic packet. Kiểm tra Wake-on-LAN trong BIOS và card mạng.',
      );
    }, WAKE_TIMEOUT_MS);
    return () => clearTimeout(id);
  }, [waking, push]);

  const onWake = useCallback(async () => {
    setDevice('pc');
    if (waking) return;
    if (pcOnline) {
      push('info', 'PC đang bật', 'Không cần gửi Wake-on-LAN.');
      return;
    }
    setWaking(true);
    setWake({
      open: true,
      title: 'Đang gửi lệnh bật…',
      detail: 'Phát Wake-on-LAN qua Jetson',
      done: false,
    });
    try {
      const res = await fetch('/api/wol/wake', { method: 'POST' });
      const data = await res.json().catch(() => null);
      if (!res.ok || !data?.ok) throw new Error(data?.error || `lỗi ${res.status}`);
      setWake({
        open: true,
        title: 'Đang chờ PC khởi động…',
        detail: 'Đã gửi magic packet · thường mất 30–60 giây',
        done: false,
      });
    } catch (error) {
      setWaking(false);
      setWake(WAKE_IDLE);
      push('error', 'Không bật được máy', (error as Error).message);
    }
  }, [waking, pcOnline, push]);

  // --- Fullscreen ------------------------------------------------------------
  const consoleRef = useRef<HTMLDivElement>(null);
  const [isFullscreen, setIsFullscreen] = useState(false);
  const [zoom, setZoom] = useState<'in' | 'out' | null>(null);

  const toggleFullscreen = useCallback(() => {
    if (document.fullscreenElement) {
      void document.exitFullscreen().catch(() => {});
      return;
    }
    consoleRef.current
      ?.requestFullscreen()
      .catch((error: Error) => push('error', 'Không vào được toàn màn hình', error.message));
  }, [push]);

  useEffect(() => {
    const onChange = () => {
      const on = Boolean(document.fullscreenElement);
      setIsFullscreen(on);
      setZoom(on ? 'in' : 'out');
    };
    document.addEventListener('fullscreenchange', onChange);
    return () => document.removeEventListener('fullscreenchange', onChange);
  }, []);

  // Esc is already spoken for by pointer lock, so fullscreen gets its own exit
  // chord. RemoteScreen deliberately does not forward this combo to the remote.
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (!(event.ctrlKey && event.shiftKey && event.code === 'Backspace')) return;
      event.preventDefault();
      if (document.pointerLockElement) document.exitPointerLock();
      if (document.fullscreenElement) void document.exitFullscreen().catch(() => {});
    };
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, []);

  // --- Derived state ---------------------------------------------------------
  const pcState: DeviceState = pcError
    ? 'offline'
    : waking
      ? 'sleep'
      : pcOnline === null
        ? 'unknown'
        : pcOnline
          ? 'online'
          : 'offline';

  const states: Record<DeviceId, DeviceState> = { pc: pcState, jetson: link.state };
  const current = device === 'pc' ? pcState : link.state;
  const currentDetail = device === 'pc' ? (pcError ?? undefined) : link.detail;

  // The PC pill talks about power, the Jetson pill about the video link.
  const PC_LABEL: Record<DeviceState, string> = {
    online: 'PC đang bật',
    sleep: 'PC đang khởi động',
    offline: pcError ? 'Không kiểm tra được' : 'PC đang tắt',
    unknown: 'Đang kiểm tra',
  };
  const currentLabel = device === 'pc' ? PC_LABEL[pcState] : STATE_LABEL[link.state];

  const onReconnect = useCallback(() => {
    if (device === 'jetson') {
      screenRef.current?.reconnect();
      push('info', 'Đang kết nối lại', 'Thiết lập lại luồng hình và kênh điều khiển.');
    } else {
      void pollPc();
    }
  }, [device, pollPc, push]);

  const onReturnToFirmware = useCallback(() => {
    if (!confirm('Đóng Chromium và trở về giao diện firmware?')) return;
    screenRef.current?.returnToFirmware();
  }, []);

  const onLogout = useCallback(async () => {
    try {
      await fetch('/api/auth/logout', { method: 'POST' });
    } catch {
      // The cookie is cleared server-side; a failed request just means we
      // reload and land back on the login screen anyway.
    }
    window.location.replace('/login');
  }, []);

  const actions: DockAction[] = [
    { id: 'power', label: 'Bật máy (Wake on LAN)', icon: Power, onSelect: onWake, busy: waking },
    {
      id: 'fullscreen',
      label: isFullscreen ? 'Thoát toàn màn hình' : 'Toàn màn hình',
      icon: isFullscreen ? Minimize2 : Maximize2,
      onSelect: toggleFullscreen,
      active: isFullscreen,
    },
    { id: 'reconnect', label: 'Kết nối lại', icon: RotateCw, onSelect: onReconnect },
    {
      id: 'firmware',
      label: 'Về firmware',
      icon: Undo2,
      onSelect: onReturnToFirmware,
      disabled: device !== 'jetson',
    },
    { id: 'controller', label: 'Tay cầm', icon: Gamepad2, disabled: true },
    { id: 'settings', label: 'Cài đặt', icon: Settings, disabled: true },
    { id: 'logout', label: 'Đăng xuất', icon: LogOut, onSelect: onLogout },
  ];

  return (
    <div
      ref={consoleRef}
      className="relative grid h-dvh w-full place-items-center overflow-hidden bg-transparent"
    >
      {/* Frame wrapper: shrinks to the screen so the switcher can straddle its
          bottom edge, and grows to the viewport in fullscreen. */}
      <div className="relative">
        <div
          onAnimationEnd={() => setZoom(null)}
          // Sized inline rather than with an arbitrary Tailwind value: `calc()`
          // requires real whitespace around `-`, and `/` inside `[]` is parsed as
          // Tailwind's modifier separator. Both silently drop the declaration.
          style={isFullscreen ? undefined : SCREEN_SIZE}
          className={cn(
            'relative overflow-hidden bg-black ring-1 ring-white/10',
            'shadow-[0_40px_120px_-24px_rgba(0,0,0,0.95)] transition-[border-radius] duration-500',
            isFullscreen ? 'h-dvh w-screen rounded-none' : 'rounded-3xl',
            zoom === 'in' && 'animate-stage-zoom-in',
            zoom === 'out' && 'animate-stage-zoom-out',
          )}
        >
          {device === 'jetson' ? (
            <RemoteScreen ref={screenRef} inputToken={inputToken} active onLink={onLink} />
          ) : (
            <PcPanel state={pcState} detail={pcError ?? undefined} onWake={onWake} waking={waking} />
          )}

          <div className="absolute left-4 top-4 z-20">
            <StatusPill state={current} label={currentLabel} detail={currentDetail} />
          </div>

          {device === 'jetson' && (
            <p className="lock-hint pointer-events-none absolute bottom-7 left-1/2 z-20 -translate-x-1/2 whitespace-nowrap rounded-full bg-black/60 px-3.5 py-1.5 text-xs text-white/70 backdrop-blur-md transition-opacity duration-200">
              Bấm vào màn hình để giữ chuột · Esc để thả chuột
              {isFullscreen && ' · Ctrl+Shift+Backspace để thoát toàn màn hình'}
            </p>
          )}

          <PowerOverlay
            open={wake.open && device === 'pc'}
            title={wake.title}
            detail={wake.detail}
            done={wake.done}
          />
        </div>

        {/* Icon rail, hugging the left edge of the screen. */}
        <div
          className={cn(
            'absolute top-1/2 z-40 -translate-y-1/2',
            isFullscreen ? 'left-5' : '-left-6 sm:-left-7',
          )}
        >
          <DeviceDock actions={actions} />
        </div>

        {/* Device switcher, straddling the bottom edge of the screen. */}
        <div
          className={cn(
            'absolute left-1/2 z-40 -translate-x-1/2',
            isFullscreen ? 'bottom-6' : '-bottom-8',
          )}
        >
          <DeviceTabs value={device} onChange={setDevice} states={states} />
        </div>
      </div>

      <Toaster toasts={toasts} onDismiss={dismiss} />
    </div>
  );
}

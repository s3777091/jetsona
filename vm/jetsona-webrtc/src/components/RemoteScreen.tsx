'use client';

// WHEP video surface + input bridge for the Jetson.
//
// Unchanged from the original client in behaviour: ICE is gathered fully before
// the single WHEP POST, and pointer/keyboard events are forwarded over the
// /input WebSocket as compact JSON frames. What changed is ownership — the
// component no longer paints its own chrome or wires toolbar buttons by DOM id.
// It reports link health upward through `onLink` and exposes `reconnect()` /
// `returnToFirmware()` on a ref, so the dock and status pill can be rendered by
// the console shell.

import { useEffect, useImperativeHandle, useRef, type Ref } from 'react';
import { KEY_CODES, MOUSE_CODES } from '@/lib/codes';
import type { DeviceState } from '@/lib/devices';

export type RemoteScreenHandle = {
  reconnect: () => void;
  returnToFirmware: () => void;
};

export type LinkStatus = { state: DeviceState; detail?: string };

type Props = {
  // Server-injected token. When JETSONA_INPUT_TOKEN is unset this is the literal
  // __JETSONA_INPUT_TOKEN__ placeholder so an external proxy can substitute it.
  inputToken: string;
  /** Tear the peer connection down when the tab is not showing this device. */
  active: boolean;
  onLink: (status: LinkStatus) => void;
  ref?: Ref<RemoteScreenHandle>;
};

type InputMessage =
  | { t: 'm'; dx: number; dy: number }
  | { t: 'w'; v: number }
  | { t: 'k'; code: number; value: 0 | 1 | 2 }
  | { t: 'return' };

export default function RemoteScreen({ inputToken, active, onLink, ref }: Props) {
  const captureRef = useRef<HTMLDivElement | null>(null);
  const videoRef = useRef<HTMLVideoElement | null>(null);

  // The effect below owns the live connection; these let the imperative handle
  // reach into it without re-running the effect.
  const commandRef = useRef<RemoteScreenHandle>({ reconnect: () => {}, returnToFirmware: () => {} });
  useImperativeHandle(ref, () => ({
    reconnect: () => commandRef.current.reconnect(),
    returnToFirmware: () => commandRef.current.returnToFirmware(),
  }));

  // onLink is called from long-lived closures; keep the latest without
  // retriggering the connection effect on every parent render.
  const onLinkRef = useRef(onLink);
  useEffect(() => {
    onLinkRef.current = onLink;
  }, [onLink]);

  useEffect(() => {
    const capture = captureRef.current;
    const video = videoRef.current;
    if (!capture || !video) return;

    if (!active) {
      onLinkRef.current({ state: 'unknown', detail: 'đã tạm dừng' });
      return;
    }

    // This app serves both the page and the WHEP/input endpoints at root.
    const pathRoot = '/';
    let pc: RTCPeerConnection | null = null;
    let ws: WebSocket | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    let disposed = false;

    const setStatus = (state: DeviceState, detail?: string) => {
      if (!disposed) onLinkRef.current({ state, detail });
    };

    const send = (message: InputMessage) => {
      if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(message));
    };

    const connectInput = () => {
      if (disposed) return;
      if (ws) {
        ws.onclose = null;
        ws.close();
      }
      const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
      ws = new WebSocket(
        `${scheme}//${location.host}${pathRoot}input?token=${encodeURIComponent(inputToken)}`,
      );
      ws.onopen = () => {
        if (pc && pc.connectionState === 'connected') setStatus('online');
      };
      ws.onclose = () => {
        if (!disposed) setTimeout(connectInput, 1500);
      };
    };

    const waitForIce = (peer: RTCPeerConnection) => {
      if (peer.iceGatheringState === 'complete') return Promise.resolve();
      return new Promise<void>((resolve) => {
        const check = () => {
          if (peer.iceGatheringState === 'complete') {
            peer.removeEventListener('icegatheringstatechange', check);
            resolve();
          }
        };
        peer.addEventListener('icegatheringstatechange', check);
        setTimeout(resolve, 2500);
      });
    };

    const connectVideo = async () => {
      if (disposed) return;
      if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
      }
      setStatus('sleep', 'đang kết nối video…');
      if (pc) pc.close();
      pc = new RTCPeerConnection({ bundlePolicy: 'max-bundle' });
      pc.addTransceiver('video', { direction: 'recvonly' });
      pc.ontrack = (event) => {
        video.srcObject = event.streams[0];
        video.play().catch(() => {});
      };
      pc.onconnectionstatechange = () => {
        if (!pc) return;
        const state = pc.connectionState;
        if (state === 'connected') {
          setStatus('online', ws && ws.readyState === 1 ? undefined : 'input đang nối');
        }
        if (state === 'failed' || state === 'disconnected' || state === 'closed') {
          if (disposed) return;
          setStatus('offline', `mất kết nối (${state})`);
          if (state !== 'closed') reconnectTimer = setTimeout(connectVideo, 2000);
        }
      };
      try {
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        await waitForIce(pc);
        const response = await fetch(`${pathRoot}media/jetsona/whep`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/sdp' },
          body: pc.localDescription!.sdp,
          cache: 'no-store',
        });
        if (!response.ok) throw new Error(`WHEP ${response.status}`);
        await pc.setRemoteDescription({ type: 'answer', sdp: await response.text() });
      } catch (error) {
        if (disposed) return;
        setStatus('offline', `chưa có stream (${(error as Error).message})`);
        reconnectTimer = setTimeout(connectVideo, 2500);
      }
    };

    commandRef.current = {
      reconnect: () => {
        void connectVideo();
        connectInput();
      },
      returnToFirmware: () => {
        if (document.pointerLockElement) document.exitPointerLock();
        send({ t: 'return' });
        setStatus('sleep', 'đang trở về firmware…');
      },
    };

    const onClickCapture = () => {
      if (document.pointerLockElement !== capture) capture.requestPointerLock();
    };
    const onPointerLockChange = () => {
      document.body.classList.toggle('pointer-locked', document.pointerLockElement === capture);
    };
    const onMouseMove = (event: MouseEvent) => {
      if (document.pointerLockElement !== capture) return;
      const scale = 800 / Math.max(1, capture.clientWidth);
      send({
        t: 'm',
        dx: Math.round(event.movementX * scale),
        dy: Math.round(event.movementY * scale),
      });
    };
    const onMouseDown = (event: MouseEvent) => {
      if (document.pointerLockElement !== capture) return;
      const code = MOUSE_CODES[event.button];
      if (code) send({ t: 'k', code, value: 1 });
      event.preventDefault();
    };
    const onMouseUp = (event: MouseEvent) => {
      const code = MOUSE_CODES[event.button];
      if (code) send({ t: 'k', code, value: 0 });
      event.preventDefault();
    };
    const onContextMenu = (event: Event) => event.preventDefault();
    const onWheel = (event: WheelEvent) => {
      send({ t: 'w', v: event.deltaY < 0 ? 1 : -1 });
      event.preventDefault();
    };
    // Reserved by the console shell for leaving fullscreen — swallowing it here
    // keeps the chord from also reaching the remote as a real key sequence.
    const isExitChord = (event: KeyboardEvent) =>
      event.ctrlKey && event.shiftKey && event.code === 'Backspace';

    const onKeyDown = (event: KeyboardEvent) => {
      if (document.pointerLockElement !== capture) return;
      if (isExitChord(event)) return;
      const code = KEY_CODES[event.code];
      if (!code) return;
      send({ t: 'k', code, value: event.repeat ? 2 : 1 });
      event.preventDefault();
    };
    const onKeyUp = (event: KeyboardEvent) => {
      if (isExitChord(event)) return;
      const code = KEY_CODES[event.code];
      if (!code) return;
      send({ t: 'k', code, value: 0 });
      if (document.pointerLockElement === capture) event.preventDefault();
    };
    const onBlur = () => {
      Object.values(KEY_CODES).forEach((code) => send({ t: 'k', code, value: 0 }));
      Object.values(MOUSE_CODES).forEach((code) => send({ t: 'k', code, value: 0 }));
    };

    capture.addEventListener('click', onClickCapture);
    document.addEventListener('pointerlockchange', onPointerLockChange);
    document.addEventListener('mousemove', onMouseMove);
    capture.addEventListener('mousedown', onMouseDown);
    capture.addEventListener('mouseup', onMouseUp);
    capture.addEventListener('contextmenu', onContextMenu);
    capture.addEventListener('wheel', onWheel, { passive: false });
    window.addEventListener('keydown', onKeyDown, true);
    window.addEventListener('keyup', onKeyUp, true);
    window.addEventListener('blur', onBlur);

    connectInput();
    void connectVideo();

    return () => {
      disposed = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      capture.removeEventListener('click', onClickCapture);
      document.removeEventListener('pointerlockchange', onPointerLockChange);
      document.removeEventListener('mousemove', onMouseMove);
      capture.removeEventListener('mousedown', onMouseDown);
      capture.removeEventListener('mouseup', onMouseUp);
      capture.removeEventListener('contextmenu', onContextMenu);
      capture.removeEventListener('wheel', onWheel);
      window.removeEventListener('keydown', onKeyDown, true);
      window.removeEventListener('keyup', onKeyUp, true);
      window.removeEventListener('blur', onBlur);
      document.body.classList.remove('pointer-locked');
      if (ws) {
        ws.onclose = null;
        ws.close();
      }
      if (pc) pc.close();
      commandRef.current = { reconnect: () => {}, returnToFirmware: () => {} };
    };
  }, [inputToken, active]);

  return (
    <>
      <video
        ref={videoRef}
        autoPlay
        muted
        playsInline
        className="size-full object-contain"
      />
      <div ref={captureRef} className="capture-surface absolute inset-0 cursor-crosshair touch-none" />
    </>
  );
}

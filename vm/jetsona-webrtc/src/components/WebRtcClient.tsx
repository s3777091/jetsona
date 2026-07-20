'use client';

import { useEffect, useRef } from 'react';
import { KEY_CODES, MOUSE_CODES } from '@/lib/codes';

type Props = {
  // Server-injected token. When JETSONA_INPUT_TOKEN is unset this is the literal
  // __JETSONA_INPUT_TOKEN__ placeholder so an external proxy can substitute it.
  inputToken: string;
};

type InputMessage =
  | { t: 'm'; dx: number; dy: number }
  | { t: 'w'; v: number }
  | { t: 'k'; code: number; value: 0 | 1 | 2 }
  | { t: 'return' };

export default function WebRtcClient({ inputToken }: Props) {
  const stageRef = useRef<HTMLElement | null>(null);
  const captureRef = useRef<HTMLDivElement | null>(null);
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const statusRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    const stage = stageRef.current;
    const capture = captureRef.current;
    const video = videoRef.current;
    const statusBox = statusRef.current;
    if (!stage || !capture || !video || !statusBox) return;

    // This app serves both the page and the WHEP/input endpoints at root.
    const pathRoot = '/';
    let pc: RTCPeerConnection | null = null;
    let ws: WebSocket | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;

    const setStatus = (text: string, state: 'ok' | 'warn' | 'bad' = 'warn') => {
      statusBox.textContent = text;
      statusBox.className = state;
    };

    const send = (message: InputMessage) => {
      if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(message));
    };

    const connectInput = () => {
      if (ws) {
        ws.onclose = null;
        ws.close();
      }
      const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
      ws = new WebSocket(
        `${scheme}//${location.host}${pathRoot}input?token=${encodeURIComponent(inputToken)}`,
      );
      ws.onopen = () => {
        if (pc && pc.connectionState === 'connected') setStatus('Trực tuyến', 'ok');
      };
      ws.onclose = () => setTimeout(connectInput, 1500);
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
      if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
      }
      setStatus('Đang kết nối video…');
      if (pc) pc.close();
      pc = new RTCPeerConnection({ bundlePolicy: 'max-bundle' });
      pc.addTransceiver('video', { direction: 'recvonly' });
      pc.ontrack = (event) => {
        video.srcObject = event.streams[0];
        video.play().catch(() => {});
      };
      pc.onconnectionstatechange = () => {
        const state = pc!.connectionState;
        if (state === 'connected')
          setStatus(ws && ws.readyState === 1 ? 'Trực tuyến' : 'Video OK · input đang nối', 'ok');
        if (state === 'failed' || state === 'disconnected' || state === 'closed') {
          setStatus(`Mất kết nối (${state})`, 'bad');
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
        setStatus(`Chưa có stream · thử lại (${(error as Error).message})`, 'bad');
        reconnectTimer = setTimeout(connectVideo, 2500);
      }
    };

    const onClickCapture = () => {
      if (document.pointerLockElement !== capture) capture.requestPointerLock();
      stage.focus();
    };
    const onPointerLockChange = () => {
      document.body.classList.toggle('locked', document.pointerLockElement === capture);
      if (document.pointerLockElement === capture) stage.focus();
    };
    const onMouseMove = (event: MouseEvent) => {
      if (document.pointerLockElement !== capture) return;
      const scale = 800 / Math.max(1, stage.clientWidth);
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
    const onKeyDown = (event: KeyboardEvent) => {
      if (document.pointerLockElement !== capture) return;
      const code = KEY_CODES[event.code];
      if (!code) return;
      send({ t: 'k', code, value: event.repeat ? 2 : 1 });
      event.preventDefault();
    };
    const onKeyUp = (event: KeyboardEvent) => {
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

    // Toolbar buttons (static in JSX below; wire handlers here by id).
    const fullscreenBtn = document.getElementById('fullscreen');
    const reconnectBtn = document.getElementById('reconnect');
    const returnBtn = document.getElementById('return');
    const onFullscreen = () => stage.requestFullscreen();
    const onReconnect = () => connectVideo();
    const onReturn = () => {
      if (confirm('Đóng Chromium và trở về giao diện firmware?')) {
        if (document.pointerLockElement) document.exitPointerLock();
        send({ t: 'return' });
        setStatus('Đang trở về firmware…');
      }
    };
    fullscreenBtn?.addEventListener('click', onFullscreen);
    reconnectBtn?.addEventListener('click', onReconnect);
    returnBtn?.addEventListener('click', onReturn);

    connectInput();
    connectVideo();

    return () => {
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
      fullscreenBtn?.removeEventListener('click', onFullscreen);
      reconnectBtn?.removeEventListener('click', onReconnect);
      returnBtn?.removeEventListener('click', onReturn);
      if (ws) {
        ws.onclose = null;
        ws.close();
      }
      if (pc) pc.close();
    };
  }, [inputToken]);

  return (
    <main id="stage" tabIndex={0} ref={stageRef}>
      <video id="video" autoPlay muted playsInline ref={videoRef} />
      <div id="capture" ref={captureRef} />
      <div id="status" className="warn" ref={statusRef}>Đang kết nối…</div>
      <div id="toolbar">
        <button id="return" type="button">Về firmware</button>
        <button id="reconnect" type="button">Kết nối lại</button>
        <button id="fullscreen" type="button">Toàn màn hình</button>
      </div>
      <div id="hint">Bấm vào màn hình để giữ chuột · Esc để thả chuột</div>
    </main>
  );
}
#!/usr/bin/env python3
"""WebSocket to Linux uinput bridge for Jetsona remote control."""

import asyncio
import json
import logging
import os
from pathlib import Path

import websockets
from evdev import InputDevice, UInput, ecodes, list_devices


HOST = os.environ.get("JETSON_WEBRTC_INPUT_HOST", "127.0.0.1")
PORT = int(os.environ.get("JETSON_WEBRTC_INPUT_PORT", "46001"))
VIRTUAL_NAME = "Jetsona WebRTC Input"
VIRTUAL_LINK = Path("/dev/input/jetsona-webrtc")

KEY_CODES = list(range(1, 256)) + [
    ecodes.BTN_LEFT,
    ecodes.BTN_RIGHT,
    ecodes.BTN_MIDDLE,
]
CAPABILITIES = {
    ecodes.EV_KEY: KEY_CODES,
    ecodes.EV_REL: [ecodes.REL_X, ecodes.REL_Y, ecodes.REL_WHEEL],
}

ui = None
pressed = set()
physical_tasks = {}


def virtual_event_path():
    for path in list_devices():
        try:
            dev = InputDevice(path)
            name = dev.name
            dev.close()
            if name == VIRTUAL_NAME:
                return path
        except OSError:
            pass
    return None


def refresh_link():
    path = virtual_event_path()
    if not path:
        return
    try:
        if VIRTUAL_LINK.is_symlink() or VIRTUAL_LINK.exists():
            VIRTUAL_LINK.unlink()
        VIRTUAL_LINK.symlink_to(path)
        logging.info("virtual input is %s", path)
    except OSError as exc:
        logging.warning("cannot create %s: %s", VIRTUAL_LINK, exc)


def emit(event_type, code, value):
    ui.write(event_type, code, value)
    ui.syn()


async def forward_physical(dev):
    logging.info("forwarding physical keyboard %s (%s)", dev.path, dev.name)
    try:
        async for event in dev.async_read_loop():
            if event.type == ecodes.EV_KEY:
                emit(event.type, event.code, event.value)
    except (OSError, asyncio.CancelledError):
        pass
    finally:
        physical_tasks.pop(dev.path, None)
        try:
            dev.close()
        except OSError:
            pass


def is_keyboard(dev):
    if dev.name == VIRTUAL_NAME:
        return False
    try:
        keys = dev.capabilities().get(ecodes.EV_KEY, [])
    except OSError:
        return False
    return all(code in keys for code in (ecodes.KEY_Q, ecodes.KEY_ENTER, ecodes.KEY_SPACE))


async def scan_physical_keyboards():
    while True:
        for path in list_devices():
            if path in physical_tasks:
                continue
            try:
                dev = InputDevice(path)
                if not is_keyboard(dev):
                    dev.close()
                    continue
                physical_tasks[path] = asyncio.ensure_future(forward_physical(dev))
            except OSError:
                pass
        await asyncio.sleep(3)


def bounded_int(value, minimum, maximum):
    return max(minimum, min(maximum, int(value)))


async def handler(websocket, _path):
    client_pressed = set()
    logging.info("input client connected")
    try:
        while True:
            try:
                raw = await websocket.recv()
            except websockets.exceptions.ConnectionClosed:
                break
            try:
                message = json.loads(raw)
                kind = message.get("t")
                if kind == "m":
                    dx = bounded_int(message.get("dx", 0), -2000, 2000)
                    dy = bounded_int(message.get("dy", 0), -2000, 2000)
                    if dx:
                        ui.write(ecodes.EV_REL, ecodes.REL_X, dx)
                    if dy:
                        ui.write(ecodes.EV_REL, ecodes.REL_Y, dy)
                    if dx or dy:
                        ui.syn()
                elif kind == "w":
                    emit(ecodes.EV_REL, ecodes.REL_WHEEL,
                         bounded_int(message.get("v", 0), -20, 20))
                elif kind == "k":
                    code = bounded_int(message.get("code", 0), 1, 511)
                    value = bounded_int(message.get("value", 0), 0, 2)
                    if code not in KEY_CODES:
                        continue
                    emit(ecodes.EV_KEY, code, value)
                    if value:
                        client_pressed.add(code)
                        pressed.add(code)
                    else:
                        client_pressed.discard(code)
                        pressed.discard(code)
            except (ValueError, TypeError, KeyError):
                continue
    finally:
        for code in client_pressed:
            if code in pressed:
                emit(ecodes.EV_KEY, code, 0)
                pressed.discard(code)
        logging.info("input client disconnected")


def main():
    global ui
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    ui = UInput(CAPABILITIES, name=VIRTUAL_NAME, version=0x0100)
    refresh_link()
    loop = asyncio.get_event_loop()
    loop.create_task(scan_physical_keyboards())
    server = websockets.serve(handler, HOST, PORT, max_size=4096)
    loop.run_until_complete(server)
    logging.info("listening on ws://%s:%d", HOST, PORT)
    try:
        loop.run_forever()
    finally:
        ui.close()


if __name__ == "__main__":
    main()

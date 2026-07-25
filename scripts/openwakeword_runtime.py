#!/usr/bin/env python3
"""Offline openWakeWord worker for the Jetson firmware.

The worker deliberately has no microphone access.  jetson_fw remains the only
ALSA owner and sends fixed 80 ms PCM frames over a Unix socket.  Production
runs this container with --network none so raw microphone audio cannot leave
the device.
"""

import argparse
import os
import signal
import socket
import struct
from pathlib import Path

import numpy as np
from openwakeword.model import Model


FRAME_SAMPLES = 1280
FRAME_BYTES = FRAME_SAMPLES * 2


def read_exact(conn: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = conn.recv(size - len(chunks))
        if not chunk:
            return b""
        chunks.extend(chunk)
    return bytes(chunks)


def make_model(model_path: str, verifier_path: str, verifier_threshold: float) -> Model:
    kwargs = {
        "wakeword_models": [model_path],
        "inference_framework": "onnx",
    }
    if verifier_path and Path(verifier_path).is_file():
        kwargs["custom_verifier_models"] = {
            Path(model_path).stem: verifier_path,
        }
        kwargs["custom_verifier_threshold"] = verifier_threshold
        print(f"openWakeWord verifier enabled: {verifier_path}", flush=True)
    else:
        print("openWakeWord verifier disabled (no enrolled verifier model)", flush=True)
    return Model(**kwargs)


def serve(args: argparse.Namespace) -> None:
    model = make_model(args.model, args.verifier, args.verifier_threshold)
    model_name = Path(args.model).stem
    socket_path = Path(args.socket)
    socket_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        socket_path.unlink()
    except FileNotFoundError:
        pass

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(socket_path))
    os.chmod(socket_path, 0o666)
    server.listen(1)

    stopping = False

    def stop_handler(_signum, _frame):
        nonlocal stopping
        stopping = True
        server.close()

    signal.signal(signal.SIGTERM, stop_handler)
    signal.signal(signal.SIGINT, stop_handler)
    print(
        f"openWakeWord ready: model={model_name} frame={FRAME_SAMPLES} socket={socket_path}",
        flush=True,
    )

    try:
        while not stopping:
            try:
                conn, _ = server.accept()
            except OSError:
                if stopping:
                    break
                raise
            with conn:
                model.reset()
                while not stopping:
                    frame = read_exact(conn, FRAME_BYTES)
                    if not frame:
                        break
                    audio = np.frombuffer(frame, dtype="<i2")
                    prediction = model.predict(audio)
                    score = float(prediction.get(model_name, 0.0))
                    conn.sendall(struct.pack("<f", score))
    finally:
        try:
            socket_path.unlink()
        except FileNotFoundError:
            pass


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--verifier", default="")
    parser.add_argument("--verifier-threshold", type=float, default=0.30)
    serve(parser.parse_args())


if __name__ == "__main__":
    main()

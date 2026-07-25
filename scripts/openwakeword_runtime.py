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

    silence = np.zeros(FRAME_SAMPLES, dtype="<i2")

    def arm_session() -> None:
        """Reset the scoring context and pay for the slow first frame.

        Measured on this Nano: the frame after reset() costs ~645 ms, against a
        ~18 ms steady state. jetson_fw feeds this socket from its ALSA capture
        thread with a 250 ms round-trip timeout, so if that cost landed on its
        first real frame every connection would time out, be dropped and be
        retried forever. Always call this while no client is waiting: before
        the socket accepts anyone, and again as soon as one disconnects.
        """
        model.reset()
        model.predict(silence)

    arm_session()

    # Wake-word debugging is otherwise blind: jetson_fw only logs the frames
    # that cross its threshold, so a phrase that scores 0.2 because the speaker
    # is too far away looks exactly like a phrase that was never spoken. Report
    # anything near the threshold as it happens, plus a periodic line carrying
    # the window's best score and input level so a dead or quiet mic is
    # obvious without recording the room.
    report_state = {"peak": 0.0, "rms": 0.0, "frames": 0}

    def report(score: float, audio: np.ndarray) -> None:
        rms = float(np.sqrt(np.mean(np.square(audio.astype(np.float32)))))
        report_state["peak"] = max(report_state["peak"], score)
        report_state["rms"] = max(report_state["rms"], rms)
        report_state["frames"] += 1
        if score >= args.log_score_above:
            print(f"score {score:.3f} (rms={rms:.0f})", flush=True)
        if report_state["frames"] >= args.log_every_frames:
            print(
                f"window peak={report_state['peak']:.3f} loudest-frame-rms={report_state['rms']:.0f}",
                flush=True,
            )
            report_state.update(peak=0.0, rms=0.0, frames=0)

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
            # jetson_fw drops and reopens this connection whenever a frame
            # round-trip times out. That is routine, so a dead peer must only
            # end the session -- never the worker, which takes ~40 s to reload
            # the model and would leave the wake word deaf for that whole time.
            try:
                with conn:
                    while not stopping:
                        frame = read_exact(conn, FRAME_BYTES)
                        if not frame:
                            break
                        audio = np.frombuffer(frame, dtype="<i2")
                        prediction = model.predict(audio)
                        score = float(prediction.get(model_name, 0.0))
                        conn.sendall(struct.pack("<f", score))
                        report(score, audio)
            except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError) as err:
                print(f"openWakeWord client disconnected: {err}", flush=True)
            if not stopping:
                arm_session()
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
    parser.add_argument("--log-score-above", type=float, default=0.10,
                        help="log every frame scoring at least this")
    parser.add_argument("--log-every-frames", type=int, default=250,
                        help="frames per periodic peak line (250 = 20 s)")
    serve(parser.parse_args())


if __name__ == "__main__":
    main()

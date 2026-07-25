#!/usr/bin/env python3
"""Print openWakeWord scores for 16 kHz mono WAV files."""

import argparse
import wave
from pathlib import Path

import numpy as np
from openwakeword.model import Model


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--threshold", type=float, default=0.01)
    parser.add_argument("wavs", nargs="+")
    args = parser.parse_args()

    detector = Model(
        wakeword_models=[args.model],
        inference_framework="onnx",
    )
    model_name = Path(args.model).stem
    for wav_path in args.wavs:
        with wave.open(wav_path, "rb") as wav:
            channels = wav.getnchannels()
            if wav.getsampwidth() != 2 or wav.getframerate() != 16000:
                raise ValueError(f"{wav_path}: expected S16LE at 16 kHz")
            samples = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2")
            if channels > 1:
                samples = samples.reshape(-1, channels)[:, 0]
        detector.reset()
        scores = []
        for offset in range(0, len(samples), 1280):
            frame = samples[offset : offset + 1280]
            if len(frame) < 1280:
                frame = np.pad(frame, (0, 1280 - len(frame)))
            scores.append(float(detector.predict(frame).get(model_name, 0.0)))
        top = sorted(scores, reverse=True)[:5]
        longest = current = 0
        for score in scores:
            current = current + 1 if score >= args.threshold else 0
            longest = max(longest, current)
        print(
            f"{wav_path}|max={max(scores, default=0.0):.6f}"
            f"|hits={sum(score >= args.threshold for score in scores)}"
            f"|max_consecutive={longest}|top={top}"
        )


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Score wake-phrase clips, and train openWakeWord's per-owner verifier.

Two modes, because the second only makes sense once the first has been read:

  --mode score   Run each recorded clip through the wake model and report the
                 peak score it reaches. This is the measurement that decides
                 whether a phrase is usable at all: the verifier is a *filter*
                 on activations, so it can trade false wakes for a lower accept
                 threshold, but it can never make the model fire on a phrase it
                 scores at the noise floor.

  --mode train   Fit the verifier from the recorded positives and negatives.

Runs inside the same voice-runtime image as the wake worker, and with the same
--network none, so the owner's recorded voice cannot leave the device while it
is on disk.
"""

import argparse
import inspect
import statistics
import sys
from pathlib import Path

import numpy as np
from openwakeword.model import Model

FRAME_SAMPLES = 1280


def clips_in(directory: str, label: str, required: bool = True) -> list:
    found = sorted(Path(directory).glob("*.wav"))
    if not found and required:
        sys.exit(f"no .wav clips in {directory} ({label})")
    return found


def read_wav_mono16(path: Path) -> np.ndarray:
    """The recorder already writes 16 kHz mono S16LE, so take the data chunk.

    Deliberately not soundfile/scipy: neither is in the runtime image, and a
    dependency added for enrollment would also have to be audited for the
    network-disabled container that hears the microphone.
    """
    raw = path.read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        sys.exit(f"not a RIFF/WAVE file: {path}")
    offset = 12
    while offset + 8 <= len(raw):
        chunk_id = raw[offset:offset + 4]
        chunk_size = int.from_bytes(raw[offset + 4:offset + 8], "little")
        body = offset + 8
        if chunk_id == b"data":
            return np.frombuffer(raw[body:body + chunk_size], dtype="<i2")
        offset = body + chunk_size + (chunk_size & 1)
    sys.exit(f"no data chunk in {path}")


def peak_score(model: Model, model_name: str, samples: np.ndarray) -> tuple:
    """Highest score any frame of this clip reaches, plus the clip's loudness.

    reset() between clips matters: openWakeWord scores a rolling window, so
    without it the tail of the previous clip leaks into the next one's context
    and the numbers stop being per-attempt.
    """
    model.reset()
    model.predict(np.zeros(FRAME_SAMPLES, dtype="<i2"))  # absorb the slow first frame
    best = 0.0
    for start in range(0, len(samples) - FRAME_SAMPLES + 1, FRAME_SAMPLES):
        frame = samples[start:start + FRAME_SAMPLES]
        best = max(best, float(model.predict(frame).get(model_name, 0.0)))
    rms = float(np.sqrt(np.mean(np.square(samples.astype(np.float32)))))
    return best, rms


def run_score(args: argparse.Namespace) -> None:
    model = Model(wakeword_models=[args.model], inference_framework="onnx")
    model_name = Path(args.model).stem

    for label, directory, required in (("POSITIVE", args.positive_dir, True),
                                       ("NEGATIVE", args.negative_dir, False)):
        clips = clips_in(directory, label.lower(), required=required)
        if not clips:
            continue
        print(f"\n=== {label} ({len(clips)} clips, model={model_name}) ===", flush=True)
        scores = []
        for clip in clips:
            score, rms = peak_score(model, model_name, read_wav_mono16(clip))
            scores.append(score)
            print(f"  {clip.name:<16} peak={score:.3f}  rms={rms:.0f}", flush=True)
        scores.sort()
        print(f"  ---- min={scores[0]:.3f} median={statistics.median(scores):.3f} "
              f"max={scores[-1]:.3f}", flush=True)


def run_train(args: argparse.Namespace) -> None:
    from openwakeword.utils import train_custom_verifier

    positive = [str(p) for p in clips_in(args.positive_dir, "positive")]
    negative = [str(p) for p in clips_in(args.negative_dir, "negative")]
    print(f"positive clips: {len(positive)}", flush=True)
    print(f"negative clips: {len(negative)}", flush=True)

    try:
        train_custom_verifier(
            positive_reference_clips=positive,
            negative_reference_clips=negative,
            output_path=args.output,
            model_name=args.model,
        )
    except TypeError as err:
        # Docker needs root on this box, so the upstream signature is the one
        # thing here that could not be checked from the build host. Print what
        # the installed version actually wants instead of letting a wrong guess
        # read as a training failure.
        print(f"train_custom_verifier rejected these arguments: {err}", file=sys.stderr)
        print(f"installed signature: {inspect.signature(train_custom_verifier)}",
              file=sys.stderr)
        raise SystemExit(2)

    print(f"verifier written: {args.output}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["score", "train"], default="score")
    parser.add_argument("--model", required=True)
    parser.add_argument("--positive-dir", required=True)
    parser.add_argument("--negative-dir", default="")
    parser.add_argument("--output", default="")
    args = parser.parse_args()

    if args.mode == "train":
        if not args.output or not args.negative_dir:
            sys.exit("--mode train needs --output and --negative-dir")
        run_train(args)
    else:
        run_score(args)


if __name__ == "__main__":
    main()

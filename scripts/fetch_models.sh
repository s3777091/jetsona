#!/bin/bash
# Download the three sherpa-onnx model packs Ekko Lite needs into
# assets/models/{kws,stt,tts}/, and write the wake-word keywords.txt.
#
# The firmware reads model paths from Settings("voice",...) with defaults
# under JETSON_ASSETS_DIR/models/<kws|stt|tts>/, so once this script lays the
# files out there the voice loop picks them up with no config change.
#
# IMPORTANT: the URLs below are the best-fit sherpa-onnx model releases for a
# Vietnamese voice assistant that fits the Nano's 4 GB RAM. Verify each
# against https://k2-fsa.github.io/sherpa/onnx/ (pretrained models) and adjust
# MODEL_*_URL if a release has moved. The script is idempotent: it skips a pack
# that is already unpacked.
set -e

JETSON_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MODELS="$JETSON_DIR/assets/models"
mkdir -p "$MODELS"

WGET="wget -q --show-progress"

# ---- KWS (wake word "nova") -----------------------------------------------
# Open-vocabulary zipformer KWS transducer (zh-en 3M, GitHub-hosted). It spots a
# custom keyword, BUT keywords.txt must hold the keyword TOKENIZED into the
# model's modelling units, not the raw word -- a raw "nova" line is silently
# never matched. We tokenize with `sherpa-onnx-cli text2token` using the
# bpe.model shipped inside the archive. A Vietnamese-accented "nova" is served
# reasonably by this zh-en model; swap in a vi-tuned KWS model if one ships.
# GitHub releases are reachable from the Jetson LAN (verified).
WAKE_WORD="${JETSON_WAKE_WORD:-nova}"
KWS_URL="${MODEL_KWS_URL:-https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2}"
KWS_DIR="$MODELS/kws"
if [ ! -f "$KWS_DIR/encoder.onnx" ]; then
    echo "==> KWS: $KWS_URL"
    mkdir -p "$KWS_DIR"
    tmp="$(mktemp -d)"; $WGET -O "$tmp/kws.tar.bz2" "$KWS_URL"
    tar -xjf "$tmp/kws.tar.bz2" -C "$tmp"
    # The archive unpacks a subdir; flatten model + tokens + bpe.model into kws/.
    find "$tmp" -maxdepth 2 \( -name '*.onnx' -o -name 'tokens.txt' -o -name 'bpe.model' \) \
        -exec cp -f {} "$KWS_DIR/" \;
    rm -rf "$tmp"
    # Normalize the transducer file names the firmware defaults to.
    [ -f "$KWS_DIR/encoder-epoch-99-avg-1.onnx" ] && mv "$KWS_DIR/encoder-epoch-99-avg-1.onnx" "$KWS_DIR/encoder.onnx"
    [ -f "$KWS_DIR/decoder-epoch-99-avg-1.onnx" ] && mv "$KWS_DIR/decoder-epoch-99-avg-1.onnx" "$KWS_DIR/decoder.onnx"
    [ -f "$KWS_DIR/joiner-epoch-99-avg-1.onnx" ] && mv "$KWS_DIR/joiner-epoch-99-avg-1.onnx" "$KWS_DIR/joiner.onnx"
fi

# keywords.txt = tokenized wake word + boost score. text2token needs the python
# sherpa-onnx CLI; if it (or bpe.model) is missing we write the raw word AND warn
# loudly, because the raw form is NOT detected until tokenized.
kw_raw="$(mktemp)"; echo "$WAKE_WORD" > "$kw_raw"
if command -v sherpa-onnx-cli >/dev/null 2>&1 && [ -f "$KWS_DIR/bpe.model" ]; then
    sherpa-onnx-cli text2token \
        --tokens "$KWS_DIR/tokens.txt" \
        --tokens-type cjkchar+bpe \
        --bpe-model "$KWS_DIR/bpe.model" \
        "$kw_raw" "$KWS_DIR/keywords.txt"
    # Append a boost score to each keyword line so the wake word triggers well.
    sed -i 's/$/ :1.5/' "$KWS_DIR/keywords.txt"
    echo "==> KWS ready in $KWS_DIR (wake word: $WAKE_WORD, tokenized)"
else
    echo "$WAKE_WORD :1.5" > "$KWS_DIR/keywords.txt"
    echo "!!  sherpa-onnx-cli or bpe.model missing: wrote RAW keyword '$WAKE_WORD'."
    echo "!!  KWS will NOT detect it until tokenized. Fix with:"
    echo "!!      pip install sherpa-onnx   # provides sherpa-onnx-cli"
    echo "!!  then rerun this script (bpe.model is inside the KWS archive)."
fi
rm -f "$kw_raw"

# ---- STT (streaming Vietnamese) -------------------------------------------
# Online zipformer transducer (Vietnamese). Transducer files: encoder/decoder/
# joiner + tokens.txt. If you switch to a zipformer2_ctc model, set
# Settings("voice","stt_ctc_model") to its .onnx and leave the transducer paths
# empty.
#
# NOTE: Vietnamese ASR/TTS models are hosted on HuggingFace
# (k2-fsa/sherpa-onnx-* HF repos), NOT on GitHub releases. HF has been observed
# to return HTTP 401 from some networks (e.g. the Jetson LAN) even for public
# repos, while GitHub releases work. If MODEL_STT_URL below 401s, download the
# pack on a network with HF access and extract it into assets/models/stt/
# (encoder.onnx / decoder.onnx / joiner.onnx / tokens.txt), or set MODEL_STT_URL
# to a reachable mirror. The default URL is a best-fit and may need adjusting.
STT_URL="${MODEL_STT_URL:-https://huggingface.co/k2-fsa/sherpa-onnx-streaming-asr-models/resolve/main/sherpa-onnx-streaming-zipformer-vi-2024-03-25/sherpa-onnx-streaming-zipformer-vi-2024-03-25.tar.bz2}"
STT_DIR="$MODELS/stt"
if [ ! -f "$STT_DIR/encoder.onnx" ]; then
    echo "==> STT: $STT_URL"
    mkdir -p "$STT_DIR"
    tmp="$(mktemp -d)"; $WGET -O "$tmp/stt.tar.bz2" "$STT_URL"
    tar -xjf "$tmp/stt.tar.bz2" -C "$tmp"
    find "$tmp" -maxdepth 2 \( -name '*.onnx' -o -name 'tokens.txt' \) \
        -exec cp -f {} "$STT_DIR/" \;
    rm -rf "$tmp"
    [ -f "$STT_DIR/encoder-epoch-99-avg-1.onnx" ] && mv "$STT_DIR/encoder-epoch-99-avg-1.onnx" "$STT_DIR/encoder.onnx"
    [ -f "$STT_DIR/decoder-epoch-99-avg-1.onnx" ] && mv "$STT_DIR/decoder-epoch-99-avg-1.onnx" "$STT_DIR/decoder.onnx"
    [ -f "$STT_DIR/joiner-epoch-99-avg-1.onnx" ] && mv "$STT_DIR/joiner-epoch-99-avg-1.onnx" "$STT_DIR/joiner.onnx"
fi
echo "==> STT ready in $STT_DIR"

# ---- TTS (Piper VITS, Vietnamese) -----------------------------------------
# vi_VN-vivos-x_low (16 kHz, small). Piper uses espeak-ng-data, not a lexicon.
# Hosted on HuggingFace (k2-fsa/sherpa-onnx-tts-models) -- see the STT note
# above about HF 401 on some networks; download manually + extract into
# assets/models/tts/ (vi_VN-vivos-x_low.onnx / tokens.txt / espeak-ng-data/) if
# the URL is unreachable. The onnx model lives in a subdir of the same name.
TTS_URL="${MODEL_TTS_URL:-https://huggingface.co/k2-fsa/sherpa-onnx-tts-models/resolve/main/vits/vi_VN-vivos-x_low/vi_VN-vivos-x_low.tar.bz2}"
TTS_DIR="$MODELS/tts"
if [ ! -f "$TTS_DIR/vi_VN-vivos-x_low.onnx" ]; then
    echo "==> TTS: $TTS_URL"
    mkdir -p "$TTS_DIR"
    tmp="$(mktemp -d)"; $WGET -O "$tmp/tts.tar.bz2" "$TTS_URL"
    tar -xjf "$tmp/tts.tar.bz2" -C "$tmp"
    # Copy the model + tokens + espeak-ng-data (a directory).
    find "$tmp" -maxdepth 2 -name 'vi_VN-vivos-x_low.onnx' -exec cp -f {} "$TTS_DIR/" \;
    find "$tmp" -maxdepth 2 -name 'tokens.txt' -exec cp -f {} "$TTS_DIR/" \;
    find "$tmp" -maxdepth 2 -type d -name 'espeak-ng-data' -exec cp -rf {} "$TTS_DIR/" \;
    rm -rf "$tmp"
fi
echo "==> TTS ready in $TTS_DIR"

echo ""
echo "==> done. Models under $MODELS. Rebuild + run to enable the voice loop."
echo "    (The sherpa engine builds each sub-model lazily on first wake/turn.)"
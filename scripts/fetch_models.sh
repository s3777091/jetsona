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

# ---- KWS (wake phrase "HEY NOVA") -----------------------------------------
# Use the 2024 English GigaSpeech 3.3M KWS model. It is tiny, responds while
# audio is still streaming, and is compatible with the older ONNX Runtime on
# JetPack 4. The newer 2025 zh-en graph uses an Unsqueeze operator unsupported
# by that runtime and panics during construction.
WAKE_WORD="${JETSON_WAKE_WORD:-HEY NOVA}"
KWS_URL="${MODEL_KWS_URL:-https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01.tar.bz2}"
KWS_DIR="$MODELS/kws_gigaspeech"
if [ ! -f "$KWS_DIR/encoder.onnx" ]; then
    echo "==> KWS: $KWS_URL"
    mkdir -p "$KWS_DIR"
    tmp="$(mktemp -d)"; $WGET -O "$tmp/kws.tar.bz2" "$KWS_URL"
    tar -xjf "$tmp/kws.tar.bz2" -C "$tmp"
    pack="$(find "$tmp" -type f -name 'bpe.model' -printf '%h\n' -quit)"
    cp -f "$pack/encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx" \
        "$KWS_DIR/encoder.onnx"
    cp -f "$pack/decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx" \
        "$KWS_DIR/decoder.onnx"
    cp -f "$pack/joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx" \
        "$KWS_DIR/joiner.onnx"
    cp -f "$pack/tokens.txt" "$pack/bpe.model" "$KWS_DIR/"
    rm -rf "$tmp"
fi

# Include the pronunciations observed from this exact microphone/user/model
# combination. A low threshold is acceptable because all entries remain
# two-word "HEY ..." phrases rather than the common bare word "NOVA".
if [ "$WAKE_WORD" = "HEY NOVA" ]; then
    cat > "$KWS_DIR/keywords.txt" <<'EOF'
▁HE Y ▁NO V A :3.0 #0.05 @HEY_NOVA
▁HE Y ▁LE W A :3.0 #0.05 @HEY_LEWA
▁HE Y ▁LE V A :3.0 #0.05 @HEY_LEVA
▁HE Y ▁NOW A :3.0 #0.05 @HEY_NOWA
▁HE Y ▁NEVER :3.0 #0.05 @HEY_NEVER
EOF
elif command -v sherpa-onnx-cli >/dev/null 2>&1; then
    kw_raw="$(mktemp)"; echo "$WAKE_WORD" > "$kw_raw"
    sherpa-onnx-cli text2token \
        --tokens "$KWS_DIR/tokens.txt" \
        --tokens-type bpe \
        --bpe-model "$KWS_DIR/bpe.model" \
        "$kw_raw" "$KWS_DIR/keywords.txt"
    sed -i 's/$/ :3.0 #0.05/' "$KWS_DIR/keywords.txt"
    rm -f "$kw_raw"
else
    echo "Custom JETSON_WAKE_WORD requires sherpa-onnx-cli for BPE tokenization." >&2
    exit 1
fi
echo "==> KWS ready in $KWS_DIR (wake phrase: $WAKE_WORD)"

# ---- VAD (Silero v4, JetPack/sherpa-onnx 1.10 compatible) ----------------
VAD_DIR="$MODELS/vad"
VAD_MODEL="$VAD_DIR/silero_vad_v4.onnx"
VAD_URL="${MODEL_VAD_URL:-https://github.com/snakers4/silero-vad/raw/refs/tags/v4.0/files/silero_vad.onnx}"
if [ ! -f "$VAD_MODEL" ]; then
    echo "==> VAD: $VAD_URL"
    mkdir -p "$VAD_DIR"
    $WGET -O "$VAD_MODEL" "$VAD_URL"
fi
echo "==> VAD ready in $VAD_DIR (Silero v4)"

# ---- STT (Vietnamese-only offline int8) -----------------------------------
# The 30M 2026 Vietnamese model recognizes the user's recorded "NOVA" samples
# more reliably and decodes about 24% faster on the Nano than the older 2025
# model. Do not replace it with the multilingual model: short Vietnamese
# commands were frequently misclassified as Chinese/Thai.
STT_MODEL_ID="sherpa-onnx-zipformer-vi-30M-int8-2026-02-09"
STT_URL="${MODEL_STT_URL:-https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/$STT_MODEL_ID.tar.bz2}"
STT_DIR="$MODELS/stt"
STT_MARKER="$STT_DIR/model.id"
if [ ! -f "$STT_MARKER" ] || [ "$(cat "$STT_MARKER")" != "$STT_MODEL_ID" ] ||
   [ ! -f "$STT_DIR/encoder.onnx" ]; then
    echo "==> STT: $STT_URL"
    mkdir -p "$STT_DIR"
    tmp="$(mktemp -d)"; $WGET -O "$tmp/stt.tar.bz2" "$STT_URL"
    tar -xjf "$tmp/stt.tar.bz2" -C "$tmp"
    pack="$(find "$tmp" -type f -name 'encoder.int8.onnx' \
        -printf '%h\n' -quit)"
    if [ -z "$pack" ]; then
        echo "STT archive does not contain the expected Vietnamese model." >&2
        rm -rf "$tmp"
        exit 1
    fi
    cp -f "$pack/encoder.int8.onnx" "$STT_DIR/encoder.onnx"
    cp -f "$pack/decoder.onnx" "$STT_DIR/decoder.onnx"
    cp -f "$pack/joiner.int8.onnx" "$STT_DIR/joiner.onnx"
    cp -f "$pack/tokens.txt" "$STT_DIR/tokens.txt"
    [ ! -f "$pack/bpe.model" ] || cp -f "$pack/bpe.model" "$STT_DIR/bpe.model"
    printf '%s\n' "$STT_MODEL_ID" > "$STT_MARKER"
    rm -rf "$tmp"
fi
echo "==> STT ready in $STT_DIR ($STT_MODEL_ID)"

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

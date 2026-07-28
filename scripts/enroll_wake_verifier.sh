#!/bin/bash
# Record wake-phrase clips from this device owner, score them, and optionally
# train the openWakeWord verifier from them.
#
#   sudo bash scripts/enroll_wake_verifier.sh            # record + score
#   sudo bash scripts/enroll_wake_verifier.sh --train    # ... and fit verifier
#   sudo bash scripts/enroll_wake_verifier.sh --score    # re-score kept clips
#
# Recording is cued by tone, not by text: the speaker plays a rising pair when
# capture opens and a single low tone when it closes, so the phrase can be
# spoken from across the room without watching the terminal. Say the phrase
# right after the rising pair.
#
# Clips are kept under $CLIP_DIR rather than deleted, because the scores they
# produce are the evidence for whatever threshold gets chosen, and a threshold
# argued from clips nobody can re-score is just a guess with a decimal point.
#
# jetson_fw is stopped for the duration: it is the only ALSA owner of the
# ReSpeaker, so arecord cannot open the capture device while it runs. The trap
# below restarts it even if recording is interrupted -- leaving the device deaf
# because someone pressed Ctrl-C is not an acceptable failure mode.
set -euo pipefail

# sudo leaves HOME pointing at the invoking user, and ALSA then probes it for
# an .asoundrc it is not allowed to read, printing "Home directory not
# accessible: Permission denied" over the recording prompts. Harmless, but it
# lands exactly where the cue text needs to be read. Point HOME somewhere root
# genuinely owns and the noise goes away.
export HOME=/root

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck disable=SC1091
. "$SCRIPT_DIR/config_loader.sh"
jetson_load_config "${JETSON_CONFIG_FILE:-/opt/jetson-fw/config.yaml}"

IMAGE="${JETSON_VOICE_RUNTIME_IMAGE:-jetsona/voice-runtime:oww0.6-edge7.2.8-genai2.14}"
MODEL_NAME="${OPENWAKEWORD_MODEL:-hey_nova.onnx}"
MODEL_DIR="/opt/jetson-fw/assets/models/openwakeword"
STATE_DIR="/var/lib/jetson-fw"
CLIP_DIR="${CLIP_DIR:-$STATE_DIR/wake-clips}"
MIC="${JETSON_VOICE_MIC:-plughw:CARD=Lite,DEV=0}"
OUT="${JETSON_VOICE_OUT:-default}"
PHRASE="${WAKE_PHRASE:-Nova}"
POSITIVE_CLIPS="${POSITIVE_CLIPS:-12}"
NEGATIVE_CLIPS="${NEGATIVE_CLIPS:-6}"
CLIP_SECONDS="${CLIP_SECONDS:-2}"

DO_TRAIN=0
DO_RECORD=1
case "${1:-}" in
    --train) DO_TRAIN=1 ;;
    --score) DO_RECORD=0 ;;
    "") ;;
    *) echo "usage: $0 [--train|--score]" >&2; exit 1 ;;
esac

[ "$(id -u)" -eq 0 ] || { echo "run me with sudo" >&2; exit 1; }
[ -f "$MODEL_DIR/$MODEL_NAME" ] || { echo "wake model missing: $MODEL_DIR/$MODEL_NAME" >&2; exit 1; }

mkdir -p "$CLIP_DIR/positive" "$CLIP_DIR/negative"

fw_was_running=0
cleanup() {
    if [ "$fw_was_running" -eq 1 ]; then
        echo
        echo "==> Khởi động lại jetson-fw"
        systemctl start jetson-fw || true
    fi
}
trap cleanup EXIT

# --- tones ----------------------------------------------------------------
# Generated once into tmpfs rather than shipped as assets: two sine bursts are
# cheaper to synthesise than to version, and ffmpeg is already required below.
TONE_DIR="$(mktemp -d /tmp/wake-tones-XXXXXX)"
make_tone() {  # freq_a freq_b outfile
    if [ -n "$2" ]; then
        ffmpeg -loglevel error -y \
            -f lavfi -i "sine=frequency=$1:duration=0.15" \
            -f lavfi -i "sine=frequency=$2:duration=0.15" \
            -filter_complex "[0:a][1:a]concat=n=2:v=0:a=1,volume=0.9" \
            -ar 48000 -ac 1 "$3"
    else
        ffmpeg -loglevel error -y \
            -f lavfi -i "sine=frequency=$1:duration=0.25" \
            -af "volume=0.9" -ar 48000 -ac 1 "$3"
    fi
}
make_tone 880 1320 "$TONE_DIR/start.wav"   # rising pair: capture is open
make_tone 520 ""    "$TONE_DIR/stop.wav"   # single low tone: capture closed

# A failed cue is worse than no cue: recording still runs, so the phrase lands
# in silence the speaker never asked for and twelve takes are wasted before
# anyone notices. Report the failure once instead of swallowing it.
tone_broken=0
play_tone() {
    [ "$tone_broken" -eq 1 ] && return 0
    if ! aplay -D "$OUT" -q "$TONE_DIR/$1.wav" >/dev/null 2>&1; then
        echo "    (không phát được tiếng bíp trên '$OUT' -- vẫn thu bình thường)" >&2
        tone_broken=1
    fi
    return 0
}

# arecord takes the ReSpeaker's two channels; the firmware listens to channel 0
# only (the XU316 puts its processed output there), so clips must come from the
# same channel they will later be scored against.
record_clip() {
    raw="$TONE_DIR/raw.wav"
    play_tone start
    arecord -D "$MIC" -f S16_LE -r 16000 -c 2 -d "$CLIP_SECONDS" -q "$raw"
    play_tone stop
    ffmpeg -loglevel error -y -i "$raw" -af "pan=mono|c0=c0" -ar 16000 "$1"
    rm -f "$raw"
}

if [ "$DO_RECORD" -eq 1 ]; then
    systemctl is-active --quiet jetson-fw && fw_was_running=1
    if [ "$fw_was_running" -eq 1 ]; then
        echo "==> Dừng jetson-fw để giải phóng micro"
        systemctl stop jetson-fw
        sleep 1
    fi

    # Prove the cue is audible before spending twelve takes on it. The whole
    # point of a tone is that the speaker can stand across the room and not
    # watch the terminal; if it never reaches them, every clip after this is
    # recorded into a silence they were still waiting through.
    echo
    echo "==> Thử tiếng bíp trước. Sắp phát: bíp đôi (bắt đầu), rồi bíp trầm (kết thúc)."
    printf '    Enter để nghe thử: '
    read -r _
    play_tone start
    sleep 0.4
    play_tone stop
    printf '    Nghe thấy cả hai tiếng chứ? [y/N] '
    read -r heard
    case "$heard" in
        [yY]*) ;;
        *)
            echo
            echo "    Dừng lại ở đây thay vì thu 12 mẫu mà bạn không nghe được nhịp."
            echo "    Kiểm tra: loa có đang mở không, và thử tay:"
            echo "        aplay -D \"$OUT\" /usr/share/sounds/alsa/Front_Center.wav"
            echo "    Hoặc chỉ định loa khác: JETSON_VOICE_OUT=... sudo -E bash $0"
            exit 1
            ;;
    esac

    rm -f "$CLIP_DIR/positive"/*.wav "$CLIP_DIR/negative"/*.wav

    echo
    echo "==> Thu $POSITIVE_CLIPS mẫu: nói \"$PHRASE\" NGAY SAU tiếng bíp đôi."
    echo "    Tiếng trầm một nhịp = đã thu xong mẫu đó."
    echo "    Đổi vị trí và khoảng cách giữa các lần -- đó là điều làm mẫu có giá trị."
    for i in $(seq 1 "$POSITIVE_CLIPS"); do
        printf '    [%2d/%2d] Enter để bắt đầu: ' "$i" "$POSITIVE_CLIPS"
        read -r _
        record_clip "$(printf '%s/positive/pos-%02d.wav' "$CLIP_DIR" "$i")"
    done

    echo
    echo "==> Thu $NEGATIVE_CLIPS mẫu ÂM NỀN: KHÔNG nói \"$PHRASE\"."
    echo "    Nói câu bình thường, hoặc để yên cho quạt/phòng kêu."
    for i in $(seq 1 "$NEGATIVE_CLIPS"); do
        printf '    [%2d/%2d] Enter để bắt đầu: ' "$i" "$NEGATIVE_CLIPS"
        read -r _
        record_clip "$(printf '%s/negative/neg-%02d.wav' "$CLIP_DIR" "$i")"
    done
fi

rm -rf -- "$TONE_DIR"

run_container() {
    docker run --rm \
        --network none \
        --mount "type=bind,src=$SCRIPT_DIR/train_wake_verifier.py,dst=/app/train_wake_verifier.py,readonly" \
        --mount "type=bind,src=$MODEL_DIR,dst=/models,readonly" \
        --mount "type=bind,src=$CLIP_DIR,dst=/clips,readonly" \
        --mount "type=bind,src=$STATE_DIR,dst=/state" \
        --entrypoint python3 \
        "$IMAGE" /app/train_wake_verifier.py \
        --model "/models/$MODEL_NAME" \
        --positive-dir /clips/positive \
        --negative-dir /clips/negative \
        "$@"
}

echo
echo "==> Chấm điểm từng mẫu qua wake model (offline)"
run_container --mode score

if [ "$DO_TRAIN" -eq 1 ]; then
    echo
    echo "==> Train verifier"
    run_container --mode train --output /state/hey_nova_verifier.pkl
    chmod 0644 "$STATE_DIR/hey_nova_verifier.pkl"
    echo "    Nạp bằng: systemctl restart jetsona-openwakeword"
    echo "    Log phải đổi từ 'verifier disabled' sang 'verifier enabled'."
fi

echo
echo "==> Clip giữ tại $CLIP_DIR (chấm lại: $0 --score)"

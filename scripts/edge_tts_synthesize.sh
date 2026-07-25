#!/bin/bash
# Render UTF-8 text with Microsoft Edge TTS, then decode to mono S16LE PCM.
set -euo pipefail

if [ "$#" -ne 6 ]; then
    echo "usage: $0 TEXT_FILE PCM_FILE VOICE RATE SAMPLE_RATE IMAGE" >&2
    exit 2
fi

text_file="$1"
pcm_file="$2"
voice="$3"
rate="$4"
sample_rate="$5"
image="$6"

case "$text_file:$pcm_file" in
    /run/jetsona-edge-tts/*:/run/jetsona-edge-tts/*) ;;
    *) echo "Edge TTS files must stay under /run/jetsona-edge-tts" >&2; exit 2 ;;
esac

out_dir="$(dirname "$pcm_file")"
media_name="$(basename "$pcm_file").mp3"
docker run --rm \
    --network bridge \
    --read-only \
    --tmpfs /tmp:rw,noexec,nosuid,size=32m \
    --mount "type=bind,src=$text_file,dst=/input.txt,readonly" \
    --mount "type=bind,src=$out_dir,dst=/out" \
    --entrypoint edge-tts \
    "$image" \
    --file /input.txt \
    --voice "$voice" \
    --rate "$rate" \
    --write-media "/out/$media_name"

ffmpeg -nostdin -hide_banner -loglevel error -y \
    -i "$out_dir/$media_name" \
    -ac 1 -ar "$sample_rate" -f s16le "$pcm_file"
rm -f -- "$out_dir/$media_name"

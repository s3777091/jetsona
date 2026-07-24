#!/bin/bash
# Push the alarm ringtones under assets/ringtones/ to the S3 asset bucket, so
# every Jetson running Ekko Lite fetches the same ringtone set at build time.
#
# The ringtones are small (a few hundred KB of MP3 each), so this is also the
# path used to ship the test tones the user asked for ("những nhạc nhỏ nên để
# testing nhạc push lên s3"): drop a short tone into assets/ringtones/ and run
# this script.
#
# Uses scripts/s3_assets.py upload-file, which uploads each key without
# rewriting the rest of the bucket. Credentials come from .env (MINIO_*),
# endpoint/bucket/region from config.yaml -- both loaded by config_loader.sh.
set -e

JETSON_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$JETSON_DIR"

# shellcheck source=scripts/config_loader.sh
. scripts/config_loader.sh
jetson_load_config "${JETSON_CONFIG_FILE:-$JETSON_DIR/config.yaml}"
jetson_load_secrets "${JETSON_ENV_FILE:-$JETSON_DIR/.env}"

RING_DIR="assets/ringtones"
if [ ! -d "$RING_DIR" ]; then
    echo "push_ringtones: $RING_DIR not found (nothing to push)." >&2
    exit 0
fi

shopt -s nullglob
count=0
for f in "$RING_DIR"/*.mp3 "$RING_DIR"/*.wav "$RING_DIR"/*.ogg "$RING_DIR"/*.m4a; do
    rel="ringtones/$(basename "$f")"
    echo "==> uploading $rel"
    python3 scripts/s3_assets.py upload-file "$rel"
    count=$((count + 1))
done

if [ "$count" -eq 0 ]; then
    echo "push_ringtones: no audio files in $RING_DIR." >&2
    exit 0
fi
echo "==> done: $count ringtones pushed to S3."
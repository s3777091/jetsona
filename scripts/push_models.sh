#!/bin/bash
# Mirror the voice model packs under assets/models/ to the S3 asset bucket, so
# a fleet of Jetsons can fetch them offline via the normal build-time
# fetch_assets.sh instead of each pulling from HuggingFace.
#
# NOTE: models are large (hundreds of MB). For a single device it is simpler to
# run scripts/fetch_models.sh on the Jetson directly. This script is for
# mirroring once, then letting fetch_assets.sh distribute.
set -e

JETSON_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$JETSON_DIR"

# shellcheck source=scripts/config_loader.sh
. scripts/config_loader.sh
jetson_load_config "${JETSON_CONFIG_FILE:-$JETSON_DIR/config.yaml}"
jetson_load_secrets "${JETSON_ENV_FILE:-$JETSON_DIR/.env}"

MODELS="assets/models"
if [ ! -d "$MODELS" ]; then
    echo "push_models: $MODELS not found. Run scripts/fetch_models.sh first." >&2
    exit 1
fi

count=0
while IFS= read -r -d '' f; do
    rel="models/${f#"$MODELS"/}"
    echo "==> uploading $rel"
    python3 scripts/s3_assets.py upload-file "$rel"
    count=$((count + 1))
done < <(find "$MODELS" -type f -print0)

echo "==> done: $count model files pushed to S3."
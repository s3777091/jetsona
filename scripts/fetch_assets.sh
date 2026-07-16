#!/bin/bash
# Fetch runtime assets from MinIO (S3), skipping files already present.
#
# Called two ways, both intentional:
#   1. By scripts/build.sh  (before `cmake --build`), and
#   2. By the CMake pre-build custom target in CMakeLists.txt,
# so a plain `cmake --build` / `make` also pulls assets.
#
# This is the "bo kiem tra" (checker): for every object in the bucket, download
# only if the local file is missing or its size differs. Rebuilds after the
# first download just re-check and touch nothing (instant, no re-download).
#
# Credentials come from .env (gitignored). If .env is missing the script still
# works as long as MINIO_* are already in the environment.
#
# Offline-tolerant: if the fetch fails (no network / server down) but assets
# are already present locally, we warn and exit 0 so a cached rebuild works
# without connectivity. If assets are missing entirely we fail loudly.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
ASSETS_DIR="${JETSON_ASSETS_DIR:-$JETSON_DIR/assets}"

# Source .env if present so MINIO_* (endpoint, creds, bucket) are exported.
# Missing .env is fine if the vars are already in the environment.
ENV_FILE="$JETSON_DIR/.env"
if [ -f "$ENV_FILE" ]; then
    set -a
    # shellcheck disable=SC1091
    . "$ENV_FILE"
    set +a
fi

PY="${PYTHON:-}"
# Find a working python interpreter. On the Jetson `python3` is real; on some
# Windows setups `python3` is a Microsoft Store stub that exits non-zero, so we
# verify by running --version and fall back to `python`.
if [ -z "$PY" ]; then
    for cand in python3 python; do
        if command -v "$cand" >/dev/null 2>&1 && "$cand" --version >/dev/null 2>&1; then
            PY="$cand"
            break
        fi
    done
fi
if [ -z "$PY" ]; then
    echo "fetch_assets.sh: no usable python found. Install python3 (apt-get install python3)." >&2
    exit 1
fi

# Count files already on disk (0 if the dir doesn't exist yet).
count_local() {
    [ -d "$ASSETS_DIR" ] && find "$ASSETS_DIR" -type f | wc -l || echo 0
}
BEFORE="$(count_local)"

# Run the fetcher. -E fails on unset vars; but s3_assets.py reads env directly.
set +e
"$PY" "$JETSON_DIR/scripts/s3_assets.py" fetch
RC=$?
set -e

if [ "$RC" -eq 0 ]; then
    exit 0
fi

# Fetch failed. Tolerate it only if we already have assets cached locally.
AFTER="$(count_local)"
if [ "$AFTER" -gt 0 ]; then
    echo "fetch_assets.sh: WARNING -- asset fetch failed (rc=$RC) but $AFTER file(s)" >&2
    echo "  already cached in $ASSETS_DIR; continuing the build with cached assets." >&2
    echo "  Re-run when the MinIO server is reachable to refresh." >&2
    exit 0
fi

echo "fetch_assets.sh: asset fetch failed (rc=$RC) and no assets are cached." >&2
echo "  Check MINIO_ENDPOINT / MINIO_ACCESS_KEY / MINIO_SECRET_KEY / MINIO_BUCKET in .env" >&2
echo "  and that the MinIO server is reachable." >&2
exit "$RC"
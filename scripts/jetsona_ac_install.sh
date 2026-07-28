#!/bin/bash
# Install the Jetsona air-conditioner bridge (LG ThinQ) on the Jetson.
#
# Run as root (sudo). Credentials are taken from, in order of precedence:
#   1. env vars passed to this script
#   2. an existing LG ThinQ .env passed as the first argument (or LGE_ENV_FILE)
#      -- e.g. the one from the IOT project, copied over from the PC
#   3. whatever is already in /etc/jetsona-ac.env
# so a bare re-run never wipes what is configured, and the PAT never has to be
# retyped:
#
#   sudo bash scripts/jetsona_ac_install.sh /home/ekkohuynh/iot.env
#
#   LGE_ACCESS_TOKEN   LG ThinQ Personal Access Token          (REQUIRED once)
#   LGE_REGION         KIC | AIC | EIC                         (default KIC)
#   LGE_COUNTRY        2-letter country code                   (default VN)
#   LGE_CLIENT_ID      stable client id, do not churn it       (default jetsona-ac-001)
#   LGE_DEVICE_ID      AC deviceId; empty = first AC found
#   AC_PORT            listen port                             (default 46003)
#   AC_TOKEN           shared secret for X-AC-Token; generated when unset
#   AC_MIN_C/AC_MAX_C  allowed setpoint range                  (default 16/30)
#   AC_DEFAULT_C       setpoint used when switching on         (default 25)
#
# Get the PAT at https://smartsolution.developer.lge.com
#   Cloud Developer -> ThinQ Connect -> PAT, with scopes:
#   view devices, view statuses, control devices.
#
# Side effects:
#   - installs /usr/local/lib/jetsona/jetsona_ac.py
#   - writes /etc/jetsona-ac.env (mode 600, root)
#   - enables + starts jetsona-ac.service
#   - prints the AC_TOKEN so you can copy it into the firmware .env
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo bash $0" >&2
    exit 1
fi

ENV_FILE="/etc/jetsona-ac.env"
LIB_DIR="/usr/local/lib/jetsona"
UNIT_DIR="/etc/systemd/system"
UNIT_SRC="$SCRIPT_DIR/jetsona-ac.service"
UNIT_DST="$UNIT_DIR/jetsona-ac.service"

# An existing LG ThinQ .env to seed credentials from (the IOT project's, say).
SEED_ENV_FILE="${LGE_ENV_FILE:-${1:-}}"
if [ -n "$SEED_ENV_FILE" ] && [ ! -f "$SEED_ENV_FILE" ]; then
    echo "No such env file: $SEED_ENV_FILE" >&2
    exit 1
fi

# Every helper below returns 0 even when it finds nothing: under `set -e` a
# non-zero return from a command substitution aborts the assignment that called
# it, and on a first install there is nothing to read yet.

# read_key FILE KEY -- last KEY=VALUE wins; strips `export`, quotes, trailing space.
read_key() {
    [ -f "$1" ] || return 0
    grep -E "^[[:space:]]*(export[[:space:]]+)?$2=" "$1" 2>/dev/null \
        | tail -1 \
        | cut -d= -f2- \
        | sed -e 's/^[[:space:]]*//' -e "s/^[\"']//" -e "s/[\"'][[:space:]]*\$//" \
              -e 's/[[:space:]]*$//' \
        || true
}

# resolve VAR_NAME ENV_KEY -- process env, then the seed file, then what is
# already installed.
resolve() {
    local current="${!1:-}"
    if [ -n "$current" ]; then printf '%s' "$current"; return 0; fi
    local found
    found="$(read_key "$SEED_ENV_FILE" "$2")"
    if [ -n "$found" ]; then printf '%s' "$found"; return 0; fi
    read_key "$ENV_FILE" "$2"
}

LGE_ACCESS_TOKEN="$(resolve LGE_ACCESS_TOKEN LGE_ACCESS_TOKEN)"
if [ -z "$LGE_ACCESS_TOKEN" ]; then
    echo "LGE_ACCESS_TOKEN is required on first install. Either:" >&2
    echo "  - point this script at an existing LG ThinQ .env (the IOT project has one):" >&2
    echo "      sudo bash $0 /path/to/iot.env" >&2
    echo "  - or pass the token directly:" >&2
    echo "      sudo LGE_ACCESS_TOKEN=... bash $0" >&2
    echo "New PATs come from https://smartsolution.developer.lge.com (ThinQ Connect -> PAT)." >&2
    exit 1
fi
if [ -n "$SEED_ENV_FILE" ]; then
    echo "==> Seeding LG credentials from $SEED_ENV_FILE"
fi

LGE_REGION="$(resolve LGE_REGION LGE_REGION)"
LGE_REGION="${LGE_REGION:-KIC}"
LGE_COUNTRY="$(resolve LGE_COUNTRY LGE_COUNTRY)"
LGE_COUNTRY="${LGE_COUNTRY:-VN}"
LGE_CLIENT_ID="$(resolve LGE_CLIENT_ID LGE_CLIENT_ID)"
LGE_CLIENT_ID="${LGE_CLIENT_ID:-jetsona-ac-001}"
LGE_DEVICE_ID="$(resolve LGE_DEVICE_ID LGE_DEVICE_ID)"

# reuse_env VAR_NAME ENV_KEY -- current value wins, else what is installed.
reuse_env() {
    local current="${!1:-}"
    if [ -n "$current" ]; then printf '%s' "$current"; return 0; fi
    read_key "$ENV_FILE" "$2"
}

AC_PORT="${AC_PORT:-$(reuse_env _unset JETSON_AC_PORT)}"
AC_PORT="${AC_PORT:-46003}"
AC_MIN_C="${AC_MIN_C:-$(reuse_env _unset JETSON_AC_MIN_C)}"
AC_MIN_C="${AC_MIN_C:-16}"
AC_MAX_C="${AC_MAX_C:-$(reuse_env _unset JETSON_AC_MAX_C)}"
AC_MAX_C="${AC_MAX_C:-30}"
AC_DEFAULT_C="${AC_DEFAULT_C:-$(reuse_env _unset JETSON_AC_DEFAULT_C)}"
AC_DEFAULT_C="${AC_DEFAULT_C:-25}"

# Reuse the existing token so a re-run does not desync the firmware secret.
AC_TOKEN="$(reuse_env AC_TOKEN JETSON_AC_TOKEN)"
token_is_new=0
if [ -z "$AC_TOKEN" ]; then
    AC_TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(24))')"
    token_is_new=1
fi

echo "==> Installing jetsona_ac.py -> $LIB_DIR"
install -d -m 0755 "$LIB_DIR"
install -m 0755 "$SCRIPT_DIR/jetsona_ac.py" "$LIB_DIR/jetsona_ac.py"

echo "==> Writing $ENV_FILE"
umask 077
cat > "$ENV_FILE" <<EOF
JETSON_AC_HOST=127.0.0.1
JETSON_AC_PORT=$AC_PORT
JETSON_AC_TOKEN=$AC_TOKEN
JETSON_AC_MIN_C=$AC_MIN_C
JETSON_AC_MAX_C=$AC_MAX_C
JETSON_AC_DEFAULT_C=$AC_DEFAULT_C
LGE_ACCESS_TOKEN=$LGE_ACCESS_TOKEN
LGE_REGION=$LGE_REGION
LGE_COUNTRY=$LGE_COUNTRY
LGE_CLIENT_ID=$LGE_CLIENT_ID
LGE_DEVICE_ID=$LGE_DEVICE_ID
EOF
chmod 600 "$ENV_FILE"

echo "==> Installing jetsona-ac.service"
install -m 0644 "$UNIT_SRC" "$UNIT_DST"

echo "==> Reloading systemd + starting jetsona-ac"
systemctl daemon-reload
systemctl enable --now jetsona-ac.service
systemctl restart jetsona-ac.service

# Give it a moment to resolve the device against LG's cloud before probing.
sleep 3

echo "==> Status"
systemctl --no-pager --lines=5 status jetsona-ac.service 2>&1 | head -14 || true

echo
echo "==> Probe"
if curl -fsS --max-time 10 "http://127.0.0.1:$AC_PORT/status" \
        -H "X-AC-Token: $AC_TOKEN"; then
    echo
    echo "    OK -- the bridge reached the air conditioner."
else
    echo "    FAILED. Check: journalctl -u jetsona-ac -n 40 --no-pager" >&2
fi

echo
echo "AC endpoint: http://127.0.0.1:$AC_PORT"
echo "Range:       $AC_MIN_C-$AC_MAX_C C, default on = $AC_DEFAULT_C C"
if [ "$token_is_new" -eq 1 ]; then
    echo
    echo "Generated a new X-AC-Token. Put it in the firmware .env"
    echo "(~/jetsona/.env and /opt/jetson-fw/.env), then restart jetson-fw:"
    echo
    echo "  JETSON_AC_TOKEN=$AC_TOKEN"
    echo
    echo "config.yaml already points JETSON_AC_URL at http://127.0.0.1:$AC_PORT."
else
    echo "Reused the existing X-AC-Token (firmware .env unchanged)."
fi

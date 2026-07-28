#!/usr/bin/env python3
"""Jetsona air-conditioner HTTP endpoint (LG ThinQ Connect).

A dependency-free HTTP service that runs on the Jetson and turns the LG
ThinQ cloud API into a small local surface the firmware's `air_conditioner`
tool can call:

  GET  /health      -> liveness probe (no auth)
  GET  /status      -> current power/mode/target/room temp/fan/humidity
  POST /power       -> {"on": true|false}
  POST /temperature -> {"celsius": 26}
  POST /mode        -> {"mode": "COOL"|"AIR_DRY"|"FAN"}
  POST /fan         -> {"speed": "LOW"|"MID"|"HIGH"|"AUTO"}
  POST /comfort     -> {"feeling": "cold"|"hot"|"humid"|"stuffy"|"ok",
                        "intensity": "slight"|"normal"|"very"}

/comfort is the one that matters for voice: "tôi thấy lạnh" is not a setpoint,
it is a complaint. Turning it into commands needs the *current* state, so the
decision is made here (read -> decide -> write in one round trip) rather than
by asking the model to chain status + set_temp calls. The model gets back a
sentence describing what actually changed, which is what it speaks.

Why this lives on the Jetson and not on the PC: the Jetson is the always-on
box. The PC is asleep most of the time (that is what the WoL endpoint is for),
so hanging climate control off it would break exactly when the user is in bed
asking for it.

This is a port of the C:\\Users\\ADMIN\\workspace\\IOT project (config.py /
lge_thinq.py / ac.py) onto the Jetson's stock Python 3.6, which has no pip
packages: requests/Flask/dotenv are replaced with urllib + http.server. The
ThinQ request shapes, headers, and the RAC_056905_WW enum set are kept
identical to that project -- change them there and here together.

Every endpoint except /health requires a shared bearer token (X-AC-Token
header, Authorization: Bearer, or ?token=), constant-time compared.

Config (env, typically /etc/jetsona-ac.env):
  JETSON_AC_HOST       listen host                  (default 127.0.0.1)
  JETSON_AC_PORT       listen port                  (default 46003)
  JETSON_AC_TOKEN      shared secret                (REQUIRED)
  LGE_ACCESS_TOKEN     LG ThinQ Personal Access Token (REQUIRED)
  LGE_REGION           KIC | AIC | EIC              (default KIC)
  LGE_COUNTRY          2-letter country code        (default VN)
  LGE_CLIENT_ID        stable client id             (default jetsona-ac-001)
  LGE_DEVICE_ID        AC deviceId; empty = first AC found
  JETSON_AC_MIN_C      coldest allowed setpoint     (default 16)
  JETSON_AC_MAX_C      warmest allowed setpoint     (default 30)
  JETSON_AC_DEFAULT_C  setpoint used when switching the unit on (default 25)
  JETSON_AC_CACHE_SEC  state cache TTL, LG rate-limits  (default 5)
"""

import json
import os
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn

HOST = os.environ.get("JETSON_AC_HOST", "127.0.0.1")
PORT = int(os.environ.get("JETSON_AC_PORT", "46003"))
TOKEN = os.environ.get("JETSON_AC_TOKEN", "")

LGE_ACCESS_TOKEN = os.environ.get("LGE_ACCESS_TOKEN", "")
LGE_REGION = os.environ.get("LGE_REGION", "KIC")
LGE_COUNTRY = os.environ.get("LGE_COUNTRY", "VN")
LGE_CLIENT_ID = os.environ.get("LGE_CLIENT_ID", "jetsona-ac-001")
LGE_DEVICE_ID = os.environ.get("LGE_DEVICE_ID", "")

MIN_C = float(os.environ.get("JETSON_AC_MIN_C", "16"))
MAX_C = float(os.environ.get("JETSON_AC_MAX_C", "30"))
DEFAULT_C = float(os.environ.get("JETSON_AC_DEFAULT_C", "25"))
CACHE_SEC = float(os.environ.get("JETSON_AC_CACHE_SEC", "5"))

# Fixed API key of the LG ThinQ Connect OpenAPI, mirrored from LG's official
# thinqconnect SDK. Not a secret; the PAT is.
LGE_API_KEY = "v6GFvkweNo7DK7yD3ylIZ9w52aKBU0eJ7wLXkSR3"

REGION_BASE_URLS = {
    "AIC": "https://api-aic.lgthinq.com",  # Americas
    "KIC": "https://api-kic.lgthinq.com",  # Korea / Asia-Pacific (VN)
    "EIC": "https://api-eic.lgthinq.com",  # Europe / MEA
}

POWER_ON = "POWER_ON"
POWER_OFF = "POWER_OFF"
# Writable enums for RAC_056905_WW, taken from the device profile. Run
# `uv run ac.py profile` in the IOT project if the unit is ever replaced.
JOB_MODES = ("COOL", "AIR_DRY", "FAN")
FAN_SPEEDS = ("LOW", "MID", "HIGH", "AUTO")
# Ordered ladder for stepping the fan up/down. AUTO sits outside it.
FAN_LADDER = ("LOW", "MID", "HIGH")


def die(message):
    print("jetsona_ac: %s" % message, file=sys.stderr, flush=True)
    sys.exit(1)


def log(message):
    print("jetsona_ac: %s" % message, flush=True)


def constant_time_eq(a, b):
    if len(a) != len(b):
        return False
    diff = 0
    for x, y in zip(a.encode(), b.encode()):
        diff |= x ^ y
    return diff == 0


class ThinQError(RuntimeError):
    """Non-2xx from the LG ThinQ API, with the decoded body when there is one."""

    def __init__(self, status, body):
        self.status = status
        self.body = body
        RuntimeError.__init__(self, "ThinQ API %s: %s" % (status, body))


class ThinQClient(object):
    """Minimal REST client for LG ThinQ Connect (urllib, no requests)."""

    def __init__(self):
        self._base = REGION_BASE_URLS.get(LGE_REGION)
        if not self._base:
            raise ValueError("bad LGE_REGION %r (want one of %s)"
                             % (LGE_REGION, ", ".join(sorted(REGION_BASE_URLS))))
        self._ssl = ssl.create_default_context()
        self._msg_seq = 0
        self._seq_lock = threading.Lock()

    def _message_id(self):
        """x-message-id must differ per request. uuid4 would do; a counter plus
        the pid keeps it readable in LG's logs and avoids the import."""
        with self._seq_lock:
            self._msg_seq += 1
            seq = self._msg_seq
        return "jetsona-%d-%d-%d" % (os.getpid(), int(time.time()), seq)

    def _request(self, method, path, payload=None, extra_headers=None):
        url = self._base + "/" + path.lstrip("/")
        data = json.dumps(payload).encode() if payload is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + LGE_ACCESS_TOKEN)
        req.add_header("Content-Type", "application/json")
        req.add_header("x-country", LGE_COUNTRY)
        req.add_header("x-client-id", LGE_CLIENT_ID)
        req.add_header("x-message-id", self._message_id())
        req.add_header("x-api-key", LGE_API_KEY)
        req.add_header("x-service-phase", "OP")
        for key, value in (extra_headers or {}).items():
            req.add_header(key, value)

        try:
            with urllib.request.urlopen(req, timeout=20, context=self._ssl) as resp:
                raw = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", "replace")
            try:
                body = json.loads(raw)
            except ValueError:
                body = raw
            raise ThinQError(exc.code, body)

        try:
            body = json.loads(raw)
        except ValueError:
            return raw
        # Payloads are wrapped in "response".
        if isinstance(body, dict) and "response" in body:
            return body["response"]
        return body

    def list_devices(self):
        return self._request("GET", "/devices") or []

    def get_state(self, device_id):
        return self._request("GET", "/devices/%s/state" % device_id) or {}

    def control(self, device_id, command):
        return self._request("POST", "/devices/%s/control" % device_id,
                             payload=command,
                             extra_headers={"x-conditional-control": "true"})

    def find_ac(self, device_id=""):
        devices = self.list_devices()
        if not devices:
            raise RuntimeError("tài khoản LG không có thiết bị nào")
        acs = [d for d in devices
               if d.get("deviceType") == 401 or "AIR_CONDITIONER" in str(d)]
        if not acs:
            available = [(d.get("deviceId"), d.get("deviceInfo", {}).get("alias"))
                         for d in devices]
            raise RuntimeError("không tìm thấy điều hòa; thiết bị có sẵn: %s" % available)
        if device_id:
            for d in acs:
                if d.get("deviceId") == device_id:
                    return d
            raise RuntimeError("không tìm thấy điều hòa deviceId=%s" % device_id)
        return acs[0]


def dig(state, *keys):
    cur = state
    for key in keys:
        if not isinstance(cur, dict) or key not in cur:
            return None
        cur = cur[key]
    return cur


def round_half(value):
    """The unit accepts 0.5 steps; snap to the nearest one."""
    return round(float(value) * 2) / 2.0


def clamp_setpoint(celsius):
    return max(MIN_C, min(MAX_C, round_half(celsius)))


def fmt_c(celsius):
    """25.0 -> '25', 25.5 -> '25.5' -- spoken back by the model verbatim."""
    if celsius is None:
        return "?"
    celsius = float(celsius)
    return str(int(celsius)) if celsius == int(celsius) else ("%.1f" % celsius)


class AirConditioner(object):
    """The AC, with the device lookup and a short state cache in front of it.

    Both exist for the same reason: LG rate-limits the cloud API, and /comfort
    reads state before every write. The cache is short enough that a user
    pressing on ("vẫn lạnh") still sees their own previous change."""

    def __init__(self, client):
        self._client = client
        self._lock = threading.Lock()
        self._device_id = None
        self._alias = None
        self._state = None
        self._state_at = 0.0

    def device_id(self):
        with self._lock:
            if self._device_id:
                return self._device_id
        device = self._client.find_ac(LGE_DEVICE_ID)
        with self._lock:
            self._device_id = device["deviceId"]
            self._alias = device.get("deviceInfo", {}).get("alias") or "Điều hòa"
            log("device %s (%s)" % (self._device_id, self._alias))
            return self._device_id

    def alias(self):
        self.device_id()
        return self._alias

    def _invalidate(self):
        with self._lock:
            self._state = None
            self._state_at = 0.0

    def status(self, allow_cache=True):
        if allow_cache:
            with self._lock:
                fresh = self._state is not None and (time.monotonic() - self._state_at) < CACHE_SEC
                if fresh:
                    return self._state
        raw = self._client.get_state(self.device_id())
        operation = dig(raw, "operation", "airConOperationMode")
        state = {
            "is_on": (operation == POWER_ON) if operation else None,
            "operation_mode": operation,
            "job_mode": dig(raw, "airConJobMode", "currentJobMode"),
            "target_temp_c": dig(raw, "temperature", "targetTemperature"),
            "current_temp_c": dig(raw, "temperature", "currentTemperature"),
            "wind_strength": dig(raw, "airFlow", "windStrength"),
            "humidity": dig(raw, "airQualitySensor", "humidity"),
        }
        with self._lock:
            self._state = state
            self._state_at = time.monotonic()
        return state

    # ---- writes ----------------------------------------------------------

    def set_power(self, on):
        self._client.control(self.device_id(),
                             {"operation": {"airConOperationMode":
                                            POWER_ON if on else POWER_OFF}})
        self._invalidate()

    def set_temperature(self, celsius):
        celsius = float(celsius)
        if not MIN_C <= celsius <= MAX_C:
            raise ValueError("nhiệt độ ngoài khoảng %s-%s°C: %s"
                             % (fmt_c(MIN_C), fmt_c(MAX_C), fmt_c(celsius)))
        celsius = round_half(celsius)
        self._client.control(self.device_id(),
                             {"temperature": {"targetTemperature": celsius}})
        self._invalidate()
        return celsius

    def set_job_mode(self, mode):
        mode = str(mode).upper()
        if mode not in JOB_MODES:
            raise ValueError("chế độ không hợp lệ: %s (chọn %s)"
                             % (mode, "/".join(JOB_MODES)))
        self._client.control(self.device_id(),
                             {"airConJobMode": {"currentJobMode": mode}})
        self._invalidate()
        return mode

    def set_fan_speed(self, speed):
        speed = str(speed).upper()
        if speed not in FAN_SPEEDS:
            raise ValueError("tốc độ quạt không hợp lệ: %s (chọn %s)"
                             % (speed, "/".join(FAN_SPEEDS)))
        self._client.control(self.device_id(),
                             {"airFlow": {"windStrength": speed}})
        self._invalidate()
        return speed


# ---- comfort -------------------------------------------------------------

# How many °C one complaint moves the setpoint.
INTENSITY_STEP = {"slight": 1.0, "normal": 2.0, "very": 3.0}

# Everything the speech recogniser realistically produces for each feeling,
# in both Vietnamese (with and without diacritics -- STT drops them often)
# and English, mapped to the canonical feeling.
FEELING_ALIASES = {
    "cold": "cold", "lanh": "cold", "lạnh": "cold", "ret": "cold", "rét": "cold",
    "buot": "cold", "buốt": "cold", "chilly": "cold", "cool": "cold",
    "hot": "hot", "nong": "hot", "nóng": "hot", "oi": "hot", "oi buc": "hot",
    "oi bức": "hot", "nuc": "hot", "nực": "hot", "warm": "hot",
    "humid": "humid", "am": "humid", "ẩm": "humid",
    "nom am": "humid", "nồm ẩm": "humid", "muggy": "humid",
    "stuffy": "stuffy", "bi": "stuffy", "bí": "stuffy", "ngot ngat": "stuffy",
    "ngột ngạt": "stuffy", "kho tho": "stuffy", "khó thở": "stuffy",
    "ok": "ok", "fine": "ok", "de chiu": "ok", "dễ chịu": "ok",
    "vua": "ok", "vừa": "ok", "on": "ok", "ổn": "ok",
}


def fan_step(current, delta):
    """Move the fan one notch along LOW->MID->HIGH. AUTO enters the ladder at
    MID so 'still too warm, fan up' has somewhere to go. Returns None when the
    fan is already at the end of the ladder."""
    base = current if current in FAN_LADDER else "MID"
    index = FAN_LADDER.index(base)
    target = index + delta
    if target < 0 or target >= len(FAN_LADDER):
        return None
    nxt = FAN_LADDER[target]
    return None if nxt == current else nxt


def apply_comfort(ac, feeling, intensity):
    """Read the AC, decide what "I feel <feeling>" should change, write it.

    Returns (summary, changes, before, after). The unit has no heater -- COOL /
    AIR_DRY / FAN are all it can do -- so "too cold" means *cool less*: raise
    the setpoint, ease the fan, and at the top of the range stop cooling
    altogether by switching to FAN.

    `after` is projected from the writes we issued rather than read back from
    LG. Two reasons: it saves a cloud round trip on the rate-limited API, and
    ThinQ is eventually consistent -- a read issued straight after a control
    call often still returns the old setpoint, which would have us report "đã
    nâng lên 26 độ, hiện tại 24 độ" and leave the model to guess which is
    true."""
    step = INTENSITY_STEP.get(intensity, INTENSITY_STEP["normal"])
    before = ac.status()
    changes = []
    after = dict(before)

    is_on = bool(before.get("is_on"))
    job_mode = before.get("job_mode")
    target = before.get("target_temp_c")
    room = before.get("current_temp_c")
    fan = before.get("wind_strength")

    # Write, and record what the unit is now set to. Every mutation below goes
    # through these so `after` cannot drift from what was actually sent.
    def set_power(on):
        ac.set_power(on)
        after["is_on"] = on
        after["operation_mode"] = POWER_ON if on else POWER_OFF

    def set_mode(mode):
        after["job_mode"] = ac.set_job_mode(mode)

    def set_temp(celsius):
        after["target_temp_c"] = ac.set_temperature(celsius)

    def set_fan(speed):
        after["wind_strength"] = ac.set_fan_speed(speed)

    if feeling == "ok":
        summary = "Giữ nguyên cài đặt: %s." % describe(before)
        return summary, changes, before, after

    if feeling == "humid":
        if not is_on:
            set_power(True)
            changes.append("bật máy")
        if job_mode != "AIR_DRY":
            set_mode("AIR_DRY")
            changes.append("chuyển chế độ hút ẩm (AIR_DRY)")
        summary = ("Đã chuyển sang hút ẩm cho đỡ nồm."
                   if changes else "Máy đang chạy hút ẩm rồi, giữ nguyên.")
        return summary, changes, before, after

    if feeling == "stuffy":
        if not is_on:
            set_power(True)
            changes.append("bật máy")
        if job_mode != "FAN":
            set_mode("FAN")
            changes.append("chuyển sang quạt gió (FAN)")
        stronger = fan_step(fan, +1)
        if stronger:
            set_fan(stronger)
            changes.append("tăng quạt lên %s" % stronger)
        summary = ("Đã chuyển sang quạt gió cho thoáng."
                   if changes else "Máy đang quạt gió sẵn rồi, giữ nguyên.")
        return summary, changes, before, after

    if feeling == "hot":
        if not is_on:
            # Cold start: cool, at a sensible setpoint, harder if they said
            # "very". Nothing to read off the old state -- it was off.
            set_power(True)
            changes.append("bật máy")
            if job_mode != "COOL":
                set_mode("COOL")
                changes.append("chế độ làm lạnh (COOL)")
            start = clamp_setpoint(DEFAULT_C - (step - INTENSITY_STEP["normal"]))
            set_temp(start)
            changes.append("đặt %s°C" % fmt_c(start))
            if intensity == "very":
                set_fan("HIGH")
                changes.append("quạt HIGH")
            return ("Đã bật điều hòa làm lạnh %s°C." % fmt_c(start)), changes, before, after

        if job_mode != "COOL":
            set_mode("COOL")
            changes.append("chuyển sang làm lạnh (COOL)")
        if target is None:
            new_target = clamp_setpoint(DEFAULT_C)
        else:
            new_target = clamp_setpoint(float(target) - step)
        if target is None or new_target != round_half(target):
            set_temp(new_target)
            changes.append("hạ xuống %s°C" % fmt_c(new_target))
            summary = "Đã hạ điều hòa xuống %s°C cho mát hơn." % fmt_c(new_target)
        else:
            # Already at the floor: the only lever left is airflow.
            stronger = fan_step(fan, +1)
            if stronger:
                set_fan(stronger)
                changes.append("tăng quạt lên %s" % stronger)
                summary = ("Đã ở mức lạnh nhất %s°C rồi nên tăng quạt lên %s."
                           % (fmt_c(new_target), stronger))
            else:
                summary = ("Điều hòa đã ở mức mạnh nhất: %s°C, quạt %s."
                           % (fmt_c(new_target), fan or "?"))
            return summary, changes, before, after

        if intensity == "very":
            stronger = fan_step(fan, +1)
            if stronger:
                set_fan(stronger)
                changes.append("tăng quạt lên %s" % stronger)
                summary = ("Đã hạ xuống %s°C và tăng quạt lên %s."
                           % (fmt_c(new_target), stronger))
        return summary, changes, before, after

    # feeling == "cold"
    if not is_on:
        summary = ("Điều hòa đang tắt nên không phải nó làm lạnh%s."
                   % ("" if room is None else ", phòng đang %s°C" % fmt_c(room)))
        return summary, changes, before, after

    if job_mode == "FAN":
        # Not cooling at all; the fan itself is the draught.
        weaker = fan_step(fan, -1)
        if weaker:
            set_fan(weaker)
            changes.append("giảm quạt xuống %s" % weaker)
            return ("Máy đang chạy quạt gió, đã giảm quạt xuống %s." % weaker), changes, before, after
        set_power(False)
        changes.append("tắt máy")
        return ("Máy chỉ đang quạt ở mức thấp nhất nên đã tắt hẳn cho đỡ lạnh.",
                changes, before, after)

    if target is None:
        new_target = clamp_setpoint(DEFAULT_C + step)
    else:
        new_target = clamp_setpoint(float(target) + step)

    if target is not None and new_target == round_half(target):
        # Setpoint is pinned at the top and it is still too cold: stop cooling.
        if job_mode != "FAN":
            set_mode("FAN")
            changes.append("chuyển sang quạt gió (FAN), ngừng làm lạnh")
            return ("Đã ở %s°C là mức ấm nhất rồi nên chuyển sang quạt gió, ngừng làm lạnh."
                    % fmt_c(new_target)), changes, before, after
        summary = "Điều hòa đã ở mức ấm nhất có thể (%s°C)." % fmt_c(new_target)
        return summary, changes, before, after

    set_temp(new_target)
    changes.append("nâng lên %s°C" % fmt_c(new_target))
    summary = "Đã nâng điều hòa lên %s°C cho đỡ lạnh." % fmt_c(new_target)

    # A strong draught reads as cold even at a mild setpoint, so ease it too.
    if intensity in ("normal", "very") and fan in ("MID", "HIGH"):
        weaker = fan_step(fan, -1)
        if weaker:
            set_fan(weaker)
            changes.append("giảm quạt xuống %s" % weaker)
            summary = ("Đã nâng lên %s°C và giảm quạt xuống %s cho đỡ lạnh."
                       % (fmt_c(new_target), weaker))
    return summary, changes, before, after


def describe(state):
    """One-line state summary; this is what the model ends up speaking."""
    if state.get("is_on") is None:
        return "chưa đọc được trạng thái"
    if not state.get("is_on"):
        room = state.get("current_temp_c")
        if room is None:
            return "điều hòa đang tắt"
        return "điều hòa đang tắt, phòng %s°C" % fmt_c(room)
    parts = ["điều hòa đang bật"]
    if state.get("job_mode"):
        parts.append("chế độ %s" % state["job_mode"])
    if state.get("target_temp_c") is not None:
        parts.append("đặt %s°C" % fmt_c(state["target_temp_c"]))
    if state.get("current_temp_c") is not None:
        parts.append("phòng %s°C" % fmt_c(state["current_temp_c"]))
    if state.get("wind_strength"):
        parts.append("quạt %s" % state["wind_strength"])
    if state.get("humidity") is not None:
        parts.append("độ ẩm %s%%" % state["humidity"])
    return ", ".join(parts)


# ---- HTTP ----------------------------------------------------------------

class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True


class Handler(BaseHTTPRequestHandler):
    server_version = "jetsona-ac/1.0"
    ac = None  # set in main()

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self):
        if not TOKEN:
            return False  # refuse if no token configured
        header = self.headers.get("X-AC-Token") or ""
        if not header:
            auth = self.headers.get("Authorization", "")
            if auth.lower().startswith("bearer "):
                header = auth[7:].strip()
        if not header:
            query = self.path.split("?", 1)[1] if "?" in self.path else ""
            for pair in query.split("&"):
                if pair.startswith("token="):
                    header = pair[6:]
                    break
        return bool(header) and constant_time_eq(header, TOKEN)

    def _body(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return {}
        if length <= 0:
            return {}
        try:
            data = json.loads(self.rfile.read(length).decode("utf-8", "replace"))
        except ValueError:
            return {}
        return data if isinstance(data, dict) else {}

    def _handle(self, fn):
        """Run one endpoint, mapping the ways LG can fail onto status codes.
        Every message is user-facing: the firmware passes it to the model,
        which reads it aloud."""
        try:
            code, payload = fn()
        except ValueError as exc:                 # bad argument from the caller
            self._send_json(400, {"ok": False, "error": str(exc)})
        except ThinQError as exc:
            detail = exc.body
            if isinstance(detail, dict):
                detail = detail.get("error", detail)
            self._send_json(502, {"ok": False,
                                  "error": "LG ThinQ lỗi %s: %s" % (exc.status, detail)})
        except urllib.error.URLError as exc:
            self._send_json(504, {"ok": False,
                                  "error": "không kết nối được LG ThinQ: %s" % exc.reason})
        except RuntimeError as exc:               # device lookup problems
            self._send_json(502, {"ok": False, "error": str(exc)})
        except Exception as exc:                  # noqa: BLE001 - never 500 silently
            self._send_json(500, {"ok": False, "error": "lỗi nội bộ: %s" % exc})
        else:
            self._send_json(code, payload)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/health":
            self._send_json(200, {"ok": True, "service": "jetsona-ac"})
            return
        if not self._authorized():
            self._send_json(401, {"ok": False, "error": "unauthorized"})
            return
        if path == "/status":
            def run():
                state = self.ac.status()
                out = {"ok": True, "summary": describe(state), "device": self.ac.alias()}
                out.update(state)
                return 200, out
            self._handle(run)
            return
        self._send_json(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if not self._authorized():
            self._send_json(401, {"ok": False, "error": "unauthorized"})
            return
        data = self._body()

        if path == "/power":
            def run():
                if "on" not in data:
                    raise ValueError("thiếu trường 'on' (true/false)")
                on = bool(data["on"])
                self.ac.set_power(on)
                return 200, {"ok": True, "on": on,
                             "summary": "Đã bật điều hòa." if on else "Đã tắt điều hòa."}
            self._handle(run)
            return

        if path == "/temperature":
            def run():
                try:
                    celsius = float(data["celsius"])
                except (KeyError, TypeError, ValueError):
                    raise ValueError("thiếu/sai trường 'celsius' (số %s-%s)"
                                     % (fmt_c(MIN_C), fmt_c(MAX_C)))
                applied = self.ac.set_temperature(celsius)
                return 200, {"ok": True, "celsius": applied,
                             "summary": "Đã đặt điều hòa %s°C." % fmt_c(applied)}
            self._handle(run)
            return

        if path == "/mode":
            def run():
                mode = self.ac.set_job_mode(data.get("mode", ""))
                return 200, {"ok": True, "mode": mode,
                             "summary": "Đã chuyển chế độ %s." % mode}
            self._handle(run)
            return

        if path == "/fan":
            def run():
                speed = self.ac.set_fan_speed(data.get("speed", ""))
                return 200, {"ok": True, "speed": speed,
                             "summary": "Đã đặt quạt %s." % speed}
            self._handle(run)
            return

        if path == "/comfort":
            def run():
                raw = str(data.get("feeling", "")).strip().lower()
                feeling = FEELING_ALIASES.get(raw)
                if not feeling:
                    raise ValueError("không hiểu cảm giác %r (dùng cold/hot/humid/stuffy/ok)" % raw)
                intensity = str(data.get("intensity", "normal")).strip().lower()
                if intensity not in INTENSITY_STEP:
                    intensity = "normal"
                summary, changes, before, after = apply_comfort(self.ac, feeling, intensity)
                log("comfort %s/%s -> %s" % (feeling, intensity, changes or "no change"))
                return 200, {"ok": True, "feeling": feeling, "intensity": intensity,
                             "summary": summary, "changes": changes,
                             "before": before, "state": after,
                             "state_summary": describe(after)}
            self._handle(run)
            return

        self._send_json(404, {"ok": False, "error": "not found"})


def main():
    if not TOKEN:
        die("JETSON_AC_TOKEN is required (set it in /etc/jetsona-ac.env)")
    if not LGE_ACCESS_TOKEN:
        die("LGE_ACCESS_TOKEN is required (LG ThinQ PAT from "
            "https://smartsolution.developer.lge.com)")
    if MIN_C >= MAX_C:
        die("JETSON_AC_MIN_C must be below JETSON_AC_MAX_C")

    try:
        client = ThinQClient()
    except ValueError as exc:
        die(str(exc))

    Handler.ac = AirConditioner(client)

    # Resolve the device once at boot so a misconfigured PAT fails loudly here
    # instead of on the first thing the user says. A cloud hiccup is not fatal:
    # the lookup is retried on demand.
    try:
        Handler.ac.device_id()
    except Exception as exc:  # noqa: BLE001
        log("WARNING: device lookup failed at startup (%s); will retry on first request" % exc)

    server = ThreadingHTTPServer((HOST, PORT), Handler)
    log("listening on %s:%d, region %s/%s, range %s-%s°C"
        % (HOST, PORT, LGE_REGION, LGE_COUNTRY, fmt_c(MIN_C), fmt_c(MAX_C)))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()

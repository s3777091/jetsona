# Jetson Nano DS-02 Firmware

Standalone C++/CMake firmware that boots the **xiaozhi DS-02 home UI** on a
**Jetson Nano 4GB B01** driving a **7" HDMI LCD B (800×480, touch)** panel.

This is **not** an Ubuntu desktop app. It is a self-contained firmware binary
that talks to the panel directly through DRM/KMS (no X server, no compositor),
renders the DS-02 standby/launcher/dock look with LVGL 9.2.2, and reads touch
events via evdev. It boots straight to the UI and can run as a systemd service.

The reference firmware (`xiaozhi-esp32`, ESP-IDF + LVGL 9.5, ILI9341 320×240
SPI panel) has been ported into this folder: ESP-IDF APIs (`esp_timer`,
`esp_lcd`, FreeRTOS event groups/tasks, NVS, `esp_log`) are replaced by thin
Linux shims backed by `std::thread` / `std::condition_variable` / a file-backed
KV store, so the original DS-02 UI code compiles and runs unmodified in spirit.

## Target hardware

| Part | Detail |
|---|---|
| Board | Jetson Nano 4GB B01 (JetPack 4.x / Ubuntu 18.04, aarch64) |
| Display | 7" HDMI LCD B, 800×480, capacitive touch (USB-HID / evdev) |
| Power | Waveshare UPS Module B (5V/5A) — informational, no driver needed |
| Storage | 32GB SD |

## Layout

```
jetson/
  CMakeLists.txt          # top build: options, libdrm/SDL2, vendored LVGL, src/**
  lv_conf.h               # LVGL 9.2.2 config (DRM, evdev, lodepng, gif, tiny_ttf)
  README.md               # this file
  scripts/
    fetch_deps.sh         # apt install build deps + (re)clone LVGL 9.2.2
    install.sh            # install jetson_fw as a systemd boot service
    jetson-fw.service     # systemd unit
  third_party/lvgl/       # vendored LVGL 9.2.2 (cloned by fetch_deps.sh)
  assets/
    fonts/arial.ttf       # fallback font (LVGL built-in). Drop NotoSans-Regular.ttf here for Vietnamese diacritics.
    backgrounds/*.png     # 10 DS-02 wallpaper PNGs (lodepng-decoded)
  src/
    shims/                # esp_log, esp_timer, esp_err, esp_lcd handles, heap_caps, esp_pm, freertos/{task,event_groups,semphr,FreeRTOS}, font_awesome
    platform/             # lvgl_runtime: DRM/SDL display + evdev touch + tick/handler threads
    app/                  # application, board, settings, fonts, system_info, device_state, audio_codec/backlight/led/button stubs
    display/              # ds02_home_display, lcd_display, lvgl_display, lvgl_image, lvgl_theme, lvgl_font, emoji_collection
    main.cc               # entry point (replaces app_main)
```

## Prerequisites (on the Jetson)

```bash
cd jetson
./scripts/fetch_deps.sh
```

This installs `build-essential cmake git pkg-config libdrm-dev libgbm-dev
libegl-dev libgles2-dev libsdl2-dev libsdl2-ttf-dev libcurl4-openssl-dev
libopus-dev libasound2-dev nlohmann-json-dev` and clones LVGL 9.2.2 into
`third_party/lvgl/` if it is not already present.

> The LVGL source is vendored in this repo, so on a fresh clone you can skip the
> network clone by leaving `third_party/lvgl/` in place — `fetch_deps.sh` only
> clones when the directory is missing.

## Build

```bash
bash ./scripts/build.sh
```

Produces `build/jetson_fw`. A POST_BUILD step copies `assets/` next to the
binary so the default relative assets path works when run from `build/`.

`scripts/build.sh` resolves the source directory from its own location, so it
also works when called from outside the repository or when the clone directory
has a different name. For example:

```bash
bash ~/xiaozhi-esp32-server/scripts/build.sh
```

Do not run `cmake .` or `cmake ..` from `~`: that makes CMake search the home
directory instead of this repository and produces the "does not appear to
contain CMakeLists.txt" error.

### CMake options

| Option | Default | Meaning |
|---|---|---|
| `JETSON_DISPLAY_BACKEND` | `DRM` | `DRM` (KMS direct, firmware-style) or `SDL` (fallback window) |
| `JETSON_DISPLAY_WIDTH` | `800` | Panel width |
| `JETSON_DISPLAY_HEIGHT` | `480` | Panel height |
| `JETSON_ASSETS_DIR` | `assets` | Assets dir, relative to CWD or absolute |

## Run

```bash
sudo ./build/jetson_fw          # DRM/KMS needs root or the video/render group
```

You should see the DS-02 home UI on the HDMI panel: gradient wallpaper +
wallpaper PNG, clock (top-right), system bar (Wi-Fi icon + drawn battery +
status), and the bottom dock. Touch works: tap a dock button to cycle
`Dim → Awake → Launcher` (the launcher shows the DS-02 avatar sphere).

### SDL fallback (if Tegra DRM mode-set misbehaves)

```bash
JETSON_DISPLAY_BACKEND=SDL bash ./scripts/build.sh
SDL_VIDEODRIVER=kmsdrm ./build/jetson_fw
```

### Environment variables

| Var | Default | Purpose |
|---|---|---|
| `JETSON_DRM_CARD` | `/dev/dri/card0` | DRM device node |
| `JETSON_TOUCH_DEVICE` | (auto: first usable `/dev/input/eventN`) | Force a specific evdev touch node |
| `JETSON_ASSETS_DIR` | `assets` (compile-time) | Override assets path at runtime |
| `JETSON_SETTINGS_FILE` | `~/.jetson-fw/settings.kv` | Settings KV file (replaces NVS) |

## Install as a boot service

```bash
sudo ./scripts/install.sh
```

Copies `jetson_fw` + `assets/` to `/opt/jetson-fw`, installs
`jetson-fw.service`, and enables it. The unit runs as `root` (DRM access) and
logs to `/var/log/jetson-fw.log`.

```bash
sudo systemctl status jetson-fw
sudo journalctl -u jetson-fw -f      # or: tail -f /var/log/jetson-fw.log
sudo systemctl disable --now jetson-fw   # stop booting to it
```

## Fonts & Vietnamese diacritics

The firmware loads text via LVGL `tiny_ttf` from `assets/fonts/`. It prefers
`NotoSans-Regular.ttf` and falls back to the bundled `arial.ttf`. To render
Vietnamese diacritics cleanly, drop a `NotoSans-Regular.ttf` (or any TTF with
full Vietnamese coverage) into `assets/fonts/` and rebuild — no code change
needed.

## Settings (replaces NVS)

A file-backed KV store (`tab`-separated `namespace.key<TAB>value`) at
`~/.jetson-fw/settings.kv` provides the same `Settings` API the ESP firmware
had (`GetString` / `SetString` / `GetInt` / `GetBool` / `EraseKey` …) with
namespaces. Set `JETSON_SETTINGS_FILE` to relocate it.

## What works now (phase 1 — UI-first scaffold)

- DRM/KMS direct rendering at 800×480 (SDL fallback behind a build switch).
- evdev touch → LVGL pointer indev (DS-02 dock tap + swipe gestures).
- DS-02 home UI: wallpaper (10 PNGs, pre-resized to 800×480 and shown 1:1),
  clock, date, system bar (Wi-Fi icon + drawn battery + status/notification),
  bottom dock (7 PNG icons under `assets/icons/dock/`), and a swipe-up app
  drawer (8 PNG tiles under `assets/icons/drawer/`). The macOS-style glass
  dock uses PNG icons, touch magnification, click bounce, and an active-app
  indicator. Dock taps launch the backed apps (calendar / gallery / WiFi /
  Bluetooth / settings / chat / terminal). 1 Hz clock/status refresh.
- **On-screen WiFi provisioning:** tap the dock button to open
  a WiFi screen that scans networks (`nmcli`), lists them with signal bars, and
  shows an on-screen keyboard for entering the password. Requires a **USB WiFi
  dongle** (the Jetson Nano 4GB B01 has no onboard WiFi) and NetworkManager
  running (`systemctl status NetworkManager`). See `src/net/wifi_manager.*` and
  `src/display/wifi_settings_view.*`.
- **On-screen Bluetooth settings:** tap the globe dock button to open a
  Bluetooth screen that scans devices (`bluetoothctl`/BlueZ), lists them with
  RSSI bars + Paired/Connected state, and pairs + connects on tap (no password
  entry — pairing goes through bluetoothctl's default-agent). Requires a **USB
  Bluetooth dongle** and `bluez` installed (`apt install bluez`). See
  `src/net/bluetooth_manager.*` and `src/display/bluetooth_settings_view.*`.
- **On-screen Terminal:** the launcher app drawer has a "Terminal" tile (tap it)
  that opens an interactive root shell over a pseudo-terminal (`forkpty` +
  `/bin/sh -i`). It runs `sudo`, pipes, redirects, and any program that reads
  stdin (e.g. a `sudo` password prompt) — the firmware service runs as root, so
  every command has full privileges. Output scrolls in a black panel; type a
  command in the input box and press **Gui**/**Enter** (on-screen keyboard) to
  run it. See `src/display/terminal_view.*`. Build note: `forkpty` needs
  `libutil` on older glibc (JetPack 4.x / Ubuntu 18.04); `CMakeLists.txt` links
  it when present.
- Linux shims for the full ESP-IDF API surface the ported code uses.
- File-backed `Settings`, `Board` singleton, `Application` event loop,
  dummy audio codec + network stub.

## What is deferred (later phases)

- **Phase 1b:** drop in the full upstream `ds02_home_display` (calendar,
  settings panel, app drawer, background gallery) with the stubs it needs
  (`WifiManager`, `Lang`, `cbin_font`, embedded `Assets`).
- **Phase 2:** ALSA `AudioCodec` + libopus encode/decode + websocket protocol
  (`libwebsockets`) against the xiaozhi / protexa-agent backend.
- **Phase 3:** wake word (openWakeWord / whisper.cpp), AEC/VAD (WebRTC APM),
  OTA self-update.

## Build verification note

The build host for this source is Windows; the target is aarch64 Linux on the
Jetson, so **the firmware is compiled and verified on the Jetson itself**. If a
build fails on the Jetson, paste the compiler error and it will be fixed in
place — the shim layer and display stack are designed to be iterated against
real Jetson compiler output. The first on-device iteration is expected to be
the full `ds02_home_display` port (phase 1b).

# Jetson Nano DS-02

Giao diện LVGL 9.2 cho Jetson Nano 4GB B01 và màn hình HDMI cảm ứng
800×480. Chương trình hỗ trợ lịch, trình duyệt tệp, Wi-Fi, Bluetooth, cài đặt,
chat và terminal trực tiếp trên màn hình.

## Chạy ngay sau khi build

Khi terminal hiện:

```text
[100%] Built target jetson_fw
==> Built: /home/ekkohuynh/jetsona/build/jetson_fw
```

chạy bản SDL toàn màn hình bằng:

```bash
cd /home/ekkohuynh/jetsona
sudo env SDL_VIDEODRIVER=kmsdrm ./build/jetson_fw
```

Nhấn `Ctrl+C` để dừng. Log ứng dụng, Wi-Fi và Bluetooth được in trực tiếp
trên terminal này.

Nếu đang chạy bên trong Ubuntu Desktop và muốn mở thành cửa sổ:

```bash
cd /home/ekkohuynh/jetsona
./build/jetson_fw
```

## Cài dependency và build

Chỉ cần cài dependency một lần:

```bash
cd /home/ekkohuynh/jetsona
bash scripts/fetch_deps.sh
```

Build SDL, phù hợp để kiểm tra nhanh và chạy qua `kmsdrm`:

```bash
cd /home/ekkohuynh/jetsona
JETSON_DISPLAY_BACKEND=SDL bash scripts/build.sh
```

Binary và assets được tạo tại:

```text
build/jetson_fw
build/assets/
```

### Build bằng backend khác

DRM/KMS trực tiếp:

```bash
JETSON_BUILD_DIR=build-drm JETSON_DISPLAY_BACKEND=DRM bash scripts/build.sh
sudo ./build-drm/jetson_fw
```

Framebuffer `/dev/fb0` cho JetPack/kernel cũ:

```bash
JETSON_BUILD_DIR=build-fbdev JETSON_DISPLAY_BACKEND=FBDEV bash scripts/build.sh
sudo ./build-fbdev/jetson_fw
```

Không dùng chung một build directory cho nhiều backend; dùng `build`,
`build-drm`, `build-fbdev` riêng để tránh CMake cache nhầm cấu hình.

## Phần cứng và dịch vụ cần có

- Jetson Nano 4GB B01.
- Màn hình HDMI 800×480 và USB touch/keyboard/mouse.
- USB Wi-Fi dongle và NetworkManager cho Wi-Fi.
- USB Bluetooth dongle và BlueZ cho Bluetooth.

Jetson Nano không có Wi-Fi/Bluetooth tích hợp. Kiểm tra thiết bị bằng:

```bash
nmcli device
nmcli device wifi list --rescan yes
bluetoothctl show
bluetoothctl devices
```

## Chạy tự động khi khởi động

Script cài đặt lấy binary từ `build/jetson_fw`:

```bash
cd /home/ekkohuynh/jetsona
sudo ./scripts/install.sh
```

Các lệnh quản lý service:

```bash
sudo systemctl status jetson-fw
sudo systemctl restart jetson-fw
tail -f /var/log/jetson-fw.log
sudo systemctl disable --now jetson-fw
```

DRM/FBDEV phù hợp nhất cho systemd. Khi debug lỗi giao diện hoặc quét mạng,
nên dừng service và chạy binary trực tiếp để xem log ngay trên terminal:

```bash
sudo systemctl stop jetson-fw
sudo env SDL_VIDEODRIVER=kmsdrm ./build/jetson_fw
```

## Biến cấu hình thường dùng

| Biến | Công dụng |
|---|---|
| `JETSON_DISPLAY_BACKEND` | Backend lúc build: `SDL`, `DRM` hoặc `FBDEV` |
| `JETSON_BUILD_DIR` | Thư mục build, mặc định là `build` |
| `JETSON_DRM_CARD` | DRM card, mặc định `/dev/dri/card0` |
| `JETSON_FB_DEVICE` | Framebuffer, mặc định `/dev/fb0` |
| `JETSON_TOUCH_DEVICE` | Ép dùng một `/dev/input/eventN` cho touch |
| `JETSON_KEYBOARD_DEVICE` | Ép thiết bị bàn phím evdev |
| `JETSON_MOUSE_DEVICE` | Ép thiết bị chuột evdev |
| `JETSON_FILES_HOME` | Thư mục gốc của ứng dụng Tệp |
| `JETSON_SETTINGS_FILE` | Đường dẫn file lưu cài đặt |

Ví dụ ép touch và thư mục Home:

```bash
sudo env \
  SDL_VIDEODRIVER=kmsdrm \
  JETSON_TOUCH_DEVICE=/dev/input/event3 \
  JETSON_FILES_HOME=/home/ekkohuynh \
  ./build/jetson_fw
```

## Xử lý lỗi nhanh

### Không lên hình

Thử lần lượt SDL/KMSDRM, DRM và FBDEV. Kiểm tra device node:

```bash
ls -l /dev/dri /dev/fb0
```

### Touch không hoạt động

Liệt kê input và xem log dòng `touch:` khi chương trình khởi động:

```bash
ls -l /dev/input/event* /dev/input/by-id/
```

Sau đó chạy lại với `JETSON_TOUCH_DEVICE=/dev/input/eventN`.

### Wi-Fi/Bluetooth không quét hoặc giao diện đứng

Mọi lệnh quét đều chạy nền và có timeout. Xem terminal hoặc
`/var/log/jetson-fw.log` để tìm các tag `Wifi`, `Bt`, `SettingsView` và
`Ds02Home`.

## Thư mục chính

```text
assets/                 font, wallpaper và icon
scripts/build.sh        cấu hình và build
scripts/install.sh      cài systemd service
src/display/            giao diện LVGL
src/net/                Wi-Fi và Bluetooth
src/platform/           runtime LVGL/Linux
third_party/lvgl/       LVGL 9.2.2
```

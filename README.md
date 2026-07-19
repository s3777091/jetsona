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
sudo env SDL_VIDEODRIVER=kmsdrm ./run.sh --sdl
```

Nhấn `Ctrl+C` để dừng. Log ứng dụng, Wi-Fi và Bluetooth được in trực tiếp
trên terminal này.

Nếu đang chạy bên trong Ubuntu Desktop và muốn mở thành cửa sổ:

```bash
cd /home/ekkohuynh/jetsona
./run.sh
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
sudo JETSON_BUILD_DIR=build-drm ./run.sh
```

Framebuffer `/dev/fb0` cho JetPack/kernel cũ:

```bash
JETSON_BUILD_DIR=build-fbdev JETSON_DISPLAY_BACKEND=FBDEV bash scripts/build.sh
sudo JETSON_BUILD_DIR=build-fbdev ./run.sh
```

Không dùng chung một build directory cho nhiều backend; dùng `build`,
`build-drm`, `build-fbdev` riêng để tránh CMake cache nhầm cấu hình.

## Assets tải từ MinIO khi build

Toàn bộ `assets/` (font, wallpaper, icon — ~120 file) **không nằm trong git**
(đã `.gitignore`). Khi build, script tự tải assets từ MinIO (S3-compatible)
nếu thiếu, và **bỏ qua file đã có sẵn** (chỉ tải file chưa có hoặc sai size).
Vì vậy build lại sau lần đầu gần như tốn 0 giây và không tải lại.

Cơ chế này gồm:

```text
config.yaml                  MINIO_ENDPOINT / MINIO_BUCKET / MINIO_REGION
.env                         MINIO_ACCESS_KEY / MINIO_SECRET_KEY (gitignored)
scripts/fetch_assets.sh      nạp config + secret rồi gọi fetch, offline-tolerant
scripts/s3_assets.py         S3/SigV4 client thuần stdlib (không cần mc/aws/boto3)
                             lệnh: fetch | upload | list
CMakeLists.txt               custom target jetson_fetch_assets chạy trước build
scripts/build.sh             gọi fetch_assets.sh trước `cmake --build`
```

Bỏ qua bước fetch (dùng assets đã cache, không mạng) bằng:

```bash
JETSON_SKIP_ASSET_FETCH=1 bash scripts/build.sh
```

Cấu hình MinIO cho máy này / Jetson: chỉnh endpoint/bucket/region trong
`config.yaml`, sau đó copy `.env.example` sang `.env` và chỉ điền
`MINIO_ACCESS_KEY` / `MINIO_SECRET_KEY`. Endpoint mặc định: `https://s3.phuongdong.cloud`
(bucket mặc định `jetsona-assets`, region `us-east-1`). Lần đầu tải assets
về Jetson cần mạng; các lần sau dùng cache nên build được cả khi offline.

Seed lại bucket (chỉ khi thêm/sửa asset rồi đẩy lên MinIO):

```bash
python3 scripts/s3_assets.py upload   # đẩy ./assets -> bucket
python3 scripts/s3_assets.py list     # liệt kê object trong bucket
```

### Font tải theo nhu cầu trong Cài đặt chung

`Cài đặt chung > Phông chữ > Phông chữ khác` đọc manifest
`fonts/cloud/catalog.tsv` từ chính bucket MinIO ở trên. Mỗi dòng là ba cột
phân cách bằng tab:

```text
Tên hiển thị<TAB>Font-Regular.ttf<TAB>Font-Bold.ttf
```

Có mẫu tại `docs/font-catalog.example.tsv`. Đặt manifest và các file `.ttf`
trong `assets/fonts/cloud/`, rồi chạy `python3 scripts/s3_assets.py upload`.
Build bình thường chỉ tải manifest; file font cloud được tải riêng khi người
dùng chạm vào font đó. Firmware không tìm hoặc tải font từ nguồn Internet khác.

Có thể kiểm tra thủ công đúng luồng tải một file bằng:

```bash
python3 scripts/s3_assets.py fetch-file fonts/cloud/Font-Regular.ttf
```

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
sudo env SDL_VIDEODRIVER=kmsdrm ./run.sh --sdl
```

Với bản FBDEV, dùng script độc quyền display. Script tự dừng
`display-manager` và `jetson-fw.service`, sau đó khôi phục đúng display owner
trước đó khi nhấn Ctrl+C:

```bash
sudo bash scripts/run_fbdev.sh
```

## Cấu hình thường dùng

Mọi thiết lập không nhạy cảm nằm trong `config.yaml`; `.env` chỉ chứa API key,
token, password và credential. Biến môi trường truyền trực tiếp khi chạy có độ
ưu tiên cao nhất, nên vẫn có thể dùng để override tạm thời.

| Biến | Công dụng |
|---|---|
| `JETSON_DISPLAY_BACKEND` | Backend lúc build: `SDL`, `DRM` hoặc `FBDEV` |
| `JETSON_BUILD_DIR` | Thư mục build, mặc định là `build` |
| `JETSON_DRM_CARD` | DRM card, mặc định `/dev/dri/card0` |
| `JETSON_FB_DEVICE` | Framebuffer, mặc định `/dev/fb0` |
| `JETSON_TOUCH_DEVICE` | Ép dùng một `/dev/input/eventN` cho touch |
| `JETSON_KEYBOARD_DEVICE` | Ép thiết bị bàn phím evdev |
| `JETSON_MOUSE_DEVICE` | Ép thiết bị chuột evdev |
| `JETSON_CAPTIVE_PORTAL_PROBE_URL` | Endpoint HTTP `204` dùng để nhận biết portal đã xác thực xong |
| `JETSON_CAPTIVE_PORTAL_ONSCREEN_KEYBOARD` | Bàn phím ảo của portal: `auto`, `1` hoặc `0` |
| `JETSON_FILES_HOME` | Thư mục gốc của ứng dụng Tệp |
| `JETSON_SETTINGS_FILE` | Đường dẫn file lưu cài đặt |
| `JETSON_VPN_EXIT_NODE` | Tên máy/IP Tailscale của VM exit node (mặc định `jetsona-vpn`) |
| `RUNPOD_API_KEY` | API key RunPod cho app **Pods**/**Studio** (thuê & quản lý GPU cloud) |
| `JETSON_STUDIO_URL` | code-server trên VM — đích mặc định của tile **Studio** |
| `JETSON_WEATHER_LAT/LON/NAME` | Toạ độ + tên hiển thị cho dòng thời tiết standby (open-meteo, mặc định TP.HCM) |

Ví dụ ép touch và thư mục Home:

```bash
sudo env \
  SDL_VIDEODRIVER=kmsdrm \
  JETSON_TOUCH_DEVICE=/dev/input/event3 \
  JETSON_FILES_HOME=/home/ekkohuynh \
  ./run.sh
```

## Trang đăng nhập Wi-Fi (Captive Portal)

Sau khi nối Wi-Fi, firmware kiểm tra quyền truy cập Internet. Nếu mạng chuyển
hướng sang Captive Portal, mục **Trang đăng nhập Wi-Fi** mở Chromium trong phiên
bare-X. Màn hình cảm ứng hoạt động như chuột; nếu không tìm thấy bàn phím USB,
launcher tự mở bàn phím ảo Onboard để nhập mật khẩu, số điện thoại hoặc OTP.

Sau khi đăng nhập, launcher yêu cầu endpoint kiểm tra trả về HTTP `204` hai lần
liên tiếp rồi tự đóng Chromium. Supervisor sau đó khởi động lại firmware và trả
về giao diện DS-02. Có thể ép luôn hiện bàn phím bằng
`JETSON_CAPTIVE_PORTAL_ONSCREEN_KEYBOARD: "1"`, hoặc tắt bằng `"0"`.

## PS5 Remote Play

Icon **Trò chơi** mở bảng điều khiển PS5 cho panel 800×480. Màn chính chỉ giữ
nút **Chơi ngay**; nút trạng thái PS5 trong bánh răng mở giao diện thiết lập
chính thức của Chiaki. Tại đây có thể liên kết máy và nhập địa chỉ khi cần.
Firmware tự đọc tên/trạng thái đăng ký từ Chiaki, đồng thời cho chọn `540p60`
hoặc `720p30` trước khi kết nối fullscreen.
Khi bắt đầu thiết lập/chơi, firmware dừng hoàn toàn rồi bàn giao framebuffer
cho bare Xorg + `chiaki-ng`; thoát Chiaki sẽ tự quay lại giao diện DS-02.

Xem flow, cách cài binary ARM64, cấu hình PS5 và chẩn đoán tại
[docs/ps-remote-play.md](docs/ps-remote-play.md).

## GPU cloud: Pods → Studio → GitHub

Bộ ba icon trong drawer biến Jetson thành "thin client" code trên GPU thuê:

- **Pods** — quản lý GPU pod trên [RunPod](https://www.runpod.io) qua REST API
  (`https://rest.runpod.io/v1`): xem danh sách pod (trạng thái, GPU, $/giờ),
  bật/tắt, xoá (nhấn thùng rác 2 lần), và **Thuê GPU** mới — chọn preset
  workspace (*VS Code Studio* chạy `code-server`, hoặc *PyTorch + Jupyter*)
  rồi chọn loại GPU với giá theo giờ lấy trực tiếp từ RunPod. Pod thuê từ app
  mở sẵn cổng web IDE + SSH; mật khẩu web IDE mặc định là `jetsona` (xem
  trong chi tiết pod).
- **Studio** — luôn mở **code-server self-host trên VM** (miễn phí, CPU)
  trong Chromium kiosk: deploy một lần bằng `python vm/code-server/deploy.py`
  rồi điền `JETSON_STUDIO_URL` mà script in ra vào `config.yaml`. Đây là chỗ code
  mặc định hằng ngày. Khi cần GPU: thuê pod trong **Pods**, rồi hoặc bấm
  **Mở Studio** trên pod (IDE của pod, URL proxy
  `https://{podId}-{port}.proxy.runpod.net`), hoặc ngay trong Studio VM mở
  terminal và SSH vào pod (lệnh SSH hiện trong sheet chi tiết pod) — code
  nằm một chỗ trên VM/GitHub, GPU chỉ là chỗ chạy.
- **GitHub** — mở `github.com` trong Chromium kiosk. Đăng nhập một lần trong
  kiosk (profile lưu bền ở `/var/lib/jetson-fw/chromium-profile`), sau đó trong Studio
  dùng terminal của code-server để `git clone`/`push` repo của bạn — tức là
  code trực tiếp trên GitHub bằng GPU thuê.

Trong Chromium kiosk, trang web chiếm toàn màn hình và Dynamic Island nổi ở
giữa cạnh trên, tự giãn theo số phiên tab. Chọn **Settings → Ứng dụng → Web
View** để dùng chế độ **Cơ bản** (chỉ icon phiên tab) hoặc **Nâng cao** (thêm
giờ và pin trong island). Chạm island để chuyển app, nhấn giữ để bung hàng
icon app ngang; nhấn `Ctrl+Alt+Backspace` để đóng phiên bare-X và quay lại
firmware. Bare-X cần `xserver-xorg-input-libinput` để nhận bàn phím/chuột,
`x11-xkb-utils` để bật phím thoát và `x11-xserver-utils` để browser user kết nối
an toàn tới Xorg; cài các gói theo
[hướng dẫn PS Remote Play](docs/ps-remote-play.md#cài-runtime-chiaki).

Cấu hình một lần: tạo API key (quyền đọc/ghi pods) tại RunPod Console →
Settings → API Keys, điền `RUNPOD_API_KEY=` vào `.env` rồi cài lại firmware
(service đọc `/opt/jetson-fw/.env`). Chưa có key thì các tile sẽ báo ngay
trên Dynamic Island. Lưu ý: pod **Đã tắt** vẫn tính phí ổ đĩa; xoá pod mới
hết phí hoàn toàn.

## VPN qua Tailscale exit node

Firmware không mở một ứng dụng Tailscale riêng. Trong **Cài đặt**, toggle
**VPN** chọn/bỏ VM exit node; Dynamic Island báo kết quả và top bar hiện chữ
`VPN` khi exit node đang được chọn.

IP public của VM chỉ dùng để SSH. Tailscale yêu cầu VM và Jetson ở cùng một
tailnet, và client phải chọn **tên máy hoặc IP Tailscale `100.x`** của exit
node. Thiết lập một lần như sau:

1. Chép script cấu hình lên VM rồi chạy ở đó (script sẽ mở URL đăng nhập nếu
   không truyền `TS_AUTHKEY`):

   ```bash
   scp scripts/setup-tailscale-exit-node.sh root@36.50.27.142:/root/
   ssh root@36.50.27.142 'bash /root/setup-tailscale-exit-node.sh'
   ```

2. Trong trang Machines của Tailscale Admin, duyệt **Use as exit node** cho
   máy `jetsona-vpn`.

3. Trên Jetson, cài/đăng nhập Tailscale vào cùng tailnet:

   ```bash
   sudo bash scripts/setup-tailscale-client.sh
   ```

   Helper đồng thời bật Tailscale SSH. `scripts/install.sh` cũng tự bật SSH nếu
   node đã đăng nhập. Lệnh `tailscale set --ssh` lưu cấu hình bền trong
   `tailscaled`; service đã được `systemctl enable` nên tự chạy nền và nhận SSH
   trở lại sau mỗi lần Jetson khởi động. Không cần gọi lệnh này trong firmware
   hoặc chạy lại ở mọi lần boot.

4. Đặt `JETSON_VPN_EXIT_NODE: "jetsona-vpn"` trong `config.yaml`, cài lại firmware và
   bật toggle VPN. Có thể thay bằng IP Tailscale `100.x` nếu không dùng
   MagicDNS.

Nếu tailnet dùng access policy tùy chỉnh, policy phải cấp quyền tới
`autogroup:internet`; chỉ cấp quyền truy cập trực tiếp vào máy exit node là
chưa đủ để định tuyến Internet qua máy đó. Sau khi cài firmware, có thể chạy
lại helper và kiểm tra từ Jetson bằng:

```bash
sudo /opt/jetson-fw/scripts/setup-tailscale-client.sh
tailscale status
tailscale exit-node list
tailscale ping --c=1 --until-direct=false jetsona-vpn
journalctl -u jetson-fw -n 100 | grep -i vpn
```

Script cấu hình VM có thể chạy lại an toàn sau khi duyệt exit node. Lỗi tối ưu
UDP GRO trên card mạng ảo chỉ làm giảm thông lượng và không còn làm dừng toàn
bộ quá trình cấu hình.

Không lưu mật khẩu SSH hoặc Tailscale auth key trong repository. Nếu dùng
`TS_AUTHKEY`, chỉ truyền nó qua biến môi trường khi chạy script.

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
assets/                 font, wallpaper và icon (gitignored; tải từ MinIO khi build)
scripts/build.sh        cấu hình và build (gọi fetch_assets.sh trước)
scripts/fetch_assets.sh tải assets từ MinIO, bỏ qua file đã có
scripts/s3_assets.py    S3 client thuần stdlib (fetch/upload/list)
scripts/install.sh      cài systemd service
src/display/            giao diện LVGL
src/net/                Wi-Fi và Bluetooth
src/platform/           runtime LVGL/Linux
third_party/lvgl/       LVGL 9.2.2
```

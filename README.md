# Jetson Nano DS-02

Giao diện LVGL 9.2 cho Jetson Nano 4GB B01 và màn hình HDMI cảm ứng
800×480. Chương trình hỗ trợ lịch, trình duyệt tệp, Wi-Fi, Bluetooth, cài đặt,
chat trực tiếp trên màn hình.

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

Upload/sync bucket bằng `uv` trên máy phát triển (build trên Jetson không cần
`uv`):

```bash
uv run --script scripts/s3_assets.py upload
uv run --script scripts/s3_assets.py upload-file icons/drawer/my-icon.png
uv run --script scripts/s3_assets.py delete-file icons/drawer/old-icon.png
uv run --script scripts/s3_assets.py list
```

Tren Jetson, cap nhat va cai dat bang mot chuoi fail-fast (install chi chay khi
pull, S3 sync va build deu thanh cong):

```bash
cd ~/jetsona
git pull --ff-only && bash scripts/build.sh && sudo bash scripts/install.sh
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
sudo journalctl -u jetson-fw -f
sudo systemctl disable --now jetson-fw
```

DRM/FBDEV phù hợp nhất cho systemd. Khi debug lỗi giao diện hoặc quét mạng,
nên dừng service và chạy binary trực tiếp để xem log ngay trên terminal:

```bash
sudo systemctl stop jetson-fw
sudo bash scripts/run_headless.sh
```

Ekko Lite không có display: chỉ cần chạy binary, không cần dừng display-manager
hay giành scanout. `run_headless.sh` nạp config/secrets rồi chạy binary ở
foreground -- Ctrl+C để thoát.

## Điều hòa (LG ThinQ)

Agent điều khiển điều hòa LG trong phòng qua tool `air_conditioner`. Tool không
gọi thẳng cloud LG mà gọi một dịch vụ cầu nối chạy ngay trên Jetson:

```
"Ekko, lạnh quá"
  -> firmware: tool air_conditioner {action:"comfort", feeling:"cold"}
     -> POST http://127.0.0.1:46003/comfort   (header X-AC-Token)
        -> scripts/jetsona_ac.py -> LG ThinQ Connect API -> điều hòa
```

Cầu nối nằm trên Jetson chứ không phải trên PC vì Jetson là máy chạy 24/7 —
PC phần lớn thời gian đang tắt (đó là lý do có Wake-on-LAN), nếu treo điều
khiển điều hòa vào PC thì đúng lúc cần nhất (nửa đêm, đang nằm trên giường)
lại không dùng được.

`jetsona_ac.py` là bản port của project `IOT/` (`config.py`, `lge_thinq.py`,
`ac.py`) sang Python 3.6 có sẵn trên Jetson: `requests`/`Flask`/`dotenv` được
thay bằng `urllib` + `http.server` nên không cần cài pip. Enum điều khiển
(`COOL`/`AIR_DRY`/`FAN`, `LOW`/`MID`/`HIGH`/`AUTO`) giữ nguyên theo device
profile của model RAC_056905_WW — đổi máy thì sửa cả hai nơi.

### Cài đặt

Credential LG đã có sẵn trong `.env` của project `IOT/`, nên cách nhanh nhất là
chép file đó sang Jetson rồi trỏ script cài vào nó:

```bash
scp C:/Users/ADMIN/workspace/IOT/.env ekkohuynh@192.168.50.96:~/iot-lge.env
```

```bash
sudo bash scripts/jetsona_ac_install.sh ~/iot-lge.env
```

Script đọc `LGE_ACCESS_TOKEN`/`LGE_REGION`/`LGE_COUNTRY`/`LGE_CLIENT_ID`/
`LGE_DEVICE_ID` từ file đó. Nếu muốn nhập tay (hoặc PAT mới, lấy tại
<https://smartsolution.developer.lge.com> → Cloud Developer → ThinQ Connect →
PAT, scope: view devices, view statuses, control devices):

```bash
sudo LGE_ACCESS_TOKEN=<PAT-cua-ban> bash scripts/jetsona_ac_install.sh
```

Script cài `/usr/local/lib/jetsona/jetsona_ac.py`, ghi `/etc/jetsona-ac.env`
(chmod 600, chứa PAT), bật `jetsona-ac.service`, rồi tự thử `/status` một lần.
Lần đầu chạy nó sinh và **in ra** `JETSON_AC_TOKEN` — chép vào `.env` của
firmware (`~/jetsona/.env` và `/opt/jetson-fw/.env`) rồi
`sudo systemctl restart jetson-fw`. Chạy lại script (không cần tham số) sẽ tái
dùng cả PAT lẫn token cũ nên không làm lệch secret hai bên.

Xoá file `~/iot-lge.env` sau khi cài xong — PAT đã nằm trong
`/etc/jetsona-ac.env` với quyền 600, không cần bản thứ hai để lộ ra.

Kiểm tra bằng tay:

```bash
curl -s -H "X-AC-Token: $JETSON_AC_TOKEN" http://127.0.0.1:46003/status
```

```bash
curl -s -X POST -H "X-AC-Token: $JETSON_AC_TOKEN" -H 'Content-Type: application/json' \
  -d '{"feeling":"cold","intensity":"normal"}' http://127.0.0.1:46003/comfort
```

Log: `journalctl -u jetsona-ac -f`.

### Vì sao "thấy lạnh" được xử lý ở cầu nối

"Tôi thấy lạnh" không phải một con số. Muốn dịch nó thành lệnh phải biết máy
đang ở chế độ gì, đặt bao nhiêu độ, quạt mức nào — nên `/comfort` tự
đọc-quyết-định-ghi trong một lượt thay vì bắt model gọi `status` rồi tự suy
luận (tốn thêm một vòng và model nhỏ chạy on-device hay suy luận sai).

Máy này **không có sưởi** (chỉ COOL/AIR_DRY/FAN), nên "lạnh quá" nghĩa là *làm
lạnh ít lại*, theo thang cố định:

| Tình huống | Xử lý |
|---|---|
| Đang làm lạnh | Nâng nhiệt độ đặt (+1/+2/+3°C theo `intensity`), quạt đang MID/HIGH thì hạ một nấc |
| Đã ở 30°C mà vẫn lạnh | Chuyển sang `FAN` — ngừng làm lạnh hẳn |
| Đang chạy `FAN` | Hạ quạt một nấc; đã ở `LOW` thì tắt máy |
| Máy đang tắt | Không đổi gì, báo lại là máy đang tắt (không phải nó gây lạnh) |

Chiều ngược lại (`feeling: "hot"`) đối xứng: máy tắt thì bật `COOL` ở 25°C;
đang chạy thì hạ nhiệt độ đặt; chạm đáy 16°C thì tăng quạt. Ngoài ra có
`humid` → `AIR_DRY` và `stuffy` → `FAN`.

## Cấu hình thường dùng

Mọi thiết lập không nhạy cảm nằm trong `config.yaml`; `.env` chỉ chứa API key,
token, password và credential. Biến môi trường truyền trực tiếp khi chạy có độ
ưu tiên cao nhất, nên vẫn có thể dùng để override tạm thời.

| Biến | Công dụng |
|---|---|
| `JETSON_BUILD_DIR` | Thư mục build, mặc định là `build` |
| `JETSON_SETTINGS_FILE` | Đường dẫn file lưu cài đặt |
| `JETSON_WEATHER_LAT` / `JETSON_WEATHER_LON` / `JETSON_WEATHER_NAME` | Cố định địa điểm thời tiết (Đà Nẵng). Để trống = tự đo theo IP |
| `JETSON_MUSIC_PLAYER` | Binary phát nhạc, mặc định `mpv` |
| `JETSON_MUSIC_ALBUMS_FILE` | File lưu album nhạc của người dùng |
| `JETSON_WEATHER_LAT/LON/NAME` | Toạ độ + tên hiển thị cho dòng thời tiết standby (open-meteo, mặc định TP.HCM) |
| `JETSON_AC_URL` | Base URL của cầu nối điều hòa, mặc định `http://127.0.0.1:46003` |
| `JETSON_AC_TOKEN` | Secret gửi kèm header `X-AC-Token` (trong `.env`, phải khớp `/etc/jetsona-ac.env`) |

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
hướng sang Captive Portal,Dynamic Island thông báo để bạn đăng nhập qua thiết
bị khác (điện thoại/laptop) trên cùng mạng — bản lite không còn kiosk Chromium
trên panel nữa. Endpoint kiểm tra trả về HTTP `204` xác nhận portal đã xác
thực xong.

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
scripts/jetsona_ac.py   cầu nối HTTP tới điều hòa LG ThinQ (port 46003)
scripts/jetsona_ac_install.sh  cài jetsona-ac.service + /etc/jetsona-ac.env
src/display/            giao diện LVGL
src/net/                Wi-Fi và Bluetooth
src/platform/           runtime LVGL/Linux
third_party/lvgl/       LVGL 9.2.2
```

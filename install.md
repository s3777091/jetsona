# Hướng dẫn cài đặt — Jetson Nano DS-02 Firmware

Cài đặt từ đầu tới cuối: tải gì, flash thẻ, build, chạy, cài kiosk.
Làm theo đúng thứ tự 6 bước.

---

## A. TẢI GÝ (trên PC Windows)

### 1. balenaEtcher — ghi image JetPack vào thẻ SD (bản Windows, không cần cài)
- Tải: https://github.com/balena-io/etcher/releases/tag/v2.1.6
- Chọn file **`balenaEtcher-win32-x64-2.1.6.zip`** → tải về, giải nén, chạy `balenaEtcher.exe`.

### 2. JetPack image cho Jetson Nano 4GB (hệ điều hành — Ubuntu 18.04 + driver NVIDIA)
- Trang chính thức: https://developer.nvidia.com/embedded/learn/get-started-jetson-nano-devkit#prepare
- Cuộn xuống mục **"Prepare for setup" → "Write Image to the microSD Card"** → bấm link **"Download the SD card image"**.
- Chọn đúng: **Jetson Nano 4GB (B01)**. **Không** chọn bản 2GB.
- Cần tài khoản NVIDIA (miễn phí) — đăng ký 1 lần nếu chưa có.
- Ra file zip ~600MB (ví dụ `jp46-...-sd-card-image.zip`). **Không cần giải nén**, Etcher đọc được luôn.
- Trang thay thế nếu link trên khó: https://developer.nvidia.com/jetpack-sdk-464 → mục **"SD Card Image Method" → "Jetson Nano Developer Kits" → "Download the SD Card Image"**.

> JetPack 4.6.x là bản cuối cùng hỗ trợ Jetson Nano (L4T 32.7.x, Ubuntu 18.04, CUDA 10.2). Thẻ 32GB là đủ.

### Lưu ý phần cứng cho bước setup
- Jetson Nano 4GB B01 **không có WiFi onboard**. Cần **cáp mạng (Ethernet)** cắm router, hoặc **cục USB WiFi**.
- Cần **bàn phím + chuột USB** và tạm thời 1 **màn hình HDMI to** (dùng cái 7" cũng được nhưng gõ setup qua touch rất mệt → khuyên dùng màn to + phím thật cho lần đầu).

---

## B. FLASH THẺ SD (1 lần, trên PC Windows)

1. Cắm thẻ microSD 32GB vào đầu đọc thẻ → cắm PC.
2. Mở `balenaEtcher.exe`:
   - **Flash from file** → chọn file zip JetPack (không cần giải nén).
   - **Select target** → chọn thẻ microSD (~30GB, cẩn thận chọn đúng ổ).
   - **Flash** → đợi ~10 phút. Xong có thông báo "Flash Complete".

---

## C. BOOT JETSON LẦN ĐẦU + SETUP

1. Rút thẻ ra, cắm vào khe microSD của Jetson.
2. Cắm: màn hình HDMI + bàn phím USB + chuột USB + cáp mạng (hoặc USB WiFi).
3. Cắm nguồn 5V/4A (qua jack hoặc qua UPS module B). Jetson tự bật.
4. Boot vào Ubuntu lần đầu (~1-2 phút) → màn hình setup:
   - Chọn ngôn ngữ, timezone (Asia/Ho_Chi_Minh).
   - Tạo **username + password** (ghi nhớ, ví dụ `jetson` / mật khẩu).
   - Chọn "NVIDIA maximized performance" hoặc "default" đều được.
5. Vào desktop → mở **Terminal** (Ctrl+Alt+T) → test internet:
```bash
ping -c 3 8.8.8.8
```
Ra kết quả → có mạng, chuyển bước D. Lỗi → kiểm tra cáp mạng/USB WiFi.

---

## D. COPY CODE `jetson/` TỪ PC WINDOWS VÀO JETSON

Cách dễ nhất: nén folder rồi copy qua USB.

**Trên PC Windows** (Git Bash):
```bash
cd /c/Users/ekko.huynh/esp32-ai-design
tar czf jetson.tar.gz jetson
```
→ sinh ra `C:\Users\ekko.huynh\esp32-ai-design\jetson.tar.gz`.

Copy `jetson.tar.gz` sang USB → cắm USB vào Jetson → trên Jetson terminal:
```bash
mkdir -p ~/work
cp /media/<username>/<usb-label>/jetson.tar.gz ~/work/   # thay đường dẫn USB thật
cd ~/work
tar xzf jetson.tar.gz
cd jetson
ls        # phải thấy CMakeLists.txt, src/, assets/, scripts/, third_party/
```

> Nếu PC và Jetson cùng mạng thì dùng `scp` nhanh hơn — chạy `ip a` trên Jetson lấy IP rồi báo để có lệnh exact.

---

## E. BUILD TRÊN JETSON (cần internet)

Trong folder `~/work/jetson`:
```bash
chmod +x scripts/*.sh
./scripts/fetch_deps.sh        # cài thư viện build + clone LVGL (~5-10 phút)
mkdir -p build && cd build
cmake .. -DJETSON_DISPLAY_BACKEND=DRM
make -j4                       # biên dịch (~3-5 phút)
```

Xong có file `~/work/jetson/build/jetson_fw`. **Paste bất kỳ lỗi compiler nào ra để sửa.**

---

## F. CHẠY THỬ + CÀI TỰ-CHẠY-KHI-BẬT (kiosk mode)

**Chạy thử** (chưa cài, chỉ xem UI có lên không):
```bash
cd ~/work/jetson/build
sudo ./jetson_fw
```
→ Màn hình HDMI hiện UI DS-02 (wallpaper + đồng hồ + dock). Touch thử nút dock. Ctrl+C để thoát.

**Cài thành "firmware tự chạy khi bật"**:
```bash
cd ~/work/jetson
sudo ./scripts/install.sh                       # cài service vào /opt/jetson-fw
sudo systemctl set-default multi-user.target    # tắt desktop, boot thẳng console
```

Xong. **Rút nguồn cắm lại → Jetson tự chạy UI DS-02**, không còn desktop.

Quản lý service:
```bash
sudo systemctl status jetson-fw
sudo journalctl -u jetson-fw -f                 # hoặc: tail -f /var/log/jetson-fw.log
sudo systemctl disable --now jetson-fw          # dừng
sudo systemctl set-default graphical.target     # về desktop để debug (reboot sau)
```

Rebuild khi sửa code: build lại rồi `sudo ./scripts/install.sh` lần nữa.

---

## Tóm tắt thứ tự

1. Tải balenaEtcher + image JetPack Nano 4GB.
2. Flash thẻ bằng Etcher.
3. Boot Jetson + setup (màn to + phím + mạng).
4. Nén + copy `jetson/` sang Jetson qua USB.
5. `fetch_deps.sh` → `cmake` → `make`.
6. `sudo ./jetson_fw` chạy thử → `install.sh` cài kiosk.

---

## Sources

- balenaEtcher releases: https://github.com/balena-io/etcher/releases/tag/v2.1.6
- Getting Started with Jetson Nano Developer Kit: https://developer.nvidia.com/embedded/learn/get-started-jetson-nano-devkit#prepare
- JetPack SDK 4.6.4: https://developer.nvidia.com/jetpack-sdk-464
- How to Install JetPack 4.6.1 (docs): https://archive.docs.nvidia.com/jetson/jetpack/4.6.1/install-jetpack/index.html
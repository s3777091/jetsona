# PS5 Remote Play trên Jetson Nano

Ứng dụng **Trò chơi** trong app drawer là một bảng điều khiển LVGL dành riêng
cho màn hình 800×480. Nó không chạy game trên Jetson. PS5 mã hóa hình ảnh và
âm thanh; Jetson nhận luồng, giải mã, hiển thị và gửi thao tác tay cầm ngược về
PS5 bằng `chiaki-ng`.

## Luồng sử dụng

1. Trên PS5, bật `Settings > System > Remote Play > Enable Remote Play`.
2. Nếu muốn đánh thức máy từ Rest Mode, bật cả `Stay Connected to the Internet`
   và `Enable Turning on PS5 from Network` trong `Power Saving`.
3. Mở app drawer, chọn icon **Trò chơi**, bấm bánh răng rồi bấm biểu tượng
   trạng thái trên dòng **Tên PS5**. Firmware nhường hẳn framebuffer cho Xorg
   và mở giao diện thiết lập chính thức của `chiaki-ng`.
4. Nhập địa chỉ PS5 và hoàn tất liên kết trong Chiaki. Trên PS5, mở
   `Remote Play > Link Device`, rồi nhập mã hiển thị vào Chiaki.
5. Thoát Chiaki để quay lại DS-02. Firmware sẽ đọc tên máy và trạng thái liên
   kết từ Chiaki. Mở lại **Trò chơi** rồi chọn **Chơi ngay**.

Địa chỉ IPv4 của PS5 nằm tại
`Settings > System > System Software > Console Information`. Nên đặt DHCP
reservation trên router để địa chỉ này không thay đổi.

## Hai cấu hình cho màn hình 800×480

| Cấu hình | Stream | Codec | Bitrate | Khi nên dùng |
|---|---:|---|---:|---|
| Mượt | 960×540, 60 FPS | H.264 | 8 Mbit/s | game hành động, ưu tiên phản hồi |
| Rõ | 1280×720, 30 FPS | H.264 | 10 Mbit/s | game chậm, ưu tiên chi tiết |

Chiaki chạy fullscreen và giữ đúng tỉ lệ 16:9. Trên panel 800×480, vùng hình
là 800×450, còn khoảng 15 px viền đen ở trên và dưới. Không kéo giãn nên hình
không bị méo.

## Cài runtime Chiaki

Phần firmware không đóng gói binary Chiaki. Cài một bản ARM64 tương thích
JetPack đang dùng, rồi đặt binary tại một trong các vị trí được tự nhận diện:

```text
/opt/chiaki-ng/chiaki-ng.AppImage
/opt/chiaki-ng/chiaki-ng
chiaki-ng trong PATH
chiaki trong PATH
```

Các thiết lập không nhạy cảm nằm trong `/opt/jetson-fw/config.yaml`:

```yaml
CHIAKI_BIN: "/opt/chiaki-ng/chiaki-ng.AppImage"
PS_REMOTE_PLAY_AUDIO_DRIVER: "alsa"
PS_REMOTE_PLAY_HW_DECODER: "software"
PS_REMOTE_PLAY_RENDER_BACKEND: "vulkan"
PS_REMOTE_PLAY_MAX_CLOCKS: "1"
```

Các gói nền cần cho chế độ bare-X:

```bash
sudo apt update
sudo apt install -y xinit xserver-xorg-video-all xserver-xorg-input-libinput \
   x11-xkb-utils x11-xserver-utils xdotool openbox onboard dbus libnss3 \
   libfuse2 libva2 alsa-utils
```

Trong màn hình thiết lập, launcher giữ con trỏ X hiện, chạy `openbox` để cửa
sổ Chiaki/Chromium luôn nhận focus bàn phím, và tự mở bàn phím ảo `onboard`
khi không phát hiện bàn phím USB. Có thể cưỡng bức bàn phím ảo bằng
`PS_REMOTE_PLAY_ONSCREEN_KEYBOARD: "1"`, hoặc tắt bằng `"0"`. Trình duyệt PSN
chạy dưới user sandbox `jetson-kiosk`, dùng cùng profile đăng nhập với Chromium
kiosk; đổi user bằng `PS_REMOTE_PLAY_BROWSER_USER` nếu cần.

Sau khi Sony trả về URL `remoteplay/redirect`, launcher mặc định tự chuyển URL
đó vào dialog Chiaki và đóng Chromium. Có thể tắt hành vi này bằng
`PS_REMOTE_PLAY_PSN_REDIRECT_AUTO: "0"` để quay lại quy trình copy/paste thủ
công. URL chứa mã đăng nhập chỉ được giữ trong bộ nhớ trong lúc chuyển tiếp,
không được ghi vào log hoặc file.

`chiaki-ng` hiện phát hành AppImage ARM64 và yêu cầu Vulkan 1.2, `libva` cùng
PipeWire. JetPack 4.6.1 có Vulkan 1.2, nhưng Ubuntu 18.04/glibc cũ và backend
giải mã Tegra khiến không thể đảm bảo mọi AppImage mới đều chạy. Ubuntu 18.04
cũng không có sẵn PipeWire hiện đại; nếu AppImage báo thiếu thư viện, cần một
runtime/backport tương thích hoặc binary được build cho JetPack. Hãy thử binary
ngay trên Jetson trước khi cài kiosk:

```bash
SDL_AUDIODRIVER=alsa QT_QPA_PLATFORM=offscreen \
  /opt/chiaki-ng/chiaki-ng.AppImage --help
```

Nếu báo thiếu phiên bản `GLIBC`, cần một AppImage được build trên nền cũ hơn
hoặc tự build tương thích JetPack; launcher không thể sửa ABI này. AppImage
ARM64 upstream không tích hợp backend `nvv4l2decoder` đặc thù của Jetson, nên
launcher mặc định dùng decoder phần mềm để tránh chọn nhầm VAAPI/Vulkan Video.
Chỉ đặt `PS_REMOTE_PLAY_HW_DECODER=auto` sau khi đã xác nhận custom build và
driver thực sự khởi tạo được. Tên preset **Mượt** là mục tiêu thử nghiệm, không
phải cam kết 60 FPS; hãy theo dõi CPU/nhiệt độ và đổi sang 720p30 nếu phù hợp
hơn với binary thực tế.

## Tối ưu tài nguyên

Firmware và Chiaki không render đồng thời:

```text
app Trò chơi -> dừng thread LVGL -> thoát firmware (43/44)
             -> bare Xorg + duy nhất chiaki-ng
             -> thoát Chiaki -> khởi động lại firmware LVGL
```

Như vậy RAM/CPU/GPU của Chromium, desktop shell và firmware không cạnh tranh
với phiên stream. Khi `PS_REMOTE_PLAY_MAX_CLOCKS=1`, launcher dùng
`jetson_clocks` theo kiểu best-effort trong thời gian chơi và khôi phục khi
Chiaki thoát. Lệnh vẫn nằm trong power mode hiện tại; cần tản nhiệt và nguồn
5 V ổn định.

Ưu tiên cắm Ethernet cho cả Jetson và PS5. Wi-Fi vẫn dùng được, nhưng packet
loss/jitter thường ảnh hưởng độ trễ nhiều hơn bitrate danh nghĩa.

## Dữ liệu và chẩn đoán

- Cấu hình UI: `/var/lib/jetson-fw/ps-remote-play.conf`.
- Dữ liệu đăng ký Chiaki: `/var/lib/jetson-fw/chiaki/.config/Chiaki/`.
- Log launcher: `/var/log/jetson-fw.log`.
- File cấu hình được giới hạn quyền đọc; helper không in registration key hay
  passcode ra status/log.

Kiểm tra bằng CLI:

```bash
sudo /opt/jetson-fw/scripts/ps_remote_play_ctl.sh status
sudo /opt/jetson-fw/scripts/ps_remote_play_ctl.sh probe --host 192.168.1.50
tail -f /var/log/jetson-fw.log
```

Tài liệu upstream:

- https://streetpea.github.io/chiaki-ng/setup/configuration/
- https://streetpea.github.io/chiaki-ng/setup/automation/
- https://github.com/streetpea/chiaki-ng/releases
- https://forums.developer.nvidia.com/t/announcing-jetpack-4-6-1-release-with-l4t-32-7-1/205680

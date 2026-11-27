# Script Linux cho SAEMS

Bản Linux (Arch/Ubuntu) của các script trong `scripts/windows/` (bản Windows giữ nguyên).
Chạy từ thư mục gốc repo. Luồng demo đầy đủ: [RUN-DEMO.md](RUN-DEMO.md).

| Script | Thay cho (Windows) | Việc |
|---|---|---|
| `run_device.sh` | `run_device.ps1` | Build/flash firmware, đọc log |
| `run_server.sh` | `run_server.ps1` | Kiểm tra server, xem dữ liệu, tắt/bật broker |
| `run_app.sh` | `run_app.ps1` | Build/cài/chạy app, xem phiên bản trên điện thoại |
| `phone_network.sh` | `phone_ap_test.ps1`, `phone_ble_test.ps1` | Đổi mạng điện thoại cho demo AP/BLE |
| `device_ap.sh` | `power_mode.ps1`, `soil_calibration.ps1` | REST API của ESP32 qua Wi-Fi AP |
| `common.sh` | | Hàm dùng chung (không chạy trực tiếp) |

## Firmware

### Cài một lần

```bash
# ESP-IDF 6.0.3 (script tìm ở $IDF_PATH, mặc định ~/esp/esp-idf-v6.0.3)
git clone --depth 1 -b v6.0.3 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.3
~/esp/esp-idf-v6.0.3/install.sh esp32s3

# Quyền cổng serial (Arch: uucp, Debian/Ubuntu: dialout), đăng nhập lại sau đó
sudo usermod -aG "$(stat -c %G /dev/ttyACM0)" "$USER"
```

Thông tin bí mật nằm trong `.local/` (đã bị git bỏ qua), không bao giờ in ra:

| File | Nội dung | Cách có |
|---|---|---|
| `.local/wifi.json` | `{"ssid": "...", "password": "..."}` | Script hỏi lần đầu rồi lưu |
| `.local/device-access.json` | Tài khoản MQTT, `device_id` | `scp admin@<pi>:saems-mini-server/device-access.json .local/` |
| `.local/firmware-ap-access.json` | Mật khẩu Wi-Fi AP của thiết bị | Tự sinh nếu chưa có |

### Lệnh

```bash
scripts/linux/run_device.sh build                     # build, profile Direct
scripts/linux/run_device.sh flash -p /dev/ttyACM0     # build + flash, giữ NVS
scripts/linux/run_device.sh read --seconds 60         # đọc log, lưu .local/device-read.log
scripts/linux/run_device.sh monitor                   # idf.py monitor (Ctrl+] để thoát)
```

Tùy chọn: `--profile direct|light|battery` (mode mặc định khi NVS chưa có),
`--no-soil-raw`, `--full-backup` (sao lưu đủ 16 MB trước khi flash), cổng mặc định
lấy từ `$SAEMS_PORT` hoặc `/dev/ttyACM0`. Broker MQTT mặc định
`mqtts://admin.tailb1f0ba.ts.net:8443`, đổi bằng `SAEMS_MQTT_BROKER_URI`.

Khi flash, script đọc bảng phân vùng trên board. Board còn layout cũ (NVS 24 KB)
chỉ bị xóa vùng mở rộng `0xF000..0x39000`; NVS cũ, mốc hiệu chuẩn và dữ liệu
offline được giữ nguyên.

## App Flutter

Cần Flutter 3.47.7 (Dart 3.13.5) ở `~/flutter` (hoặc trong `PATH`, hoặc biến `FLUTTER`) và
Android SDK ở `~/Android/Sdk` (biến `ANDROID_HOME`):

```bash
curl -LO https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_3.47.7-stable.tar.xz
tar -xJf flutter_linux_3.47.7-stable.tar.xz -C ~

scripts/linux/run_app.sh info        # app trên điện thoại cài lúc nào, so với lần sửa code cuối
scripts/linux/run_app.sh install     # build APK debug + cài qua ADB
scripts/linux/run_app.sh run         # chạy kèm log Flutter
```

APK build trên máy này ký bằng khóa debug của máy này. Nếu điện thoại đang có app build từ máy
khác, Android từ chối cập nhật: dùng `install --reinstall` (gỡ app cũ, mất cài đặt trong app).

## Server

`run_server.sh` đọc API token từ `.local/device-access.json` và SSH vào Pi qua Tailscale
(`SAEMS_PI`, mặc định `admin@100.104.200.110`; cần SSH key đã cài trên Pi).

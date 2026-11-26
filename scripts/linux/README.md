# Script Linux cho thiết bị SAEMS

Tương đương `scripts/windows/run_device.ps1`, chạy từ thư mục gốc repo.

## Cài một lần

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

## Lệnh

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

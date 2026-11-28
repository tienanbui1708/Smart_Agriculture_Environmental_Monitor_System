# Cài đặt sau khi clone repo

Cần cài gì tùy phần định làm. Phần nào không đụng tới thì bỏ qua. Mọi lệnh chạy từ
**thư mục gốc repo**. Chi tiết từng script Linux: [scripts/linux/README.md](scripts/linux/README.md);
kịch bản demo: [scripts/linux/RUN-DEMO.md](scripts/linux/RUN-DEMO.md).

| Muốn làm | Cần mục |
|---|---|
| Chỉ xem app chạy thử, không có board | 2 (build `--mock`) |
| Nạp firmware cho board | 1 + 5 |
| Build app dùng server thật | 2 + 5 + mật khẩu admin |
| Kiểm tra / vận hành server từ xa | 3 + 6 |
| Sửa code và chạy test | 4 |

## 1. Firmware ESP32-S3

| Cài gì | Ghi chú |
|---|---|
| ESP-IDF **6.0.3** | Script tìm ở `$IDF_PATH`, mặc định `~/esp/esp-idf-v6.0.3` |
| git, python3 | Thường có sẵn |
| Quyền cổng serial | Arch: nhóm `uucp`; Debian/Ubuntu: nhóm `dialout`; đăng nhập lại sau khi thêm |

```bash
git clone --depth 1 -b v6.0.3 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.3
~/esp/esp-idf-v6.0.3/install.sh esp32s3
sudo usermod -aG "$(stat -c %G /dev/ttyACM0)" "$USER"   # cắm board trước

scripts/linux/run_device.sh build
scripts/linux/run_device.sh flash        # giữ nguyên NVS
scripts/linux/run_device.sh monitor      # thoát: Ctrl+]
```

Windows: extension ESP-IDF cho VS Code và các script trong `scripts/windows/`.

## 2. App Flutter (Android)

| Cài gì | Ghi chú |
|---|---|
| Flutter **3.47.7** (Dart 3.13.5) | Ở `~/flutter`, trong `PATH`, hoặc chỉ bằng biến `FLUTTER` |
| Android SDK + platform-tools (adb) | Ở `~/Android/Sdk` hoặc biến `ANDROID_HOME`; cài qua Android Studio hoặc command-line tools |
| JDK 17 trở lên | |
| Điện thoại | Bật USB debugging |

```bash
curl -LO https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_3.47.7-stable.tar.xz
tar -xJf flutter_linux_3.47.7-stable.tar.xz -C ~

scripts/linux/run_app.sh install          # server thật (cần mật khẩu admin để đăng nhập)
scripts/linux/run_app.sh install --mock   # chế độ Mô phỏng, không cần file bí mật
```

Lần build đầu tự chạy `flutter pub get`. Điện thoại đang có app build từ máy khác thì
Android từ chối cập nhật: dùng `install --reinstall` (mất cài đặt trong app).
Build thẳng bằng `flutter` mà không truyền `--dart-define` thì app mở ở chế độ Mô phỏng.

## 3. Server

Chạy server (Pi hoặc máy Linux khác): Python 3, Mosquitto và

```bash
cd services/mini-server
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt   # fastapi, uvicorn, paho-mqtt, python-dotenv
```

Hướng dẫn dựng đầy đủ: [services/mini-server/README.md](services/mini-server/README.md).

Kiểm tra server đang chạy từ máy mình (`scripts/linux/run_server.sh`): `curl`, `ssh`,
và quyền truy cập ở mục 6.

## 4. Test và phân tích tĩnh (không bắt buộc)

| Phần | Cần | Lệnh |
|---|---|---|
| Firmware host test | `gcc` | `cd firmware/esp32s3 && python tests/run_host_tests.py` |
| Phân tích tĩnh | `cppcheck` ≥ 2.14 (addon MISRA), `clang-tidy` | `cd firmware/esp32s3 && python tools/static_analysis.py` |
| Server | Python 3 + package ở mục 3 (thiếu thì test báo `No module named 'paho'`) | `cd services/mini-server && python -m unittest discover -s tests -v` |
| App | Flutter (mục 2) | `cd mobile/saems_app && flutter test` |

## 5. File bí mật không có trên git

`.gitignore` loại `.local/`, `sdkconfig` và `.env`. Xin chủ repo gửi riêng (không qua chat
công khai, không commit):

| File | Nội dung | Cách có |
|---|---|---|
| `.local/device-access.json` | Tài khoản MQTT của ESP32, `device_id`, API token | Chủ repo gửi, hoặc `scp admin@<pi>:saems-mini-server/device-access.json .local/` |
| `.local/wifi.json` | SSID và mật khẩu Wi-Fi | Script hỏi ở lần chạy đầu rồi lưu |
| `.local/firmware-ap-access.json` | Mật khẩu Wi-Fi AP của ESP32 | Tự sinh nếu chưa có; muốn dùng board hiện tại thì xin file của chủ repo |
| `.env` (trên server) | Mật khẩu MQTT, token Telegram, hash mật khẩu admin | Chỉ cần khi dựng server riêng (`scripts/prepare_runtime.py`) |

Mật khẩu admin để đăng nhập app cũng xin chủ repo.

## 6. Quyền truy cập Pi

App và ESP32 đi qua Tailscale Funnel (`https://admin.tailb1f0ba.ts.net`) là địa chỉ công khai,
**không cần Tailscale**. Chỉ SSH vào Pi (`run_server.sh logs|db|services|gaps|broker-*`) mới cần:

1. Cài Tailscale và đăng nhập bằng **tài khoản của chính mình**. Chủ tailnet chia sẻ máy Pi cho
   tài khoản đó (Tailscale admin → Machines → Pi → Share), hoặc mời vào tailnet.
2. Gửi public key SSH (`~/.ssh/id_ed25519.pub`) cho chủ repo để thêm vào
   `~/.ssh/authorized_keys` của user `admin` trên Pi.
3. Thử: `ssh admin@100.104.200.110` rồi `scripts/linux/run_server.sh services`.

Không dùng chung tài khoản Google/Tailscale của chủ tailnet.

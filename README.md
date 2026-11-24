# SAEMS

SAEMS là viết tắt của **Smart Agriculture Environmental Monitoring System**. Hệ thống
gồm ba phần:

- Thiết bị ESP32-S3: đo nhiệt độ, độ ẩm, độ ẩm đất, ánh sáng; tưới tự động; lưu
  offline.
- Mini-server trên Raspberry Pi: MQTT, REST API, cảnh báo Telegram.
- Ứng dụng Flutter: xem dữ liệu qua server hoặc kết nối trực tiếp BLE/Wi-Fi AP.

Ba phần dùng chung một hợp đồng dữ liệu ([`docs/api-contract.md`](docs/api-contract.md)).

```text
firmware/esp32s3/       Firmware ESP-IDF 6.0.3 cho ESP32-S3-DevKitC N16R8
services/mini-server/   MQTT ingestor, SQLite, REST API, đăng nhập, cảnh báo, dashboard
mobile/saems_app/       Ứng dụng Flutter (server, BLE, Wi-Fi AP)
scripts/windows/        Build/flash/monitor, chạy server và app trên Windows
scripts/acceptance/     Bài test Giai đoạn 4 và tính năng lượng
docs/                   Hợp đồng API, giao thức, thiết kế, báo cáo kiểm thử
legacy/root-firmware/   Firmware baseline cũ của nhóm (lưu trữ, không bảo trì)
```

## Chức năng chính

| Nhóm | Nội dung | Tài liệu |
|---|---|---|
| Cảm biến | DHT20, BH1750 (I²C), LM393 đã hiệu chuẩn (dry 4095 / wet 2080) | [firmware](firmware/esp32s3/README.md) |
| Ngưỡng và cảnh báo | Ngưỡng có hysteresis trên thiết bị và server, cấu hình từ xa (MQTT retained), cảnh báo Telegram, báo mất kết nối | [UPDATE-PLAN GĐ A](docs/UPDATE-PLAN.md) |
| Máy bơm và pin | Tưới tự động theo xung, ngắt an toàn, đo pin ADC/mô phỏng, tiết kiệm điện khi pin yếu | [pump-battery.md](docs/pump-battery.md) |
| Offline | Vòng ghi 6 MB trên flash (~6.100 bản ghi), gửi bù theo lô, server chống trùng | [offline-storage.md](docs/offline-storage.md) |
| Kết nối trực tiếp | BLE GATT (passkey) và REST trên Wi-Fi AP (token) | [direct-connection-protocol.md](docs/direct-connection-protocol.md) |
| Năng lượng | Direct / Light Sleep / Deep Sleep đổi được lúc chạy | [power-management-demo.md](docs/power-management-demo.md), [energy-measurement.md](docs/energy-measurement.md) |
| LED trạng thái | WS2812 GPIO48: online, mất server, BLE/AP, lỗi cảm biến, gửi bù | [firmware](firmware/esp32s3/README.md) |
| Bảo mật | Wi-Fi WPA2, AP token, BLE passkey, đăng nhập server, TLS MQTT/HTTPS | [security.md](docs/security.md) |
| Chất lượng code | MISRA C:2012 (cppcheck) + clang-tidy, host test, test server/app | [misra-deviations.md](docs/misra-deviations.md) |
| Lưu trữ server | SQLite có index, gộp theo giờ, retention | [data-storage.md](docs/data-storage.md) |

Tiến độ và quyết định theo từng giai đoạn: [docs/UPDATE-PLAN.md](docs/UPDATE-PLAN.md).

## Chạy nhanh

Mọi script Windows chạy từ thư mục gốc repo:

```powershell
Set-Location 'E:\Code\C++\ESP32S3_SAEMS\Smart_Agriculture_Environmental_Monitor_System\merge_github'  # đổi theo nơi clone repo
.\scripts\windows\run_device.cmd -Action Flash -Port COM3     # build + flash (giữ NVS)
.\scripts\windows\run_device.cmd -Action Read -Seconds 60     # đọc log thiết bị
.\scripts\windows\run_server.ps1 -Action Watch                # theo dõi server
.\scripts\windows\run_app.ps1 -Action Run -Target Pixel6      # chạy app
```

Chi tiết: [scripts/windows/RUN-DEMO.md](scripts/windows/RUN-DEMO.md).

## Kiểm thử

```bash
cd firmware/esp32s3 && python tests/run_host_tests.py          # 5 bộ host test
python tools/static_analysis.py --build-dir <build> --report ../../docs/static-analysis-report.md
cd services/mini-server && python -m unittest discover -s tests # server
cd mobile/saems_app && flutter analyze && flutter test          # app
python -m unittest discover -s scripts/acceptance               # phân tích Giai đoạn 4
```

## Tài liệu khác

- [Báo cáo hiện thực Device – Server – App](docs/IMPLEMENTATION-REPORT.md)
- [Mini-server Raspberry Pi](services/mini-server/README.md)
- [Mobile app Flutter](mobile/saems_app/README.md)
- [Triển khai Internet và provisioning](docs/internet-and-provisioning.md)
- [Hiệu chuẩn LM393](docs/lm393-build-calibration-demo.md)
- Biên bản nghiệm thu các đợt trước:
  [firmware mới](docs/new-firmware-device-server-app-acceptance.md),
  [2026-10-04](docs/hardware-app-acceptance-2026-10-04.md)

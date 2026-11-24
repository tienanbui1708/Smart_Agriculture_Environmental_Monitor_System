# Kế hoạch bổ sung SAEMS theo PRD

Tài liệu theo dõi việc hoàn thiện project so với đề bài (tab **Specification** trong
Google Doc nhóm). Mỗi giai đoạn là một commit riêng trên nhánh
`feature/prd-completion`; firmware phải build đạt và test phải chạy đạt trước khi
commit. Phần cần board thật (flash, đo dòng, test Giai đoạn 4) có script/hướng dẫn
để chạy tay.

Ký hiệu: `[x]` xong · `[~]` đang làm · `[ ]` chưa làm · 🔧 cần phần cứng thật.

## Quyết định đã chốt (2026-11-07)

| Chủ đề | Quyết định |
|---|---|
| Kênh cảnh báo | Telegram bot (`TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID` trong `.env` của server) |
| Board | ESP32-S3-DevKitC N16R8 (thegioiic) |
| LED trạng thái | LED RGB WS2812 onboard, GPIO48 (cấu hình được trong Kconfig) |
| ADC đo pin | GPIO6 (ADC1_CH5) qua cầu chia áp |
| Đăng nhập | Làm trong đợt này: server `POST /auth/login` cấp token, app có màn hình đăng nhập |
| Soil LM393 | Đã hiệu chuẩn thật: dry = 4095, wet = 2080 (lưu NVS qua `soil_calibration.ps1`) |
| Pin và máy bơm | Chưa có phần cứng, nhưng vẫn phải có kịch bản đầy đủ và chế độ mô phỏng |

## Tổng quan tiến độ

| GĐ | Nội dung | Mục PRD | Trạng thái |
|---|---|---|---|
| A | Ngưỡng, cấu hình từ xa, cảnh báo Telegram | Output Server "Cảnh báo", Input Device "cập nhật ngưỡng" | `[x]` code · 🔧 chờ nghiệm thu board/Pi |
| B | Kịch bản máy bơm và pin (thật + mô phỏng) | Battery Level, Relay bơm | `[x]` code · 🔧 chờ board |
| C | Lưu offline bằng LittleFS ring buffer | Offline Resilience "không mất dữ liệu" | `[ ]` |
| D | LED trạng thái WS2812 | "LED chỉ thị mạng/server" | `[ ]` |
| E | Bảo mật và đăng nhập | Bảo mật cơ bản, App "đăng nhập" | `[ ]` |
| F | Chất lượng code, test, bằng chứng nghiệm thu | MISRA/best practice, Giai đoạn 4, năng lượng | `[ ]` |
| G | Dọn repo và tài liệu | Quản lý source | `[ ]` |

---

## GĐ A — Ngưỡng, cấu hình từ xa, cảnh báo

**Mục tiêu:** người dùng đặt ngưỡng và chu kỳ lấy mẫu từ app (qua server hoặc kết nối
trực tiếp); thiết bị tự đánh dấu cảnh báo kể cả khi offline; server gửi Telegram khi
vượt ngưỡng hoặc thiết bị mất kết nối.

### Hợp đồng dữ liệu

- Thứ tự ngưỡng dùng chung (mảng 9 phần tử, `null` = tắt):
  `[temp_min, temp_max, hum_min, hum_max, soil_min, soil_max, light_min, light_max, battery_min]`.
  Dạng mảng giữ cấu hình BLE dưới giới hạn đọc 512 byte của ATT.
- `alarm_mask` trong telemetry, bit theo đúng thứ tự trên:
  bit0 nhiệt thấp, bit1 nhiệt cao, bit2 ẩm thấp, bit3 ẩm cao, bit4 đất khô,
  bit5 đất quá ướt, bit6 thiếu sáng, bit7 quá sáng, bit8 pin yếu.
- Hysteresis (vào cảnh báo khi vượt ngưỡng, thoát khi quay lại trong ngưỡng một
  khoảng): nhiệt 0.5 °C, ẩm 2 %, đất 2 %, ánh sáng max(10 lux, 5 %), pin 2 %.
- `config_version`: server tăng mỗi lần lưu; thiết bị báo lại trong telemetry để
  server biết cấu hình đã được áp dụng.
- MQTT `saems/{device_id}/config` (QoS 1, retained). Thiết bị chỉ nhận các khóa an
  toàn qua kênh này: `config_version`, `telemetry_interval_s`, `power_mode`,
  `thresholds`. Wi-Fi/MQTT/AP/soil chỉ đổi được qua BLE/AP.
- Server là nguồn chuẩn khi online: cấu hình đổi qua BLE/AP đặt `config_version = 0`;
  khi thiết bị nối lại server, cấu hình retained trên server được áp dụng lại.

### Firmware

- [x] Component `threshold_monitor` (C thuần, test được trên máy tính): đánh giá ngưỡng
  có hysteresis, sinh `alarm_mask`.
- [x] `config_manager`: lưu ngưỡng (NVS blob `thresholds`) và `cfg_ver`; đọc/ghi mảng
  `thresholds`; `config_manager_update_remote_json` chỉ nhận khóa an toàn
  (khóa khác → `ESP_ERR_NOT_ALLOWED`).
- [x] `mqtt_manager`: subscribe `saems/{id}/config`, bỏ qua bản tin bị phân mảnh.
- [x] `data_manager`: telemetry thêm `config_version` và `alarm_mask`.
- [x] `application_manager`: đánh giá ngưỡng mỗi chu kỳ, giữ hysteresis qua Deep Sleep
  (RTC), log khi mask đổi.
- [x] Host test `tests/test_threshold_monitor.c` (29 checks).
- [x] BLE: đọc characteristic > 512 byte trả lỗi thay vì JSON cắt cụt; firmware gửi
  telemetry dạng frame ngay khi app subscribe; app chịu lỗi đọc và dùng frame.
  (Chuyển từ GĐ B lên vì telemetry mới có thể chạm giới hạn 512 byte.)
- [x] Build ESP-IDF 6.0.3 đạt, không warning; binary 0x142bc0 (còn 58 % app slot).

### Server

- [x] `schema.py`: tạo bảng `device_config`, `alerts`, `alert_state` dùng chung cho API và ingestor.
- [x] `GET/PUT /api/v1/devices/{id}/configuration`: validate, tăng version, publish retained;
  chỉ commit DB sau khi broker xác nhận.
- [x] `alert_engine.py`: đánh giá ngưỡng trên server có hysteresis, lưu bảng `alerts`,
  không gửi Telegram từng bản ghi gửi bù cũ hơn 10 phút.
- [x] Cảnh báo thiết bị offline/online lại (mặc định quá `max(3 × chu kỳ + 60 s, 300 s)`,
  900 s khi chưa đặt chu kỳ).
- [x] `notifier.py`: gửi Telegram qua hàng đợi nền, thử lại khi lỗi, không log token.
- [x] `GET /api/v1/alerts`, `GET /api/v1/devices/{id}/alerts`, `POST /api/v1/notifications/test`.
- [x] ACL Mosquitto mẫu: thiết bị đọc `saems/{id}/config`, server ghi `saems/+/config`.
- [x] Unit test: 14 test mới (cấu hình, alert engine, API với broker giả) + 3 test cũ đạt.
- [x] Triển khai lên Pi (2026-11-24): sao lưu `/home/admin/backups/saems-20261124-180131`,
  chép code commit `e788e0c`, thêm 2 dòng ACL `config`, restart 4 service; Pi chạy 13 test
  (4 test API skip vì Pi không có httpx). `POST /notifications/test` gửi Telegram thành công;
  offline monitor tạo cảnh báo mất kết nối cho `farm-01` (im lặng từ 2026-10-10).
- [x] Hiển thị thời gian mất kết nối dạng ngày/giờ và giờ Việt Nam ("45 ngày", "3 giờ 5 phút"); đã triển khai Pi 2026-11-24.

### App

- [x] Model `ThresholdConfig`, `ServerConfiguration`, `DeviceAlert`; `Telemetry` đọc
  `alarm_mask`, `config_version`.
- [x] Màn hình **Ngưỡng & chu kỳ** (Thiết bị → Ngưỡng & chu kỳ lấy mẫu): lưu qua Server
  (PUT) hoặc BLE/AP (ghi trực tiếp), hiển thị trạng thái đồng bộ.
- [x] Màn hình **Cảnh báo** (Thiết bị → Cảnh báo): danh sách từ server; khi kết nối trực
  tiếp hiển thị cờ từ `alarm_mask`.
- [x] Dashboard hiển thị chip cảnh báo đang hoạt động.
- [x] `flutter analyze` sạch; 29 test đạt (8 test mới).

### Nghiệm thu GĐ A 🔧

1. Tạo bot bằng @BotFather, lấy token; nhắn bot một lần rồi lấy `chat_id`
   (hướng dẫn chi tiết trong `services/mini-server/README.md`).
2. Đặt `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID` trong `.env` trên Pi, restart service.
3. `POST /api/v1/notifications/test` → nhận tin Telegram.
4. App → Thiết bị → Ngưỡng: đặt `temp_max` thấp hơn nhiệt độ phòng → nhận Telegram,
   telemetry có `alarm_mask` bit1, app hiện chip cảnh báo.
5. Trả ngưỡng về bình thường → nhận tin "đã trở lại bình thường".
6. Rút nguồn ESP32 → sau thời gian offline nhận cảnh báo mất kết nối; cắm lại → tin online.

---

## GĐ B — Kịch bản máy bơm và pin

Chi tiết nối dây, cấu hình và kịch bản thử: [`pump-battery.md`](pump-battery.md).

- [x] Component `irrigation` (C thuần): tưới theo xung — bật khi đất dưới ngưỡng, bơm tối
  đa `pump_max_runtime_sec`, dừng sớm khi đất đạt ngưỡng + 5 %, nghỉ `pump_min_rest_sec`
  (tính qua Deep Sleep bằng đồng hồ RTC). Không tưới khi cảm biến đất lỗi/chưa hiệu chuẩn.
- [x] Cấu hình thiết bị dạng mảng gọn `"irrigation":[bật, %, s, s]` qua BLE/AP/MQTT
  (NVS blob `irrigation`); cấu hình BLE tối đa 495/512 byte. Đổi qua BLE/AP đặt
  `config_version = 0` như ngưỡng.
- [x] Lệnh bơm tay giữ nguyên; telemetry có `pump_reason` (`auto` / `manual` /
  `safety_off` / `low_battery`). Trạng thái bơm có mutex chung cho timer, lệnh và tưới tự động.
- [x] Kconfig `SAEMS_PUMP_SIMULATED` (mặc định bật): vẫn đóng relay, log `[simulated pump]`.
- [x] Component `battery_monitor`: nguồn `none` / `adc` (GPIO6, cầu chia ×2, hiệu chuẩn
  curve fitting, đường xả LiPo, dùng chung handle ADC1 với cảm biến đất) / `simulated`
  (giữ trong RTC, lệnh `battery_sim`). Telemetry có `battery_pct`, `battery_source`, `battery_state`.
- [x] Pin yếu (< 20 %, trễ 3 %) → tự vào Battery/Deep Sleep không ghi đè NVS, chu kỳ ≥ 600 s;
  pin cạn (< 5 %) → tắt bơm, khóa lệnh bật bơm, Deep Sleep 3600 s. Battery mode chờ lần bơm
  đang chạy xong rồi mới ngủ.
- [x] Đánh giá ngưỡng `battery_min` trên thiết bị (`alarm_mask` bit8) và trên server (Telegram).
- [x] Host test `tests/test_pump_battery.c` (269 checks); build ESP-IDF đạt cả 3 cấu hình
  pin (none / simulated / adc), không warning, binary ~0x144000 (58 % trống).
- [x] Server: `auto_watering` trong `/configuration` (publish `irrigation`),
  `POST /commands/battery-sim`, API trả thêm `battery_source`, `battery_state`, `pump_reason`.
  21 test đạt.
- [x] App: mục **Tưới tự động** trong Ngưỡng & chu kỳ (Server có ô "Server quản lý"),
  dashboard hiện lý do bơm và thẻ pin (đánh dấu "mô phỏng"). `flutter analyze` sạch, 34 test.
- [x] Tài liệu nối dây relay/bơm và cầu chia áp, `api-contract.md`, `direct-connection-protocol.md`.

### Nghiệm thu GĐ B 🔧

Theo mục 4 của [`pump-battery.md`](pump-battery.md): tưới tự động, dừng sớm, ngắt an toàn,
pin yếu, pin cạn, hồi pin — chạy với `SAEMS_BATTERY_SOURCE_SIMULATED=y`.

## GĐ C — Lưu offline bền hơn

- [ ] Phân vùng LittleFS 2–4 MB sau `coredump` (giữ nguyên NVS/OTA). 🔧 cần `flash` toàn bộ một lần.
- [ ] Ring buffer khoảng 5.000 bản ghi, khi đầy bỏ bản **cũ nhất**, đếm số bản bị bỏ.
- [ ] Chuyển bản ghi đang chờ từ NVS sang LittleFS ở lần boot đầu.
- [ ] Gửi bù theo lô; server nhận lô và khử trùng.
- [ ] Test ring buffer trên máy tính.

## GĐ D — LED trạng thái

- [ ] Driver WS2812 GPIO48 (`led_strip`), Kconfig chọn GPIO/tắt LED.
- [ ] Xanh lá online · vàng nhấp nháy mất server · xanh dương BLE/AP · đỏ lỗi cảm biến ·
  tím đang gửi bù · tắt khi ngủ.

## GĐ E — Bảo mật và đăng nhập

- [ ] Firmware từ chối Wi-Fi mở (tối thiểu WPA2).
- [ ] REST cục bộ chỉ trên AP và yêu cầu token thiết bị.
- [ ] BLE pairing bằng passkey (Kconfig) thay Just Works.
- [ ] Server `POST /auth/login` (user/password trong `.env`, mật khẩu băm), cấp token có hạn.
- [ ] App màn hình đăng nhập, lưu token trong FlutterSecureStorage, đăng xuất.

## GĐ F — Chất lượng code và bằng chứng

- [ ] Script `cppcheck --addon=misra` + clang-tidy, sửa vi phạm chính, ghi deviation.
- [ ] Host test cho `data_manager`, ring buffer, ngưỡng, pin, logic bơm.
- [ ] Test server: cấu hình, cảnh báo, nhận lô.
- [ ] Script tự động bài test Giai đoạn 4, xuất báo cáo kiểm tra liên tục `seq`. 🔧
- [ ] Mẫu báo cáo đo dòng Direct/Light/Deep Sleep. 🔧
- [ ] SQLite: index, retention, gộp theo giờ; ghi lý do chọn SQLite thay time-series DB.

## GĐ G — Dọn repo và tài liệu

- [ ] Chuyển firmware cũ ở gốc repo (`components/`, `main/`, `CMakeLists.txt`) vào `legacy/`.
- [ ] Kconfig mặc định bật soil, dry 4095 / wet 2080.
- [ ] Cập nhật README, `current-progress.md`, `api-contract.md`.
- [ ] Sửa đường dẫn cũ `C:\Users\Tien An\esp\...` trong tài liệu.

---

## Nhật ký

| Ngày | Nội dung |
|---|---|
| 2026-11-07 | Tạo plan, chốt quyết định, bắt đầu GĐ A trên nhánh `feature/prd-completion`. |
| 2026-11-07 | GĐ A xong phần code: firmware build đạt, host test 29 checks, server 17 test, app 29 test. Chờ flash board và triển khai Pi để nghiệm thu. |
| 2026-11-24 | Cài SSH key laptop → Pi. Triển khai server GĐ A lên Pi, Telegram hoạt động. Còn: flash ESP32, cài app lên máy Android. |
| 2026-11-24 | Cài app GĐ A lên Samsung A24 và Pixel 6 (sửa lỗi Kotlin incremental do khác ổ đĩa). Push nhánh lên GitHub. Board đã tháo, chờ lắp lại. |
| 2026-11-24 | GĐ B xong phần code (firmware, server, app, tài liệu); server GĐ B triển khai lên Pi. Chờ board để nghiệm thu GĐ A + B. |

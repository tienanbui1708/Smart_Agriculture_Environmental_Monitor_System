# RUN DEMO SAEMS trên Linux: Device → Server → App

Bản Linux của [`scripts/windows/RUN-DEMO.md`](../windows/RUN-DEMO.md). Mọi lệnh chạy từ
**thư mục gốc repo**. Cài đặt công cụ và chuẩn bị `.local/`: [README.md](README.md).

| Script | Việc |
|---|---|
| `run_device.sh` | Build/flash firmware, đọc log ESP32 |
| `run_server.sh` | Kiểm tra server, xem dữ liệu, tắt/bật broker giả lập mất server |
| `run_app.sh` | Build/cài/chạy app Flutter, xem phiên bản app trên điện thoại |
| `phone_network.sh` | Đổi mạng điện thoại cho demo Wi-Fi AP / BLE và khôi phục |
| `device_ap.sh` | Gọi REST API của ESP32 qua Wi-Fi AP: đổi power mode, hiệu chuẩn đất |

## 0. Trước khi demo

```bash
scripts/linux/run_server.sh services     # 5 service phải "active"
scripts/linux/run_server.sh check        # server ok + bản ghi mới nhất
scripts/linux/run_app.sh info            # app trên điện thoại có mới hơn lần sửa code cuối?
```

- ESP32 cắm USB (`/dev/ttyACM0`), điện thoại cắm USB và bật USB debugging.
- App cũ hơn code: `scripts/linux/run_app.sh install` (thêm `--reinstall` nếu app được
  cài từ máy khác; cài đặt trong app sẽ mất).
- Firmware chưa phải bản mới: `scripts/linux/run_device.sh flash`.
- Ngắt tải máy bơm khỏi relay khi chưa nghiệm thu relay.

Mở 2 terminal và để chạy suốt buổi demo:

```bash
scripts/linux/run_device.sh monitor      # T1: log ESP32 (thoát: Ctrl+])
scripts/linux/run_server.sh watch        # T2: mỗi bản ghi mới về server một dòng
```

## 1. Online: ESP32 → MQTT TLS → Pi → HTTPS → App

1. App: đăng nhập `admin` (mật khẩu đặt trên Pi bằng `auth.py set-password`).
2. Settings → nguồn **Server**; URL và `farm-01` đã có sẵn trong bản build.
3. Dashboard hiện nhiệt độ, độ ẩm, ánh sáng; `data_mode = real`.

Đạt khi số trên app khớp dòng mới nhất ở T2, `seq` ở T1 và T2 tăng đều, LED ESP32 **xanh lá**.

## 2. Wi-Fi AP trực tiếp (không cần server)

```bash
scripts/linux/phone_network.sh ap        # tắt mobile data, nối điện thoại vào SAEMS-farm-01
```

App: **Kết nối trực tiếp → Dùng Wi-Fi trực tiếp**, nhập mã thiết bị (= mật khẩu AP trong
`.local/firmware-ap-access.json`), quay lại Dashboard làm mới, rồi **Đọc cấu hình thiết bị**.
Đạt khi app đọc được telemetry/cấu hình từ `192.168.4.1` lúc mobile data tắt; LED **xanh dương**.
Không gửi lệnh bật relay.

```bash
scripts/linux/phone_network.sh restore
```

## 3. BLE trực tiếp

```bash
scripts/linux/phone_network.sh ble       # tắt Wi-Fi + mobile data; giữ Bluetooth bật
```

App: **Kết nối trực tiếp → Quét ESP32 → SAEMS-farm-01**. Android hỏi passkey 6 số: số này
in trong log T1 (tìm `passkey`). Đạt khi Dashboard có telemetry lúc Wi-Fi và mobile data đều tắt.

```bash
scripts/linux/phone_network.sh restore
```

## 4. Bài test Giai đoạn 4 của PRD: mất server → offline → gửi bù

```bash
scripts/linux/run_server.sh broker-stop  # giả lập server sập
```

1. T1: ESP32 vẫn lấy mẫu, `pending` tăng; LED **vàng nhấp nháy**. T2 báo không có bản ghi mới.
2. Trong lúc server tắt, điện thoại xem dữ liệu trực tiếp (mục 2 hoặc 3).
3. Sau 2–3 chu kỳ:

```bash
scripts/linux/run_server.sh broker-start
```

4. T1: MQTT nối lại, `SYNC_MANAGER: Sync ... pending=` giảm về 0; LED **tím** rồi **xanh lá**.
5. Kiểm tra không mất bản ghi (cột `missing` phải là 0):

```bash
scripts/linux/run_server.sh gaps
```

Bản có báo cáo tự động (ĐẠT/KHÔNG ĐẠT, ghi Markdown):
`python3 scripts/acceptance/stage4_resilience.py --ssh admin@100.104.200.110 --offline-minutes 3`.

## 5. Cảnh báo Telegram

App: **Thiết bị → Ngưỡng & chu kỳ**, đặt `temp_max` thấp hơn nhiệt độ phòng → Telegram báo,
Dashboard có chip cảnh báo, T2 cột `alarm` khác 0. Trả ngưỡng về cũ → tin "đã trở lại bình thường".

## 6. Đổi chế độ năng lượng (không flash lại)

Qua app (BLE/AP) hoặc từ laptop đã nối Wi-Fi `SAEMS-farm-01`:

```bash
nmcli dev wifi connect SAEMS-farm-01 --ask
scripts/linux/device_ap.sh power                # đọc mode hiện tại
scripts/linux/device_ap.sh power light          # Light Sleep
scripts/linux/device_ap.sh power battery 60     # Deep Sleep, chu kỳ 60 s
scripts/linux/device_ap.sh power direct 10      # trở lại Direct
```

Mode áp dụng ở chu kỳ lấy mẫu kế tiếp, T1 in dòng `Power profile`. Ở Battery, ESP32 chỉ thức
trong cửa sổ ngắn; gọi lệnh lúc nó đang thức, và ngắt BLE để nó được ngủ.

## 7. Hiệu chuẩn cảm biến đất (khi có que đo hoạt động)

```bash
scripts/linux/device_ap.sh soil                       # đọc soil_raw hiện tại
scripts/linux/device_ap.sh soil-calibrate 4095 2080   # mốc khô / ướt đo thật
```

Nhấn EN/reset ESP32 sau khi lưu. Không cần flash lại.

## Lỗi thường gặp

| Hiện tượng | Cách xử lý |
|---|---|
| `No permission on /dev/ttyACM0` | `sudo usermod -aG uucp $USER` rồi đăng nhập lại (Ubuntu: nhóm `dialout`) |
| `No Android phone over ADB` | Bật USB debugging, rút cắm cáp, chấp nhận hộp thoại trên điện thoại |
| `signatures do not match` khi cài app | App cũ cài từ máy khác: `run_app.sh install --reinstall` |
| `API watch failed` | Server tắt (đúng lúc đang test mục 4) hoặc token trong `.local/device-access.json` sai |
| `not on the ESP32 AP` | Laptop phải nối Wi-Fi `SAEMS-farm-01` trước khi dùng `device_ap.sh` |
| Độ ẩm đất luôn 0 %, `soil_raw=4095` | Module/dây LM393 hỏng hoặc AO không vào GPIO4 |

## Tiêu chí kết luận

- Online: ESP32 thật → MQTT TLS → Pi/SQLite → HTTPS → app.
- Wi-Fi AP và BLE: app đọc telemetry/cấu hình khi không có Internet.
- Offline: ESP32 không treo, `pending` tăng khi mất server, về 0 sau khi nối lại, `gaps` báo `missing = 0`.
- Cảnh báo: Telegram khi vượt ngưỡng và khi trở lại bình thường.
- Runtime: đổi `direct ↔ light ↔ battery` mà không flash lại.

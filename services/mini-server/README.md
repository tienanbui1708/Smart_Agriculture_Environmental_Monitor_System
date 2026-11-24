# SAEMS mini-server

Mini-server nhận telemetry MQTT từ ESP32-S3, kiểm tra schema, lưu SQLite và cung
cấp REST API/dashboard cho ứng dụng. Bản triển khai hiện tại chạy trên Raspberry
Pi 4 tại `/home/admin/saems-mini-server`.

## Kiến trúc đang chạy

```text
ESP32-S3
  └─ MQTT TLS :8443
      └─ Mosquitto (saems-mqtt, origin loopback :1884)
          └─ mqtt_ingestor.py (saems-ingestor)
              └─ SQLite data/telemetry.db
                  └─ FastAPI api.py (saems-api, loopback :3000)
                      └─ Tailscale Funnel HTTPS
                          ├─ Android app
                          └─ Web dashboard
```

Endpoint công khai hiện dùng:

- HTTPS API/dashboard: `https://admin.tailb1f0ba.ts.net`
- MQTT TLS: `mqtts://admin.tailb1f0ba.ts.net:8443`

Bốn service là `saems-mqtt`, `saems-ingestor`, `saems-api` và
`saems-legacy-ingestor`. Service legacy chỉ đọc topic firmware cũ để tương
thích; firmware mới publish trực tiếp vào broker TLS SAEMS.

## Thành phần mã nguồn

| Thành phần | Vai trò |
| --- | --- |
| `mqtt_ingestor.py` | Subscribe telemetry, validate, deduplicate và insert SQLite |
| `legacy_mqtt_ingestor.py` | Chuyển envelope topic cũ sang schema chung |
| `api.py` | API thiết bị, telemetry, command, cấu hình, cảnh báo và dashboard |
| `device_config.py` | Validate cấu hình ngưỡng/chu kỳ, chuyển sang mảng cho firmware |
| `alert_engine.py` | Đánh giá ngưỡng có hysteresis, cảnh báo offline/online |
| `notifier.py` | Gửi Telegram qua hàng đợi nền, có thử lại |
| `schema.py` | Bảng `device_config`, `alerts`, `alert_state` |
| `mqtt_security.py` | TLS/certificate cho MQTT client |
| `dashboard/index.html` | Dashboard không phụ thuộc CDN |
| `scripts/` | Khởi chạy, theo dõi và health check trên Pi |
| `systemd/` | Mẫu service tự khởi động |
| `config/` | Mẫu Mosquitto listener và ACL |

SQLite khử trùng theo `device_id + boot_id + seq`. Bản tin QoS 1 được gửi lại
không tạo dòng trùng.

## Xác thực và credentials

- `/health` và `POST /auth/login` công khai.
- **Đăng nhập (app):** `POST /auth/login` với `{"username","password"}` trả
  `access_token` (hết hạn sau `SAEMS_SESSION_TTL_HOURS`, mặc định 30 ngày). Gửi
  token đó như `Authorization: Bearer <token>`. `POST /auth/logout` thu hồi token;
  `GET /auth/me` cho biết đang đăng nhập bằng gì. Server chỉ lưu SHA-256 của token
  (`data/auth.db`). Sai mật khẩu 5 lần trong 15 phút thì bị khóa 5 phút (HTTP 429).
- **Đặt mật khẩu admin** (chạy trên Pi, mật khẩu không hiện ra và không vào log):

  ```bash
  cd ~/saems-mini-server && .venv/bin/python auth.py set-password --user admin
  sudo systemctl restart saems-api
  ```

  Mật khẩu tối thiểu 10 ký tự, lưu dạng băm PBKDF2-SHA256 trong `.env`
  (`SAEMS_ADMIN_PASSWORD_HASH`). Chưa đặt mật khẩu thì `/auth/login` trả 503.
- API token cũ (`API_ACCESS_TOKEN`) vẫn dùng được cho script và app bản cũ.
- Dashboard (HTTP Basic): user admin + mật khẩu, hoặc username `saems` + API token.
- MQTT dùng tài khoản/ACL riêng cho ESP32 và server.
- Credentials cục bộ nằm trong `.local/device-access.json` hoặc file `.env`
  trên Pi; không được ghi vào README, source hoặc commit Git.

HTTPS cho app/API không đồng nghĩa với quyền SSH vào Pi. `Logs`, `Db` và
`Services` trong script Windows vẫn cần đường SSH hợp lệ tới Pi.

## Demo server từ thư mục script chung

Chạy tại thư mục gốc repository:

```powershell
Set-Location 'C:\Users\Tien An\esp\SAEMS_esp32_test\merge_github'

# Kiểm tra health và bản telemetry mới nhất qua Internet
.\scripts\windows\run_server.ps1 -Action Check

# Theo dõi bản ghi mới từ API/SQLite theo thời gian
.\scripts\windows\run_server.ps1 -Action Watch

# Các lệnh dưới cần SSH tới Pi
.\scripts\windows\run_server.ps1 -Action Services
.\scripts\windows\run_server.ps1 -Action Logs
.\scripts\windows\run_server.ps1 -Action Db
```

`Check` và `Watch` dùng HTTPS nên laptop không cần chung Wi-Fi với Pi. Mặc
định `Logs/Db/Services` dùng `admin@192.168.1.4`; có thể truyền `-PiHost`
khi đã có địa chỉ Tailscale/SSH hợp lệ.

Luồng demo ba lớp và các terminal cần mở nằm tại
[scripts/windows/RUN-DEMO.md](../../scripts/windows/RUN-DEMO.md).

## API chính

| Method | Endpoint | Mục đích |
| --- | --- | --- |
| GET | `/health` | Health check |
| GET | `/api/v1/devices` | Danh sách thiết bị |
| GET | `/api/v1/devices/{id}/status` | Trạng thái thiết bị |
| GET | `/api/v1/devices/{id}/telemetry/latest` | Mẫu mới nhất |
| GET | `/api/v1/devices/{id}/telemetry?limit=50` | Lịch sử |
| POST | `/api/v1/devices/{id}/commands/pump` | Xếp lệnh bơm vào MQTT |
| GET | `/api/v1/devices/{id}/configuration` | Cấu hình server và trạng thái đồng bộ với thiết bị |
| PUT | `/api/v1/devices/{id}/configuration` | Lưu ngưỡng/chu kỳ/chế độ năng lượng, publish retained MQTT |
| GET | `/api/v1/alerts?device_id=&state=active` | Danh sách cảnh báo |
| GET | `/api/v1/devices/{id}/alerts` | Cảnh báo của một thiết bị |
| POST | `/api/v1/notifications/test` | Gửi tin Telegram thử nghiệm |
| GET | `/docs` | OpenAPI UI có xác thực |

Lệnh bơm mẫu:

```json
{"state":"on","duration_s":30}
```

API trả `queued` chỉ có nghĩa broker đã nhận lệnh. Trạng thái relay phải được
xác nhận bằng telemetry tiếp theo; firmware vẫn chịu trách nhiệm auto-off.

## Cấu hình ngưỡng và chu kỳ

`PUT /api/v1/devices/farm-01/configuration` (chỉ gửi phần muốn quản lý; `null`
nghĩa là server không quản lý giá trị đó):

```json
{
  "telemetry_interval_s": 300,
  "power_mode": null,
  "offline_alert_after_s": null,
  "thresholds": {
    "temperature_c": {"min": 10, "max": 35},
    "humidity_pct": {"min": 40, "max": 90},
    "soil_moisture_pct": {"min": 35, "max": null},
    "light_lux": {"min": null, "max": null},
    "battery_pct": {"min": 20}
  },
  "auto_watering": {
    "enabled": true,
    "soil_moisture_threshold_pct": 30,
    "pump_max_runtime_sec": 30,
    "pump_min_rest_sec": 1800
  }
}
```

PUT thay **toàn bộ** cấu hình: phần nào bỏ trống hoặc `null` thì server thôi quản
lý phần đó. App luôn gửi đủ các phần đang quản lý.

- Server tăng `version`, publish retained lên `saems/{id}/config` dạng gọn
  `{"config_version":3,"telemetry_interval_s":300,"thresholds":[10.0,35.0,...]}`.
  Database chỉ commit sau khi broker xác nhận publish.
- Thiết bị áp dụng ở chu kỳ lấy mẫu kế tiếp và báo `config_version` trong
  telemetry. `GET .../configuration` trả `sync_state`: `applied`, `pending`,
  `unknown` (chưa có telemetry mới) hoặc `not_configured`.
- Qua MQTT, firmware chỉ nhận `config_version`, `telemetry_interval_s`,
  `power_mode`, `thresholds`, `irrigation` (tưới tự động dạng mảng
  `[bật, ngưỡng %, bơm tối đa s, nghỉ s]`, xem `docs/pump-battery.md`); Wi-Fi/MQTT/AP/hiệu chuẩn đất chỉ đổi qua BLE/AP.
- Đổi ngưỡng/chu kỳ qua BLE/AP sẽ đặt `config_version = 0` trên thiết bị; khi
  thiết bị nối lại server, cấu hình retained của server được áp dụng lại.
- ACL Mosquitto phải có `topic read saems/<id>/config` cho thiết bị và
  `topic write saems/+/config` cho server (xem `config/acl.example`).

## Cảnh báo Telegram

Ingestor đánh giá mỗi bản ghi mới theo ngưỡng đang lưu trên server, cùng
hysteresis với firmware (nhiệt 0.5 °C; ẩm, đất, pin 2 %; ánh sáng
max(10 lux, 5 %)). Mỗi lần vào/ra cảnh báo tạo một dòng trong bảng `alerts`
và gửi một tin Telegram. Bản ghi gửi bù cũ hơn 10 phút được lưu với
`backlog=true` nhưng không gửi từng tin. Thiết bị im lặng quá
`offline_alert_after_s` (mặc định `max(3 × chu kỳ + 60, 300)` giây, hoặc 900
giây khi chưa đặt chu kỳ) tạo cảnh báo mất kết nối; bản ghi kế tiếp đóng cảnh
báo và báo đã kết nối lại.

Cài đặt bot:

1. Trong Telegram, nhắn `@BotFather` lệnh `/newbot`, đặt tên, lưu token.
2. Nhắn cho bot vừa tạo một tin bất kỳ (hoặc thêm bot vào group).
3. Trên Pi: `curl -s https://api.telegram.org/bot<TOKEN>/getUpdates` và lấy
   `message.chat.id`.
4. Ghi vào `.env`: `TELEGRAM_BOT_TOKEN=<TOKEN>` và `TELEGRAM_CHAT_ID=<id>`
   (nhiều chat cách nhau bằng dấu phẩy), rồi
   `sudo systemctl restart saems-ingestor saems-api`.
5. Kiểm tra: `curl -X POST -H "Authorization: Bearer <API token>"
   http://127.0.0.1:3000/api/v1/notifications/test`.

Token bot là secret: không commit, không dán vào log; notifier không ghi URL
chứa token ra log.

## Cài đặt hoặc phục hồi trên Raspberry Pi

```bash
sudo apt update
sudo apt install -y mosquitto mosquitto-clients sqlite3 python3 python3-venv

cd /home/admin/saems-mini-server
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install -r requirements.txt
cp .env.example .env
chmod 600 .env
```

Sau khi điền `.env`, chạy thủ công chỉ khi systemd đang dừng để tránh hai
ingestor/API cùng dùng một database hoặc port:

```bash
./scripts/run_ingestor.sh
./scripts/run_api.sh
./scripts/monitor_mqtt.sh
./scripts/check_system.sh
```

Kiểm tra service đang triển khai:

```bash
systemctl is-active saems-mqtt saems-ingestor saems-legacy-ingestor saems-api tailscaled
journalctl -u saems-mqtt -u saems-ingestor -u saems-api -f -o short-iso
curl http://127.0.0.1:3000/health
```

## Kiểm thử source server

Từ `services/mini-server`:

```bash
.venv/bin/python -m unittest discover -s tests -v
python3 -m py_compile api.py mqtt_ingestor.py legacy_mqtt_ingestor.py mqtt_security.py \
    device_config.py alert_engine.py notifier.py schema.py
```

## Giới hạn vận hành

- Không commit `.env`, database, MQTT password file, `.venv` hoặc log.
- Broker TCP 1883 cũ chỉ dành cho tương thích LAN; thiết bị mới dùng TLS 8443.
- CORS không mở wildcard trong public mode.
- Tailscale SSH hiện phụ thuộc ACL riêng; HTTPS vẫn hoạt động khi SSH bị từ chối.
- Trước khi thay service/config trên Pi phải sao lưu database và kiểm tra đường
  dẫn `/home/admin/saems-mini-server`.

Báo cáo hiện trạng: [IMPLEMENTATION-REPORT.md](../../docs/IMPLEMENTATION-REPORT.md).

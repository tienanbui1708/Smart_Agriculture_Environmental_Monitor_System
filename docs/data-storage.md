# Lưu trữ dữ liệu trên server

## Lựa chọn: SQLite (WAL) thay vì time-series database

| Tiêu chí | SQLite | InfluxDB / TimescaleDB |
|---|---|---|
| Quy mô SAEMS | 1 thiết bị, 1 mẫu/10 s ≈ 8.640 dòng/ngày; 180 ngày ≈ 1,6 triệu dòng (~1 GB) | Thiết kế cho hàng triệu điểm mỗi giây |
| Tài nguyên Raspberry Pi 4 | Thư viện trong tiến trình Python, không thêm dịch vụ | Thêm một dịch vụ thường trú, vài trăm MB RAM |
| Ghi đồng thời | WAL: một ghi (ingestor) và nhiều đọc (API) không chặn nhau | Có |
| Truy vấn cần dùng | Bản ghi mới nhất, khoảng thời gian theo thiết bị, gộp theo giờ | Có sẵn hàm gộp và retention |
| Sao lưu | `sqlite3 .backup` ra một file (đã dùng khi triển khai) | Cần công cụ riêng |
| Ràng buộc chống trùng | `UNIQUE(device_id, boot_id, seq)` cho gửi bù QoS 1 | Phải tự thiết kế tag/timestamp |

Ở quy mô đồ án, SQLite đủ nhanh và đơn giản hơn để vận hành, sao lưu và kiểm thử
(test chạy trên DB tạm). Phần TSDB làm sẵn (gộp theo thời gian, retention) được bổ
sung bằng `retention.py`. Khi số thiết bị tăng lên hàng chục và chu kỳ ngắn, nên
chuyển sang TimescaleDB (PostgreSQL), vì vẫn dùng SQL nên đổi ít code.

## Bảng và index

| Bảng | Nội dung | Index |
|---|---|---|
| `telemetry` | Bản ghi thô, `raw_json` giữ nguyên payload | `UNIQUE(device_id, boot_id, seq)`, `(device_id, received_at)`, `(device_id, timestamp)` |
| `telemetry_hourly` | Mỗi thiết bị mỗi giờ (UTC): số mẫu, trung bình/nhỏ nhất/lớn nhất của nhiệt độ, độ ẩm, đất, ánh sáng, pin | Khóa chính `(device_id, hour)` |
| `alerts`, `alert_state` | Cảnh báo và trạng thái hysteresis | `(device_id, created_at)` |
| `device_config` | Cấu hình server quản lý | Khóa chính `device_id` |
| `maintenance_state` | Mốc gộp đã xử lý | — |
| `auth.db/sessions` | Băm SHA-256 của token phiên | Khóa chính |

## Gộp theo giờ và retention (`retention.py`)

- Ingestor chạy bảo trì lúc khởi động và mỗi 6 giờ.
- Mỗi lần chạy, tính lại mọi cặp (thiết bị, giờ) có bản ghi mới kể từ lần trước,
  kể cả dữ liệu gửi bù về muộn, nên số liệu gộp luôn chính xác.
- Bản ghi thô cũ hơn `SAEMS_RAW_RETENTION_DAYS` (mặc định 180) bị xóa, **chỉ sau khi**
  giờ của nó đã được gộp. Cảnh báo đã đóng cũ hơn `SAEMS_ALERT_RETENTION_DAYS`
  (mặc định 365) bị xóa.
- API: `GET /api/v1/devices/{id}/telemetry/hourly?from=2026-11-01T00&to=…&limit=168`
  vẫn trả dữ liệu gộp sau khi dữ liệu thô đã bị xóa.

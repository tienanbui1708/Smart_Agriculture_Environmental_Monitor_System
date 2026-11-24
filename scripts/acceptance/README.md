# Script nghiệm thu

| Script | Mục đích |
|---|---|
| `stage4_resilience.py` | Bài test Giai đoạn 4 của PRD: tắt server → thiết bị lưu offline → điện thoại kết nối trực tiếp → bật server → tự đồng bộ. Xuất báo cáo Markdown, kết luận dựa trên tính liên tục của `seq`. |
| `energy_estimate.py` | Tính dòng trung bình và thời gian chạy pin từ số đo từng pha (mẫu báo cáo: `docs/energy-measurement.md`). |

## Bài test Giai đoạn 4

Chuẩn bị: ESP32 đang gửi dữ liệu, app trên điện thoại, Python 3.10 trở lên.

```bash
# Cách 1: tự tắt/bật broker khi script nhắc
python scripts/acceptance/stage4_resilience.py --offline-minutes 5

# Cách 2: script tắt/bật saems-mqtt qua SSH (Pi sẽ hỏi mật khẩu sudo)
python scripts/acceptance/stage4_resilience.py --ssh admin@100.104.200.110
```

- Đăng nhập: đặt `SAEMS_API_TOKEN`, hoặc nhập user/mật khẩu khi được hỏi
  (`/auth/login`).
- Bằng chứng tự động qua Wi-Fi AP (tùy chọn): trong bước 3, cho laptop nối Wi-Fi
  `SAEMS-farm-01` và đặt `SAEMS_DEVICE_TOKEN`. Script đọc `/v1/status` để ghi
  `pending_records` lúc thiết bị đang offline.
- Kết quả là file `stage4-report-YYYYMMDD-HHMM.md`, gồm các mốc thời gian, số bản gửi
  bù, độ trễ giao nhận, bảng `seq` theo `boot_id` (thiếu/trùng) và kết luận.
- Chỉ phân tích một khoảng thời gian có sẵn:
  `--analyze-only --from 2026-12-01T08:00:00Z --to 2026-12-01T09:00:00Z`.

Ngày 2026-11-24, phân tích toàn bộ dữ liệu thật trên Pi (21/09 – 10/10/2026) cho kết
quả: 3.532 bản ghi, 37 lần khởi động, **0 `seq` bị thiếu**; bản gửi bù trễ nhất đến
sau 8,3 giờ.

Test phần phân tích: `python -m unittest discover -s scripts/acceptance`

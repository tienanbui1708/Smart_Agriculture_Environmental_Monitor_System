# Firmware baseline (lưu trữ)

Thư mục này là project ESP-IDF đầu tiên của nhóm, nhánh `feature/Hien/firmware`.
Các commit `f1422e2` (blank project), `6731712` (DHT20 + ADC độ ẩm đất), `1a6b02b`
(task và sensor management) trước đây nằm ở **thư mục gốc** của repo.

Ngày 2026-11-24 (GĐ G trong [`docs/UPDATE-PLAN.md`](../../docs/UPDATE-PLAN.md)),
nhóm chuyển nguyên trạng vào đây bằng `git mv`, nên lịch sử vẫn xem được:
`git log --follow legacy/root-firmware/main/main.c`.

- Firmware đang dùng: [`firmware/esp32s3/`](../../firmware/esp32s3/README.md).
  Script `scripts/windows/` chỉ build và flash project đó.
- Mã ở đây không còn được bảo trì và không có trong phân tích tĩnh hay test.
- Vẫn build độc lập được để tham khảo:
  `cd legacy/root-firmware && idf.py set-target esp32s3 build`.

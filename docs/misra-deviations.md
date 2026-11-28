# MISRA C:2012 — phạm vi và deviation

Yêu cầu PRD: *"Mã nguồn (Code Quality): Viết theo chuẩn MISRA-C hoặc các best practice
của C/C++ (đối với Firmware)."*

## Cách kiểm tra

```bash
cd firmware/esp32s3
python tools/static_analysis.py --build-dir <thư mục build ESP-IDF> \
    --report ../../docs/static-analysis-report.md
python tests/run_host_tests.py
```

Script dùng cppcheck (addon `misra.py`) và clang-tidy của esp-clang. Kết quả gần nhất:
[`static-analysis-report.md`](static-analysis-report.md).

| Cổng | Phạm vi | Tiêu chí |
|---|---|---|
| 1 | Module C thuần: `threshold_monitor`, `irrigation`, `battery_policy`, `record_log`, `status_led_policy` | 0 vi phạm MISRA ngoài các deviation D1–D4 |
| 2 | Toàn bộ firmware của project | 0 cảnh báo/lỗi cppcheck |
| 3 | Toàn bộ firmware của project | 0 cảnh báo clang-tidy (`bugprone-*`, `cert-*`, `clang-analyzer-*`) |
| — | Phần tích hợp ESP-IDF | MISRA chỉ thống kê theo quy tắc, không chặn |

Logic quyết định (ngưỡng, tưới, pin, vòng ghi flash, LED) nằm trong các module C
thuần. Các module này không phụ thuộc ESP-IDF, có host test riêng và phải tuân thủ
MISRA. Phần còn lại là "keo" gọi API ESP-IDF/FreeRTOS/NimBLE, nơi macro của framework
(ví dụ `ESP_RETURN_ON_ERROR` có `return` ẩn, `OS_MBUF_PKTLEN` ép kiểu qua `void *`)
tự sinh vi phạm mà project không sửa được. Ở phần này nhóm theo các best practice
của CERT C và clang-analyzer (cổng 2 và 3).

Đã sửa khi áp dụng (GĐ F): 22.8/22.9 (`errno` quanh `strtof`/`strtoul`), 17.7 (bỏ qua
giá trị trả về), 14.2 (vòng `for` không đúng dạng), 10.8 (ép kiểu biểu thức phức hợp),
8.9 (bảng hằng đưa vào hàm dùng nó), 5.9 (tên static trùng), 17.8 (sửa tham số),
18.4 (số học con trỏ), 12.1 (khoảng 70 chỗ thêm ngoặc làm rõ ưu tiên). Với clang-tidy:
thay `strcat` bằng `memcpy` có độ dài, đánh dấu NOLINT có lý do cho hai chỗ đúng thiết
kế.

## Deviation

| ID | Quy tắc | Loại | Phạm vi | Lý do | Biện pháp bù |
|---|---|---|---|---|---|
| D1 | 15.5 Một điểm thoát duy nhất | Advisory | Toàn bộ module C thuần | Kiểm tra tham số đầu hàm rồi `return` sớm giúp code phẳng, dễ đọc hơn một biến kết quả lồng nhiều tầng | Hàm ngắn, không cấp phát tài nguyên nên không có gì để giải phóng khi `return` sớm |
| D2 | 8.7 Hàm chỉ dùng trong một file nên là `static` | Advisory | API công khai của module | cppcheck chỉ phân tích module, không thấy `application_manager` gọi các hàm này | API khai báo trong header, được host test và firmware gọi |
| D3 | 21.6 Không dùng `<stdio.h>` | Required | `snprintf` trong `threshold_monitor`, `irrigation` | Dựng JSON cho BLE/AP/MQTT; không có hàm định dạng nào khác trong thư viện chuẩn | Luôn truyền kích thước buffer, kiểm tra giá trị trả về; cắt cụt thì trả lỗi và xóa chuỗi |
| D4 | 11.5 Ép `void *` sang con trỏ đối tượng | Advisory | `record_log_crc32`, callback flash | API theo byte nhận mọi kiểu dữ liệu; chỉ ép sang `const uint8_t *` để đọc | Độ dài luôn đi kèm con trỏ; có host test với flash giả lập |

Deviation được khai báo trong `firmware/esp32s3/tools/misra_suppressions.txt`. Muốn
thêm deviation thì phải ghi vào bảng trên trước.

Chưa làm: cppcheck không kèm văn bản quy tắc MISRA (tài liệu có bản quyền), nên báo
cáo chỉ ghi mã quy tắc. Muốn có tên quy tắc thì truyền `--rule-texts` vào `misra.py`
với file văn bản do nhóm tự mua.

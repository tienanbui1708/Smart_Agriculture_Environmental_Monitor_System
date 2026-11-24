# Báo cáo phân tích tĩnh firmware

Tạo lúc 2026-11-24 16:41 UTC bằng `firmware/esp32s3/tools/static_analysis.py`. Deviation và lý do: [`misra-deviations.md`](misra-deviations.md).

- cppcheck: Cppcheck 2.14.0, addon MISRA C:2012
- clang-tidy: LLVM version 20.1.1, checks `-*,bugprone-*,cert-*,clang-analyzer-*,-bugprone-easily-swappable-parameters,-bugprone-assignment-in-if-condition,-cert-dcl37-c,-cert-dcl51-cpp,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling`
- Nguồn: 23 file C của project (không tính ESP-IDF, managed_components)

| Cổng | Phạm vi | Kết quả |
|---|---|---|
| 1. MISRA C:2012 nghiêm ngặt | 5 module C thuần | ĐẠT (0 vi phạm; deviation: misra-c2012-11.5, misra-c2012-15.5, misra-c2012-21.6, misra-c2012-8.7) |
| 2. cppcheck lỗi/cảnh báo | toàn bộ firmware | ĐẠT (0) |
| 3. clang-tidy | toàn bộ firmware | ĐẠT (0) |

## 1. MISRA trên module C thuần

Không có.

## 2. cppcheck (warning/error)

Không có.

## 3. clang-tidy

Không có.

## 4. MISRA trên phần tích hợp ESP-IDF (tham khảo, không chặn)

Phần này gọi API ESP-IDF/FreeRTOS/NimBLE (macro, con trỏ void, nhiều điểm return theo `ESP_RETURN_ON_ERROR`), nên chỉ thống kê để theo dõi xu hướng.

| Quy tắc | Số lần |
|---|---|
| `misra-c2012-2.5` | 1172 |
| `misra-c2012-15.5` | 283 |
| `misra-config` | 226 |
| `misra-c2012-12.1` | 215 |
| `misra-c2012-10.4` | 51 |
| `misra-c2012-17.7` | 26 |
| `misra-c2012-17.3` | 22 |
| `misra-c2012-15.7` | 13 |
| `misra-c2012-5.9` | 12 |
| `misra-c2012-18.4` | 10 |
| `misra-c2012-8.9` | 10 |
| `misra-c2012-21.6` | 7 |
| `misra-c2012-11.5` | 6 |
| `misra-c2012-8.7` | 3 |
| `misra-c2012-21.10` | 2 |
| `misra-c2012-9.3` | 2 |
| `misra-c2012-10.8` | 2 |
| `misra-c2012-22.8` | 2 |
| `misra-c2012-13.3` | 2 |
| `misra-c2012-16.6` | 1 |
| `misra-c2012-12.2` | 1 |
| `misra-c2012-10.3` | 1 |
| `misra-c2012-20.9` | 1 |
| `misra-c2012-21.3` | 1 |
| `misra-c2012-11.8` | 1 |
| `misra-c2012-21.16` | 1 |
| `misra-c2012-15.4` | 1 |
| `misra-c2012-8.4` | 1 |
| `misra-c2012-2.3` | 1 |

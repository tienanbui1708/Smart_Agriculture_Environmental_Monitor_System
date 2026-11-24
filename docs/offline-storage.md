# Lưu trữ offline và gửi bù

Tài liệu cho GĐ C trong [`UPDATE-PLAN.md`](UPDATE-PLAN.md). Yêu cầu PRD: mất mạng
không mất dữ liệu; khi có mạng lại thì gửi bù.

## 1. Thiết kế

| Thành phần | Vị trí | Vai trò |
|---|---|---|
| `record_log` | `firmware/esp32s3/components/record_log` | Vòng ghi trên flash, C thuần, test trên máy tính |
| `storage_manager` | `components/storage_manager` | Gắn `record_log` với phân vùng `telemetry`, dự phòng NVS, chuyển dữ liệu cũ |
| `sync_manager` | `components/sync_manager` | Gửi bù theo lô qua MQTT QoS 1 |
| `mqtt_ingestor.py` | `services/mini-server` | Nhận bản tin đơn lẻ hoặc lô, bỏ bản trùng |

### Phân vùng flash (16 MB)

```text
0x009000  nvs        192 KB  (giữ nguyên: Wi-Fi, MQTT, hiệu chuẩn đất, cấu hình)
0x039000  otadata      8 KB
0x03B000  phy_init     4 KB
0x040000  ota_0        3 MB
0x340000  ota_1        3 MB
0x640000  coredump    64 KB
0x650000  telemetry    6 MB   ← mới: vòng ghi telemetry offline
```

### Vì sao không dùng LittleFS

Kế hoạch ban đầu ghi LittleFS. Khi làm, nhóm chọn ghi thẳng vòng bản ghi lên phân
vùng flash vì:

- Dữ liệu chỉ ghi nối tiếp, kích thước cố định (≤ 768 byte) và đọc theo thứ tự
  FIFO. Ghi tuần tự quanh vòng làm mòn flash đều mà không cần lớp file system.
- Mỗi bản ghi chỉ tốn đúng một lần ghi và mỗi sector chỉ một lần xóa mỗi vòng.
  LittleFS phải ghi thêm metadata và chép lại khối khi đổi file.
- Không thêm thư viện ngoài vào firmware.
- Toàn bộ logic test được trên máy tính với flash NOR giả lập, kể cả mất điện giữa
  lúc ghi (`tests/test_record_log.c`).

### Định dạng

- Ô 1 KB, 4 ô/sector 4 KB → 6.144 ô. Một sector luôn bị xóa trước khi ghi, nên
  dùng được tối đa khoảng 6.140 bản ghi (yêu cầu kế hoạch: khoảng 5.000).
- Mỗi ô có header 20 byte: `id`, độ dài, CRC32, cờ "đã gửi", rồi tới `magic`.
- **Thứ tự ghi an toàn khi mất điện:** ghi payload trước, header sau, `magic` cuối
  cùng. Ô thiếu `magic` coi như chưa từng ghi. Ô sai CRC bị bỏ qua và đánh dấu đã gửi.
- **Đánh dấu đã gửi:** ghi đè cờ `0xFFFFFFFF → 0` ngay tại chỗ (NOR chỉ cần đổi bit
  1 → 0, không phải xóa).
- **Khởi động lại:** quét header của tất cả các ô (khoảng 120 KB đọc). Ô có `id` lớn
  nhất cho biết vị trí ghi tiếp theo; ô chưa gửi có `id` nhỏ nhất là bản cũ nhất.
  Khi thức dậy từ Deep Sleep thì không phải quét lại, vì trạng thái đã được giữ
  trong RTC.
- **Đầy:** sector cũ nhất bị xóa để ghi tiếp. Các bản chưa gửi trong sector đó bị
  mất (bỏ bản **cũ nhất**) và được cộng vào `queue_dropped`, lưu trong NVS
  `saems_log/dropped`.
- Bản đã gửi vẫn đọc lại được làm lịch sử (BLE/AP `/v1/telemetry/history`) cho tới
  khi bị ghi đè.

### Gửi bù theo lô

- Một bản chờ: gửi nguyên bản như trước.
- Nhiều bản chờ: gửi lô tối đa 8 bản `{"batch":[{...},{...}]}` trên cùng topic
  `saems/{id}/telemetry`, nên không phải sửa ACL Mosquitto.
- Chỉ một bản tin đang chờ PUBACK tại một thời điểm. PUBACK xác nhận cả lô theo
  `id` của bản cuối. Nếu sector chứa lô đó bị xóa trước khi PUBACK về, các bản sau
  lô vẫn còn nguyên (đã có test).
- Mất kết nối khi đang chờ PUBACK thì gửi lại cả lô. Server bỏ bản trùng theo
  `(device_id, boot_id, seq)`.
- Server ghi cả lô trong một transaction. Bản ghi hỏng trong lô bị bỏ qua và ghi
  log, các bản khác vẫn được lưu. Cảnh báo Telegram cho bản gửi bù cũ hơn 10 phút
  chỉ được lưu, không gửi tin (đã có từ GĐ A).

### Tương thích và chuyển đổi

- Nếu chỉ nạp app (`app-flash`) mà chưa nạp bảng phân vùng mới, firmware không
  thấy phân vùng `telemetry` và tự quay về hàng đợi NVS 32 bản như cũ. Log báo
  `Partition "telemetry" unavailable`.
- Lần đầu chạy với phân vùng mới, các bản còn chờ trong hàng đợi NVS cũ được chuyển
  sang vòng flash rồi xóa khỏi NVS.
- `idf.py flash` (hoặc `scripts/windows/run_device.ps1 -Action Flash`) ghi bảng phân
  vùng mới. NVS ở 0x9000 giữ nguyên nên không mất Wi-Fi, MQTT hay hiệu chuẩn đất.
  Lần quét đầu bỏ qua dữ liệu rác cũ trong vùng 0x650000+ (không có `magic`).

## 2. Trường dữ liệu mới

- Telemetry: `"queue_dropped": N`, tổng số bản chưa gửi đã mất vì đầy bộ nhớ. App
  hiện chip "Bộ nhớ offline đầy" khi N > 0.
- BLE/AP status: `pending_records`, `dropped_records`, `storage`
  (`flash_log` hoặc `nvs`).
- AP `/v1/telemetry/history?limit=N`: N bản **mới nhất** (đã gửi hoặc chưa),
  kèm `pending_count` và `dropped_count`.

## 3. Nghiệm thu 🔧

1. Nạp firmware bằng `run_device.ps1 -Action Flash`. Log khởi động có
   `Flash log: 6144 slots, pending=…, scan … ms`.
2. **Mất mạng ngắn:** tắt Wi-Fi router (hoặc dừng `saems-mqtt` trên Pi) trong 10 phút
   với chu kỳ 10 s. `pending` tăng khoảng 60. Bật lại thì log có
   `Sync 8 record(s) through id …`. Server có
   `Offline batch from farm-01: 8 stored, 0 duplicates`. Kiểm tra `seq` liên tục:
   `GET /api/v1/devices/farm-01/telemetry?limit=100`.
3. **Mất điện khi đang offline:** khi `pending > 0`, rút nguồn ESP32 rồi cắm lại.
   Log quét lại đúng `pending`, sau đó gửi bù đủ.
4. **Đầy bộ nhớ (tùy chọn):** cần khoảng 17 giờ offline ở chu kỳ 10 s. Kết quả mong
   đợi: `queue_dropped` tăng theo bội số 4, các bản gửi bù bắt đầu từ bản cũ nhất
   còn giữ. Kiểm chứng nhanh trên máy tính bằng `tests/test_record_log.c`.

Host test:

```bash
cd firmware/esp32s3
gcc -std=c11 -Wall -Wextra -Werror -Icomponents/record_log/include \
    tests/test_record_log.c components/record_log/record_log.c -o test_record_log
./test_record_log
```

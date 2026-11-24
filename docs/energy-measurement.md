# Báo cáo đo năng lượng (mẫu) 🔧

Yêu cầu PRD: *"Thiết bị phải sử dụng chế độ Deep Sleep hoặc Light Sleep giữa các chu
kỳ lấy mẫu để tiết kiệm pin."* Điền bảng bên dưới bằng số đo thật, rồi tính dòng
trung bình và thời gian chạy pin bằng `scripts/acceptance/energy_estimate.py`.

## 1. Chuẩn bị

| Hạng mục | Giá trị |
|---|---|
| Board | ESP32-S3-DevKitC N16R8 |
| Firmware (commit) | |
| Dụng cụ đo | ví dụ: USB power meter có ghi log / INA219 / đồng hồ vạn năng nối tiếp |
| Điểm đo | ví dụ: chân 5V (gồm LDO và LED nguồn) hoặc 3V3 sau LDO |
| Nhiệt độ phòng | |
| Chu kỳ lấy mẫu | |

Lưu ý khi đo:

- Rút cáp USB-UART khi đo Deep Sleep. Chip USB-UART và LED nguồn trên DevKitC tốn vài
  mA, che mất dòng ngủ của ESP32 (khoảng 10 µA). Ghi rõ điểm đo trong báo cáo.
- Tắt LED trạng thái (`SAEMS_STATUS_LED_ENABLED=n`). WS2812 tốn khoảng 1 mA kể cả khi
  tối.
- Mỗi chế độ đo ít nhất 5 chu kỳ. Lấy trung bình pha thức và pha ngủ riêng.
- Relay và bơm không cấp điện trong lúc đo (bơm có nguồn riêng).

## 2. Kết quả đo

| Chế độ (`power_mode`) | Pha | Thời lượng mỗi chu kỳ (s) | Dòng trung bình (mA) | Ghi chú |
|---|---|---|---|---|
| Direct (modem save) | liên tục | = chu kỳ | | Wi-Fi giữ kết nối |
| Light Sleep | thức lấy mẫu + gửi | | | |
| Light Sleep | ngủ giữa hai mẫu | | | DFS + tickless idle |
| Battery (Deep Sleep) | thức: boot, cảm biến, Wi-Fi, MQTT, cửa sổ BLE/AP | | | tối thiểu 10 s, tối đa 15 s |
| Battery (Deep Sleep) | ngủ sâu | | | |

## 3. Dòng trung bình và thời gian chạy pin

Công thức cho mỗi chu kỳ T:

```text
I_tb = (Σ t_pha × I_pha + (T − Σ t_pha) × I_ngủ) / T
Thời gian chạy (giờ) = dung lượng (mAh) × tỉ lệ dùng được / I_tb
```

Ví dụ (số giả định, **thay bằng số đo**): Battery mode, chu kỳ 300 s, thức 12 s ở
95 mA, ngủ 0,15 mA, pin 2000 mAh dùng 80 %:

```bash
python scripts/acceptance/energy_estimate.py --interval 300 \
    --phase awake:12:95 --sleep-ma 0.15 --capacity-mah 2000
# Average current: 3.944 mA
# Battery life (2000 mAh x 80%): 406 h = 16.9 days
```

| Chế độ | Chu kỳ (s) | I_tb (mA) | Pin 2000 mAh (ngày) |
|---|---|---|---|
| Direct | | | |
| Light Sleep | | | |
| Battery (Deep Sleep) | | | |

## 4. Nhận xét

- Phần lớn năng lượng ở Battery mode tốn ở pha thức (Wi-Fi + TLS). Kéo dài chu kỳ hoặc
  để pin yếu tự tăng chu kỳ (GĐ B: ≥ 600 s khi pin < 20 %) sẽ giảm I_tb gần như tỉ lệ.
- Ghi lại các yếu tố làm sai số: LED nguồn, chip USB, LDO trên board, nhiễu của dụng cụ.

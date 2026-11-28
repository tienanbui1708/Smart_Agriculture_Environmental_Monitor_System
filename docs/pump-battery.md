# Máy bơm, tưới tự động và pin

Tài liệu cho GĐ B trong [`UPDATE-PLAN.md`](UPDATE-PLAN.md). Hiện board chưa có máy
bơm và pin thật, nên firmware có hai chế độ mô phỏng để chạy đủ kịch bản. Khi lắp
phần cứng thật chỉ cần đổi menuconfig, không phải sửa code.

## 1. Nối dây

### Relay và máy bơm

```text
             ESP32-S3 DevKitC                     Module relay 2 kênh (5 V)
             ┌────────────┐                       ┌────────────────────┐
             │      GPIO10├──────────────────────►│IN1                 │
             │      GPIO11├──────────────────────►│IN2 (dự phòng)      │
             │         5V ├──────────────────────►│DC+ (cuộn relay)    │
             │        GND ├──────────────────────►│DC-                 │
             └────────────┘                       │  COM1 ───┐         │
                                                  │  NO1  ─┐ │         │
                                                  └────────┼─┼─────────┘
   Nguồn bơm riêng (5–12 V DC) (+) ────────────────────────┘ │
                                Bơm (+) ◄────────────────────┘
   Nguồn bơm (−) ─────────────► Bơm (−)
```

- Bơm dùng **nguồn riêng**. Không cấp dòng bơm từ chân 5V của board.
- Dùng tiếp điểm **NO** (thường hở) để bơm tắt khi ESP32 reset hoặc mất điện.
- Bơm DC có chổi than: lắp diode chống ngược (1N4007/1N5819) song song với bơm,
  catốt về phía (+).
- Module relay thường kích ở mức thấp: giữ `SAEMS_RELAY_ACTIVE_LOW=y` (mặc định).

### Cầu chia áp đo pin (LiPo 1S, 3.0–4.2 V)

```text
   Pin (+) ──┬── R1 100 kΩ ──┬── R2 100 kΩ ── GND
             │               │
             │               └──► GPIO6 (ADC1_CH5)   [tụ 100 nF xuống GND, tùy chọn]
             └──► mạch sạc / boost 5 V cấp cho board
```

- Tỉ lệ chia 2 (`SAEMS_BATTERY_DIVIDER_X100=200`): 4.2 V ở pin thành 2.1 V ở
  chân ADC, nằm trong dải đo 12 dB.
- Điện trở 100 kΩ chỉ tốn khoảng 21 µA. Tụ 100 nF giúp ADC đọc ổn định.
- Phải dùng chân **ADC1**: ADC2 không đọc được khi Wi-Fi đang chạy. Firmware dùng
  chung handle ADC1 với cảm biến đất (GPIO4).
- Firmware đổi điện áp sang % theo đường xả LiPo điển hình (4.2 V = 100 %,
  3.84 V ≈ 50 %, 3.27 V = 0 %).

## 2. Cấu hình firmware (`idf.py menuconfig` → SAEMS firmware)

| Tùy chọn | Mặc định | Ý nghĩa |
|---|---|---|
| `SAEMS_PUMP_SIMULATED` | `y` | Vẫn đóng relay và báo trạng thái, log ghi `[simulated pump]`. Tắt khi đã lắp bơm thật. |
| `SAEMS_RELAY_MAX_ON_SECONDS` | 60 | Giới hạn an toàn cho mỗi lần bơm (tay hoặc tự động). |
| `SAEMS_BATTERY_SOURCE` | `none` | `none` (cắm USB, `battery_pct=null`), `adc` (cầu chia áp), `simulated` (mô phỏng). |
| `SAEMS_BATTERY_ADC_GPIO` | 6 | Chân ADC1 của cầu chia áp. |
| `SAEMS_BATTERY_DIVIDER_X100` | 200 | Tỉ lệ (điện áp pin / điện áp chân) × 100. |
| `SAEMS_BATTERY_SIM_START_PCT` | 80 | Mức pin mô phỏng sau khi cấp nguồn. |
| `SAEMS_BATTERY_SIM_DRAIN_X10` | 5 | Mỗi mẫu mô phỏng trừ 0.5 %. |
| `SAEMS_BATTERY_LOW_PCT` | 20 | Dưới mức này coi là pin yếu. |
| `SAEMS_BATTERY_CRITICAL_PCT` | 5 | Dưới mức này coi là pin cạn. |
| `SAEMS_BATTERY_LOW_INTERVAL_SECONDS` | 600 | Chu kỳ tối thiểu khi pin yếu. |
| `SAEMS_BATTERY_CRITICAL_INTERVAL_SECONDS` | 3600 | Chu kỳ Deep Sleep khi pin cạn. |

Các mức pin có độ trễ 3 %: pin yếu chỉ hết khi lên lại ≥ 23 %, pin cạn chỉ hết khi
≥ 8 %.

## 3. Hành vi

### Tưới tự động (tưới theo xung)

Cấu hình (server/app): `auto_watering_enabled`, `soil_moisture_threshold_pct`,
`pump_max_runtime_sec`, `pump_min_rest_sec`. Trên thiết bị (BLE/AP/MQTT) cấu hình
này là mảng gọn `"irrigation":[bật, ngưỡng %, bơm tối đa s, nghỉ s]`, để cấu hình
đọc qua BLE vẫn dưới 512 byte.

1. Mỗi lần lấy mẫu, nếu tưới tự động bật, độ ẩm đất hợp lệ và **dưới ngưỡng**, và
   đã qua thời gian nghỉ, thì bật bơm (`pump_reason="auto"`).
2. Bơm tắt khi hết `min(pump_max_runtime_sec, SAEMS_RELAY_MAX_ON_SECONDS)`, hoặc sớm
   hơn nếu một mẫu cho thấy đất đã đạt **ngưỡng + 5 %**.
3. Sau mỗi lần tưới tự động, thiết bị nghỉ `pump_min_rest_sec` để nước kịp ngấm rồi
   mới đánh giá lại. Thời gian nghỉ vẫn được tính qua Deep Sleep (đồng hồ RTC).
4. Cảm biến đất lỗi hoặc chưa hiệu chuẩn thì không tưới tự động.
5. Lệnh bơm tay (app/server) giữ nguyên như cũ (`pump_reason="manual"`). Tưới tự
   động không tắt lần bơm tay; lần bơm tay chỉ tắt theo lệnh hoặc giới hạn an toàn
   (`safety_off`).
6. Ở chế độ Battery (Deep Sleep), thiết bị chờ lần bơm đang chạy kết thúc rồi mới
   ngủ, tối đa bằng giới hạn an toàn.

### Pin

| Mức | Điều kiện (mặc định) | Thiết bị làm gì |
|---|---|---|
| `normal` | ≥ 20 % | Theo `power_mode` và chu kỳ đã cấu hình |
| `low` | < 20 % | Tự chuyển Battery (Deep Sleep) **không ghi đè NVS**, chu kỳ ≥ 600 s; `power_mode` cũ tự quay lại khi pin hồi |
| `critical` | < 5 % | Tắt bơm (`pump_reason="low_battery"`), từ chối lệnh bật bơm, Deep Sleep 3600 s |

Server gửi cảnh báo Telegram khi `battery_pct` dưới ngưỡng `battery_min` (đặt trong
app → Ngưỡng & chu kỳ). `battery_min` là ngưỡng cảnh báo cho người dùng, độc lập với
hai mức `low`/`critical` của firmware.

### Trường telemetry mới

```json
"battery_pct": 18.5, "battery_source": "simulated", "battery_state": "low",
"pump_state": "off", "pump_reason": "auto"
```

`pump_reason` là lý do của lần chuyển trạng thái gần nhất (`null` nếu từ khi khởi
động bơm chưa chuyển lần nào). `device_info` (BLE/AP) có thêm `battery_source` và
`pump_simulated`.

## 4. Kịch bản nghiệm thu khi chưa có bơm và pin

Build với `SAEMS_BATTERY_SOURCE_SIMULATED=y` (giữ `SAEMS_PUMP_SIMULATED=y`), flash,
mở `idf.py monitor`. Để không phải chờ lâu khi thử, đặt thêm
`SAEMS_BATTERY_LOW_INTERVAL_SECONDS=30` và `SAEMS_BATTERY_CRITICAL_INTERVAL_SECONDS=60`,
và chu kỳ lấy mẫu 5 s trong app.

1. **Tưới tự động:** App → Thiết bị → Ngưỡng & chu kỳ → bật "Tưới khi đất dưới",
   đặt ngưỡng cao hơn độ ẩm đất hiện tại (ví dụ 99 %), bơm 10 s, nghỉ 1 phút, rồi lưu.
   - Log: `Auto watering: soil …% < 99%`, `Pump ON (auto) for 10 s [simulated pump]`,
     rồi `Pump OFF (auto)`.
   - Relay CH1 kêu tách hai lần, telemetry có `pump_reason":"auto"`.
   - Trong 1 phút nghỉ không bật lại; sau 1 phút bật lại nếu đất vẫn dưới ngưỡng.
2. **Dừng sớm khi đủ ẩm:** đặt bơm 60 s, chu kỳ 5 s. Khi bơm đang chạy, hạ ngưỡng
   xuống thấp hơn độ ẩm hiện tại ít nhất 5 % → ở mẫu kế tiếp bơm tắt
   (`Pump OFF (auto)`), không chờ hết 60 s.
3. **Ngắt an toàn:** bơm tay 600 s → sau 60 s firmware tắt, `pump_reason="safety_off"`.
4. **Pin yếu:** gửi lệnh mô phỏng pin 15 %:
   - Server: `POST /api/v1/devices/farm-01/commands/battery-sim` với `{"pct": 15}`
   - hoặc BLE: ghi `{"command":"battery_sim","pct":15}` vào characteristic lệnh;
     AP: `POST http://192.168.4.1/v1/commands/pump` với cùng nội dung.

   Log `Battery level normal -> low`, thiết bị chuyển Deep Sleep, chu kỳ ≥ 600 s,
   telemetry `battery_state":"low"`. Nếu `battery_min` = 20, Telegram báo pin thấp.
5. **Pin cạn:** mô phỏng 3 % trong lúc bơm đang chạy → bơm tắt
   (`pump_reason="low_battery"`), lệnh bật bơm bị từ chối, chu kỳ 3600 s.
6. **Hồi pin:** mô phỏng 50 % → `battery_state":"normal"`, `power_mode` cũ quay lại.

Ở Deep Sleep, thiết bị chỉ nhận lệnh MQTT trong cửa sổ thức (10–15 s sau mỗi mẫu).
Muốn đổi nhanh, dùng BLE trong cửa sổ thức hoặc đặt chu kỳ ngắn trước khi thử.

## 5. Khi lắp phần cứng thật

1. Nối relay và bơm theo mục 1. Chạy thử với bơm tay 5 s trước.
2. `SAEMS_PUMP_SIMULATED=n`.
3. Nối cầu chia áp và đo điện áp pin bằng đồng hồ. So với log `Battery ADC …` và
   `battery_pct`; nếu lệch, chỉnh `SAEMS_BATTERY_DIVIDER_X100` theo điện trở thực tế.
4. `SAEMS_BATTERY_SOURCE_ADC=y`, build và flash.

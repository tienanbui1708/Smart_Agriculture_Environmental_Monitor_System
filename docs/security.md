# Bảo mật cơ bản và đăng nhập

Tài liệu cho GĐ E trong [`UPDATE-PLAN.md`](UPDATE-PLAN.md).

## Tóm tắt

| Lớp | Biện pháp |
|---|---|
| Wi-Fi thiết bị | Chỉ nối mạng WPA2-PSK trở lên; từ chối mạng mở/WEP và mật khẩu < 8 ký tự |
| Wi-Fi AP của thiết bị | WPA2, mật khẩu ≥ 8 ký tự (như trước) |
| REST cục bộ (AP) | Chỉ phục vụ request đến qua địa chỉ AP 192.168.4.1; mọi endpoint trừ `/v1/health` cần `Authorization: Bearer <mã thiết bị>` |
| BLE | Ghép đôi LE Secure Connections + passkey 6 số (chống MITM); telemetry, cấu hình, lệnh cần link đã xác thực |
| MQTT | TLS qua Tailscale Funnel, tài khoản + ACL riêng cho thiết bị và server (từ trước); thiết bị không nhận credential qua MQTT |
| API server | Đăng nhập user/mật khẩu → token phiên có hạn; API token cũ vẫn dùng cho script |
| App | Màn hình đăng nhập; token phiên, mã thiết bị lưu trong Android Keystore (`flutter_secure_storage`); mật khẩu không lưu |

## 1. Thiết bị (firmware)

### Wi-Fi

- `threshold.authmode = WPA2_PSK`: ESP32 không nối mạng mở/WEP, kể cả khi có AP giả
  cùng SSID nhưng bảo mật yếu hơn.
- Ghi cấu hình có `wifi_ssid` mà mật khẩu dưới 8 ký tự bị từ chối (`ESP_ERR_INVALID_ARG`).

### REST trên Wi-Fi AP

- HTTP server lắng nghe mọi giao diện, nên firmware kiểm tra địa chỉ local của socket.
  Request đến qua mạng Wi-Fi gia đình (STA) nhận **403**.
- Mã thiết bị mặc định là **mật khẩu Wi-Fi AP**: ai đã được cấp mật khẩu AP thì dùng
  luôn được. Muốn tách riêng, ghi `"ap_api_token":"<8-64 ký tự>"` qua BLE/AP.
  Mã này chỉ ghi được, không bao giờ được trả về khi đọc cấu hình, và không đổi được
  qua MQTT.
- So sánh mã theo thời gian hằng (constant time).
- Script Windows (`power_mode.ps1`, `soil_calibration.ps1`) đọc mã từ
  `$env:SAEMS_DEVICE_TOKEN`; không có thì hỏi bạn nhập (nhập ẩn).

### BLE

- Mỗi thiết bị tự sinh **passkey 6 số** ở lần chạy đầu, lưu NVS (`ble_pk`), và in ra
  log serial mỗi lần khởi động:
  `BLE pairing passkey: 123456`. Đổi bằng `"ble_passkey":654321` qua BLE/AP (không
  đổi được qua MQTT).
- ESP32 khai báo "chỉ hiển thị" (DisplayOnly), MITM, Secure Connections. Android hiện
  hộp thoại để người dùng nhập passkey.
- `device_info` vẫn đọc được trước khi ghép (để app nhận ra thiết bị, có trường
  `"pairing":"passkey"`). Telemetry (đọc và notify), cấu hình và lệnh cần link đã
  xác thực.
- Điện thoại từng ghép với firmware cũ (Just Works) sẽ được ghép lại: firmware xóa bond
  cũ khi điện thoại yêu cầu ghép lại. Nếu Android vẫn báo lỗi, vào **Cài đặt
  Bluetooth → SAEMS-farm-01 → Hủy ghép đôi**, rồi kết nối lại từ app.
- Tắt được bằng `SAEMS_BLE_PASSKEY_PAIRING=n` (quay về Just Works). Chỉ nên tắt khi
  debug.

## 2. Server

- `POST /auth/login` `{"username","password"}` → `{"access_token","expires_at"}`.
  Token ngẫu nhiên 256 bit; server chỉ lưu SHA-256 của token trong `data/auth.db`.
  Mặc định hết hạn sau 30 ngày (`SAEMS_SESSION_TTL_HOURS`).
- `POST /auth/logout` thu hồi ngay. `GET /auth/me` cho biết ai đang đăng nhập.
- Mật khẩu admin lưu dạng PBKDF2-SHA256 với 310.000 vòng (`SAEMS_ADMIN_PASSWORD_HASH`).
  Đặt mật khẩu trên Pi:

  ```bash
  cd ~/saems-mini-server && .venv/bin/python auth.py set-password --user admin
  sudo systemctl restart saems-api
  ```

- Sai 5 lần trong 15 phút → khóa 5 phút (HTTP 429). Qua Tailscale Funnel mọi request
  đều đến từ 127.0.0.1, nên khóa áp dụng chung cho cả server. Đổi lại, kẻ dò mật khẩu
  không thể thử nhanh.
- API token cũ (`API_ACCESS_TOKEN`) vẫn hợp lệ, để script và app bản cũ không bị ngắt.
  Khi mọi máy đã đăng nhập bằng tài khoản, có thể đổi token này.
- Dashboard web: HTTP Basic với user admin + mật khẩu (kết quả kiểm tra được nhớ 10
  phút để không phải tính PBKDF2 ở mỗi request), hoặc `saems` + API token.

## 3. App

- Chọn nguồn Server mà chưa có thông tin đăng nhập → app hiện **màn hình đăng nhập**.
  App đã lưu API token từ trước thì vẫn chạy như cũ.
- **Cài đặt → tài khoản:** đăng nhập, đăng xuất (thu hồi token trên server). API token
  thủ công nằm ở mục "nâng cao".
- **Cài đặt → Mã thiết bị (Wi-Fi AP):** nhập mật khẩu AP (hoặc `ap_api_token`).
- Gặp lỗi 401 từ server, dashboard hiện nút **Đăng nhập lại**.
- BLE: trên Android, app ghép đôi trước khi bật notify; Android hỏi passkey ở lần đầu.

## 4. Nghiệm thu 🔧

1. Pi: đặt mật khẩu admin, restart `saems-api`. `POST /auth/login` sai mật khẩu → 401;
   đúng → token; dùng token gọi `/api/v1/devices` → 200; logout rồi gọi lại → 401.
2. App: Cài đặt → Đăng xuất → màn hình đăng nhập → đăng nhập → dashboard có dữ liệu.
3. Flash firmware, mở monitor, ghi lại `BLE pairing passkey`. App → BLE → Android hỏi
   passkey: nhập sai → không đọc được; nhập đúng → có telemetry.
4. Laptop nối AP `SAEMS-farm-01`:
   `curl http://192.168.4.1/v1/config` → 401;
   `curl -H "Authorization: Bearer <mật khẩu AP>" http://192.168.4.1/v1/config` → 200.
   Từ máy khác trong LAN gọi `http://<IP STA của ESP32>/v1/config` → 403.
5. Qua BLE đặt `wifi_ssid` của một mạng mở với mật khẩu rỗng → bị từ chối.

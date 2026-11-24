# SAEMS API contract

This document records the API that exists in `services/mini-server/api.py` and
the additive telemetry fields expected by the mobile client. Fields not yet
emitted by the server are optional so existing ESP32 payloads remain compatible.

## Existing endpoints

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/health` | API/database readiness |
| GET | `/api/v1/devices` | Known devices and their latest telemetry |
| GET | `/api/v1/devices/{device_id}/status` | Online state and latest telemetry |
| GET | `/api/v1/devices/{device_id}/telemetry/latest` | Latest telemetry object |
| GET | `/api/v1/devices/{device_id}/telemetry?limit=100&from=&to=` | Ordered history |
| POST | `/api/v1/devices/{device_id}/commands/pump` | Queue MQTT pump command |
| GET/PUT | `/api/v1/devices/{device_id}/configuration` | Server-managed thresholds, interval, power mode, auto watering (PUT replaces the whole document and publishes it retained on `saems/{id}/config`) |
| GET | `/api/v1/alerts?state=active` | Threshold/connectivity alerts of all devices |
| GET | `/api/v1/devices/{device_id}/alerts` | Alerts of one device |
| POST | `/api/v1/notifications/test` | Send a Telegram test message |
| POST | `/api/v1/devices/{device_id}/commands/battery-sim` | Demo: set a simulated battery level `{"pct": 15}` |

Pump command body:

```json
{"state":"on","duration_s":60}
```

Configuration body (every key optional; `null` = not managed by the server):

```json
{
  "telemetry_interval_s": 300,
  "power_mode": "light",
  "offline_alert_after_s": null,
  "thresholds": {"temperature_c": {"min": 10, "max": 35}, "battery_pct": {"min": 20}},
  "auto_watering": {"enabled": true, "soil_moisture_threshold_pct": 30,
                    "pump_max_runtime_sec": 30, "pump_min_rest_sec": 1800}
}
```

The device receives the compact form on MQTT: `thresholds` as a 9-number array
and `irrigation` as `[enabled, threshold_pct, max_runtime_s, min_rest_s]`
(see `firmware/esp32s3/components/{threshold_monitor,irrigation}`).

## Telemetry schema v1

Required for new producers:

- `schema_version`: integer, currently `1`.
- `device_id`: stable device identifier.
- `timestamp`: ISO-8601 UTC timestamp, hoặc `null` khi firmware chưa đồng bộ giờ.
- `timestamp_source`: `system` hoặc `unsynced`; server dùng thời gian nhận bản tin
  khi timestamp là `null`.
- `uptime_ms`: thời gian chạy của firmware từ lần boot hiện tại.
- `data_mode`: `simulated` or `real`.

Sensor values are nullable because a missing/disconnected sensor must not be
reported as a real zero:

- `temperature_c`, `humidity_pct`, `soil_moisture_pct`, `light_lux`
- `battery_pct`, `pump_state`
- `battery_source` (`none`/`adc`/`simulated`), `battery_state`
  (`unknown`/`normal`/`low`/`critical`), `pump_reason` (`auto`/`manual`/
  `safety_off`/`low_battery`/`null`) — see `docs/pump-battery.md`
- `config_version` (server configuration applied, `0` = local) and `alarm_mask`
  (threshold alarm bits, order in `threshold_monitor.h`)
- `sensor_health`: `ok`, `degraded`, or `unavailable`
- `sensor_status`: trạng thái `dht20`, `bh1750`, `soil` riêng lẻ; mỗi giá trị là
  `ok`, `error`, `unavailable`, hoặc `uncalibrated` (soil đọc được ADC nhưng chưa
  có hai mốc hiệu chuẩn đã xác nhận; phần trăm vẫn `null`).
- `boot_id`, `seq`, `received_at`

The app accepts the current legacy response with no `schema_version`,
`data_mode`, or `sensor_health`. In that case it displays “Chưa xác nhận nguồn
dữ liệu”; it never silently marks legacy/simulated data as real.

See `docs/samples/telemetry-real-partial.json` and
`docs/samples/telemetry-simulated.json`.
# Internet authentication (2026-10-04)

Deployed base URL: https://admin.tailb1f0ba.ts.net. API clients send
Authorization: Bearer <api_access_token>. Dashboard uses HTTP Basic username
saems and the same token as password. Only /health is public; unauthorized
requests to telemetry, commands, docs and dashboard receive 401. Never include
tokens in URLs or compile them into APKs. See internet-and-provisioning.md for
runtime credentials, MQTT TLS and deployment/test limits.

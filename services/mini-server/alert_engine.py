"""Threshold and connectivity alerts.

The server evaluates telemetry against the configuration it owns (the same
limits it publishes to the device), with the same hysteresis as the
firmware threshold_monitor. Device-side alarm_mask is for offline/local
display; notifications come from here.

Records that arrive late through offline sync are stored as alerts with
backlog=1 but do not each send a Telegram message.
"""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
import os
from typing import Any, Callable

from device_config import METRICS, load_configuration

FRESH_SAMPLE_SECONDS = 600
DEFAULT_OFFLINE_AFTER_SECONDS = 900
CONNECTIVITY = "connectivity"
# Times in Telegram messages are shown in the farm's local time (Vietnam, UTC+7).
LOCAL_TZ = timezone(timedelta(hours=float(os.environ.get("SAEMS_UTC_OFFSET_HOURS", "7"))))

LABELS = {
    "temperature_c": ("Nhiệt độ", "°C"),
    "humidity_pct": ("Độ ẩm không khí", "%"),
    "soil_moisture_pct": ("Độ ẩm đất", "%"),
    "light_lux": ("Ánh sáng", "lux"),
    "battery_pct": ("Pin", "%"),
}
LEVEL_TEXT = {"low": "thấp hơn ngưỡng", "high": "cao hơn ngưỡng"}

Notify = Callable[[str], None]


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def parse_time(value: Any) -> datetime | None:
    try:
        parsed = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except (TypeError, ValueError):
        return None
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=timezone.utc)


def hysteresis(metric: str, limit: float) -> float:
    if metric == "temperature_c":
        return 0.5
    if metric == "light_lux":
        return max(10.0, abs(limit) * 0.05)
    return 2.0


def evaluate_level(metric: str, value: float | None, limits: dict, active: str | None) -> str | None:
    """Return 'low', 'high' or None, keeping an active alarm inside the band."""
    if value is None:
        return None
    low, high = limits.get("min"), limits.get("max")
    if low is not None:
        band = hysteresis(metric, low)
        if value < low or (active == "low" and value < low + band):
            return "low"
    if high is not None:
        band = hysteresis(metric, high)
        if value > high or (active == "high" and value > high - band):
            return "high"
    return None


def _reading(payload: dict, metric: str) -> float | None:
    value = payload.get(metric)
    if metric == "light_lux" and value is None:
        value = payload.get("light_value")
    if value is None or isinstance(value, bool):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _open_alert(connection, device_id, kind, metric, level, value, limit, message,
                sample_timestamp, backlog, notified) -> None:
    now = utc_now().isoformat()
    cursor = connection.execute(
        """
        INSERT INTO alerts (device_id, kind, metric, level, state, value, limit_value,
                            message, sample_timestamp, created_at, notified, backlog)
        VALUES (?, ?, ?, ?, 'active', ?, ?, ?, ?, ?, ?, ?)
        """,
        (device_id, kind, metric, level, value, limit, message, sample_timestamp,
         now, int(notified), int(backlog)),
    )
    connection.execute(
        """
        INSERT OR REPLACE INTO alert_state (device_id, metric, level, alert_id, since)
        VALUES (?, ?, ?, ?, ?)
        """,
        (device_id, metric, level, cursor.lastrowid, now),
    )


def _resolve_alert(connection, device_id, metric, alert_id) -> None:
    connection.execute(
        "UPDATE alerts SET state = 'resolved', resolved_at = ? WHERE id = ?",
        (utc_now().isoformat(), alert_id),
    )
    connection.execute(
        "DELETE FROM alert_state WHERE device_id = ? AND metric = ?", (device_id, metric)
    )


def _active_states(connection, device_id: str) -> dict[str, tuple[str, int]]:
    rows = connection.execute(
        "SELECT metric, level, alert_id FROM alert_state WHERE device_id = ?", (device_id,)
    ).fetchall()
    return {row[0]: (row[1], int(row[2])) for row in rows}


def process_telemetry(connection, device_id: str, payload: dict, notify: Notify,
                      received_at: datetime | None = None) -> list[str]:
    """Update alert state for one new telemetry record; return sent messages.

    The caller owns the transaction and commits.
    """
    received_at = received_at or utc_now()
    sample_time = parse_time(payload.get("timestamp"))
    backlog = sample_time is not None and (
        received_at - sample_time).total_seconds() > FRESH_SAMPLE_SECONDS
    sample_timestamp = sample_time.isoformat() if sample_time else None
    _version, configuration, _updated = load_configuration(connection, device_id)
    states = _active_states(connection, device_id)
    messages: list[str] = []

    def emit(text: str) -> bool:
        if backlog:
            return False
        notify(text)
        messages.append(text)
        return True

    if CONNECTIVITY in states:
        _level, alert_id = states[CONNECTIVITY]
        _resolve_alert(connection, device_id, CONNECTIVITY, alert_id)
        # Reconnection is about "now", so announce it even for backlog records.
        text = f"✅ {device_id}: thiết bị đã kết nối lại server."
        notify(text)
        messages.append(text)

    for metric, *_rest in METRICS:
        limits = configuration["thresholds"][metric]
        value = _reading(payload, metric)
        active = states.get(metric)
        active_level = active[0] if active else None
        level = evaluate_level(metric, value, limits, active_level)
        if level == active_level:
            continue
        label, unit = LABELS[metric]
        if active is not None:
            _resolve_alert(connection, device_id, metric, active[1])
            if level is None and value is not None:
                emit(f"✅ {device_id}: {label} đã trở lại bình thường ({value:g} {unit}).")
            elif level is None:
                emit(f"ℹ️ {device_id}: {label} không còn dữ liệu cảm biến; đóng cảnh báo.")
        if level is not None:
            limit = limits["min"] if level == "low" else limits["max"]
            text = (
                f"⚠️ {device_id}: {label} {value:g} {unit} {LEVEL_TEXT[level]} "
                f"{limit:g} {unit}."
            )
            if backlog:
                text += f" (dữ liệu gửi bù lúc {sample_timestamp})"
            notified = emit(text)
            _open_alert(connection, device_id, "threshold", metric, level, value, limit,
                        text, sample_timestamp, backlog, notified)
    return messages


def format_duration(seconds: float) -> str:
    """Human Vietnamese duration: '45 ngày 3 giờ', '2 giờ 5 phút', '7 phút'."""
    total_minutes = max(int(seconds // 60), 0)
    days, rest = divmod(total_minutes, 24 * 60)
    hours, minutes = divmod(rest, 60)
    if days:
        return f"{days} ngày {hours} giờ" if hours else f"{days} ngày"
    if hours:
        return f"{hours} giờ {minutes} phút" if minutes else f"{hours} giờ"
    return f"{minutes} phút"


def offline_after_seconds(configuration: dict) -> int:
    if configuration.get("offline_alert_after_s"):
        return int(configuration["offline_alert_after_s"])
    interval = configuration.get("telemetry_interval_s")
    if interval:
        return max(3 * int(interval) + 60, 300)
    return DEFAULT_OFFLINE_AFTER_SECONDS


def check_offline(connection, notify: Notify, now: datetime | None = None) -> list[str]:
    """Open a connectivity alert for devices silent longer than allowed."""
    now = now or utc_now()
    rows = connection.execute(
        "SELECT device_id, MAX(received_at) FROM telemetry GROUP BY device_id"
    ).fetchall()
    messages: list[str] = []
    for device_id, last_seen in rows:
        last_time = parse_time(last_seen)
        if last_time is None:
            continue
        if CONNECTIVITY in _active_states(connection, device_id):
            continue
        _version, configuration, _updated = load_configuration(connection, device_id)
        limit = offline_after_seconds(configuration)
        silent = (now - last_time).total_seconds()
        if silent <= limit:
            continue
        local_seen = last_time.astimezone(LOCAL_TZ).strftime("%H:%M %d/%m/%Y")
        text = (
            f"🔴 {device_id}: mất kết nối với server {format_duration(silent)} "
            f"(lần cuối {local_seen}). "
            "Thiết bị vẫn lưu dữ liệu offline và sẽ gửi bù khi có mạng."
        )
        notify(text)
        messages.append(text)
        _open_alert(connection, device_id, "connectivity", CONNECTIVITY, "offline", None,
                    float(limit), text, last_time.isoformat(), False, True)
    return messages


def list_alerts(connection, device_id: str | None, state: str | None, limit: int) -> list[dict]:
    conditions, parameters = [], []
    if device_id:
        conditions.append("device_id = ?")
        parameters.append(device_id)
    if state:
        conditions.append("state = ?")
        parameters.append(state)
    where = f"WHERE {' AND '.join(conditions)}" if conditions else ""
    rows = connection.execute(
        f"""
        SELECT id, device_id, kind, metric, level, state, value, limit_value, message,
               sample_timestamp, created_at, resolved_at, notified, backlog
        FROM alerts {where} ORDER BY id DESC LIMIT ?
        """,
        [*parameters, limit],
    ).fetchall()
    keys = ("id", "device_id", "kind", "metric", "level", "state", "value", "limit_value",
            "message", "sample_timestamp", "created_at", "resolved_at", "notified", "backlog")
    items = []
    for row in rows:
        item = dict(zip(keys, row))
        item["notified"] = bool(item["notified"])
        item["backlog"] = bool(item["backlog"])
        items.append(item)
    return items

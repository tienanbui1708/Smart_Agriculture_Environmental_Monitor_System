"""Device configuration managed by the server.

The API stores one configuration per device and publishes it retained on
MQTT topic ``saems/{device_id}/config``. The firmware accepts only the keys
produced by :func:`device_document`; credentials and calibration stay
BLE/AP-only.
"""
from __future__ import annotations

import json
import math
from datetime import datetime, timezone
from typing import Any

# Order shared with firmware threshold_monitor.h and telemetry alarm_mask.
METRICS = (
    # name,               unit,  minimum, maximum, has_max
    ("temperature_c", "°C", -40.0, 85.0, True),
    ("humidity_pct", "%", 0.0, 100.0, True),
    ("soil_moisture_pct", "%", 0.0, 100.0, True),
    ("light_lux", "lux", 0.0, 100000.0, True),
    ("battery_pct", "%", 0.0, 100.0, False),
)
METRIC_NAMES = tuple(metric[0] for metric in METRICS)
POWER_MODES = ("direct", "light", "battery")
INTERVAL_RANGE = (1, 3600)
OFFLINE_ALERT_RANGE = (60, 86400)
# Same limits as firmware irrigation.h. The device additionally caps one
# pump run at CONFIG_SAEMS_RELAY_MAX_ON_SECONDS (60 s by default).
WATERING_THRESHOLD_RANGE = (1, 95)
WATERING_RUNTIME_RANGE = (1, 3600)
WATERING_REST_RANGE = (0, 86400)
AUTO_WATERING_KEYS = (
    "enabled", "soil_moisture_threshold_pct", "pump_max_runtime_sec", "pump_min_rest_sec",
)


def empty_thresholds() -> dict[str, dict[str, float | None]]:
    thresholds: dict[str, dict[str, float | None]] = {}
    for name, _unit, _minimum, _maximum, has_max in METRICS:
        thresholds[name] = {"min": None, "max": None} if has_max else {"min": None}
    return thresholds


def default_configuration() -> dict[str, Any]:
    """Nothing is managed until the user saves a value."""
    return {
        "telemetry_interval_s": None,
        "power_mode": None,
        "offline_alert_after_s": None,
        "thresholds": empty_thresholds(),
        "auto_watering": None,
    }


def _optional_number(value: Any, field: str, minimum: float, maximum: float):
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{field} must be a number or null")
    number = float(value)
    if not math.isfinite(number) or number < minimum or number > maximum:
        raise ValueError(f"{field} must be between {minimum:g} and {maximum:g}")
    # The device stores one decimal; keep the server value identical.
    return round(number, 1)


def _optional_int(value: Any, field: str, bounds: tuple[int, int]):
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{field} must be an integer or null")
    if not bounds[0] <= value <= bounds[1]:
        raise ValueError(f"{field} must be between {bounds[0]} and {bounds[1]}")
    return value


def _required_int(value: Any, field: str, bounds: tuple[int, int]) -> int:
    if value is None:
        raise ValueError(f"{field} is required")
    return _optional_int(value, field, bounds)


def validate_auto_watering(value: Any) -> dict[str, Any] | None:
    """Pulse watering: run up to pump_max_runtime_sec when soil is below the
    threshold, then rest pump_min_rest_sec before the next automatic run."""
    if value is None:
        return None
    if not isinstance(value, dict):
        raise ValueError("auto_watering must be an object or null")
    unknown = set(value) - set(AUTO_WATERING_KEYS)
    if unknown:
        raise ValueError(f"unknown auto_watering keys: {', '.join(sorted(unknown))}")
    enabled = value.get("enabled")
    if not isinstance(enabled, bool):
        raise ValueError("auto_watering.enabled must be true or false")
    return {
        "enabled": enabled,
        "soil_moisture_threshold_pct": _required_int(
            value.get("soil_moisture_threshold_pct"),
            "auto_watering.soil_moisture_threshold_pct", WATERING_THRESHOLD_RANGE),
        "pump_max_runtime_sec": _required_int(
            value.get("pump_max_runtime_sec"),
            "auto_watering.pump_max_runtime_sec", WATERING_RUNTIME_RANGE),
        "pump_min_rest_sec": _required_int(
            value.get("pump_min_rest_sec"),
            "auto_watering.pump_min_rest_sec", WATERING_REST_RANGE),
    }


def auto_watering_to_array(value: dict[str, Any]) -> list[int]:
    """Firmware order: [enabled, start_below_pct, max_runtime_s, min_rest_s]."""
    return [
        1 if value["enabled"] else 0,
        value["soil_moisture_threshold_pct"],
        value["pump_max_runtime_sec"],
        value["pump_min_rest_sec"],
    ]


def validate_configuration(document: Any) -> dict[str, Any]:
    """Return a normalized configuration or raise ValueError.

    Missing keys keep their default (unmanaged) value, so a client may send
    only the parts it changes.
    """
    if not isinstance(document, dict):
        raise ValueError("configuration must be a JSON object")
    allowed = {"telemetry_interval_s", "power_mode", "offline_alert_after_s", "thresholds",
               "auto_watering"}
    unknown = set(document) - allowed
    if unknown:
        raise ValueError(f"unknown configuration keys: {', '.join(sorted(unknown))}")

    result = default_configuration()
    result["telemetry_interval_s"] = _optional_int(
        document.get("telemetry_interval_s"), "telemetry_interval_s", INTERVAL_RANGE
    )
    result["offline_alert_after_s"] = _optional_int(
        document.get("offline_alert_after_s"), "offline_alert_after_s", OFFLINE_ALERT_RANGE
    )
    power_mode = document.get("power_mode")
    if power_mode is not None and power_mode not in POWER_MODES:
        raise ValueError(f"power_mode must be one of {', '.join(POWER_MODES)} or null")
    result["power_mode"] = power_mode
    result["auto_watering"] = validate_auto_watering(document.get("auto_watering"))

    thresholds = document.get("thresholds") or {}
    if not isinstance(thresholds, dict):
        raise ValueError("thresholds must be an object")
    unknown = set(thresholds) - set(METRIC_NAMES)
    if unknown:
        raise ValueError(f"unknown threshold metrics: {', '.join(sorted(unknown))}")
    for name, _unit, minimum, maximum, has_max in METRICS:
        limits = thresholds.get(name) or {}
        if not isinstance(limits, dict):
            raise ValueError(f"thresholds.{name} must be an object")
        allowed_limits = {"min", "max"} if has_max else {"min"}
        if set(limits) - allowed_limits:
            raise ValueError(f"thresholds.{name} accepts only {', '.join(sorted(allowed_limits))}")
        low = _optional_number(limits.get("min"), f"thresholds.{name}.min", minimum, maximum)
        result["thresholds"][name]["min"] = low
        if has_max:
            high = _optional_number(limits.get("max"), f"thresholds.{name}.max", minimum, maximum)
            if low is not None and high is not None and not low < high:
                raise ValueError(f"thresholds.{name}.min must be below max")
            result["thresholds"][name]["max"] = high
    return result


def thresholds_to_array(thresholds: dict[str, dict[str, float | None]]) -> list[float | None]:
    """Firmware order: [t_min, t_max, h_min, h_max, s_min, s_max, l_min, l_max, b_min]."""
    values: list[float | None] = []
    for name, _unit, _minimum, _maximum, has_max in METRICS:
        values.append(thresholds[name]["min"])
        if has_max:
            values.append(thresholds[name]["max"])
    return values


def device_document(configuration: dict[str, Any], version: int) -> dict[str, Any]:
    """Payload for the retained MQTT config topic.

    Unmanaged (null) interval/power mode/auto_watering are omitted so the
    device keeps its local value; thresholds are always sent once the server
    owns them.
    """
    document: dict[str, Any] = {"config_version": version}
    if configuration["telemetry_interval_s"] is not None:
        document["telemetry_interval_s"] = configuration["telemetry_interval_s"]
    if configuration["power_mode"] is not None:
        document["power_mode"] = configuration["power_mode"]
    document["thresholds"] = thresholds_to_array(configuration["thresholds"])
    if configuration.get("auto_watering") is not None:
        document["irrigation"] = auto_watering_to_array(configuration["auto_watering"])
    return document


def load_configuration(connection, device_id: str) -> tuple[int, dict[str, Any], str | None]:
    row = connection.execute(
        "SELECT version, config_json, updated_at FROM device_config WHERE device_id = ?",
        (device_id,),
    ).fetchone()
    if row is None:
        return 0, default_configuration(), None
    try:
        configuration = validate_configuration(json.loads(row[1]))
    except (ValueError, TypeError):
        configuration = default_configuration()
    return int(row[0]), configuration, row[2]


def store_configuration(connection, device_id: str, configuration: dict[str, Any]) -> tuple[int, str]:
    """Insert or update without committing; the caller commits after MQTT publish."""
    current = connection.execute(
        "SELECT version FROM device_config WHERE device_id = ?", (device_id,)
    ).fetchone()
    version = (int(current[0]) if current else 0) + 1
    updated_at = datetime.now(timezone.utc).isoformat()
    connection.execute(
        """
        INSERT INTO device_config (device_id, version, config_json, updated_at)
        VALUES (?, ?, ?, ?)
        ON CONFLICT(device_id) DO UPDATE SET
            version = excluded.version,
            config_json = excluded.config_json,
            updated_at = excluded.updated_at
        """,
        (device_id, version, json.dumps(configuration, separators=(",", ":")), updated_at),
    )
    return version, updated_at

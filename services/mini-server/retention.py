"""Telemetry indexes, hourly roll-up and retention (SQLite).

- Indexes for the API's per-device time queries.
- telemetry_hourly: per device and UTC hour, sample count and avg/min/max of
  each metric. Hours that receive rows after the last run (including late
  offline batches) are recomputed, so the roll-up stays exact.
- Raw rows older than SAEMS_RAW_RETENTION_DAYS (default 180) are deleted,
  but only after their hour has been rolled up. Resolved alerts older than
  SAEMS_ALERT_RETENTION_DAYS (default 365) are deleted.

The ingestor runs run_maintenance() at start and every 6 hours.
"""
from __future__ import annotations

from contextlib import closing
from datetime import datetime, timedelta, timezone
import os
from pathlib import Path
import sqlite3

RAW_RETENTION_DAYS = int(os.getenv("SAEMS_RAW_RETENTION_DAYS", "180"))
ALERT_RETENTION_DAYS = int(os.getenv("SAEMS_ALERT_RETENTION_DAYS", "365"))
METRICS = (
    ("temperature", "temperature_c"),
    ("humidity", "humidity_pct"),
    ("soil", "soil_moisture_pct"),
    ("light", "light_value"),
    ("battery", "battery_pct"),
)
# Device time when it was synchronized, otherwise the server receive time.
SAMPLE_HOUR = "substr(COALESCE(NULLIF(timestamp, ''), received_at), 1, 13)"


def ensure_schema(connection) -> None:
    connection.execute(
        "CREATE INDEX IF NOT EXISTS ix_telemetry_device_received "
        "ON telemetry(device_id, received_at)")
    connection.execute(
        "CREATE INDEX IF NOT EXISTS ix_telemetry_device_timestamp "
        "ON telemetry(device_id, timestamp)")
    columns = ",\n".join(
        f"{name}_avg REAL, {name}_min REAL, {name}_max REAL" for name, _ in METRICS)
    connection.execute(
        f"""
        CREATE TABLE IF NOT EXISTS telemetry_hourly (
            device_id TEXT NOT NULL,
            hour TEXT NOT NULL,
            samples INTEGER NOT NULL,
            {columns},
            PRIMARY KEY (device_id, hour)
        )
        """
    )
    connection.execute(
        "CREATE TABLE IF NOT EXISTS maintenance_state (key TEXT PRIMARY KEY, value TEXT NOT NULL)")


def _state(connection, key: str, default: str) -> str:
    row = connection.execute("SELECT value FROM maintenance_state WHERE key = ?", (key,)).fetchone()
    return row[0] if row else default


def _set_state(connection, key: str, value: str) -> None:
    connection.execute(
        "INSERT INTO maintenance_state (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value", (key, value))


def roll_up(connection) -> int:
    """Recompute every (device, hour) that received rows since the last run."""
    last_id = int(_state(connection, "hourly_last_id", "0"))
    max_id = connection.execute("SELECT COALESCE(MAX(id), 0) FROM telemetry").fetchone()[0]
    if max_id <= last_id:
        return 0
    touched = connection.execute(
        f"SELECT DISTINCT device_id, {SAMPLE_HOUR} FROM telemetry WHERE id > ? AND id <= ?",
        (last_id, max_id)).fetchall()
    aggregates = ", ".join(
        f"AVG({column}), MIN({column}), MAX({column})" for _, column in METRICS)
    names = ", ".join(f"{name}_avg, {name}_min, {name}_max" for name, _ in METRICS)
    for device_id, hour in touched:
        connection.execute(
            f"""
            INSERT OR REPLACE INTO telemetry_hourly (device_id, hour, samples, {names})
            SELECT ?, ?, COUNT(*), {aggregates}
            FROM telemetry WHERE device_id = ? AND {SAMPLE_HOUR} = ?
            """,
            (device_id, hour, device_id, hour))
    _set_state(connection, "hourly_last_id", str(max_id))
    return len(touched)


def purge(connection, now: datetime) -> tuple[int, int]:
    """Delete rolled-up raw rows and old resolved alerts."""
    rolled_up_through = int(_state(connection, "hourly_last_id", "0"))
    raw_cutoff = (now - timedelta(days=RAW_RETENTION_DAYS)).isoformat()
    raw = connection.execute(
        "DELETE FROM telemetry WHERE received_at < ? AND id <= ?",
        (raw_cutoff, rolled_up_through)).rowcount
    alert_cutoff = (now - timedelta(days=ALERT_RETENTION_DAYS)).isoformat()
    alerts = 0
    if connection.execute(
            "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'alerts'").fetchone():
        alerts = connection.execute(
            "DELETE FROM alerts WHERE state = 'resolved' AND resolved_at < ?",
            (alert_cutoff,)).rowcount
    return raw, alerts


def run_maintenance(db_path: Path, now: datetime | None = None) -> dict[str, int]:
    now = now or datetime.now(timezone.utc)
    with closing(sqlite3.connect(db_path, timeout=30)) as connection:
        if not connection.execute(
                "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'telemetry'"
        ).fetchone():
            return {"hours": 0, "raw_deleted": 0, "alerts_deleted": 0}
        ensure_schema(connection)
        hours = roll_up(connection)
        raw, alerts = purge(connection, now)
        connection.commit()
    return {"hours": hours, "raw_deleted": raw, "alerts_deleted": alerts}

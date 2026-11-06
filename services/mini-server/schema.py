"""SQLite tables for configuration and alerts, shared by API and ingestor.

The telemetry table stays owned by mqtt_ingestor.initialize_database().
"""


def ensure_schema(connection) -> None:
    connection.execute(
        """
        CREATE TABLE IF NOT EXISTS device_config (
            device_id TEXT PRIMARY KEY,
            version INTEGER NOT NULL,
            config_json TEXT NOT NULL,
            updated_at TEXT NOT NULL
        )
        """
    )
    connection.execute(
        """
        CREATE TABLE IF NOT EXISTS alerts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL,
            kind TEXT NOT NULL,
            metric TEXT NOT NULL,
            level TEXT NOT NULL,
            state TEXT NOT NULL,
            value REAL,
            limit_value REAL,
            message TEXT NOT NULL,
            sample_timestamp TEXT,
            created_at TEXT NOT NULL,
            resolved_at TEXT,
            notified INTEGER NOT NULL DEFAULT 0,
            backlog INTEGER NOT NULL DEFAULT 0
        )
        """
    )
    connection.execute(
        "CREATE INDEX IF NOT EXISTS ix_alerts_device_created ON alerts(device_id, created_at)"
    )
    # One open alert per device and metric; hysteresis state survives restarts.
    connection.execute(
        """
        CREATE TABLE IF NOT EXISTS alert_state (
            device_id TEXT NOT NULL,
            metric TEXT NOT NULL,
            level TEXT NOT NULL,
            alert_id INTEGER NOT NULL,
            since TEXT NOT NULL,
            PRIMARY KEY (device_id, metric)
        )
        """
    )

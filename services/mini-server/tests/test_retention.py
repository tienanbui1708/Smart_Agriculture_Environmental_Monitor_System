"""Hourly roll-up and retention (temporary database)."""
from contextlib import closing
from datetime import datetime, timedelta, timezone
from pathlib import Path
import sqlite3
import tempfile
import unittest

import mqtt_ingestor
import retention

NOW = datetime(2026, 11, 24, 12, 0, tzinfo=timezone.utc)


class RetentionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.saved = (mqtt_ingestor.DATA_DIR, mqtt_ingestor.DB_PATH)
        mqtt_ingestor.DATA_DIR = Path(self.temp.name)
        mqtt_ingestor.DB_PATH = Path(self.temp.name) / "telemetry.db"
        mqtt_ingestor.initialize_database()
        self.db = mqtt_ingestor.DB_PATH

    def tearDown(self):
        mqtt_ingestor.DATA_DIR, mqtt_ingestor.DB_PATH = self.saved
        self.temp.cleanup()

    def insert(self, seq, when, temperature, received=None):
        with closing(sqlite3.connect(self.db)) as connection:
            connection.execute(
                "INSERT INTO telemetry (device_id, boot_id, seq, timestamp, received_at, "
                "temperature_c, raw_json) VALUES ('farm-01', 'b1', ?, ?, ?, ?, '{}')",
                (seq, when.isoformat(), (received or when).isoformat(), temperature))
            connection.commit()

    def hourly(self):
        with closing(sqlite3.connect(self.db)) as connection:
            return connection.execute(
                "SELECT hour, samples, temperature_avg, temperature_min, temperature_max "
                "FROM telemetry_hourly ORDER BY hour").fetchall()

    def test_indexes_exist(self):
        with closing(sqlite3.connect(self.db)) as connection:
            names = {row[0] for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type = 'index'")}
        self.assertIn("ix_telemetry_device_received", names)
        self.assertIn("ix_telemetry_device_timestamp", names)

    def test_roll_up_and_late_backlog(self):
        hour = datetime(2026, 11, 24, 9, 0, tzinfo=timezone.utc)
        self.insert(1, hour + timedelta(minutes=1), 20.0)
        self.insert(2, hour + timedelta(minutes=30), 24.0)
        self.insert(3, hour + timedelta(hours=1), 30.0)
        self.assertEqual(retention.run_maintenance(self.db, NOW)["hours"], 2)
        self.assertEqual(self.hourly(), [
            ("2026-11-24T09", 2, 22.0, 20.0, 24.0),
            ("2026-11-24T10", 1, 30.0, 30.0, 30.0),
        ])
        # Nothing new: no work.
        self.assertEqual(retention.run_maintenance(self.db, NOW)["hours"], 0)
        # An offline batch arrives later for 09:xx; that hour is recomputed.
        self.insert(4, hour + timedelta(minutes=45), 28.0, received=NOW)
        self.assertEqual(retention.run_maintenance(self.db, NOW)["hours"], 1)
        self.assertEqual(self.hourly()[0], ("2026-11-24T09", 3, 24.0, 20.0, 28.0))

    def test_purge_only_after_roll_up(self):
        old = NOW - timedelta(days=retention.RAW_RETENTION_DAYS + 5)
        self.insert(1, old, 18.0)
        self.insert(2, NOW - timedelta(days=1), 25.0)
        stats = retention.run_maintenance(self.db, NOW)
        self.assertEqual(stats["raw_deleted"], 1)
        with closing(sqlite3.connect(self.db)) as connection:
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM telemetry").fetchone()[0], 1)
        # The old hour survives as an aggregate.
        self.assertEqual(self.hourly()[0][1:3], (1, 18.0))

    def test_old_resolved_alerts_are_deleted(self):
        from schema import ensure_schema
        with closing(sqlite3.connect(self.db)) as connection:
            ensure_schema(connection)
            old = (NOW - timedelta(days=retention.ALERT_RETENTION_DAYS + 1)).isoformat()
            for state, resolved in (("resolved", old), ("active", None)):
                connection.execute(
                    "INSERT INTO alerts (device_id, kind, metric, level, state, message, "
                    "created_at, resolved_at) VALUES ('farm-01', 'threshold', 'temperature_c', "
                    "'high', ?, 'm', ?, ?)", (state, old, resolved))
            connection.commit()
        self.insert(1, NOW, 20.0)
        self.assertEqual(retention.run_maintenance(self.db, NOW)["alerts_deleted"], 1)

    def test_hourly_endpoint(self):
        try:
            from fastapi.testclient import TestClient
        except (ImportError, RuntimeError) as error:
            self.skipTest(f"TestClient unavailable: {error}")
        import api
        hour = datetime(2026, 11, 24, 9, 0, tzinfo=timezone.utc)
        self.insert(1, hour, 20.0)
        retention.run_maintenance(self.db, NOW)
        saved = (api.DB_PATH, api.API_ACCESS_TOKEN, api.ADMIN_PASSWORD_HASH)
        api.DB_PATH, api.API_ACCESS_TOKEN, api.ADMIN_PASSWORD_HASH = self.db, "", ""
        try:
            body = TestClient(api.app).get(
                "/api/v1/devices/farm-01/telemetry/hourly?from=2026-11-24T00").json()
        finally:
            api.DB_PATH, api.API_ACCESS_TOKEN, api.ADMIN_PASSWORD_HASH = saved
        self.assertEqual(body["count"], 1)
        self.assertEqual(body["items"][0]["hour"], "2026-11-24T09")
        self.assertEqual(body["items"][0]["temperature_avg"], 20.0)


if __name__ == "__main__":
    unittest.main()

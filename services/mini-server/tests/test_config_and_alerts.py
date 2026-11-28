"""Configuration validation and alert engine tests (no MQTT, no network).

Run from services/mini-server:
    python -m unittest discover -s tests
"""
from datetime import datetime, timedelta, timezone
import json
import sqlite3
import unittest

import alert_engine
import device_config
from schema import ensure_schema


def database():
    connection = sqlite3.connect(":memory:")
    connection.execute(
        """
        CREATE TABLE telemetry (
            id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT, boot_id TEXT,
            seq INTEGER, timestamp TEXT, received_at TEXT, raw_json TEXT
        )
        """
    )
    ensure_schema(connection)
    return connection


def configure(connection, document):
    configuration = device_config.validate_configuration(document)
    device_config.store_configuration(connection, "farm-01", configuration)
    connection.commit()


class ConfigurationTests(unittest.TestCase):
    def test_defaults_are_unmanaged(self):
        configuration = device_config.validate_configuration({})
        self.assertIsNone(configuration["telemetry_interval_s"])
        self.assertEqual(device_config.thresholds_to_array(configuration["thresholds"]),
                         [None] * 9)

    def test_partial_thresholds_and_rounding(self):
        configuration = device_config.validate_configuration(
            {"thresholds": {"temperature_c": {"max": 35.04}, "battery_pct": {"min": 20}}}
        )
        self.assertEqual(
            device_config.thresholds_to_array(configuration["thresholds"]),
            [None, 35.0, None, None, None, None, None, None, 20.0],
        )

    def test_rejects_invalid_values(self):
        invalid = [
            {"thresholds": {"humidity_pct": {"min": 60, "max": 40}}},
            {"thresholds": {"temperature_c": {"max": 120}}},
            {"thresholds": {"battery_pct": {"max": 90}}},
            {"thresholds": {"co2": {"max": 1}}},
            {"telemetry_interval_s": 0},
            {"telemetry_interval_s": True},
            {"power_mode": "turbo"},
            {"wifi_password": "secret"},
            {"auto_watering": {"enabled": True}},
            {"auto_watering": {"enabled": 1, "soil_moisture_threshold_pct": 30,
                               "pump_max_runtime_sec": 30, "pump_min_rest_sec": 600}},
            {"auto_watering": {"enabled": True, "soil_moisture_threshold_pct": 0,
                               "pump_max_runtime_sec": 30, "pump_min_rest_sec": 600}},
            {"auto_watering": {"enabled": True, "soil_moisture_threshold_pct": 30,
                               "pump_max_runtime_sec": 3601, "pump_min_rest_sec": 600}},
            {"auto_watering": {"enabled": True, "soil_moisture_threshold_pct": 30,
                               "pump_max_runtime_sec": 30, "pump_min_rest_sec": 600,
                               "pump_gpio": 4}},
        ]
        for document in invalid:
            with self.subTest(document=document):
                with self.assertRaises(ValueError):
                    device_config.validate_configuration(document)

    def test_device_document_omits_unmanaged_fields(self):
        configuration = device_config.validate_configuration(
            {"telemetry_interval_s": 300, "thresholds": {"soil_moisture_pct": {"min": 35}}}
        )
        document = device_config.device_document(configuration, 4)
        self.assertEqual(document["config_version"], 4)
        self.assertEqual(document["telemetry_interval_s"], 300)
        self.assertNotIn("power_mode", document)
        self.assertEqual(document["thresholds"][4], 35.0)
        # Firmware rejects credential keys over MQTT; never produce them.
        self.assertLessEqual(set(document), {"config_version", "telemetry_interval_s",
                                             "power_mode", "thresholds", "irrigation"})

    def test_auto_watering_document(self):
        configuration = device_config.validate_configuration({
            "auto_watering": {"enabled": True, "soil_moisture_threshold_pct": 30,
                              "pump_max_runtime_sec": 20, "pump_min_rest_sec": 1800},
        })
        document = device_config.device_document(configuration, 2)
        self.assertEqual(document["irrigation"], [1, 30, 20, 1800])
        unmanaged = device_config.device_document(device_config.validate_configuration({}), 3)
        self.assertNotIn("irrigation", unmanaged)

    def test_store_increments_version(self):
        connection = database()
        configure(connection, {"telemetry_interval_s": 60})
        configure(connection, {"telemetry_interval_s": 120})
        version, configuration, _updated = device_config.load_configuration(
            connection, "farm-01")
        self.assertEqual(version, 2)
        self.assertEqual(configuration["telemetry_interval_s"], 120)


class AlertEngineTests(unittest.TestCase):
    def setUp(self):
        self.connection = database()
        self.sent = []
        configure(self.connection, {"thresholds": {"temperature_c": {"max": 30}}})

    def feed(self, temperature, timestamp=None, received_at=None):
        received_at = received_at or datetime.now(timezone.utc)
        payload = {"temperature_c": temperature,
                   "timestamp": (timestamp or received_at).isoformat()}
        return alert_engine.process_telemetry(
            self.connection, "farm-01", payload, self.sent.append, received_at)

    def test_threshold_alert_with_hysteresis(self):
        self.assertEqual(self.feed(29.0), [])
        self.assertEqual(len(self.feed(31.0)), 1)
        self.assertIn("cao hơn ngưỡng", self.sent[-1])
        self.assertEqual(self.feed(29.8), [])  # inside 0.5 degC band
        resolved = self.feed(29.4)
        self.assertEqual(len(resolved), 1)
        self.assertIn("bình thường", resolved[0])
        items = alert_engine.list_alerts(self.connection, "farm-01", None, 10)
        self.assertEqual([item["state"] for item in items], ["resolved"])

    def test_backlog_records_are_stored_without_notification(self):
        now = datetime.now(timezone.utc)
        self.feed(32.0, timestamp=now - timedelta(hours=1), received_at=now)
        self.assertEqual(self.sent, [])
        items = alert_engine.list_alerts(self.connection, "farm-01", "active", 10)
        self.assertEqual(len(items), 1)
        self.assertTrue(items[0]["backlog"])
        self.assertFalse(items[0]["notified"])

    def test_missing_reading_closes_alert_without_crash(self):
        self.feed(31.0)
        self.feed(None)
        self.assertIn("không còn dữ liệu", self.sent[-1])

    def test_offline_and_reconnect(self):
        now = datetime.now(timezone.utc)
        self.connection.execute(
            "INSERT INTO telemetry (device_id, timestamp, received_at, raw_json) "
            "VALUES ('farm-01', ?, ?, '{}')",
            ((now - timedelta(hours=1)).isoformat(),) * 2,
        )
        offline = alert_engine.check_offline(self.connection, self.sent.append, now)
        self.assertEqual(len(offline), 1)
        self.assertIn("mất kết nối", offline[0])
        # A second check does not repeat the alert.
        self.assertEqual(alert_engine.check_offline(self.connection, self.sent.append, now), [])
        self.feed(25.0)
        self.assertIn("kết nối lại", self.sent[-1])

    def test_format_duration(self):
        self.assertEqual(alert_engine.format_duration(64815 * 60), "45 ngày")
        self.assertEqual(alert_engine.format_duration(2 * 86400 + 3 * 3600), "2 ngày 3 giờ")
        self.assertEqual(alert_engine.format_duration(3 * 3600 + 5 * 60), "3 giờ 5 phút")
        self.assertEqual(alert_engine.format_duration(3600), "1 giờ")
        self.assertEqual(alert_engine.format_duration(16 * 60 + 30), "16 phút")

    def test_offline_limit_follows_interval(self):
        self.assertEqual(alert_engine.offline_after_seconds({}), 900)
        self.assertEqual(alert_engine.offline_after_seconds({"telemetry_interval_s": 10}), 300)
        self.assertEqual(alert_engine.offline_after_seconds({"telemetry_interval_s": 600}), 1860)
        self.assertEqual(alert_engine.offline_after_seconds(
            {"telemetry_interval_s": 600, "offline_alert_after_s": 120}), 120)


class ApiTests(unittest.TestCase):
    """Exercise the HTTP layer with a temporary database and a fake broker."""

    def setUp(self):
        import tempfile
        from pathlib import Path

        try:
            from fastapi.testclient import TestClient
        except (ImportError, RuntimeError) as error:
            # fastapi missing, or starlette without its httpx backend
            # (RuntimeError at import time) as on the production Pi.
            self.skipTest(f"TestClient unavailable: {error}")
        import api

        self.api = api
        self.temp = tempfile.TemporaryDirectory()
        self.original_db = api.DB_PATH
        self.original_publish = api.publish_mqtt
        api.DB_PATH = Path(self.temp.name) / "telemetry.db"
        self.published = []
        api.publish_mqtt = lambda topic, payload, retain: self.published.append(
            (topic, json.loads(payload), retain))
        self.client = TestClient(api.app)

    def tearDown(self):
        self.api.DB_PATH = self.original_db
        self.api.publish_mqtt = self.original_publish
        self.temp.cleanup()

    def test_put_configuration_publishes_retained(self):
        response = self.client.put(
            "/api/v1/devices/farm-01/configuration",
            json={"telemetry_interval_s": 300,
                  "thresholds": {"temperature_c": {"min": 10, "max": 35}}},
        )
        self.assertEqual(response.status_code, 200, response.text)
        body = response.json()
        self.assertEqual(body["version"], 1)
        self.assertEqual(body["sync_state"], "unknown")
        topic, payload, retain = self.published[0]
        self.assertEqual(topic, "saems/farm-01/config")
        self.assertTrue(retain)
        self.assertEqual(payload["thresholds"][:2], [10.0, 35.0])

    def test_failed_publish_does_not_store(self):
        from fastapi import HTTPException

        def fail(*_args, **_kwargs):
            raise HTTPException(503, "MQTT unavailable")

        self.api.publish_mqtt = fail
        response = self.client.put("/api/v1/devices/farm-01/configuration",
                                   json={"telemetry_interval_s": 60})
        self.assertEqual(response.status_code, 503)
        body = self.client.get("/api/v1/devices/farm-01/configuration").json()
        self.assertEqual(body["version"], 0)
        self.assertEqual(body["sync_state"], "not_configured")

    def test_invalid_configuration_is_400(self):
        response = self.client.put("/api/v1/devices/farm-01/configuration",
                                   json={"thresholds": {"humidity_pct": {"min": 90, "max": 10}}})
        self.assertEqual(response.status_code, 400)
        self.assertEqual(self.published, [])

    def test_battery_sim_command(self):
        response = self.client.post("/api/v1/devices/farm-01/commands/battery-sim",
                                    json={"pct": 15})
        self.assertEqual(response.status_code, 200, response.text)
        topic, payload, retain = self.published[0]
        self.assertEqual(topic, "saems/farm-01/commands")
        self.assertEqual((payload["command"], payload["pct"]), ("battery_sim", 15.0))
        self.assertFalse(retain)
        self.assertEqual(self.client.post("/api/v1/devices/farm-01/commands/battery-sim",
                                          json={"pct": 120}).status_code, 422)

    def test_put_auto_watering(self):
        response = self.client.put(
            "/api/v1/devices/farm-01/configuration",
            json={"auto_watering": {"enabled": True, "soil_moisture_threshold_pct": 35,
                                    "pump_max_runtime_sec": 30, "pump_min_rest_sec": 900}},
        )
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(self.published[0][1]["irrigation"], [1, 35, 30, 900])
        body = self.client.get("/api/v1/devices/farm-01/configuration").json()
        self.assertTrue(body["configuration"]["auto_watering"]["enabled"])

    def test_alert_list_and_unconfigured_telegram(self):
        self.assertEqual(self.client.get("/api/v1/alerts").json()["count"], 0)
        if not self.api.notifier.configured:
            self.assertEqual(self.client.post("/api/v1/notifications/test").status_code, 503)


if __name__ == "__main__":
    unittest.main()

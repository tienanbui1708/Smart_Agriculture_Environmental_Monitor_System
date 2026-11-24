"""Offline batch ingestion and de-duplication (temporary database, no MQTT)."""
from contextlib import closing
from pathlib import Path
import sqlite3
import tempfile
import unittest

import mqtt_ingestor


def record(seq, boot="0000abcd", **extra):
    payload = {"schema_version": 1, "device_id": "farm-01", "boot_id": boot,
               "seq": seq, "timestamp": None, "temperature_c": 25.0}
    payload.update(extra)
    return payload


class BatchIngestionTests(unittest.TestCase):
    def setUp(self):
        # initialize_database() leaves WAL side files that Windows may still
        # hold briefly; the data itself is always closed.
        self.temp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.saved = (mqtt_ingestor.DATA_DIR, mqtt_ingestor.DB_PATH)
        mqtt_ingestor.DATA_DIR = Path(self.temp.name)
        mqtt_ingestor.DB_PATH = Path(self.temp.name) / "telemetry.db"
        mqtt_ingestor.initialize_database()
        self.alerts = []
        self.saved_alerts = mqtt_ingestor.evaluate_alerts
        mqtt_ingestor.evaluate_alerts = lambda device, payload: self.alerts.append(
            payload["seq"])

    def tearDown(self):
        mqtt_ingestor.DATA_DIR, mqtt_ingestor.DB_PATH = self.saved
        mqtt_ingestor.evaluate_alerts = self.saved_alerts
        self.temp.cleanup()

    def count(self):
        with closing(sqlite3.connect(mqtt_ingestor.DB_PATH)) as connection:
            return connection.execute("SELECT COUNT(*) FROM telemetry").fetchone()[0]

    def test_single_record_still_works(self):
        self.assertEqual(mqtt_ingestor.ingest_payload("farm-01", record(1)), (1, 0))
        self.assertEqual(self.count(), 1)

    def test_batch_is_stored_and_resend_is_deduplicated(self):
        batch = {"batch": [record(seq) for seq in range(1, 9)]}
        self.assertEqual(mqtt_ingestor.ingest_payload("farm-01", batch), (8, 0))
        # PUBACK lost: the device sends an overlapping batch again.
        again = {"batch": [record(seq) for seq in range(5, 13)]}
        self.assertEqual(mqtt_ingestor.ingest_payload("farm-01", again), (4, 4))
        self.assertEqual(self.count(), 12)
        # Alerts only for newly stored records, in order.
        self.assertEqual(self.alerts, list(range(1, 13)))

    def test_same_seq_after_reboot_is_new(self):
        mqtt_ingestor.ingest_payload("farm-01", record(1, boot="aaaa0001"))
        self.assertEqual(
            mqtt_ingestor.ingest_payload("farm-01", record(1, boot="aaaa0002")), (1, 0))

    def test_invalid_record_does_not_drop_the_batch(self):
        batch = {"batch": [record(1), "garbage", record(2, device_id="farm-02"), record(3)]}
        self.assertEqual(mqtt_ingestor.ingest_payload("farm-01", batch), (2, 0))
        self.assertEqual(self.count(), 2)

    def test_rejects_malformed_batches(self):
        for payload in ({"batch": []}, {"batch": "x"}, ["not", "object"],
                        {"batch": [record(seq) for seq in range(1, 66)]}):
            with self.subTest(payload=str(payload)[:40]):
                with self.assertRaises(ValueError):
                    mqtt_ingestor.ingest_payload("farm-01", payload)
        self.assertEqual(self.count(), 0)


if __name__ == "__main__":
    unittest.main()

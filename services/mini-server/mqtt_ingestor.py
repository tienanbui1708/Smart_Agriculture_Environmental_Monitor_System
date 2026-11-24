#!/usr/bin/env python3

from contextlib import closing
import json
import logging
import os
import sqlite3
import threading
from datetime import datetime, timezone
from pathlib import Path

import paho.mqtt.client as mqtt

import alert_engine
import retention
from mqtt_security import configure_mqtt_tls
from notifier import NullNotifier, notifier_from_env
from schema import ensure_schema


BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
DB_PATH = DATA_DIR / "telemetry.db"

MQTT_HOST = os.getenv("MQTT_HOST", "192.168.1.4")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USERNAME = os.getenv("MQTT_USERNAME", "saems_server")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD")
MQTT_TOPIC = "saems/+/telemetry"
MQTT_CLIENT_ID = "saems-mini-server-ingestor"
OFFLINE_CHECK_SECONDS = 30
MAINTENANCE_SECONDS = 6 * 3600

# Replaced by notifier_from_env() in main(); tests may inject their own.
notifier = NullNotifier()

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def optional_float(payload: dict, key: str):
    value = payload.get(key)
    return None if value is None else float(value)


def drop_legacy_sequence_indexes(connection) -> None:
    """Remove old unique indexes that ignored boot_id."""
    indexes = connection.execute("PRAGMA index_list(telemetry)").fetchall()
    for index in indexes:
        index_name = index[1]
        is_unique = bool(index[2])
        if (
            not is_unique
            or index_name == "uq_telemetry_device_boot_seq"
            or index_name.startswith("sqlite_autoindex_")
        ):
            continue

        quoted_name = '"' + index_name.replace('"', '""') + '"'
        columns = [
            row[2]
            for row in connection.execute(
                f"PRAGMA index_info({quoted_name})"
            )
        ]
        if columns == ["device_id", "seq"]:
            logging.warning("Dropping legacy unique index: %s", index_name)
            connection.execute(f"DROP INDEX {quoted_name}")


def initialize_database() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    with sqlite3.connect(DB_PATH, timeout=10) as connection:
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS telemetry (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                boot_id TEXT,
                seq INTEGER,
                timestamp TEXT NOT NULL,
                received_at TEXT NOT NULL,
                temperature_c REAL,
                humidity_pct REAL,
                soil_moisture_pct REAL,
                light_value REAL,
                light_unit TEXT,
                battery_pct REAL,
                pump_state TEXT,
                raw_json TEXT NOT NULL
            )
            """
        )

        columns = {
            row[1]
            for row in connection.execute("PRAGMA table_info(telemetry)")
        }
        if "boot_id" not in columns:
            connection.execute(
                "ALTER TABLE telemetry ADD COLUMN boot_id TEXT"
            )

        # The old index dropped valid data after a device rebooted and seq
        # restarted at 1. boot_id makes a sequence unique within one boot.
        connection.execute("DROP INDEX IF EXISTS uq_telemetry_device_seq")
        drop_legacy_sequence_indexes(connection)
        connection.execute(
            """
            CREATE UNIQUE INDEX IF NOT EXISTS
            uq_telemetry_device_boot_seq
            ON telemetry(device_id, boot_id, seq)
            WHERE boot_id IS NOT NULL AND seq IS NOT NULL
            """
        )
        ensure_schema(connection)
        retention.ensure_schema(connection)
        connection.commit()


def on_connect(client, userdata, flags, reason_code, properties) -> None:
    if reason_code.is_failure:
        logging.error("MQTT connection failed: %s", reason_code)
        return

    logging.info("Connected to MQTT broker %s:%s", MQTT_HOST, MQTT_PORT)
    result, _ = client.subscribe(MQTT_TOPIC, qos=1)
    if result != mqtt.MQTT_ERR_SUCCESS:
        logging.error("Subscribe failed: %s", result)
    else:
        logging.info("Subscribed to %s", MQTT_TOPIC)


def on_disconnect(client, userdata, disconnect_flags, reason_code, properties):
    logging.warning("MQTT disconnected: %s; waiting for reconnect", reason_code)


MAX_BATCH_RECORDS = 64


def store_record(connection, topic_device_id: str, payload: dict) -> bool:
    """Insert one telemetry record; return False for a duplicate.

    Duplicates (same device_id, boot_id, seq) come from QoS 1 redelivery or
    from a batch re-sent after a lost PUBACK, and are ignored.
    """
    if not isinstance(payload, dict):
        raise ValueError("Telemetry record must be a JSON object")
    device_id = str(payload.get("device_id") or topic_device_id)
    if device_id != topic_device_id:
        raise ValueError(
            f"device_id mismatch: topic={topic_device_id} payload={device_id}"
        )
    boot_value = payload.get("boot_id")
    boot_id = str(boot_value) if boot_value is not None else None
    seq_value = payload.get("seq")
    seq = int(seq_value) if seq_value is not None else None
    timestamp = str(payload.get("timestamp") or utc_now())
    light_value = payload.get(
        "light_lux", payload.get("light_value", payload.get("light"))
    )
    light_unit = "lux" if "light_lux" in payload else None
    raw_json = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    cursor = connection.execute(
        """
        INSERT OR IGNORE INTO telemetry (
            device_id, boot_id, seq, timestamp, received_at,
            temperature_c, humidity_pct, soil_moisture_pct,
            light_value, light_unit, battery_pct, pump_state, raw_json
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            device_id,
            boot_id,
            seq,
            timestamp,
            utc_now(),
            optional_float(payload, "temperature_c"),
            optional_float(payload, "humidity_pct"),
            optional_float(payload, "soil_moisture_pct"),
            float(light_value) if light_value is not None else None,
            light_unit,
            optional_float(payload, "battery_pct"),
            payload.get("pump_state"),
            raw_json,
        ),
    )
    if cursor.rowcount == 0:
        logging.info("Duplicate ignored: device=%s boot=%s seq=%s",
                     device_id, boot_id, seq)
        return False
    logging.info("Telemetry saved: device=%s boot=%s seq=%s", device_id, boot_id, seq)
    return True


def ingest_payload(topic_device_id: str, payload) -> tuple[int, int]:
    """Store a single record or an offline batch {"batch": [...]}.

    The whole batch is one transaction, so a crash never leaves half of it;
    the device re-sends unacknowledged batches and duplicates are ignored.
    Returns (stored, duplicates); invalid records are logged and skipped.
    """
    if isinstance(payload, dict) and "batch" in payload:
        records = payload["batch"]
        if not isinstance(records, list) or not records:
            raise ValueError("batch must be a non-empty list")
        if len(records) > MAX_BATCH_RECORDS:
            raise ValueError(f"batch larger than {MAX_BATCH_RECORDS} records")
    elif isinstance(payload, dict):
        records = [payload]
    else:
        raise ValueError("Payload must be a JSON object")

    new_records = []
    invalid = 0
    with closing(sqlite3.connect(DB_PATH, timeout=10)) as connection:
        for index, record in enumerate(records):
            # The broker has already acknowledged the batch to the device, so
            # one malformed record must not cost the rest of it.
            try:
                if store_record(connection, topic_device_id, record):
                    new_records.append(record)
            except (TypeError, ValueError) as error:
                invalid += 1
                logging.warning("Skipping invalid record %d from %s: %s",
                                index, topic_device_id, error)
        connection.commit()
    duplicates = len(records) - len(new_records) - invalid
    if len(records) > 1:
        logging.info("Offline batch from %s: %d stored, %d duplicates, %d invalid",
                     topic_device_id, len(new_records), duplicates, invalid)
    for record in new_records:
        evaluate_alerts(topic_device_id, record)
    return len(new_records), duplicates


def on_message(client, userdata, message) -> None:
    try:
        topic_parts = message.topic.split("/")
        if (
            len(topic_parts) != 3
            or topic_parts[0] != "saems"
            or topic_parts[2] != "telemetry"
        ):
            logging.warning("Ignoring unexpected topic: %s", message.topic)
            return
        payload = json.loads(message.payload.decode("utf-8"))
        ingest_payload(topic_parts[1], payload)
    except (UnicodeDecodeError, json.JSONDecodeError):
        logging.exception("Invalid JSON payload on %s", message.topic)
    except (TypeError, ValueError, sqlite3.Error):
        logging.exception("Failed to process MQTT message on %s", message.topic)


def evaluate_alerts(device_id: str, payload: dict) -> None:
    """Alerts never block ingestion: telemetry is already committed."""
    try:
        with sqlite3.connect(DB_PATH, timeout=10) as connection:
            alert_engine.process_telemetry(
                connection, device_id, payload, notifier.send
            )
            connection.commit()
    except (sqlite3.Error, TypeError, ValueError, KeyError):
        logging.exception("Alert evaluation failed for %s", device_id)


def offline_monitor(stop: threading.Event) -> None:
    while not stop.wait(OFFLINE_CHECK_SECONDS):
        try:
            with sqlite3.connect(DB_PATH, timeout=10) as connection:
                alert_engine.check_offline(connection, notifier.send)
                connection.commit()
        except (sqlite3.Error, TypeError, ValueError, KeyError):
            logging.exception("Offline check failed")


def maintenance_loop(stop: threading.Event) -> None:
    """Hourly roll-up and retention; first run right after start."""
    while True:
        try:
            stats = retention.run_maintenance(DB_PATH)
            logging.info("Maintenance: %d hours rolled up, %d raw rows and %d alerts deleted",
                         stats["hours"], stats["raw_deleted"], stats["alerts_deleted"])
        except sqlite3.Error:
            logging.exception("Maintenance failed")
        if stop.wait(MAINTENANCE_SECONDS):
            return


def create_mqtt_client():
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=MQTT_CLIENT_ID,
        protocol=mqtt.MQTTv311,
        clean_session=False,
    )
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    configure_mqtt_tls(client)
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    return client


def main() -> None:
    if not MQTT_PASSWORD:
        raise RuntimeError("MQTT_PASSWORD environment variable is not set")

    initialize_database()
    logging.info("Database: %s", DB_PATH)
    global notifier
    notifier = notifier_from_env()
    logging.info("Telegram notifications: %s",
                 "enabled" if notifier.configured else "not configured")
    threading.Thread(
        target=offline_monitor, args=(threading.Event(),),
        name="offline-monitor", daemon=True,
    ).start()
    threading.Thread(
        target=maintenance_loop, args=(threading.Event(),),
        name="maintenance", daemon=True,
    ).start()
    client = create_mqtt_client()
    client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=60)
    # Retry the initial connection as well as later disconnects. This keeps
    # the service alive during broker/network restarts.
    client.loop_forever(retry_first_connection=True)


if __name__ == "__main__":
    main()

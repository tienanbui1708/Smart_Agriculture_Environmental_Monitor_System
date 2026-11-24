/*
 * Host test for data_manager (telemetry JSON) with stub ESP-IDF headers.
 * Run through tests/run_host_tests.py, which also checks every line that
 * starts with "JSON:" with a real JSON parser.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "data_manager.h"

static int checks;
#define CHECK(condition) \
    do {                 \
        assert(condition); \
        checks++;        \
    } while (0)

/* Test doubles for the ESP-IDF / sensor_manager symbols data_manager uses. */
static int64_t fake_uptime_us = 1234567000;

int64_t esp_timer_get_time(void)
{
    return fake_uptime_us;
}

const char *sensor_health_to_string(sensor_health_t health)
{
    switch (health) {
    case SENSOR_HEALTH_OK:
        return "ok";
    case SENSOR_HEALTH_ERROR:
        return "error";
    case SENSOR_HEALTH_UNCALIBRATED:
        return "uncalibrated";
    case SENSOR_HEALTH_UNAVAILABLE:
    default:
        return "unavailable";
    }
}

static sensor_snapshot_t full_snapshot(void)
{
    sensor_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.temperature_c = 26.64f;
    snapshot.humidity_pct = 67.0f;
    snapshot.soil_moisture_pct = 95.1f;
    snapshot.light_lux = 218.3f;
    snapshot.soil_raw = 2183;
    snapshot.soil_digital = true;
    snapshot.temperature_valid = true;
    snapshot.humidity_valid = true;
    snapshot.light_valid = true;
    snapshot.soil_moisture_valid = true;
    snapshot.soil_raw_valid = true;
    snapshot.dht20_health = SENSOR_HEALTH_OK;
    snapshot.bh1750_health = SENSOR_HEALTH_OK;
    snapshot.soil_health = SENSOR_HEALTH_OK;
    return snapshot;
}

static telemetry_metadata_t metadata(void)
{
    const telemetry_metadata_t value = {
        .device_id = "farm-01",
        .boot_id = 0x0000abcdU,
        .sequence = 42U,
        .pump_state = "off",
        .pump_reason = "auto",
        .battery_valid = true,
        .battery_pct = 18.46f,
        .battery_source = "simulated",
        .battery_state = "low",
        .queue_dropped = 4U,
        .config_version = 3U,
        .alarm_mask = 0x102U,
    };
    return value;
}

static void test_full_record(void)
{
    const sensor_snapshot_t snapshot = full_snapshot();
    const telemetry_metadata_t meta = metadata();
    char payload[768];
    CHECK(data_manager_build_telemetry(&snapshot, &meta, payload,
                                       sizeof(payload)) == ESP_OK);
    printf("JSON:%s\n", payload);
    CHECK(strstr(payload, "\"device_id\":\"farm-01\"") != NULL);
    CHECK(strstr(payload, "\"boot_id\":\"0000abcd\"") != NULL);
    CHECK(strstr(payload, "\"seq\":42,") != NULL);
    CHECK(strstr(payload, "\"uptime_ms\":1234567,") != NULL);
    CHECK(strstr(payload, "\"temperature_c\":26.6,") != NULL);
    CHECK(strstr(payload, "\"sensor_health\":\"ok\"") != NULL);
    CHECK(strstr(payload, "\"soil_raw\":2183,") != NULL);
    CHECK(strstr(payload, "\"soil_digital\":true") != NULL);
    CHECK(strstr(payload, "\"battery_pct\":18.5,") != NULL);
    CHECK(strstr(payload, "\"battery_source\":\"simulated\"") != NULL);
    CHECK(strstr(payload, "\"pump_reason\":\"auto\"") != NULL);
    CHECK(strstr(payload, "\"config_version\":3,") != NULL);
    CHECK(strstr(payload, "\"alarm_mask\":258,") != NULL);
    CHECK(strstr(payload, "\"queue_dropped\":4}") != NULL);
}

static void test_missing_values_are_null(void)
{
    sensor_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot)); /* all invalid / unavailable */
    snapshot.dht20_health = SENSOR_HEALTH_ERROR;
    telemetry_metadata_t meta = metadata();
    meta.pump_reason = NULL;
    meta.battery_valid = false;
    meta.battery_source = "none";
    meta.battery_state = "unknown";
    char payload[768];
    CHECK(data_manager_build_telemetry(&snapshot, &meta, payload,
                                       sizeof(payload)) == ESP_OK);
    printf("JSON:%s\n", payload);
    CHECK(strstr(payload, "\"temperature_c\":null") != NULL);
    CHECK(strstr(payload, "\"soil_raw\":null") != NULL);
    CHECK(strstr(payload, "\"soil_digital\":null") != NULL);
    CHECK(strstr(payload, "\"battery_pct\":null") != NULL);
    CHECK(strstr(payload, "\"pump_reason\":null") != NULL);
    /* One sensor in error, the rest unavailable: degraded, never "ok". */
    CHECK(strstr(payload, "\"sensor_health\":\"degraded\"") != NULL);
    CHECK(strstr(payload, "\"dht20\":\"error\"") != NULL);
}

static void test_worst_case_fits_storage_record(void)
{
    sensor_snapshot_t snapshot = full_snapshot();
    snapshot.temperature_c = -40.0f;
    snapshot.light_lux = 65535.0f;
    snapshot.dht20_health = SENSOR_HEALTH_UNCALIBRATED;
    snapshot.bh1750_health = SENSOR_HEALTH_UNCALIBRATED;
    snapshot.soil_health = SENSOR_HEALTH_UNCALIBRATED;
    snapshot.soil_raw = 4095;
    telemetry_metadata_t meta = metadata();
    meta.device_id = "abcdefghijklmnopqrstuvwxyz012345"; /* 32 characters */
    meta.boot_id = 0xffffffffU;
    meta.sequence = 4294967295U;
    meta.pump_state = "unknown";
    meta.pump_reason = "low_battery";
    meta.battery_pct = 100.0f;
    meta.battery_state = "critical";
    meta.config_version = 4294967295U;
    meta.alarm_mask = 0x1ffU;
    meta.queue_dropped = 4294967295U;
    fake_uptime_us = INT64_MAX;
    char payload[768];
    CHECK(data_manager_build_telemetry(&snapshot, &meta, payload,
                                       sizeof(payload)) == ESP_OK);
    printf("JSON:%s\n", payload);
    printf("worst-case telemetry: %zu bytes (limit 767)\n", strlen(payload));
    CHECK(strlen(payload) < 700U); /* keeps margin below the 768 record */
    fake_uptime_us = 1234567000;
}

static void test_errors(void)
{
    const sensor_snapshot_t snapshot = full_snapshot();
    telemetry_metadata_t meta = metadata();
    char small[64];
    CHECK(data_manager_build_telemetry(&snapshot, &meta, small, sizeof(small)) ==
          ESP_ERR_INVALID_SIZE);
    CHECK(small[0] == '\0'); /* never a truncated document */
    CHECK(data_manager_build_telemetry(NULL, &meta, small, sizeof(small)) ==
          ESP_ERR_INVALID_ARG);
    meta.battery_source = NULL;
    CHECK(data_manager_build_telemetry(&snapshot, &meta, small, sizeof(small)) ==
          ESP_ERR_INVALID_ARG);
}

int main(void)
{
    test_full_record();
    test_missing_values_are_null();
    test_worst_case_fits_storage_record();
    test_errors();
    printf("data_manager tests: %d checks passed\n", checks);
    return 0;
}

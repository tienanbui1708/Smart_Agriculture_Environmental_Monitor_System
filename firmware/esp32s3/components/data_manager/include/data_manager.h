#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sensor_manager.h"

#define SAEMS_TELEMETRY_SCHEMA_VERSION 1

typedef struct {
    const char *device_id;
    uint32_t boot_id;
    uint32_t sequence;
    const char *pump_state;
    /* Last pump transition: "auto", "manual", "safety_off",
     * "low_battery" or NULL (JSON null) when none since boot. */
    const char *pump_reason;
    /* Battery: source "none"/"adc"/"simulated", level name. */
    bool battery_valid;
    float battery_pct;
    const char *battery_source;
    const char *battery_state;
    /* Unsent records lost because the offline log was full (total). */
    uint32_t queue_dropped;
    /* Server configuration version applied on the device (0 = local). */
    uint32_t config_version;
    /* Active threshold alarms, bit order in threshold_monitor.h. */
    uint16_t alarm_mask;
} telemetry_metadata_t;

esp_err_t data_manager_build_telemetry(
    const sensor_snapshot_t *snapshot,
    const telemetry_metadata_t *metadata,
    char *payload,
    size_t payload_size);

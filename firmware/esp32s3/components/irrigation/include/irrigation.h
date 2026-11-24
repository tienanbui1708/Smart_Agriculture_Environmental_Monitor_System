#pragma once

/*
 * Automatic watering decisions, shared by the firmware and host unit tests.
 *
 * Pure C, no ESP-IDF dependency. Watering is pulse based: when the soil is
 * drier than start_below_pct the pump runs for at most max_runtime_s (the
 * caller arms a one-shot timer), then the controller waits min_rest_s before
 * another automatic pulse so water can soak in before the next reading.
 * An automatic pulse also stops early once a sample shows the soil at
 * start_below_pct + IRRIGATION_STOP_MARGIN_PCT or wetter.
 *
 * Device contract (BLE/AP/MQTT config key "irrigation"), a 4-number array
 * that keeps the BLE config read below the 512-byte ATT limit:
 *
 *   [enabled (0/1), start_below_pct, max_runtime_s, min_rest_s]
 *
 * Server/API names: auto_watering_enabled, soil_moisture_threshold_pct,
 * pump_max_runtime_sec, pump_min_rest_sec.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IRRIGATION_STOP_MARGIN_PCT 5U
#define IRRIGATION_MIN_START_PCT 1U
#define IRRIGATION_MAX_START_PCT 95U
#define IRRIGATION_MAX_RUNTIME_S 3600U
#define IRRIGATION_MAX_REST_S 86400U

typedef struct {
    bool enabled;
    uint8_t start_below_pct;
    uint16_t max_runtime_s;
    uint32_t min_rest_s;
} irrigation_config_t;

/* Why the pump is in its current state (last transition). */
typedef enum {
    PUMP_REASON_NONE = 0,   /* no transition since boot */
    PUMP_REASON_AUTO,       /* automatic watering started or stopped it */
    PUMP_REASON_MANUAL,     /* app/server command */
    PUMP_REASON_SAFETY_OFF, /* runtime limit or sleep forced it off */
    PUMP_REASON_LOW_BATTERY,
} pump_reason_t;

typedef struct {
    bool pump_on;
    pump_reason_t reason;
    /* Monotonic seconds when the last automatic pulse ended. */
    bool has_last_auto_stop;
    int64_t last_auto_stop_s;
} irrigation_state_t;

typedef struct {
    bool soil_valid;
    float soil_pct;
    /* Critical battery blocks watering and stops a running pump. */
    bool battery_critical;
    /* Monotonic seconds; must keep counting across Deep Sleep. */
    int64_t now_s;
} irrigation_inputs_t;

typedef enum {
    IRRIGATION_KEEP = 0,
    IRRIGATION_START,
    IRRIGATION_STOP,
} irrigation_action_t;

typedef struct {
    irrigation_action_t action;
    /* Reason to record with the transition (START/STOP only). */
    pump_reason_t reason;
} irrigation_decision_t;

/* Disabled, start below 30 %, 30 s pulse, 30 min rest. */
void irrigation_config_default(irrigation_config_t *config);

bool irrigation_config_is_valid(const irrigation_config_t *config);

irrigation_decision_t irrigation_decide(
    const irrigation_config_t *config,
    const irrigation_state_t *state,
    const irrigation_inputs_t *inputs);

/* "auto", "manual", "safety_off", "low_battery" or NULL for NONE. */
const char *pump_reason_name(pump_reason_t reason);

/* Writes "[e,p,r,s]"; returns snprintf-style length or -1. */
int irrigation_config_to_json(
    const irrigation_config_t *config, char *output, size_t output_size);

/* Parses exactly four non-negative integers; validates ranges. */
bool irrigation_config_from_json(const char *text, irrigation_config_t *config);

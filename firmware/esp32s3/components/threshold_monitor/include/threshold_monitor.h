#pragma once

/*
 * Threshold evaluation shared by the firmware and host unit tests.
 *
 * Pure C, no ESP-IDF dependency. The limit order is part of the device
 * contract (BLE/AP/MQTT "thresholds" array and telemetry "alarm_mask"):
 *
 *   index 0 temperature min  (bit0 temperature low)
 *   index 1 temperature max  (bit1 temperature high)
 *   index 2 humidity min     (bit2 humidity low)
 *   index 3 humidity max     (bit3 humidity high)
 *   index 4 soil min         (bit4 soil dry)
 *   index 5 soil max         (bit5 soil too wet)
 *   index 6 light min        (bit6 light low)
 *   index 7 light max        (bit7 light high)
 *   index 8 battery min      (bit8 battery low)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define THRESHOLD_LIMIT_COUNT 9U

typedef enum {
    THRESHOLD_METRIC_TEMPERATURE = 0,
    THRESHOLD_METRIC_HUMIDITY,
    THRESHOLD_METRIC_SOIL,
    THRESHOLD_METRIC_LIGHT,
    THRESHOLD_METRIC_BATTERY,
    THRESHOLD_METRIC_COUNT,
} threshold_metric_t;

typedef struct {
    float value[THRESHOLD_LIMIT_COUNT];
    bool enabled[THRESHOLD_LIMIT_COUNT];
} threshold_limits_t;

typedef struct {
    bool valid;
    float value;
} threshold_reading_t;

typedef struct {
    threshold_reading_t reading[THRESHOLD_METRIC_COUNT];
} threshold_readings_t;

/* Disable every limit. */
void threshold_limits_clear(threshold_limits_t *limits);

/* Range and min < max checks for every enabled pair. */
bool threshold_limits_are_valid(const threshold_limits_t *limits);

/*
 * Evaluate readings against limits with hysteresis.
 *
 * previous_mask is the mask returned by the previous evaluation. An alarm
 * enters when a reading crosses the limit and leaves only after the reading
 * is back inside by the metric hysteresis. Invalid readings and disabled
 * limits clear their bits.
 */
uint16_t threshold_evaluate(const threshold_limits_t *limits,
                            const threshold_readings_t *readings,
                            uint16_t previous_mask);

/*
 * Serialize limits as a JSON array of 9 numbers or null, for example
 * [null,35.0,...]. Returns the written length or -1 when output is too small.
 */
int threshold_limits_to_json(const threshold_limits_t *limits,
                             char *output,
                             size_t output_size);

/*
 * Parse a JSON array of exactly 9 numbers or null starting at text (which
 * must point at '['). On success limits is replaced and true is returned;
 * limits is left unchanged on failure. Range validation is separate.
 */
bool threshold_limits_from_json(const char *text, threshold_limits_t *limits);

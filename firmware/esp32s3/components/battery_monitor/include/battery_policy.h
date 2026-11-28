#pragma once

/*
 * Battery level math shared by the firmware and host unit tests.
 * Pure C, no ESP-IDF dependency.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BATTERY_LEVEL_UNKNOWN = 0, /* no measurement (source "none" or error) */
    BATTERY_LEVEL_NORMAL,
    BATTERY_LEVEL_LOW,      /* force Battery mode, longer interval */
    BATTERY_LEVEL_CRITICAL, /* pump blocked, longest Deep Sleep */
} battery_level_t;

typedef struct {
    uint8_t low_pct;
    uint8_t critical_pct;
    /* A level is left only after recovering by this many percent. */
    uint8_t hysteresis_pct;
} battery_policy_t;

/*
 * State of charge of a single-cell LiPo/Li-ion at rest, by linear
 * interpolation of a typical discharge curve (4200 mV = 100 %,
 * 3270 mV = 0 %). Values outside the table are clamped.
 */
float battery_percent_from_mv(uint32_t millivolts);

/* Classify with hysteresis against the previous level. */
battery_level_t battery_classify(const battery_policy_t *policy,
                                 bool valid,
                                 float percent,
                                 battery_level_t previous);

/* "unknown", "normal", "low", "critical". */
const char *battery_level_name(battery_level_t level);

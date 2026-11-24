#pragma once

/*
 * Battery measurement for SAEMS.
 *
 * The source is chosen in menuconfig (SAEMS_BATTERY_SOURCE):
 *   none       - no battery circuit; battery_pct stays null (USB power).
 *   adc        - 1S LiPo through a resistor divider on an ADC1 pin
 *                (GPIO6 / ADC1_CH5 by default).
 *   simulated  - a level kept in RTC memory that drains a little every
 *                sample and can be set with the "battery_sim" command, so
 *                low/critical battery scenarios can be demonstrated without
 *                hardware.
 */

#include <stdbool.h>
#include <stdint.h>

#include "battery_policy.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

typedef enum {
    BATTERY_SOURCE_NONE = 0,
    BATTERY_SOURCE_ADC,
    BATTERY_SOURCE_SIMULATED,
} battery_source_t;

typedef struct {
    bool valid;
    float percent;
    uint32_t millivolts; /* 0 when unknown or simulated */
    battery_level_t level;
} battery_reading_t;

typedef struct {
    battery_source_t source;
    adc_oneshot_unit_handle_t adc;
    adc_cali_handle_t calibration;
    int channel;
    bool owns_adc;
    bool initialized;
    battery_reading_t last;
} battery_monitor_t;

/*
 * shared_adc1 may be the ADC1 handle already created by another driver
 * (the soil sensor); ESP-IDF allows a single oneshot handle per unit.
 * Pass NULL to let the monitor create its own.
 */
esp_err_t battery_monitor_init(battery_monitor_t *monitor,
                               adc_oneshot_unit_handle_t shared_adc1);

/* Measure and classify; previous level comes from the last reading. */
esp_err_t battery_monitor_read(battery_monitor_t *monitor,
                               battery_reading_t *reading);

/* Set the simulated level (0..100). ESP_ERR_NOT_SUPPORTED otherwise. */
esp_err_t battery_monitor_simulate(battery_monitor_t *monitor, float percent);

const char *battery_source_name(battery_source_t source);

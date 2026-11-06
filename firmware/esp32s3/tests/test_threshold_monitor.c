/*
 * Host test for threshold_monitor. Build from firmware/esp32s3:
 *   gcc -std=c11 -Wall -Wextra -Icomponents/threshold_monitor/include \
 *       tests/test_threshold_monitor.c \
 *       components/threshold_monitor/threshold_monitor.c -lm -o test_threshold
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "threshold_monitor.h"

static int checks;
#define CHECK(condition) \
    do {                 \
        assert(condition); \
        checks++;        \
    } while (0)

static threshold_readings_t readings_with(threshold_metric_t metric,
                                          float value)
{
    threshold_readings_t readings;
    memset(&readings, 0, sizeof(readings));
    readings.reading[metric].valid = true;
    readings.reading[metric].value = value;
    return readings;
}

static void test_validation(void)
{
    threshold_limits_t limits;
    threshold_limits_clear(&limits);
    CHECK(threshold_limits_are_valid(&limits));

    limits.enabled[0] = true;
    limits.value[0] = 10.0f;
    limits.enabled[1] = true;
    limits.value[1] = 35.0f;
    CHECK(threshold_limits_are_valid(&limits));

    limits.value[1] = 10.0f; /* min must be strictly below max */
    CHECK(!threshold_limits_are_valid(&limits));

    limits.value[1] = 90.0f; /* above DHT20 range */
    CHECK(!threshold_limits_are_valid(&limits));

    threshold_limits_clear(&limits);
    limits.enabled[3] = true;
    limits.value[3] = 101.0f; /* humidity > 100 % */
    CHECK(!threshold_limits_are_valid(&limits));

    threshold_limits_clear(&limits);
    limits.enabled[8] = true;
    limits.value[8] = 20.0f; /* battery min has no pair */
    CHECK(threshold_limits_are_valid(&limits));
}

static void test_temperature_hysteresis(void)
{
    threshold_limits_t limits;
    threshold_limits_clear(&limits);
    limits.enabled[1] = true;
    limits.value[1] = 30.0f;

    threshold_readings_t r = readings_with(THRESHOLD_METRIC_TEMPERATURE, 29.9f);
    uint16_t mask = threshold_evaluate(&limits, &r, 0U);
    CHECK(mask == 0U);

    r = readings_with(THRESHOLD_METRIC_TEMPERATURE, 30.2f);
    mask = threshold_evaluate(&limits, &r, mask);
    CHECK(mask == 0x0002U);

    /* Still inside the 0.5 degC band: alarm holds. */
    r = readings_with(THRESHOLD_METRIC_TEMPERATURE, 29.7f);
    mask = threshold_evaluate(&limits, &r, mask);
    CHECK(mask == 0x0002U);

    r = readings_with(THRESHOLD_METRIC_TEMPERATURE, 29.4f);
    mask = threshold_evaluate(&limits, &r, mask);
    CHECK(mask == 0U);
}

static void test_low_limits_and_battery(void)
{
    threshold_limits_t limits;
    threshold_limits_clear(&limits);
    limits.enabled[4] = true;
    limits.value[4] = 35.0f;
    limits.enabled[8] = true;
    limits.value[8] = 20.0f;

    threshold_readings_t r;
    memset(&r, 0, sizeof(r));
    r.reading[THRESHOLD_METRIC_SOIL].valid = true;
    r.reading[THRESHOLD_METRIC_SOIL].value = 30.0f;
    r.reading[THRESHOLD_METRIC_BATTERY].valid = true;
    r.reading[THRESHOLD_METRIC_BATTERY].value = 19.0f;
    uint16_t mask = threshold_evaluate(&limits, &r, 0U);
    CHECK(mask == (0x0010U | 0x0100U));

    /* 36 % is above the limit but inside the 2 % band. */
    r.reading[THRESHOLD_METRIC_SOIL].value = 36.0f;
    r.reading[THRESHOLD_METRIC_BATTERY].value = 23.0f;
    mask = threshold_evaluate(&limits, &r, mask);
    CHECK(mask == 0x0010U);
}

static void test_light_relative_band(void)
{
    threshold_limits_t limits;
    threshold_limits_clear(&limits);
    limits.enabled[7] = true;
    limits.value[7] = 1000.0f; /* band = 5 % = 50 lux */

    threshold_readings_t r = readings_with(THRESHOLD_METRIC_LIGHT, 1001.0f);
    uint16_t mask = threshold_evaluate(&limits, &r, 0U);
    CHECK(mask == 0x0080U);
    r = readings_with(THRESHOLD_METRIC_LIGHT, 960.0f);
    mask = threshold_evaluate(&limits, &r, mask);
    CHECK(mask == 0x0080U);
    r = readings_with(THRESHOLD_METRIC_LIGHT, 940.0f);
    mask = threshold_evaluate(&limits, &r, mask);
    CHECK(mask == 0U);
}

static void test_invalid_reading_clears(void)
{
    threshold_limits_t limits;
    threshold_limits_clear(&limits);
    limits.enabled[2] = true;
    limits.value[2] = 40.0f;
    threshold_readings_t r;
    memset(&r, 0, sizeof(r)); /* humidity sensor unavailable */
    CHECK(threshold_evaluate(&limits, &r, 0x0004U) == 0U);
}

static void test_json_round_trip(void)
{
    threshold_limits_t limits;
    threshold_limits_clear(&limits);
    limits.enabled[0] = true;
    limits.value[0] = -5.5f;
    limits.enabled[7] = true;
    limits.value[7] = 20000.0f;

    char json[160];
    int length = threshold_limits_to_json(&limits, json, sizeof(json));
    CHECK(length > 0);
    CHECK(strcmp(json,
                 "[-5.5,null,null,null,null,null,null,20000.0,null]") == 0);

    threshold_limits_t parsed;
    threshold_limits_clear(&parsed);
    CHECK(threshold_limits_from_json(json, &parsed));
    CHECK(parsed.enabled[0] && parsed.value[0] == -5.5f);
    CHECK(parsed.enabled[7] && parsed.value[7] == 20000.0f);
    CHECK(!parsed.enabled[1] && !parsed.enabled[8]);

    CHECK(threshold_limits_from_json(
        " [ 1 , 2,null,null,null,null,null,null, 15 ]", &parsed));
    CHECK(parsed.value[1] == 2.0f && parsed.value[8] == 15.0f);

    /* Wrong count, garbage and too-small buffers are rejected. */
    threshold_limits_t untouched = parsed;
    CHECK(!threshold_limits_from_json("[1,2,3]", &parsed));
    CHECK(!threshold_limits_from_json("[1,2,3,4,5,6,7,8,9,10]", &parsed));
    CHECK(!threshold_limits_from_json("[a,null,null,null,null,null,null,null,null]",
                                      &parsed));
    CHECK(memcmp(&untouched, &parsed, sizeof(parsed)) == 0);
    CHECK(threshold_limits_to_json(&limits, json, 10U) == -1);
}

int main(void)
{
    test_validation();
    test_temperature_hysteresis();
    test_low_limits_and_battery();
    test_light_relative_band();
    test_invalid_reading_clears();
    test_json_round_trip();
    printf("Threshold monitor: %d checks PASS\n", checks);
    return 0;
}

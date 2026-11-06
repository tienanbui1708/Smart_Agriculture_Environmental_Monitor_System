#include "threshold_monitor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float minimum;
    float maximum;
} threshold_range_t;

/* Accepted limit ranges per metric, matching sensor capability. */
static const threshold_range_t metric_range[THRESHOLD_METRIC_COUNT] = {
    {-40.0f, 85.0f},     /* DHT20 temperature, degC */
    {0.0f, 100.0f},      /* relative humidity, % */
    {0.0f, 100.0f},      /* soil moisture index, % */
    {0.0f, 100000.0f},   /* BH1750 light, lux */
    {0.0f, 100.0f},      /* battery, % */
};

static float hysteresis(threshold_metric_t metric, float limit)
{
    switch (metric) {
    case THRESHOLD_METRIC_TEMPERATURE:
        return 0.5f;
    case THRESHOLD_METRIC_HUMIDITY:
    case THRESHOLD_METRIC_SOIL:
    case THRESHOLD_METRIC_BATTERY:
        return 2.0f;
    case THRESHOLD_METRIC_LIGHT: {
        const float relative = fabsf(limit) * 0.05f;
        return relative > 10.0f ? relative : 10.0f;
    }
    default:
        return 0.0f;
    }
}

static threshold_metric_t limit_metric(size_t index)
{
    return (threshold_metric_t)(index / 2U);
}

void threshold_limits_clear(threshold_limits_t *limits)
{
    if (limits == NULL) {
        return;
    }
    for (size_t index = 0U; index < THRESHOLD_LIMIT_COUNT; index++) {
        limits->value[index] = 0.0f;
        limits->enabled[index] = false;
    }
}

bool threshold_limits_are_valid(const threshold_limits_t *limits)
{
    if (limits == NULL) {
        return false;
    }
    for (size_t index = 0U; index < THRESHOLD_LIMIT_COUNT; index++) {
        if (!limits->enabled[index]) {
            continue;
        }
        const threshold_range_t range = metric_range[limit_metric(index)];
        const float value = limits->value[index];
        if (!isfinite(value) || value < range.minimum ||
            value > range.maximum) {
            return false;
        }
    }
    /* Battery has a single minimum limit (index 8); pairs are 0..7. */
    for (size_t index = 0U; index + 1U < THRESHOLD_LIMIT_COUNT - 1U;
         index += 2U) {
        if (limits->enabled[index] && limits->enabled[index + 1U] &&
            !(limits->value[index] < limits->value[index + 1U])) {
            return false;
        }
    }
    return true;
}

static bool low_alarm(float value, float limit, float band, bool active)
{
    return active ? (value < limit + band) : (value < limit);
}

static bool high_alarm(float value, float limit, float band, bool active)
{
    return active ? (value > limit - band) : (value > limit);
}

uint16_t threshold_evaluate(const threshold_limits_t *limits,
                            const threshold_readings_t *readings,
                            uint16_t previous_mask)
{
    if (limits == NULL || readings == NULL) {
        return 0U;
    }
    uint16_t mask = 0U;
    for (size_t index = 0U; index < THRESHOLD_LIMIT_COUNT; index++) {
        const threshold_metric_t metric = limit_metric(index);
        const threshold_reading_t reading = readings->reading[metric];
        if (!limits->enabled[index] || !reading.valid ||
            !isfinite(reading.value)) {
            continue;
        }
        const uint16_t bit = (uint16_t)(1U << index);
        const bool active = (previous_mask & bit) != 0U;
        const float limit = limits->value[index];
        const float band = hysteresis(metric, limit);
        /* Even indexes are minimum limits, odd indexes maximum limits;
         * index 8 (battery) is a minimum. */
        const bool is_minimum = (index % 2U) == 0U;
        const bool alarm = is_minimum
                               ? low_alarm(reading.value, limit, band, active)
                               : high_alarm(reading.value, limit, band, active);
        if (alarm) {
            mask = (uint16_t)(mask | bit);
        }
    }
    return mask;
}

int threshold_limits_to_json(const threshold_limits_t *limits,
                             char *output,
                             size_t output_size)
{
    if (limits == NULL || output == NULL || output_size == 0U) {
        return -1;
    }
    size_t used = 0U;
    for (size_t index = 0U; index < THRESHOLD_LIMIT_COUNT; index++) {
        const char *prefix = index == 0U ? "[" : ",";
        int written = limits->enabled[index]
                          ? snprintf(output + used, output_size - used,
                                     "%s%.1f", prefix,
                                     (double)limits->value[index])
                          : snprintf(output + used, output_size - used,
                                     "%snull", prefix);
        if (written < 0 || (size_t)written >= output_size - used) {
            output[0] = '\0';
            return -1;
        }
        used += (size_t)written;
    }
    if (used + 1U >= output_size) {
        output[0] = '\0';
        return -1;
    }
    output[used] = ']';
    output[used + 1U] = '\0';
    return (int)(used + 1U);
}

static const char *skip_space(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    return text;
}

bool threshold_limits_from_json(const char *text, threshold_limits_t *limits)
{
    if (text == NULL || limits == NULL) {
        return false;
    }
    threshold_limits_t parsed;
    threshold_limits_clear(&parsed);
    const char *position = skip_space(text);
    if (*position != '[') {
        return false;
    }
    position++;
    for (size_t index = 0U; index < THRESHOLD_LIMIT_COUNT; index++) {
        position = skip_space(position);
        if (strncmp(position, "null", 4U) == 0) {
            position += 4;
        } else {
            char *end = NULL;
            const float value = strtof(position, &end);
            if (end == position || !isfinite(value)) {
                return false;
            }
            parsed.value[index] = value;
            parsed.enabled[index] = true;
            position = end;
        }
        position = skip_space(position);
        const char expected = index + 1U < THRESHOLD_LIMIT_COUNT ? ',' : ']';
        if (*position != expected) {
            return false;
        }
        position++;
    }
    *limits = parsed;
    return true;
}

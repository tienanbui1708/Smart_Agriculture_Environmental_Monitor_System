#include "irrigation.h"

#include <stdio.h>
#include <stdlib.h>

void irrigation_config_default(irrigation_config_t *config)
{
    if (config == NULL) {
        return;
    }
    config->enabled = false;
    config->start_below_pct = 30U;
    config->max_runtime_s = 30U;
    config->min_rest_s = 1800U;
}

bool irrigation_config_is_valid(const irrigation_config_t *config)
{
    return config != NULL &&
           config->start_below_pct >= IRRIGATION_MIN_START_PCT &&
           config->start_below_pct <= IRRIGATION_MAX_START_PCT &&
           config->max_runtime_s >= 1U &&
           config->max_runtime_s <= IRRIGATION_MAX_RUNTIME_S &&
           config->min_rest_s <= IRRIGATION_MAX_REST_S;
}

static irrigation_decision_t decision(
    irrigation_action_t action, pump_reason_t reason)
{
    const irrigation_decision_t result = {action, reason};
    return result;
}

static bool resting(const irrigation_config_t *config,
                    const irrigation_state_t *state,
                    int64_t now_s)
{
    if (!state->has_last_auto_stop) {
        return false;
    }
    const int64_t elapsed = now_s - state->last_auto_stop_s;
    /* A clock that went backwards means the stored time is stale. */
    return elapsed >= 0 && elapsed < (int64_t)config->min_rest_s;
}

irrigation_decision_t irrigation_decide(
    const irrigation_config_t *config,
    const irrigation_state_t *state,
    const irrigation_inputs_t *inputs)
{
    if (config == NULL || state == NULL || inputs == NULL) {
        return decision(IRRIGATION_KEEP, PUMP_REASON_NONE);
    }
    if (state->pump_on) {
        if (inputs->battery_critical) {
            return decision(IRRIGATION_STOP, PUMP_REASON_LOW_BATTERY);
        }
        /* Manual runs end by command or by the safety timer only. */
        if (state->reason != PUMP_REASON_AUTO) {
            return decision(IRRIGATION_KEEP, PUMP_REASON_NONE);
        }
        if (!config->enabled) {
            return decision(IRRIGATION_STOP, PUMP_REASON_AUTO);
        }
        const float stop_at = (float)config->start_below_pct +
                              (float)IRRIGATION_STOP_MARGIN_PCT;
        if (inputs->soil_valid && inputs->soil_pct >= stop_at) {
            return decision(IRRIGATION_STOP, PUMP_REASON_AUTO);
        }
        return decision(IRRIGATION_KEEP, PUMP_REASON_NONE);
    }
    if (!config->enabled || inputs->battery_critical || !inputs->soil_valid) {
        return decision(IRRIGATION_KEEP, PUMP_REASON_NONE);
    }
    if (inputs->soil_pct < (float)config->start_below_pct &&
        !resting(config, state, inputs->now_s)) {
        return decision(IRRIGATION_START, PUMP_REASON_AUTO);
    }
    return decision(IRRIGATION_KEEP, PUMP_REASON_NONE);
}

const char *pump_reason_name(pump_reason_t reason)
{
    switch (reason) {
    case PUMP_REASON_AUTO:
        return "auto";
    case PUMP_REASON_MANUAL:
        return "manual";
    case PUMP_REASON_SAFETY_OFF:
        return "safety_off";
    case PUMP_REASON_LOW_BATTERY:
        return "low_battery";
    case PUMP_REASON_NONE:
    default:
        return NULL;
    }
}

int irrigation_config_to_json(
    const irrigation_config_t *config, char *output, size_t output_size)
{
    if (config == NULL || output == NULL || output_size == 0U) {
        return -1;
    }
    return snprintf(output,
                    output_size,
                    "[%u,%u,%u,%lu]",
                    config->enabled ? 1U : 0U,
                    (unsigned)config->start_below_pct,
                    (unsigned)config->max_runtime_s,
                    (unsigned long)config->min_rest_s);
}

static const char *skip_space(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    return text;
}

bool irrigation_config_from_json(const char *text, irrigation_config_t *config)
{
    if (text == NULL || config == NULL) {
        return false;
    }
    unsigned long values[4] = {0UL, 0UL, 0UL, 0UL};
    const char *position = skip_space(text);
    if (*position != '[') {
        return false;
    }
    position++;
    for (size_t index = 0U; index < 4U; index++) {
        position = skip_space(position);
        if (*position < '0' || *position > '9') {
            return false;
        }
        char *end = NULL;
        values[index] = strtoul(position, &end, 10);
        if (end == position || values[index] > IRRIGATION_MAX_REST_S) {
            return false;
        }
        position = skip_space(end);
        const char expected = index + 1U < 4U ? ',' : ']';
        if (*position != expected) {
            return false;
        }
        position++;
    }
    if (values[0] > 1UL || values[1] > 255UL ||
        values[2] > IRRIGATION_MAX_RUNTIME_S) {
        return false;
    }
    const irrigation_config_t parsed = {
        .enabled = values[0] == 1UL,
        .start_below_pct = (uint8_t)values[1],
        .max_runtime_s = (uint16_t)values[2],
        .min_rest_s = (uint32_t)values[3],
    };
    if (!irrigation_config_is_valid(&parsed)) {
        return false;
    }
    *config = parsed;
    return true;
}

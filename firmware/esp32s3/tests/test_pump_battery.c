/*
 * Host test for automatic watering (irrigation) and battery policy.
 * Build from firmware/esp32s3:
 *   gcc -std=c11 -Wall -Wextra -Werror \
 *       -Icomponents/irrigation/include \
 *       -Icomponents/battery_monitor/include \
 *       tests/test_pump_battery.c \
 *       components/irrigation/irrigation.c \
 *       components/battery_monitor/battery_policy.c -lm -o test_pump_battery
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "battery_policy.h"
#include "irrigation.h"

static int checks;
#define CHECK(condition) \
    do {                 \
        assert(condition); \
        checks++;        \
    } while (0)

static irrigation_config_t enabled_config(void)
{
    irrigation_config_t config;
    irrigation_config_default(&config);
    config.enabled = true;
    config.start_below_pct = 30U;
    config.max_runtime_s = 20U;
    config.min_rest_s = 600U;
    return config;
}

static irrigation_inputs_t soil(float pct, int64_t now_s)
{
    const irrigation_inputs_t inputs = {true, pct, false, now_s};
    return inputs;
}

static irrigation_decision_t decide(const irrigation_config_t *config,
                                    const irrigation_state_t *state,
                                    float pct,
                                    int64_t now_s)
{
    const irrigation_inputs_t inputs = soil(pct, now_s);
    return irrigation_decide(config, state, &inputs);
}

static void test_defaults_and_validation(void)
{
    irrigation_config_t config;
    irrigation_config_default(&config);
    CHECK(!config.enabled);
    CHECK(irrigation_config_is_valid(&config));
    config.start_below_pct = 0U;
    CHECK(!irrigation_config_is_valid(&config));
    config.start_below_pct = 96U;
    CHECK(!irrigation_config_is_valid(&config));
    config.start_below_pct = 40U;
    config.max_runtime_s = 0U;
    CHECK(!irrigation_config_is_valid(&config));
    config.max_runtime_s = 3601U;
    CHECK(!irrigation_config_is_valid(&config));
    config.max_runtime_s = 60U;
    config.min_rest_s = 86401U;
    CHECK(!irrigation_config_is_valid(&config));
}

static void test_disabled_never_starts(void)
{
    irrigation_config_t config = enabled_config();
    config.enabled = false;
    irrigation_state_t state = {0};
    CHECK(decide(&config, &state, 5.0f, 0).action ==
          IRRIGATION_KEEP);
}

static void test_start_rest_and_restart(void)
{
    const irrigation_config_t config = enabled_config();
    irrigation_state_t state = {0};
    irrigation_decision_t decision =
        decide(&config, &state, 31.0f, 0);
    CHECK(decision.action == IRRIGATION_KEEP);
    decision = decide(&config, &state, 29.0f, 0);
    CHECK(decision.action == IRRIGATION_START);
    CHECK(decision.reason == PUMP_REASON_AUTO);

    /* Pulse ended at t=100: still dry, but resting until t=700. */
    state.pump_on = false;
    state.reason = PUMP_REASON_AUTO;
    state.has_last_auto_stop = true;
    state.last_auto_stop_s = 100;
    CHECK(decide(&config, &state, 20.0f, 699).action ==
          IRRIGATION_KEEP);
    CHECK(decide(&config, &state, 20.0f, 700).action ==
          IRRIGATION_START);
    /* A clock that moved backwards does not block watering forever. */
    CHECK(decide(&config, &state, 20.0f, 50).action ==
          IRRIGATION_START);
}

static void test_auto_run_stops_when_wet_enough(void)
{
    const irrigation_config_t config = enabled_config();
    irrigation_state_t state = {true, PUMP_REASON_AUTO, false, 0};
    CHECK(decide(&config, &state, 34.0f, 10).action ==
          IRRIGATION_KEEP); /* below 30 + 5 margin */
    irrigation_decision_t decision =
        decide(&config, &state, 35.0f, 10);
    CHECK(decision.action == IRRIGATION_STOP);
    CHECK(decision.reason == PUMP_REASON_AUTO);

    irrigation_config_t disabled = config;
    disabled.enabled = false;
    CHECK(decide(&disabled, &state, 10.0f, 10).action ==
          IRRIGATION_STOP);
}

static void test_manual_run_is_left_alone(void)
{
    const irrigation_config_t config = enabled_config();
    irrigation_state_t state = {true, PUMP_REASON_MANUAL, false, 0};
    CHECK(decide(&config, &state, 90.0f, 10).action ==
          IRRIGATION_KEEP);
}

static void test_invalid_soil_and_critical_battery(void)
{
    const irrigation_config_t config = enabled_config();
    irrigation_state_t state = {0};
    irrigation_inputs_t inputs = soil(5.0f, 0);
    inputs.soil_valid = false;
    CHECK(irrigation_decide(&config, &state, &inputs).action == IRRIGATION_KEEP);

    inputs = soil(5.0f, 0);
    inputs.battery_critical = true;
    CHECK(irrigation_decide(&config, &state, &inputs).action == IRRIGATION_KEEP);

    /* Critical battery stops even a manual run. */
    state.pump_on = true;
    state.reason = PUMP_REASON_MANUAL;
    const irrigation_decision_t decision =
        irrigation_decide(&config, &state, &inputs);
    CHECK(decision.action == IRRIGATION_STOP);
    CHECK(decision.reason == PUMP_REASON_LOW_BATTERY);
}

static void test_json_round_trip(void)
{
    irrigation_config_t config = enabled_config();
    char text[48];
    CHECK(irrigation_config_to_json(&config, text, sizeof(text)) > 0);
    CHECK(strcmp(text, "[1,30,20,600]") == 0);

    irrigation_config_t parsed;
    CHECK(irrigation_config_from_json(" [ 0 , 45 , 120 , 86400 ] ,", &parsed));
    CHECK(!parsed.enabled);
    CHECK(parsed.start_below_pct == 45U);
    CHECK(parsed.max_runtime_s == 120U);
    CHECK(parsed.min_rest_s == 86400U);

    const char *invalid[] = {
        "[1,30,20]",       "[1,30,20,600,1]", "[2,30,20,600]",
        "[1,0,20,600]",    "[1,30,0,600]",    "[1,30,3601,600]",
        "[1,30,20,86401]", "[1,-5,20,600]",   "[1,30.5,20,600]",
        "null",            "[true,30,20,600]",
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]);
         index++) {
        CHECK(!irrigation_config_from_json(invalid[index], &parsed));
    }
    CHECK(strcmp(pump_reason_name(PUMP_REASON_SAFETY_OFF), "safety_off") == 0);
    CHECK(pump_reason_name(PUMP_REASON_NONE) == NULL);
}

static void test_battery_curve(void)
{
    CHECK(battery_percent_from_mv(4300U) == 100.0f);
    CHECK(battery_percent_from_mv(4200U) == 100.0f);
    CHECK(battery_percent_from_mv(3000U) == 0.0f);
    CHECK(fabsf(battery_percent_from_mv(3840U) - 50.0f) < 0.01f);
    CHECK(fabsf(battery_percent_from_mv(3760U) - 27.5f) < 0.01f);
    /* Monotonic over the whole range. */
    float previous = -1.0f;
    for (uint32_t mv = 3200U; mv <= 4250U; mv += 5U) {
        const float pct = battery_percent_from_mv(mv);
        CHECK(pct >= previous);
        previous = pct;
    }
}

static void test_battery_levels(void)
{
    const battery_policy_t policy = {20U, 5U, 3U};
    CHECK(battery_classify(&policy, false, 50.0f, BATTERY_LEVEL_NORMAL) ==
          BATTERY_LEVEL_UNKNOWN);
    CHECK(battery_classify(&policy, true, 50.0f, BATTERY_LEVEL_UNKNOWN) ==
          BATTERY_LEVEL_NORMAL);
    CHECK(battery_classify(&policy, true, 19.9f, BATTERY_LEVEL_NORMAL) ==
          BATTERY_LEVEL_LOW);
    /* Hysteresis: stays low until 23 %. */
    CHECK(battery_classify(&policy, true, 22.0f, BATTERY_LEVEL_LOW) ==
          BATTERY_LEVEL_LOW);
    CHECK(battery_classify(&policy, true, 23.0f, BATTERY_LEVEL_LOW) ==
          BATTERY_LEVEL_NORMAL);
    CHECK(battery_classify(&policy, true, 4.0f, BATTERY_LEVEL_LOW) ==
          BATTERY_LEVEL_CRITICAL);
    CHECK(battery_classify(&policy, true, 7.0f, BATTERY_LEVEL_CRITICAL) ==
          BATTERY_LEVEL_CRITICAL);
    CHECK(battery_classify(&policy, true, 8.0f, BATTERY_LEVEL_CRITICAL) ==
          BATTERY_LEVEL_LOW);
    CHECK(battery_classify(&policy, true, 30.0f, BATTERY_LEVEL_CRITICAL) ==
          BATTERY_LEVEL_NORMAL);
    CHECK(strcmp(battery_level_name(BATTERY_LEVEL_CRITICAL), "critical") == 0);
}

int main(void)
{
    test_defaults_and_validation();
    test_disabled_never_starts();
    test_start_rest_and_restart();
    test_auto_run_stops_when_wet_enough();
    test_manual_run_is_left_alone();
    test_invalid_soil_and_critical_battery();
    test_json_round_trip();
    test_battery_curve();
    test_battery_levels();
    printf("pump/battery tests: %d checks passed\n", checks);
    return 0;
}

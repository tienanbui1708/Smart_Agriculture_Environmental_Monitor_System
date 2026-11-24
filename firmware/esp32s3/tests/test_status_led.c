/*
 * Host test for the status LED colour policy. Build from firmware/esp32s3:
 *   gcc -std=c11 -Wall -Wextra -Werror -Icomponents/status_led/include \
 *       tests/test_status_led.c components/status_led/status_led_policy.c \
 *       -o test_status_led
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "status_led_policy.h"

static int checks;
#define CHECK(condition) \
    do {                 \
        assert(condition); \
        checks++;        \
    } while (0)

static status_led_inputs_t online(void)
{
    status_led_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    inputs.server_connected = true;
    return inputs;
}

static void test_priorities(void)
{
    status_led_inputs_t inputs = online();
    CHECK(status_led_select(&inputs) == STATUS_LED_ONLINE);

    inputs.server_connected = false;
    CHECK(status_led_select(&inputs) == STATUS_LED_OFFLINE);
    /* Backlog without a server is still just offline. */
    inputs.backlog_pending = true;
    CHECK(status_led_select(&inputs) == STATUS_LED_OFFLINE);

    inputs = online();
    inputs.direct_client = true;
    CHECK(status_led_select(&inputs) == STATUS_LED_DIRECT);
    inputs.backlog_pending = true;
    CHECK(status_led_select(&inputs) == STATUS_LED_SYNCING);
    inputs.sensor_error = true;
    CHECK(status_led_select(&inputs) == STATUS_LED_SENSOR_ERROR);
    inputs.booting = true;
    CHECK(status_led_select(&inputs) == STATUS_LED_BOOT);
    inputs.sleeping = true;
    CHECK(status_led_select(&inputs) == STATUS_LED_OFF);
    CHECK(status_led_select(NULL) == STATUS_LED_OFF);

    /* Offline phone session shows blue, not blinking yellow. */
    memset(&inputs, 0, sizeof(inputs));
    inputs.direct_client = true;
    CHECK(status_led_select(&inputs) == STATUS_LED_DIRECT);
}

static void test_colors(void)
{
    status_led_color_t color = status_led_color(STATUS_LED_ONLINE, 255U, true);
    CHECK(color.red == 0U && color.green == 255U && color.blue == 0U);
    color = status_led_color(STATUS_LED_ONLINE, 16U, true);
    CHECK(color.green == 16U && color.red == 0U);
    color = status_led_color(STATUS_LED_SENSOR_ERROR, 255U, false);
    CHECK(color.red == 255U); /* solid states ignore the blink phase */

    CHECK(status_led_blinks(STATUS_LED_OFFLINE));
    CHECK(!status_led_blinks(STATUS_LED_ONLINE));
    color = status_led_color(STATUS_LED_OFFLINE, 255U, false);
    CHECK(color.red == 0U && color.green == 0U && color.blue == 0U);
    color = status_led_color(STATUS_LED_OFFLINE, 255U, true);
    CHECK(color.red == 255U && color.green == 160U && color.blue == 0U);

    color = status_led_color(STATUS_LED_OFF, 255U, true);
    CHECK(color.red == 0U && color.green == 0U && color.blue == 0U);
    /* Every visible state stays visible at the lowest brightness. */
    const status_led_state_t visible[] = {
        STATUS_LED_BOOT, STATUS_LED_SENSOR_ERROR, STATUS_LED_SYNCING,
        STATUS_LED_DIRECT, STATUS_LED_ONLINE, STATUS_LED_OFFLINE,
    };
    for (size_t index = 0; index < sizeof(visible) / sizeof(visible[0]); index++) {
        color = status_led_color(visible[index], 2U, true);
        CHECK(color.red + color.green + color.blue > 0);
        CHECK(strcmp(status_led_state_name(visible[index]), "off") != 0);
    }
}

int main(void)
{
    test_priorities();
    test_colors();
    printf("status LED tests: %d checks passed\n", checks);
    return 0;
}

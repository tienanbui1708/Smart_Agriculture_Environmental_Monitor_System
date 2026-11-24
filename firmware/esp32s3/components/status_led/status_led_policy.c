#include "status_led_policy.h"

#include <stddef.h>

status_led_state_t status_led_select(const status_led_inputs_t *inputs)
{
    if (inputs == NULL || inputs->sleeping) {
        return STATUS_LED_OFF;
    }
    if (inputs->booting) {
        return STATUS_LED_BOOT;
    }
    if (inputs->sensor_error) {
        return STATUS_LED_SENSOR_ERROR;
    }
    if (inputs->server_connected && inputs->backlog_pending) {
        return STATUS_LED_SYNCING;
    }
    if (inputs->direct_client) {
        return STATUS_LED_DIRECT;
    }
    if (inputs->server_connected) {
        return STATUS_LED_ONLINE;
    }
    return STATUS_LED_OFFLINE;
}

bool status_led_blinks(status_led_state_t state)
{
    return state == STATUS_LED_OFFLINE;
}

static uint8_t scale(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * (uint16_t)brightness + 127U) / 255U);
}

status_led_color_t status_led_color(status_led_state_t state,
                                    uint8_t brightness,
                                    bool blink_phase_on)
{
    status_led_color_t color = {0U, 0U, 0U};
    switch (state) {
    case STATUS_LED_BOOT:
        color = (status_led_color_t){255U, 255U, 255U};
        break;
    case STATUS_LED_SENSOR_ERROR:
        color = (status_led_color_t){255U, 0U, 0U};
        break;
    case STATUS_LED_SYNCING:
        color = (status_led_color_t){160U, 0U, 255U};
        break;
    case STATUS_LED_DIRECT:
        color = (status_led_color_t){0U, 0U, 255U};
        break;
    case STATUS_LED_ONLINE:
        color = (status_led_color_t){0U, 255U, 0U};
        break;
    case STATUS_LED_OFFLINE:
        color = (status_led_color_t){255U, 160U, 0U};
        break;
    case STATUS_LED_OFF:
    default:
        break;
    }
    if (status_led_blinks(state) && !blink_phase_on) {
        return (status_led_color_t){0U, 0U, 0U};
    }
    color.red = scale(color.red, brightness);
    color.green = scale(color.green, brightness);
    color.blue = scale(color.blue, brightness);
    return color;
}

const char *status_led_state_name(status_led_state_t state)
{
    switch (state) {
    case STATUS_LED_BOOT:
        return "boot";
    case STATUS_LED_SENSOR_ERROR:
        return "sensor_error";
    case STATUS_LED_SYNCING:
        return "syncing";
    case STATUS_LED_DIRECT:
        return "direct";
    case STATUS_LED_ONLINE:
        return "online";
    case STATUS_LED_OFFLINE:
        return "offline";
    case STATUS_LED_OFF:
    default:
        return "off";
    }
}

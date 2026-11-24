#pragma once

/*
 * Which colour the status LED shows. Pure C, host tested.
 *
 * Highest priority first:
 *   off            sleeping (Deep Sleep is about to start) or LED disabled
 *   white          booting, not yet through the first sample
 *   red            a sensor reports an error
 *   purple         connected to the server and sending offline backlog
 *   blue           a phone is connected over BLE or the Wi-Fi AP
 *   green          connected to the MQTT server, nothing pending
 *   yellow, blink  Wi-Fi or server unreachable (data is kept offline)
 *
 * Blinking needs a periodic timer; in Light Sleep / Battery mode the
 * firmware shows the same colour without blinking to save power.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool sleeping;
    bool booting;
    bool sensor_error;
    bool server_connected;
    bool backlog_pending;
    bool direct_client;
} status_led_inputs_t;

typedef enum {
    STATUS_LED_OFF = 0,
    STATUS_LED_BOOT,
    STATUS_LED_SENSOR_ERROR,
    STATUS_LED_SYNCING,
    STATUS_LED_DIRECT,
    STATUS_LED_ONLINE,
    STATUS_LED_OFFLINE,
} status_led_state_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} status_led_color_t;

status_led_state_t status_led_select(const status_led_inputs_t *inputs);

/* True when the state is shown blinking (on/off every half period). */
bool status_led_blinks(status_led_state_t state);

/*
 * Colour for a state at full scale (0..255), then scaled by brightness
 * (0..255). blink_phase_on selects the "on" half of a blinking state.
 */
status_led_color_t status_led_color(status_led_state_t state,
                                    uint8_t brightness,
                                    bool blink_phase_on);

const char *status_led_state_name(status_led_state_t state);

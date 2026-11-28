#pragma once

/*
 * WS2812 status LED (DevKitC-1 onboard RGB LED, GPIO48 by default).
 * Colours and priorities: see status_led_policy.h.
 *
 * The application supplies a provider that fills status_led_inputs_t.
 * status_led_refresh() re-evaluates now (call it on events). In Direct mode
 * a 500 ms timer also re-evaluates and blinks; in Light Sleep / Battery mode
 * the timer is stopped so the CPU can sleep, and the colour stays solid.
 */

#include <stdbool.h>

#include "esp_err.h"
#include "status_led_policy.h"

typedef void (*status_led_provider_t)(void *context, status_led_inputs_t *inputs);

typedef struct {
    void *strip;    /* led_strip_handle_t */
    void *timer;    /* esp_timer_handle_t */
    void *mutex;    /* SemaphoreHandle_t */
    status_led_provider_t provider;
    void *context;
    status_led_state_t state;
    status_led_color_t shown;
    bool blink_phase;
    bool periodic;
    bool initialized;
} status_led_t;

/* Returns ESP_OK and does nothing when SAEMS_STATUS_LED_ENABLED is off. */
esp_err_t status_led_init(status_led_t *led,
                          status_led_provider_t provider,
                          void *context);
void status_led_refresh(status_led_t *led);
/* Enable the 500 ms blink/poll timer (Direct mode only). */
void status_led_set_periodic(status_led_t *led, bool enabled);
/* Turn the LED dark before Deep Sleep; WS2812 keeps its colour otherwise. */
void status_led_off(status_led_t *led);

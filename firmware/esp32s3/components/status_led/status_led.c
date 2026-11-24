#include "status_led.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "sdkconfig.h"

#define STATUS_LED_PERIOD_US 500000ULL

static const char *TAG = "STATUS_LED";

#ifdef CONFIG_SAEMS_STATUS_LED_ENABLED

static void show_locked(status_led_t *led, status_led_color_t color)
{
    if (memcmp(&color, &led->shown, sizeof(color)) == 0) {
        return;
    }
    esp_err_t result = ESP_OK;
    if (color.red == 0U && color.green == 0U && color.blue == 0U) {
        result = led_strip_clear(led->strip);
    } else {
        result = led_strip_set_pixel(led->strip, 0, color.red, color.green,
                                     color.blue);
        if (result == ESP_OK) {
            result = led_strip_refresh(led->strip);
        }
    }
    if (result == ESP_OK) {
        led->shown = color;
    } else {
        ESP_LOGW(TAG, "LED update failed: %s", esp_err_to_name(result));
    }
}

static void evaluate_locked(status_led_t *led, bool advance_blink)
{
    status_led_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    led->provider(led->context, &inputs);
    const status_led_state_t state = status_led_select(&inputs);
    if (state != led->state) {
        ESP_LOGI(TAG, "Status %s -> %s", status_led_state_name(led->state),
                 status_led_state_name(state));
        led->state = state;
        led->blink_phase = true;
    } else if (advance_blink) {
        led->blink_phase = !led->blink_phase;
    }
    /* Without the timer a blinking colour would freeze dark half the time. */
    const bool phase_on = led->periodic ? led->blink_phase : true;
    show_locked(led, status_led_color(state,
                                      (uint8_t)CONFIG_SAEMS_STATUS_LED_BRIGHTNESS,
                                      phase_on));
}

static void timer_tick(void *arg)
{
    status_led_t *led = arg;
    xSemaphoreTake((SemaphoreHandle_t)led->mutex, portMAX_DELAY);
    if (led->periodic) {
        evaluate_locked(led, true);
    }
    xSemaphoreGive((SemaphoreHandle_t)led->mutex);
}

esp_err_t status_led_init(status_led_t *led,
                          status_led_provider_t provider,
                          void *context)
{
    if (led == NULL || provider == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(led, 0, sizeof(*led));
    led->provider = provider;
    led->context = context;
    led->state = STATUS_LED_OFF;
    led->mutex = xSemaphoreCreateMutex();
    if (led->mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_SAEMS_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10U * 1000U * 1000U,
    };
    led_strip_handle_t strip = NULL;
    esp_err_t result = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (result != ESP_OK) {
        return result;
    }
    led->strip = strip;
    const esp_timer_create_args_t timer_args = {
        .callback = timer_tick,
        .arg = led,
        .name = "status_led",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer = NULL;
    result = esp_timer_create(&timer_args, &timer);
    if (result != ESP_OK) {
        return result;
    }
    led->timer = timer;
    led->initialized = true;
    ESP_LOGI(TAG, "WS2812 status LED on GPIO%d, brightness %d/255",
             CONFIG_SAEMS_STATUS_LED_GPIO, CONFIG_SAEMS_STATUS_LED_BRIGHTNESS);
    status_led_refresh(led);
    return ESP_OK;
}

void status_led_refresh(status_led_t *led)
{
    if (led == NULL || !led->initialized) {
        return;
    }
    xSemaphoreTake((SemaphoreHandle_t)led->mutex, portMAX_DELAY);
    evaluate_locked(led, false);
    xSemaphoreGive((SemaphoreHandle_t)led->mutex);
}

void status_led_set_periodic(status_led_t *led, bool enabled)
{
    if (led == NULL || !led->initialized) {
        return;
    }
    xSemaphoreTake((SemaphoreHandle_t)led->mutex, portMAX_DELAY);
    if (enabled != led->periodic) {
        led->periodic = enabled;
        if (enabled) {
            (void)esp_timer_start_periodic(led->timer, STATUS_LED_PERIOD_US);
        } else {
            (void)esp_timer_stop(led->timer);
        }
        evaluate_locked(led, false);
    }
    xSemaphoreGive((SemaphoreHandle_t)led->mutex);
}

void status_led_off(status_led_t *led)
{
    if (led == NULL || !led->initialized) {
        return;
    }
    xSemaphoreTake((SemaphoreHandle_t)led->mutex, portMAX_DELAY);
    (void)esp_timer_stop(led->timer);
    led->periodic = false;
    led->state = STATUS_LED_OFF;
    show_locked(led, (status_led_color_t){0U, 0U, 0U});
    xSemaphoreGive((SemaphoreHandle_t)led->mutex);
}

#else /* LED disabled in menuconfig */

esp_err_t status_led_init(status_led_t *led,
                          status_led_provider_t provider,
                          void *context)
{
    (void)provider;
    (void)context;
    if (led != NULL) {
        memset(led, 0, sizeof(*led));
    }
    ESP_LOGI(TAG, "Status LED disabled");
    return ESP_OK;
}

void status_led_refresh(status_led_t *led)
{
    (void)led;
}

void status_led_set_periodic(status_led_t *led, bool enabled)
{
    (void)led;
    (void)enabled;
}

void status_led_off(status_led_t *led)
{
    (void)led;
}

#endif

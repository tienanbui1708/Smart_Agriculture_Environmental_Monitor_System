#include "battery_monitor.h"

#include <string.h>

#include "esp_adc/adc_cali_scheme.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define BATTERY_ADC_SAMPLES 16
#define BATTERY_SIM_MAGIC 0x42415453U /* "BATS" */

static const char *TAG = "BATTERY";

/* Simulated charge survives Deep Sleep; a power cycle restarts it. */
typedef struct {
    uint32_t magic;
    float percent;
} battery_sim_state_t;

RTC_DATA_ATTR static battery_sim_state_t sim_state;

static battery_policy_t configured_policy(void)
{
#if defined(CONFIG_SAEMS_BATTERY_SOURCE_ADC) || \
    defined(CONFIG_SAEMS_BATTERY_SOURCE_SIMULATED)
    const battery_policy_t policy = {
        .low_pct = (uint8_t)CONFIG_SAEMS_BATTERY_LOW_PCT,
        .critical_pct = (uint8_t)CONFIG_SAEMS_BATTERY_CRITICAL_PCT,
        .hysteresis_pct = 3U,
    };
#else
    const battery_policy_t policy = {20U, 5U, 3U};
#endif
    return policy;
}

const char *battery_source_name(battery_source_t source)
{
    switch (source) {
    case BATTERY_SOURCE_ADC:
        return "adc";
    case BATTERY_SOURCE_SIMULATED:
        return "simulated";
    case BATTERY_SOURCE_NONE:
    default:
        return "none";
    }
}

#ifdef CONFIG_SAEMS_BATTERY_SOURCE_ADC
static esp_err_t init_adc(battery_monitor_t *monitor,
                          adc_oneshot_unit_handle_t shared_adc1)
{
    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    esp_err_t result = adc_oneshot_io_to_channel(
        CONFIG_SAEMS_BATTERY_ADC_GPIO, &unit, &channel);
    if (result != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin", CONFIG_SAEMS_BATTERY_ADC_GPIO);
        return ESP_ERR_INVALID_ARG;
    }
    if (shared_adc1 != NULL) {
        monitor->adc = shared_adc1;
    } else {
        const adc_oneshot_unit_init_cfg_t unit_config = {
            .unit_id = ADC_UNIT_1,
            .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        result = adc_oneshot_new_unit(&unit_config, &monitor->adc);
        if (result != ESP_OK) {
            return result;
        }
        monitor->owns_adc = true;
    }
    monitor->channel = (int)channel;
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    result = adc_oneshot_config_channel(monitor->adc, channel, &channel_config);
    if (result != ESP_OK) {
        return result;
    }
    const adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(
            &cali_config, &monitor->calibration) != ESP_OK) {
        /* Uncalibrated readings would be wrong by up to ~10 %; refuse. */
        ESP_LOGE(TAG, "ADC calibration unavailable; battery level disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "Battery ADC on GPIO%d (ADC1_CH%d), divider x%.2f",
             CONFIG_SAEMS_BATTERY_ADC_GPIO, (int)channel,
             (double)CONFIG_SAEMS_BATTERY_DIVIDER_X100 / 100.0);
    return ESP_OK;
}

static esp_err_t measure_adc(battery_monitor_t *monitor, uint32_t *millivolts)
{
    int64_t sum_mv = 0;
    for (int sample = 0; sample < BATTERY_ADC_SAMPLES; sample++) {
        int raw = 0;
        int pin_mv = 0;
        esp_err_t result = adc_oneshot_read(
            monitor->adc, (adc_channel_t)monitor->channel, &raw);
        if (result == ESP_OK) {
            result = adc_cali_raw_to_voltage(monitor->calibration, raw, &pin_mv);
        }
        if (result != ESP_OK) {
            return result;
        }
        sum_mv += pin_mv;
    }
    const int64_t pin_average = sum_mv / BATTERY_ADC_SAMPLES;
    *millivolts = (uint32_t)(
        (pin_average * CONFIG_SAEMS_BATTERY_DIVIDER_X100) / 100);
    return ESP_OK;
}
#endif

esp_err_t battery_monitor_init(battery_monitor_t *monitor,
                               adc_oneshot_unit_handle_t shared_adc1)
{
    if (monitor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)shared_adc1;
    memset(monitor, 0, sizeof(*monitor));
#if defined(CONFIG_SAEMS_BATTERY_SOURCE_ADC)
    monitor->source = BATTERY_SOURCE_ADC;
    esp_err_t result = init_adc(monitor, shared_adc1);
    if (result != ESP_OK) {
        monitor->source = BATTERY_SOURCE_NONE;
        return result;
    }
#elif defined(CONFIG_SAEMS_BATTERY_SOURCE_SIMULATED)
    monitor->source = BATTERY_SOURCE_SIMULATED;
    if (sim_state.magic != BATTERY_SIM_MAGIC) {
        sim_state.magic = BATTERY_SIM_MAGIC;
        sim_state.percent = (float)CONFIG_SAEMS_BATTERY_SIM_START_PCT;
    }
    ESP_LOGW(TAG, "Battery is SIMULATED: %.1f%%, drain %.1f%%/sample",
             (double)sim_state.percent,
             (double)CONFIG_SAEMS_BATTERY_SIM_DRAIN_X10 / 10.0);
#else
    monitor->source = BATTERY_SOURCE_NONE;
#endif
    monitor->initialized = true;
    return ESP_OK;
}

esp_err_t battery_monitor_read(battery_monitor_t *monitor,
                               battery_reading_t *reading)
{
    if (monitor == NULL || reading == NULL || !monitor->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    const battery_policy_t policy = configured_policy();
    battery_reading_t next = {0};
    esp_err_t result = ESP_OK;
    switch (monitor->source) {
#ifdef CONFIG_SAEMS_BATTERY_SOURCE_ADC
    case BATTERY_SOURCE_ADC:
        result = measure_adc(monitor, &next.millivolts);
        if (result == ESP_OK) {
            next.valid = true;
            next.percent = battery_percent_from_mv(next.millivolts);
        }
        break;
#endif
#ifdef CONFIG_SAEMS_BATTERY_SOURCE_SIMULATED
    case BATTERY_SOURCE_SIMULATED: {
        const float drain = (float)CONFIG_SAEMS_BATTERY_SIM_DRAIN_X10 / 10.0f;
        sim_state.percent = sim_state.percent > drain
                                ? sim_state.percent - drain
                                : 0.0f;
        next.valid = true;
        next.percent = sim_state.percent;
        break;
    }
#endif
    case BATTERY_SOURCE_NONE:
    default:
        break;
    }
    next.level = battery_classify(
        &policy, next.valid, next.percent, monitor->last.level);
    if (next.level != monitor->last.level) {
        ESP_LOGW(TAG, "Battery level %s -> %s (%.1f%%)",
                 battery_level_name(monitor->last.level),
                 battery_level_name(next.level),
                 (double)next.percent);
    }
    monitor->last = next;
    *reading = next;
    return result;
}

esp_err_t battery_monitor_simulate(battery_monitor_t *monitor, float percent)
{
    if (monitor == NULL || !monitor->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    if (monitor->source != BATTERY_SOURCE_SIMULATED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!(percent >= 0.0f && percent <= 100.0f)) {
        return ESP_ERR_INVALID_ARG;
    }
    sim_state.magic = BATTERY_SIM_MAGIC;
    sim_state.percent = percent;
    ESP_LOGW(TAG, "Simulated battery set to %.1f%%", (double)percent);
    return ESP_OK;
}

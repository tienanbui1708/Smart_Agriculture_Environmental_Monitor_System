#include "config_manager.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#define CONFIG_NAMESPACE "saems_cfg"
#define CONFIG_DOCUMENT_MAX 1024
#define THRESHOLD_BLOB_KEY "thresholds"
#define THRESHOLD_BLOB_VERSION 1U
#define IRRIGATION_BLOB_KEY "irrigation"
#define IRRIGATION_BLOB_VERSION 1U

/* Stored layout of the alarm limits. Bump the version when it changes. */
typedef struct {
    uint8_t version;
    uint8_t reserved;
    uint16_t enabled_mask;
    float value[THRESHOLD_LIMIT_COUNT];
} threshold_blob_t;

typedef struct {
    uint8_t version;
    uint8_t enabled;
    uint8_t start_below_pct;
    uint8_t reserved;
    uint16_t max_runtime_s;
    uint16_t reserved2;
    uint32_t min_rest_s;
} irrigation_blob_t;

/* Keys the server must never change over MQTT. */
static const char *const remote_forbidden_keys[] = {
    "device_id",      "wifi_ssid",        "wifi_password",
    "mqtt_uri",       "mqtt_username",    "mqtt_password",
    "ap_ssid",        "ap_password",      "soil_dry_raw",
    "soil_wet_raw",   "soil_enabled",     "soil_calibrated",
    "ap_enabled",     "ble_enabled",      "power_save_enabled",
};

const char *saems_power_mode_name(saems_power_mode_t mode)
{
    switch (mode) {
    case SAEMS_POWER_MODE_DIRECT:
        return "direct";
    case SAEMS_POWER_MODE_LIGHT:
        return "light";
    case SAEMS_POWER_MODE_BATTERY:
        return "battery";
    default:
        return "direct";
    }
}

esp_err_t saems_power_mode_from_string(
    const char *value, saems_power_mode_t *mode)
{
    if (value == NULL || mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(value, "direct") == 0) {
        *mode = SAEMS_POWER_MODE_DIRECT;
    } else if (strcmp(value, "light") == 0) {
        *mode = SAEMS_POWER_MODE_LIGHT;
    } else if (strcmp(value, "battery") == 0) {
        *mode = SAEMS_POWER_MODE_BATTERY;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static SemaphoreHandle_t manager_mutex(config_manager_t *manager)
{
    return (SemaphoreHandle_t)manager->mutex;
}

static void load_u32(nvs_handle_t handle, const char *key, uint32_t *value)
{
    uint32_t stored = 0;
    if (nvs_get_u32(handle, key, &stored) == ESP_OK) {
        *value = stored;
    }
}

static void load_i32(nvs_handle_t handle, const char *key, int *value)
{
    int32_t stored = 0;
    if (nvs_get_i32(handle, key, &stored) == ESP_OK) {
        *value = (int)stored;
    }
}

static void load_bool(nvs_handle_t handle, const char *key, bool *value)
{
    uint8_t stored = 0;
    if (nvs_get_u8(handle, key, &stored) == ESP_OK) {
        *value = stored != 0;
    }
}

static void load_string(
    nvs_handle_t handle, const char *key, char *value, size_t value_size)
{
    size_t stored_size = value_size;
    if (nvs_get_str(handle, key, value, &stored_size) != ESP_OK) {
        return;
    }
    value[value_size - 1] = '\0';
}

static const char *json_value(const char *json, const char *key)
{
    char pattern[64];
    int length = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (length < 0 || length >= (int)sizeof(pattern)) {
        return NULL;
    }
    const char *position = strstr(json, pattern);
    if (position == NULL) {
        return NULL;
    }
    position = strchr(position + length, ':');
    if (position == NULL) {
        return NULL;
    }
    position++;
    while (*position == ' ' || *position == '\t' || *position == '\r' ||
           *position == '\n') {
        position++;
    }
    return position;
}

static esp_err_t parse_optional_int(
    const char *json,
    const char *key,
    int minimum,
    int maximum,
    int *value)
{
    const char *position = json_value(json, key);
    if (position == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    char *end = NULL;
    long parsed = strtol(position, &end, 10);
    if (end == position || parsed < minimum || parsed > maximum) {
        return ESP_ERR_INVALID_ARG;
    }
    *value = (int)parsed;
    return ESP_OK;
}

static esp_err_t parse_optional_bool(
    const char *json, const char *key, bool *value)
{
    const char *position = json_value(json, key);
    if (position == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strncmp(position, "true", 4) == 0) {
        *value = true;
        return ESP_OK;
    }
    if (strncmp(position, "false", 5) == 0) {
        *value = false;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t parse_optional_string(
    const char *json, const char *key, char *value, size_t value_size)
{
    const char *position = json_value(json, key);
    if (position == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (*position != '"' || value_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    position++;
    const char *end = position;
    while (*end != '\0' && *end != '"') {
        /* The provisioning contract intentionally accepts plain strings only.
         * Reject escapes instead of interpreting malformed JSON differently. */
        if (*end == '\\' || (unsigned char)*end < 0x20) {
            return ESP_ERR_INVALID_ARG;
        }
        end++;
    }
    if (*end != '"') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = (size_t)(end - position);
    if (length >= value_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(value, position, length);
    value[length] = '\0';
    return ESP_OK;
}

static esp_err_t parse_optional_thresholds(
    const char *json, threshold_limits_t *limits)
{
    const char *position = json_value(json, "thresholds");
    if (position == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    threshold_limits_t parsed;
    if (!threshold_limits_from_json(position, &parsed) ||
        !threshold_limits_are_valid(&parsed)) {
        return ESP_ERR_INVALID_ARG;
    }
    *limits = parsed;
    return ESP_OK;
}

static esp_err_t parse_optional_irrigation(
    const char *json, irrigation_config_t *irrigation)
{
    const char *position = json_value(json, "irrigation");
    if (position == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    irrigation_config_t parsed;
    if (!irrigation_config_from_json(position, &parsed)) {
        return ESP_ERR_INVALID_ARG;
    }
    *irrigation = parsed;
    return ESP_OK;
}

static bool irrigation_equal(
    const irrigation_config_t *left, const irrigation_config_t *right)
{
    return left->enabled == right->enabled &&
           left->start_below_pct == right->start_below_pct &&
           left->max_runtime_s == right->max_runtime_s &&
           left->min_rest_s == right->min_rest_s;
}

static bool thresholds_equal(
    const threshold_limits_t *left, const threshold_limits_t *right)
{
    for (size_t index = 0U; index < THRESHOLD_LIMIT_COUNT; index++) {
        if (left->enabled[index] != right->enabled[index] ||
            (left->enabled[index] &&
             left->value[index] != right->value[index])) {
            return false;
        }
    }
    return true;
}

static bool server_managed_changed(
    const saems_config_t *before, const saems_config_t *after)
{
    return before->telemetry_interval_s != after->telemetry_interval_s ||
           before->power_mode != after->power_mode ||
           !thresholds_equal(&before->thresholds, &after->thresholds) ||
           !irrigation_equal(&before->irrigation, &after->irrigation);
}

static void load_thresholds(nvs_handle_t handle, threshold_limits_t *limits)
{
    threshold_blob_t blob;
    size_t blob_size = sizeof(blob);
    if (nvs_get_blob(handle, THRESHOLD_BLOB_KEY, &blob, &blob_size) != ESP_OK ||
        blob_size != sizeof(blob) || blob.version != THRESHOLD_BLOB_VERSION) {
        return;
    }
    threshold_limits_t stored;
    for (size_t index = 0U; index < THRESHOLD_LIMIT_COUNT; index++) {
        stored.enabled[index] = (blob.enabled_mask & (1U << index)) != 0U;
        stored.value[index] = blob.value[index];
    }
    if (threshold_limits_are_valid(&stored)) {
        *limits = stored;
    }
}

static esp_err_t save_thresholds(
    nvs_handle_t handle, const threshold_limits_t *limits)
{
    threshold_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = (uint8_t)THRESHOLD_BLOB_VERSION;
    for (size_t index = 0U; index < THRESHOLD_LIMIT_COUNT; index++) {
        if (limits->enabled[index]) {
            blob.enabled_mask = (uint16_t)(blob.enabled_mask | (1U << index));
        }
        blob.value[index] = limits->value[index];
    }
    return nvs_set_blob(handle, THRESHOLD_BLOB_KEY, &blob, sizeof(blob));
}

static void load_irrigation(nvs_handle_t handle, irrigation_config_t *config)
{
    irrigation_blob_t blob;
    size_t blob_size = sizeof(blob);
    if (nvs_get_blob(handle, IRRIGATION_BLOB_KEY, &blob, &blob_size) != ESP_OK ||
        blob_size != sizeof(blob) || blob.version != IRRIGATION_BLOB_VERSION) {
        return;
    }
    const irrigation_config_t stored = {
        .enabled = blob.enabled != 0U,
        .start_below_pct = blob.start_below_pct,
        .max_runtime_s = blob.max_runtime_s,
        .min_rest_s = blob.min_rest_s,
    };
    if (irrigation_config_is_valid(&stored)) {
        *config = stored;
    }
}

static esp_err_t save_irrigation(
    nvs_handle_t handle, const irrigation_config_t *config)
{
    irrigation_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = (uint8_t)IRRIGATION_BLOB_VERSION;
    blob.enabled = config->enabled ? 1U : 0U;
    blob.start_below_pct = config->start_below_pct;
    blob.max_runtime_s = config->max_runtime_s;
    blob.min_rest_s = config->min_rest_s;
    return nvs_set_blob(handle, IRRIGATION_BLOB_KEY, &blob, sizeof(blob));
}

static esp_err_t save_locked(config_manager_t *manager)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_set_str(handle, "device_id", manager->current.device_id);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "wifi_ssid", manager->current.wifi_ssid);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "wifi_pass", manager->current.wifi_password);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "mqtt_uri", manager->current.mqtt_uri);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "mqtt_user", manager->current.mqtt_username);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "mqtt_pass", manager->current.mqtt_password);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "ap_ssid", manager->current.ap_ssid);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "ap_pass", manager->current.ap_password);
    }
    if (result == ESP_OK) {
        result = nvs_set_u32(
        handle, "interval", manager->current.telemetry_interval_s);
    }
    if (result == ESP_OK) {
        result = nvs_set_i32(
            handle, "soil_dry", manager->current.soil_dry_raw);
    }
    if (result == ESP_OK) {
        result = nvs_set_i32(
            handle, "soil_wet", manager->current.soil_wet_raw);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(
            handle, "soil_on", manager->current.soil_enabled ? 1 : 0);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(
            handle, "soil_cal", manager->current.soil_calibrated ? 1 : 0);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(
            handle, "ap_on", manager->current.ap_enabled ? 1 : 0);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(
            handle, "ble_on", manager->current.ble_enabled ? 1 : 0);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(
            handle,
            "power_save",
            manager->current.power_save_enabled ? 1 : 0);
    }
    if (result == ESP_OK) {
        result = nvs_set_u8(
            handle, "power_mode", (uint8_t)manager->current.power_mode);
    }
    if (result == ESP_OK) {
        result = save_thresholds(handle, &manager->current.thresholds);
    }
    if (result == ESP_OK) {
        result = save_irrigation(handle, &manager->current.irrigation);
    }
    if (result == ESP_OK) {
        result = nvs_set_u32(
            handle, "cfg_ver", manager->current.config_version);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

esp_err_t config_manager_init(
    config_manager_t *manager, const saems_config_t *defaults)
{
    if (manager == NULL || defaults == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(manager, 0, sizeof(*manager));
    manager->current = *defaults;
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    manager->mutex = mutex;

    nvs_handle_t handle;
    esp_err_t result = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        load_string(handle, "device_id", manager->current.device_id,
                    sizeof(manager->current.device_id));
        load_string(handle, "wifi_ssid", manager->current.wifi_ssid,
                    sizeof(manager->current.wifi_ssid));
        load_string(handle, "wifi_pass", manager->current.wifi_password,
                    sizeof(manager->current.wifi_password));
        load_string(handle, "mqtt_uri", manager->current.mqtt_uri,
                    sizeof(manager->current.mqtt_uri));
        load_string(handle, "mqtt_user", manager->current.mqtt_username,
                    sizeof(manager->current.mqtt_username));
        load_string(handle, "mqtt_pass", manager->current.mqtt_password,
                    sizeof(manager->current.mqtt_password));
        load_string(handle, "ap_ssid", manager->current.ap_ssid,
                    sizeof(manager->current.ap_ssid));
        load_string(handle, "ap_pass", manager->current.ap_password,
                    sizeof(manager->current.ap_password));
        load_u32(handle, "interval", &manager->current.telemetry_interval_s);
        load_i32(handle, "soil_dry", &manager->current.soil_dry_raw);
        load_i32(handle, "soil_wet", &manager->current.soil_wet_raw);
        load_bool(handle, "soil_on", &manager->current.soil_enabled);
        /* Missing key means uncalibrated; old placeholder endpoints must not
         * silently become a measured percentage after a firmware upgrade. */
        load_bool(handle, "soil_cal", &manager->current.soil_calibrated);
        load_bool(handle, "ap_on", &manager->current.ap_enabled);
        load_bool(handle, "ble_on", &manager->current.ble_enabled);
        load_bool(handle, "power_save", &manager->current.power_save_enabled);
        uint8_t stored_power_mode = 0;
        if (nvs_get_u8(handle, "power_mode", &stored_power_mode) == ESP_OK &&
            stored_power_mode <= SAEMS_POWER_MODE_BATTERY) {
            manager->current.power_mode =
                (saems_power_mode_t)stored_power_mode;
        }
        load_thresholds(handle, &manager->current.thresholds);
        load_irrigation(handle, &manager->current.irrigation);
        load_u32(handle, "cfg_ver", &manager->current.config_version);
        nvs_close(handle);
    } else if (result != ESP_ERR_NVS_NOT_FOUND) {
        return result;
    }

    manager->initialized = true;
    return ESP_OK;
}

esp_err_t config_manager_get(
    config_manager_t *manager, saems_config_t *config)
{
    if (manager == NULL || config == NULL || !manager->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    *config = manager->current;
    xSemaphoreGive(manager_mutex(manager));
    return ESP_OK;
}

esp_err_t config_manager_update_json(
    config_manager_t *manager, const char *json, size_t json_length)
{
    if (manager == NULL || json == NULL || json_length == 0 ||
        !manager->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    if (json_length >= CONFIG_DOCUMENT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    char document[CONFIG_DOCUMENT_MAX];
    memcpy(document, json, json_length);
    document[json_length] = '\0';
    if (strchr(document, '{') == NULL || strchr(document, '}') == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    saems_config_t updated = manager->current;
    esp_err_t parse_result = parse_optional_string(
        document, "device_id", updated.device_id, sizeof(updated.device_id));
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_string(
        document, "wifi_ssid", updated.wifi_ssid, sizeof(updated.wifi_ssid));
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_string(
        document, "wifi_password", updated.wifi_password,
        sizeof(updated.wifi_password));
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_string(
        document, "mqtt_uri", updated.mqtt_uri, sizeof(updated.mqtt_uri));
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_string(
        document, "mqtt_username", updated.mqtt_username,
        sizeof(updated.mqtt_username));
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_string(
        document, "mqtt_password", updated.mqtt_password,
        sizeof(updated.mqtt_password));
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_string(
        document, "ap_ssid", updated.ap_ssid, sizeof(updated.ap_ssid));
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_string(
        document, "ap_password", updated.ap_password,
        sizeof(updated.ap_password));
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    int parsed = 0;
    parse_result = parse_optional_int(
        document, "telemetry_interval_s", 1, 3600, &parsed);
    if (parse_result == ESP_OK) {
        updated.telemetry_interval_s = (uint32_t)parsed;
    } else if (parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_int(
        document, "soil_dry_raw", 0, 4095, &parsed);
    if (parse_result == ESP_OK) {
        updated.soil_dry_raw = parsed;
    } else if (parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_int(
        document, "soil_wet_raw", 0, 4095, &parsed);
    if (parse_result == ESP_OK) {
        updated.soil_wet_raw = parsed;
    } else if (parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_bool(
        document, "soil_enabled", &updated.soil_enabled);
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    const bool dry_supplied = json_value(document, "soil_dry_raw") != NULL;
    const bool wet_supplied = json_value(document, "soil_wet_raw") != NULL;
    if (dry_supplied || wet_supplied) {
        updated.soil_calibrated = false;
    }
    parse_result = parse_optional_bool(
        document, "soil_calibrated", &updated.soil_calibrated);
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    if (parse_result == ESP_OK && updated.soil_calibrated &&
        (!updated.soil_enabled || !dry_supplied || !wet_supplied ||
         abs(updated.soil_dry_raw - updated.soil_wet_raw) < 100)) {
        xSemaphoreGive(manager_mutex(manager));
        return ESP_ERR_INVALID_ARG;
    }
    parse_result = parse_optional_bool(
        document, "ap_enabled", &updated.ap_enabled);
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_bool(
        document, "ble_enabled", &updated.ble_enabled);
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_bool(
        document, "power_save_enabled", &updated.power_save_enabled);
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    char power_mode[16];
    parse_result = parse_optional_string(
        document, "power_mode", power_mode, sizeof(power_mode));
    if (parse_result == ESP_OK) {
        parse_result = saems_power_mode_from_string(
            power_mode, &updated.power_mode);
    }
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_thresholds(document, &updated.thresholds);
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    parse_result = parse_optional_irrigation(document, &updated.irrigation);
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    /* A local (BLE/AP) change to a server-managed value is no longer the
     * server's version; the retained server config re-applies on reconnect. */
    if (server_managed_changed(&manager->current, &updated)) {
        updated.config_version = 0U;
    }

    if (updated.device_id[0] == '\0' ||
        (updated.ap_enabled &&
         (updated.ap_ssid[0] == '\0' || strlen(updated.ap_password) < 8)) ||
        (updated.soil_enabled &&
         updated.soil_dry_raw == updated.soil_wet_raw)) {
        xSemaphoreGive(manager_mutex(manager));
        return ESP_ERR_INVALID_ARG;
    }

    manager->current = updated;
    esp_err_t result = save_locked(manager);
    xSemaphoreGive(manager_mutex(manager));
    return result;
}

esp_err_t config_manager_update_remote_json(
    config_manager_t *manager,
    const char *json,
    size_t json_length,
    bool *applied)
{
    if (applied != NULL) {
        *applied = false;
    }
    if (manager == NULL || json == NULL || json_length == 0 ||
        applied == NULL || !manager->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    if (json_length >= CONFIG_DOCUMENT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    char document[CONFIG_DOCUMENT_MAX];
    memcpy(document, json, json_length);
    document[json_length] = '\0';
    if (strchr(document, '{') == NULL || strchr(document, '}') == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U;
         index < sizeof(remote_forbidden_keys) / sizeof(remote_forbidden_keys[0]);
         index++) {
        if (json_value(document, remote_forbidden_keys[index]) != NULL) {
            return ESP_ERR_NOT_ALLOWED;
        }
    }
    int version = 0;
    if (parse_optional_int(document, "config_version", 1, INT_MAX, &version) !=
        ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    if ((uint32_t)version == manager->current.config_version) {
        xSemaphoreGive(manager_mutex(manager));
        return ESP_OK;
    }
    saems_config_t updated = manager->current;
    int parsed = 0;
    esp_err_t parse_result = parse_optional_int(
        document, "telemetry_interval_s", 1, 3600, &parsed);
    if (parse_result == ESP_OK) {
        updated.telemetry_interval_s = (uint32_t)parsed;
    }
    if (parse_result == ESP_OK || parse_result == ESP_ERR_NOT_FOUND) {
        char power_mode[16];
        parse_result = parse_optional_string(
            document, "power_mode", power_mode, sizeof(power_mode));
        if (parse_result == ESP_OK) {
            parse_result = saems_power_mode_from_string(
                power_mode, &updated.power_mode);
        }
    }
    if (parse_result == ESP_OK || parse_result == ESP_ERR_NOT_FOUND) {
        parse_result = parse_optional_thresholds(
            document, &updated.thresholds);
    }
    if (parse_result == ESP_OK || parse_result == ESP_ERR_NOT_FOUND) {
        parse_result = parse_optional_irrigation(
            document, &updated.irrigation);
    }
    if (parse_result != ESP_OK && parse_result != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return parse_result;
    }
    updated.config_version = (uint32_t)version;
    manager->current = updated;
    esp_err_t result = save_locked(manager);
    xSemaphoreGive(manager_mutex(manager));
    *applied = result == ESP_OK;
    return result;
}

esp_err_t config_manager_to_json(
    config_manager_t *manager, char *output, size_t output_size)
{
    if (manager == NULL || output == NULL || output_size == 0 ||
        !manager->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    saems_config_t config;
    ESP_RETURN_ON_ERROR(config_manager_get(manager, &config),
                        "CONFIG_MANAGER",
                        "get config");
    char thresholds[THRESHOLD_LIMIT_COUNT * 10U + 4U];
    if (threshold_limits_to_json(
            &config.thresholds, thresholds, sizeof(thresholds)) < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    char irrigation[48];
    const int irrigation_length = irrigation_config_to_json(
        &config.irrigation, irrigation, sizeof(irrigation));
    if (irrigation_length < 0 || (size_t)irrigation_length >= sizeof(irrigation)) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* Keep this document below the 512-byte BLE attribute read limit
     * (worst case 495 bytes: 32-char device_id, every limit set). */
    int length = snprintf(
        output,
        output_size,
        "{\"device_id\":\"%s\",\"telemetry_interval_s\":%" PRIu32
        ",\"soil_dry_raw\":%d,\"soil_wet_raw\":%d,"
        "\"soil_enabled\":%s,\"soil_calibrated\":%s,"
        "\"soil_raw_readout_enabled\":%s,\"ap_enabled\":%s,"
        "\"ble_enabled\":%s,\"power_save_enabled\":%s,"
        "\"power_mode\":\"%s\","
        "\"wifi_configured\":%s,\"mqtt_configured\":%s,"
        "\"ap_password_configured\":%s,"
        "\"config_version\":%" PRIu32 ",\"thresholds\":%s,"
        "\"irrigation\":%s}",
        config.device_id,
        config.telemetry_interval_s,
        config.soil_dry_raw,
        config.soil_wet_raw,
        config.soil_enabled ? "true" : "false",
        config.soil_calibrated ? "true" : "false",
#ifdef CONFIG_SAEMS_SOIL_RAW_READOUT_ENABLED
        "true",
#else
        "false",
#endif
        config.ap_enabled ? "true" : "false",
        config.ble_enabled ? "true" : "false",
        config.power_save_enabled ? "true" : "false",
        saems_power_mode_name(config.power_mode),
        config.wifi_ssid[0] != '\0' ? "true" : "false",
        config.mqtt_uri[0] != '\0' ? "true" : "false",
        strlen(config.ap_password) >= 8 ? "true" : "false",
        config.config_version,
        thresholds,
        irrigation);
    if (length < 0 || (size_t)length >= output_size) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

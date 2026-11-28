#include "sync_manager.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BATCH_PREFIX "{\"batch\":["
#define BATCH_SUFFIX "]}"
#define BATCH_BUFFER_SIZE                                                  (sizeof(BATCH_PREFIX) + sizeof(BATCH_SUFFIX) +                          SYNC_MANAGER_BATCH_MAX * (size_t)SAEMS_STORAGE_RECORD_SIZE)

static const char *TAG = "SYNC_MANAGER";

static SemaphoreHandle_t manager_mutex(sync_manager_t *manager)
{
    return (SemaphoreHandle_t)manager->mutex;
}

esp_err_t sync_manager_init(
    sync_manager_t *manager,
    storage_manager_t *storage,
    mqtt_manager_t *mqtt)
{
    if (manager == NULL || storage == NULL || mqtt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    manager->storage = storage;
    manager->mqtt = mqtt;
    manager->in_flight_message_id = -1;
    manager->in_flight_last_id = 0U;
    manager->buffer = malloc(BATCH_BUFFER_SIZE);
    manager->mutex = xSemaphoreCreateMutex();
    if (manager->mutex == NULL || manager->buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    manager->initialized = true;
    return ESP_OK;
}

void sync_manager_kick(sync_manager_t *manager)
{
    if (manager == NULL || !manager->initialized ||
        !mqtt_manager_is_connected(manager->mqtt)) {
        return;
    }
    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    if (manager->in_flight_message_id >= 0) {
        xSemaphoreGive(manager_mutex(manager));
        return;
    }

    /* Leave room in front for the batch prefix and behind for the suffix. */
    const size_t prefix_length = strlen(BATCH_PREFIX);
    char *records = manager->buffer + prefix_length;
    size_t count = 0U;
    uint32_t last_id = 0U;
    esp_err_t result = storage_manager_read_pending(
        manager->storage,
        SYNC_MANAGER_BATCH_MAX,
        records,
        BATCH_BUFFER_SIZE - prefix_length - strlen(BATCH_SUFFIX),
        &count,
        &last_id);
    if (result == ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(manager_mutex(manager));
        return;
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Read pending records failed: %s", esp_err_to_name(result));
        xSemaphoreGive(manager_mutex(manager));
        return;
    }

    const char *payload = records;
    if (count > 1U) {
        /* read_pending left room for the suffix (bounded above). */
        const size_t records_length = strlen(records);
        (void)memcpy(manager->buffer, BATCH_PREFIX, prefix_length);
        (void)memcpy(&records[records_length], BATCH_SUFFIX,
                     sizeof(BATCH_SUFFIX));
        payload = manager->buffer;
    }
    int message_id = mqtt_manager_publish(manager->mqtt, payload);
    if (message_id >= 0) {
        manager->in_flight_message_id = message_id;
        manager->in_flight_last_id = last_id;
        ESP_LOGI(TAG,
                 "Sync %u record(s) through id %" PRIu32 "; msg_id=%d pending=%u",
                 (unsigned)count,
                 last_id,
                 message_id,
                 (unsigned)storage_manager_pending_count(manager->storage));
    }
    xSemaphoreGive(manager_mutex(manager));
}

void sync_manager_on_published(sync_manager_t *manager, int message_id)
{
    if (manager == NULL || !manager->initialized) {
        return;
    }
    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    if (message_id != manager->in_flight_message_id) {
        xSemaphoreGive(manager_mutex(manager));
        return;
    }
    esp_err_t result = storage_manager_mark_sent(
        manager->storage, manager->in_flight_last_id);
    manager->in_flight_message_id = -1;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Acknowledge records failed: %s", esp_err_to_name(result));
    }
    xSemaphoreGive(manager_mutex(manager));
    sync_manager_kick(manager);
}

void sync_manager_on_connection_changed(
    sync_manager_t *manager, bool connected)
{
    if (manager == NULL || !manager->initialized) {
        return;
    }
    if (!connected) {
        xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
        manager->in_flight_message_id = -1;
        xSemaphoreGive(manager_mutex(manager));
        return;
    }
    sync_manager_kick(manager);
}

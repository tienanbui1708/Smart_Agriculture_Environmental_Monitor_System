#include "storage_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

/* NVS fallback queue (and the source of the one-time migration). */
#define STORAGE_NAMESPACE "saems_store"
#define STORAGE_HEAD_KEY "head"
#define STORAGE_COUNT_KEY "count"
/* Flash log bookkeeping. */
#define LOG_NAMESPACE "saems_log"
#define LOG_DROPPED_KEY "dropped"
#define LOG_RTC_MAGIC 0x4C4F4753U /* "SGOL" */

static const char *TAG = "STORAGE";

/* Ring state cached across Deep Sleep so a timer wake skips the scan. */
typedef struct {
    uint32_t magic;
    uint32_t partition_address;
    record_log_state_t state;
} log_rtc_cache_t;

RTC_DATA_ATTR static log_rtc_cache_t rtc_cache;

static SemaphoreHandle_t manager_mutex(storage_manager_t *manager)
{
    return (SemaphoreHandle_t)manager->mutex;
}

/* ---- flash callbacks ---------------------------------------------------- */

static int partition_read(void *context, uint32_t offset, void *data,
                          uint32_t size)
{
    return esp_partition_read(context, offset, data, size) == ESP_OK ? 0 : -1;
}

static int partition_write(void *context, uint32_t offset, const void *data,
                           uint32_t size)
{
    return esp_partition_write(context, offset, data, size) == ESP_OK ? 0 : -1;
}

static int partition_erase(void *context, uint32_t offset, uint32_t size)
{
    return esp_partition_erase_range(context, offset, size) == ESP_OK ? 0 : -1;
}

static void cache_log_state(storage_manager_t *manager)
{
    const esp_partition_t *partition = manager->partition;
    rtc_cache.magic = LOG_RTC_MAGIC;
    rtc_cache.partition_address = partition->address;
    record_log_save_state(&manager->log, &rtc_cache.state);
}

static void persist_dropped(storage_manager_t *manager, uint32_t dropped_now)
{
    if (dropped_now == 0U) {
        return;
    }
    manager->dropped_total += dropped_now;
    ESP_LOGW(TAG, "Offline log full: dropped %" PRIu32 " oldest unsent records "
             "(total %" PRIu32 ")", dropped_now, manager->dropped_total);
    nvs_handle_t handle;
    if (nvs_open(LOG_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_set_u32(handle, LOG_DROPPED_KEY, manager->dropped_total);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
}

/* ---- NVS fallback queue ------------------------------------------------- */

static void record_key(uint16_t index, char *key, size_t key_size)
{
    (void)snprintf(key, key_size, "r%02u", (unsigned)index);
}

static esp_err_t save_metadata(
    nvs_handle_t handle, const storage_manager_t *manager)
{
    esp_err_t result = nvs_set_u16(handle, STORAGE_HEAD_KEY, manager->head);
    if (result == ESP_OK) {
        result = nvs_set_u16(handle, STORAGE_COUNT_KEY, manager->count);
    }
    return result;
}

static esp_err_t legacy_load(storage_manager_t *manager)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    uint16_t head = 0;
    uint16_t count = 0;
    if (nvs_get_u16(handle, STORAGE_HEAD_KEY, &head) != ESP_OK) {
        head = 0;
    }
    if (nvs_get_u16(handle, STORAGE_COUNT_KEY, &count) != ESP_OK) {
        count = 0;
    }
    nvs_close(handle);
    manager->head = head % SAEMS_STORAGE_LEGACY_MAX_RECORDS;
    manager->count = count > SAEMS_STORAGE_LEGACY_MAX_RECORDS
                         ? SAEMS_STORAGE_LEGACY_MAX_RECORDS
                         : count;
    return ESP_OK;
}

static esp_err_t legacy_save(storage_manager_t *manager, const char *payload)
{
    if (manager->count == SAEMS_STORAGE_LEGACY_MAX_RECORDS) {
        /* Do not evict the head while Sync Manager may be waiting for its
         * PUBACK: that acknowledgement would otherwise remove the next,
         * unsent record. Preserve pending records and reject the new sample. */
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    uint16_t tail = (manager->head + manager->count) %
                    SAEMS_STORAGE_LEGACY_MAX_RECORDS;
    char key[8];
    record_key(tail, key, sizeof(key));
    result = nvs_set_str(handle, key, payload);
    if (result == ESP_OK) {
        manager->count++;
        result = save_metadata(handle, manager);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static esp_err_t legacy_read(storage_manager_t *manager, uint16_t offset,
                             char *payload, size_t payload_size)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        char key[8];
        record_key((manager->head + offset) % SAEMS_STORAGE_LEGACY_MAX_RECORDS,
                   key, sizeof(key));
        result = nvs_get_str(handle, key, payload, &payload_size);
        nvs_close(handle);
    }
    return result;
}

static esp_err_t legacy_pop(storage_manager_t *manager)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        char key[8];
        record_key(manager->head, key, sizeof(key));
        result = nvs_erase_key(handle, key);
        if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) {
            manager->head =
                (manager->head + 1U) % SAEMS_STORAGE_LEGACY_MAX_RECORDS;
            manager->count--;
            result = save_metadata(handle, manager);
        }
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    return result;
}

/* ---- flash log ------------------------------------------------------------ */

static esp_err_t log_append(storage_manager_t *manager, const char *payload)
{
    uint32_t dropped_now = 0U;
    const record_log_status_t status = record_log_append(
        &manager->log, payload, (uint32_t)strlen(payload), &dropped_now);
    cache_log_state(manager);
    persist_dropped(manager, dropped_now);
    if (status == RECORD_LOG_ERR_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    return status == RECORD_LOG_OK ? ESP_OK : ESP_FAIL;
}

/* Move records left in the NVS queue by older firmware into the log. */
static void migrate_legacy_queue(storage_manager_t *manager)
{
    if (legacy_load(manager) != ESP_OK || manager->count == 0U) {
        manager->head = 0U;
        manager->count = 0U;
        return;
    }
    char record[SAEMS_STORAGE_RECORD_SIZE];
    unsigned moved = 0U;
    for (uint16_t offset = 0U; offset < manager->count; offset++) {
        if (legacy_read(manager, offset, record, sizeof(record)) == ESP_OK &&
            log_append(manager, record) == ESP_OK) {
            moved++;
        }
    }
    nvs_handle_t handle;
    if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_erase_all(handle);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Moved %u/%u pending records from NVS to the flash log",
             moved, (unsigned)manager->count);
    manager->head = 0U;
    manager->count = 0U;
}

static esp_err_t open_flash_log(storage_manager_t *manager)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        /* Custom data subtypes 0x40..0xFE are valid partition subtypes even
         * though the enum does not name them. */
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        (esp_partition_subtype_t)SAEMS_STORAGE_PARTITION_SUBTYPE,
        SAEMS_STORAGE_PARTITION_LABEL);
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    manager->partition = partition;
    const record_log_flash_t flash = {
        .read = partition_read,
        .write = partition_write,
        .erase = partition_erase,
        .context = (void *)partition,
        .size = partition->size,
        .sector_size = partition->erase_size,
        .slot_size = SAEMS_STORAGE_SLOT_SIZE,
    };
    const bool timer_wake =
        (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0U;
    if (timer_wake && rtc_cache.magic == LOG_RTC_MAGIC &&
        rtc_cache.partition_address == partition->address &&
        record_log_restore(&manager->log, &flash, &rtc_cache.state) ==
            RECORD_LOG_OK) {
        return ESP_OK;
    }
    const int64_t started = esp_log_timestamp();
    if (record_log_open(&manager->log, &flash) != RECORD_LOG_OK) {
        return ESP_FAIL;
    }
    cache_log_state(manager);
    ESP_LOGI(TAG,
             "Flash log: %" PRIu32 " slots, pending=%" PRIu32
             ", next id=%" PRIu32 ", scan %" PRIi64 " ms",
             manager->log.slot_count, manager->log.pending,
             manager->log.next_id, (int64_t)esp_log_timestamp() - started);
    return ESP_OK;
}

/* ---- public API ----------------------------------------------------------- */

esp_err_t storage_manager_init(storage_manager_t *manager)
{
    if (manager == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(manager, 0, sizeof(*manager));
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    manager->mutex = mutex;

    nvs_handle_t handle;
    if (nvs_open(LOG_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u32(handle, LOG_DROPPED_KEY, &manager->dropped_total);
        nvs_close(handle);
    }

    esp_err_t result = open_flash_log(manager);
    if (result == ESP_OK) {
        manager->backend = STORAGE_BACKEND_FLASH_LOG;
        manager->initialized = true;
        migrate_legacy_queue(manager);
        return ESP_OK;
    }
    ESP_LOGW(TAG,
             "Partition \"%s\" unavailable (%s); using the 32-record NVS queue. "
             "Flash the new partition table (idf.py flash) to enable the log.",
             SAEMS_STORAGE_PARTITION_LABEL, esp_err_to_name(result));
    manager->backend = STORAGE_BACKEND_NVS;
    result = legacy_load(manager);
    if (result != ESP_OK) {
        return result;
    }
    manager->initialized = true;
    return ESP_OK;
}

esp_err_t storage_manager_save(
    storage_manager_t *manager, const char *payload)
{
    if (manager == NULL || payload == NULL || !manager->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t payload_length = strlen(payload);
    if (payload_length == 0 || payload_length >= SAEMS_STORAGE_RECORD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    const esp_err_t result = manager->backend == STORAGE_BACKEND_FLASH_LOG
                                 ? log_append(manager, payload)
                                 : legacy_save(manager, payload);
    xSemaphoreGive(manager_mutex(manager));
    return result;
}

static esp_err_t append_record(char *output, size_t output_size, size_t *used,
                               const char *record, size_t length, bool first)
{
    const size_t needed = length + (first ? 0U : 1U);
    if (*used + needed + 1U > output_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!first) {
        output[(*used)++] = ',';
    }
    memcpy(output + *used, record, length);
    *used += length;
    output[*used] = '\0';
    return ESP_OK;
}

esp_err_t storage_manager_read_pending(
    storage_manager_t *manager,
    size_t max_records,
    char *output,
    size_t output_size,
    size_t *count,
    uint32_t *last_id)
{
    if (manager == NULL || output == NULL || output_size == 0U ||
        count == NULL || last_id == NULL || max_records == 0U ||
        !manager->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0U;
    *last_id = 0U;
    output[0] = '\0';
    size_t used = 0U;
    esp_err_t result = ESP_OK;
    char record[SAEMS_STORAGE_RECORD_SIZE];
    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    if (manager->backend == STORAGE_BACKEND_NVS) {
        /* The NVS queue acknowledges one record at a time. */
        if (manager->count == 0U) {
            result = ESP_ERR_NOT_FOUND;
        } else {
            result = legacy_read(manager, 0U, record, sizeof(record));
            if (result == ESP_OK) {
                result = append_record(output, output_size, &used, record,
                                       strlen(record), true);
            }
            if (result == ESP_OK) {
                *count = 1U;
                *last_id = 1U;
            }
        }
    } else {
        record_log_cursor_t cursor;
        record_log_pending_begin(&manager->log, &cursor);
        while (*count < max_records) {
            uint32_t length = 0U;
            uint32_t id = 0U;
            const record_log_status_t status = record_log_pending_next(
                &manager->log, &cursor, record, sizeof(record), &length, &id);
            if (status == RECORD_LOG_EMPTY) {
                break;
            }
            if (status != RECORD_LOG_OK) {
                result = ESP_FAIL;
                break;
            }
            if (append_record(output, output_size, &used, record, length,
                              *count == 0U) != ESP_OK) {
                break; /* batch buffer full; send what fits */
            }
            (*count)++;
            *last_id = id;
        }
        cache_log_state(manager); /* corrupt records may have been skipped */
        if (result == ESP_OK && *count == 0U) {
            result = ESP_ERR_NOT_FOUND;
        }
    }
    xSemaphoreGive(manager_mutex(manager));
    return result;
}

esp_err_t storage_manager_mark_sent(storage_manager_t *manager, uint32_t last_id)
{
    if (manager == NULL || !manager->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (manager->backend == STORAGE_BACKEND_NVS) {
        result = manager->count == 0U ? ESP_ERR_NOT_FOUND : legacy_pop(manager);
    } else {
        result = record_log_mark_sent_through(&manager->log, last_id) ==
                         RECORD_LOG_OK
                     ? ESP_OK
                     : ESP_FAIL;
        cache_log_state(manager);
    }
    xSemaphoreGive(manager_mutex(manager));
    return result;
}

size_t storage_manager_pending_count(storage_manager_t *manager)
{
    if (manager == NULL || !manager->initialized) {
        return 0;
    }
    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    size_t count = manager->backend == STORAGE_BACKEND_FLASH_LOG
                       ? manager->log.pending
                       : manager->count;
    xSemaphoreGive(manager_mutex(manager));
    return count;
}

uint32_t storage_manager_dropped_count(storage_manager_t *manager)
{
    if (manager == NULL || !manager->initialized) {
        return 0U;
    }
    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    const uint32_t dropped = manager->dropped_total;
    xSemaphoreGive(manager_mutex(manager));
    return dropped;
}

const char *storage_manager_backend_name(const storage_manager_t *manager)
{
    return manager != NULL && manager->backend == STORAGE_BACKEND_FLASH_LOG
               ? "flash_log"
               : "nvs";
}

typedef struct {
    char *output;
    size_t output_size;
    size_t used;
    size_t emitted;
    esp_err_t result;
} history_writer_t;

static bool history_visit(void *context, const void *payload, uint32_t length,
                          uint32_t id, bool sent)
{
    (void)id;
    (void)sent;
    history_writer_t *writer = context;
    writer->result = append_record(writer->output, writer->output_size,
                                   &writer->used, payload, length,
                                   writer->emitted == 0U);
    if (writer->result != ESP_OK) {
        return false;
    }
    writer->emitted++;
    return true;
}

esp_err_t storage_manager_history_json(
    storage_manager_t *manager,
    size_t limit,
    char *output,
    size_t output_size)
{
    if (manager == NULL || output == NULL || output_size < 16 ||
        !manager->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *prefix = "{\"items\":[";
    const size_t prefix_length = strlen(prefix);
    memcpy(output, prefix, prefix_length + 1U);
    history_writer_t writer = {
        .output = output + prefix_length,
        .output_size = output_size - prefix_length,
        .used = 0U,
        .emitted = 0U,
        .result = ESP_OK,
    };
    writer.output[0] = '\0';

    xSemaphoreTake(manager_mutex(manager), portMAX_DELAY);
    esp_err_t result = ESP_OK;
    size_t pending = 0U;
    if (manager->backend == STORAGE_BACKEND_FLASH_LOG) {
        char record[SAEMS_STORAGE_RECORD_SIZE];
        if (record_log_recent(&manager->log, (uint32_t)limit, record,
                              sizeof(record), history_visit,
                              &writer) != RECORD_LOG_OK) {
            result = ESP_FAIL;
        } else {
            result = writer.result;
        }
        pending = manager->log.pending;
    } else {
        size_t count = manager->count < limit ? manager->count : limit;
        char record[SAEMS_STORAGE_RECORD_SIZE];
        for (size_t offset = 0; offset < count && result == ESP_OK; offset++) {
            result = legacy_read(manager, (uint16_t)offset, record,
                                 sizeof(record));
            if (result == ESP_OK) {
                (void)history_visit(&writer, record, (uint32_t)strlen(record),
                                    0U, false);
                result = writer.result;
            }
        }
        pending = manager->count;
    }
    if (result == ESP_OK) {
        int written = snprintf(output + prefix_length + writer.used,
                               output_size - prefix_length - writer.used,
                               "],\"pending_count\":%u,\"dropped_count\":%" PRIu32 "}",
                               (unsigned)pending, manager->dropped_total);
        if (written < 0 ||
            (size_t)written >= output_size - prefix_length - writer.used) {
            result = ESP_ERR_INVALID_SIZE;
        }
    }
    if (result != ESP_OK) {
        output[0] = '\0';
    }
    xSemaphoreGive(manager_mutex(manager));
    return result;
}

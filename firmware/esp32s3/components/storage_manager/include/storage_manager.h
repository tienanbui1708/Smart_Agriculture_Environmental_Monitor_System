#pragma once

/*
 * Offline telemetry storage.
 *
 * Primary backend: record_log ring buffer on the "telemetry" data partition
 * (6 MB, 1 KB slots, about 6 100 records). When full, the oldest sector is
 * recycled and its unsent records are counted as dropped. Sent records stay
 * readable as history until overwritten.
 *
 * Fallback backend (partition missing, e.g. only the app was flashed):
 * the previous NVS queue of 32 records that rejects new samples when full.
 * Records left in that NVS queue are moved to the flash log on first boot.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "record_log.h"

#define SAEMS_STORAGE_RECORD_SIZE 768
#define SAEMS_STORAGE_LEGACY_MAX_RECORDS 32
#define SAEMS_STORAGE_PARTITION_LABEL "telemetry"
#define SAEMS_STORAGE_PARTITION_SUBTYPE 0x40
#define SAEMS_STORAGE_SLOT_SIZE 1024U

typedef enum {
    STORAGE_BACKEND_NVS = 0,
    STORAGE_BACKEND_FLASH_LOG,
} storage_backend_t;

typedef struct {
    storage_backend_t backend;
    record_log_t log;
    const void *partition; /* const esp_partition_t * */
    /* NVS fallback queue. */
    uint16_t head;
    uint16_t count;
    /* Unsent records lost to wrap-around since first use (persisted). */
    uint32_t dropped_total;
    void *mutex;
    bool initialized;
} storage_manager_t;

esp_err_t storage_manager_init(storage_manager_t *manager);
esp_err_t storage_manager_save(
    storage_manager_t *manager, const char *payload);

/*
 * Copy up to max_records unsent records, oldest first, into output joined
 * by commas (a JSON array body without brackets, NUL terminated). *count
 * gets the number copied and *last_id the identifier to acknowledge with
 * storage_manager_mark_sent(). Returns ESP_ERR_NOT_FOUND when nothing is
 * pending.
 */
esp_err_t storage_manager_read_pending(
    storage_manager_t *manager,
    size_t max_records,
    char *output,
    size_t output_size,
    size_t *count,
    uint32_t *last_id);

/* Acknowledge every pending record up to last_id (FIFO). */
esp_err_t storage_manager_mark_sent(storage_manager_t *manager, uint32_t last_id);

size_t storage_manager_pending_count(storage_manager_t *manager);
uint32_t storage_manager_dropped_count(storage_manager_t *manager);
const char *storage_manager_backend_name(const storage_manager_t *manager);

/* Most recent records (sent or not), chronological, as {"items":[...]}. */
esp_err_t storage_manager_history_json(
    storage_manager_t *manager,
    size_t limit,
    char *output,
    size_t output_size);

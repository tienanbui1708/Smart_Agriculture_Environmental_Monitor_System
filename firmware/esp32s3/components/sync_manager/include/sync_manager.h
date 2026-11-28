#pragma once

/*
 * Drains the offline storage over MQTT (QoS 1), one message in flight.
 *
 * A single pending record is published unchanged. A backlog is sent in
 * batches of up to SYNC_MANAGER_BATCH_MAX records as
 *   {"batch":[<record>,<record>,...]}
 * on the same telemetry topic; the server stores each record and ignores
 * duplicates by (device_id, boot_id, seq). The PUBACK acknowledges the whole
 * batch at once, by the id of its last record.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mqtt_manager.h"
#include "storage_manager.h"

#define SYNC_MANAGER_BATCH_MAX 8U

typedef struct {
    storage_manager_t *storage;
    mqtt_manager_t *mqtt;
    void *mutex;
    char *buffer;
    int in_flight_message_id;
    uint32_t in_flight_last_id;
    bool initialized;
} sync_manager_t;

esp_err_t sync_manager_init(
    sync_manager_t *manager,
    storage_manager_t *storage,
    mqtt_manager_t *mqtt);
void sync_manager_kick(sync_manager_t *manager);
void sync_manager_on_published(sync_manager_t *manager, int message_id);
void sync_manager_on_connection_changed(
    sync_manager_t *manager, bool connected);

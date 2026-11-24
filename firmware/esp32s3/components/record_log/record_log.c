#include "record_log.h"

#include <string.h>

#define SENT_OFFSET 12U
#define MAGIC_OFFSET 16U
#define NOT_SENT 0xFFFFFFFFU

/* Stored little-endian on both the ESP32-S3 and x86 hosts. */
typedef struct {
    uint32_t id;
    uint16_t length;
    uint16_t reserved;
    uint32_t crc;
    uint32_t sent;
    uint32_t magic;
} slot_header_t;

_Static_assert(sizeof(slot_header_t) == RECORD_LOG_HEADER_SIZE,
               "slot header layout");

uint32_t record_log_crc32(const void *data, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t index = 0U; index < length; index++) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; bit++) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static uint32_t slot_offset(const record_log_t *log, uint32_t slot)
{
    return slot * log->flash.slot_size;
}

static uint32_t next(const record_log_t *log, uint32_t slot)
{
    return (slot + 1U) % log->slot_count;
}

static uint32_t previous(const record_log_t *log, uint32_t slot)
{
    return slot == 0U ? log->slot_count - 1U : slot - 1U;
}

static bool read_header(record_log_t *log, uint32_t slot, slot_header_t *header)
{
    return log->flash.read(log->flash.context, slot_offset(log, slot), header,
                           RECORD_LOG_HEADER_SIZE) == 0;
}

static bool header_valid(const record_log_t *log, const slot_header_t *header)
{
    return header->magic == RECORD_LOG_MAGIC && header->length > 0U &&
           header->length <= record_log_max_payload(log);
}

static bool geometry_valid(const record_log_flash_t *flash)
{
    return flash != NULL && flash->read != NULL && flash->write != NULL &&
           flash->erase != NULL && flash->slot_size > RECORD_LOG_HEADER_SIZE &&
           flash->sector_size >= flash->slot_size &&
           flash->sector_size % flash->slot_size == 0U &&
           flash->size % flash->sector_size == 0U &&
           flash->size / flash->sector_size >= 2U &&
           flash->slot_size <= 0xFFFFU + RECORD_LOG_HEADER_SIZE;
}

static void attach(record_log_t *log, const record_log_flash_t *flash)
{
    memset(log, 0, sizeof(*log));
    log->flash = *flash;
    log->slot_count = flash->size / flash->slot_size;
    log->slots_per_sector = flash->sector_size / flash->slot_size;
}

uint32_t record_log_max_payload(const record_log_t *log)
{
    return log == NULL ? 0U : log->flash.slot_size - RECORD_LOG_HEADER_SIZE;
}

static bool slot_blank(record_log_t *log, uint32_t slot)
{
    uint8_t chunk[64];
    const uint32_t base = slot_offset(log, slot);
    for (uint32_t done = 0U; done < log->flash.slot_size; done += sizeof(chunk)) {
        uint32_t size = log->flash.slot_size - done;
        if (size > sizeof(chunk)) {
            size = sizeof(chunk);
        }
        if (log->flash.read(log->flash.context, base + done, chunk, size) != 0) {
            return false;
        }
        for (uint32_t index = 0U; index < size; index++) {
            if (chunk[index] != 0xFFU) {
                return false;
            }
        }
    }
    return true;
}

record_log_status_t record_log_open(record_log_t *log,
                                    const record_log_flash_t *flash)
{
    if (log == NULL || !geometry_valid(flash)) {
        return RECORD_LOG_ERR_ARG;
    }
    attach(log, flash);
    bool any = false;
    uint32_t newest_slot = 0U;
    uint32_t newest_id = 0U;
    bool any_pending = false;
    uint32_t oldest_pending_id = 0U;
    for (uint32_t slot = 0U; slot < log->slot_count; slot++) {
        slot_header_t header;
        if (!read_header(log, slot, &header)) {
            return RECORD_LOG_ERR_FLASH;
        }
        if (!header_valid(log, &header)) {
            continue;
        }
        if (!any || header.id > newest_id) {
            any = true;
            newest_id = header.id;
            newest_slot = slot;
        }
        if (header.sent == NOT_SENT) {
            log->pending++;
            if (!any_pending || header.id < oldest_pending_id) {
                any_pending = true;
                oldest_pending_id = header.id;
                log->oldest_slot = slot;
            }
        }
    }
    if (!any) {
        log->next_slot = 0U;
        log->next_id = 1U;
        log->oldest_slot = 0U;
        return RECORD_LOG_OK;
    }
    log->next_id = newest_id + 1U;
    log->next_slot = next(log, newest_slot);
    /* A torn write after the newest record leaves a non-blank slot that
     * cannot be programmed again; skip to the next blank slot or to the next
     * sector boundary, which is erased before use anyway. */
    while (log->next_slot % log->slots_per_sector != 0U &&
           !slot_blank(log, log->next_slot)) {
        log->next_slot = next(log, log->next_slot);
    }
    if (!any_pending) {
        log->oldest_slot = log->next_slot;
    }
    return RECORD_LOG_OK;
}

record_log_status_t record_log_restore(record_log_t *log,
                                       const record_log_flash_t *flash,
                                       const record_log_state_t *state)
{
    if (log == NULL || state == NULL || !geometry_valid(flash)) {
        return RECORD_LOG_ERR_ARG;
    }
    attach(log, flash);
    if (state->next_slot >= log->slot_count ||
        state->oldest_slot >= log->slot_count ||
        state->pending > log->slot_count || state->next_id == 0U) {
        return RECORD_LOG_ERR_ARG;
    }
    log->next_slot = state->next_slot;
    log->next_id = state->next_id;
    log->oldest_slot = state->oldest_slot;
    log->pending = state->pending;
    return RECORD_LOG_OK;
}

void record_log_save_state(const record_log_t *log, record_log_state_t *state)
{
    if (log == NULL || state == NULL) {
        return;
    }
    state->next_slot = log->next_slot;
    state->next_id = log->next_id;
    state->oldest_slot = log->oldest_slot;
    state->pending = log->pending;
}

/* Erase the sector that starts at slot, dropping its unsent records. */
static record_log_status_t recycle_sector(record_log_t *log,
                                          uint32_t first_slot,
                                          uint32_t *dropped_now)
{
    uint32_t dropped = 0U;
    bool holds_oldest = false;
    for (uint32_t index = 0U; index < log->slots_per_sector; index++) {
        const uint32_t slot = first_slot + index;
        slot_header_t header;
        if (!read_header(log, slot, &header)) {
            return RECORD_LOG_ERR_FLASH;
        }
        if (slot == log->oldest_slot) {
            holds_oldest = true;
        }
        if (header_valid(log, &header) && header.sent == NOT_SENT) {
            dropped++;
        }
    }
    if (log->flash.erase(log->flash.context, slot_offset(log, first_slot),
                         log->flash.sector_size) != 0) {
        return RECORD_LOG_ERR_FLASH;
    }
    if (dropped > log->pending) {
        dropped = log->pending;
    }
    log->pending -= dropped;
    log->dropped += dropped;
    if (dropped_now != NULL) {
        *dropped_now += dropped;
    }
    if (holds_oldest && log->pending > 0U) {
        /* The rest of the ring after this sector holds the older data. */
        log->oldest_slot = (first_slot + log->slots_per_sector) % log->slot_count;
    }
    return RECORD_LOG_OK;
}

record_log_status_t record_log_append(record_log_t *log,
                                      const void *payload,
                                      uint32_t length,
                                      uint32_t *dropped_now)
{
    if (dropped_now != NULL) {
        *dropped_now = 0U;
    }
    if (log == NULL || payload == NULL || log->slot_count == 0U) {
        return RECORD_LOG_ERR_ARG;
    }
    if (length == 0U || length > record_log_max_payload(log)) {
        return RECORD_LOG_ERR_SIZE;
    }
    const uint32_t slot = log->next_slot;
    if (slot % log->slots_per_sector == 0U) {
        const record_log_status_t status =
            recycle_sector(log, slot, dropped_now);
        if (status != RECORD_LOG_OK) {
            return status;
        }
    }
    const uint32_t base = slot_offset(log, slot);
    const slot_header_t header = {
        .id = log->next_id,
        .length = (uint16_t)length,
        .reserved = 0xFFFFU,
        .crc = record_log_crc32(payload, length),
        .sent = NOT_SENT,
        .magic = RECORD_LOG_MAGIC,
    };
    /* The slot is consumed even if a write below fails part-way. */
    log->next_slot = next(log, slot);
    log->next_id++;
    if (log->flash.write(log->flash.context, base + RECORD_LOG_HEADER_SIZE,
                         payload, length) != 0 ||
        log->flash.write(log->flash.context, base, &header, MAGIC_OFFSET) != 0 ||
        log->flash.write(log->flash.context, base + MAGIC_OFFSET, &header.magic,
                         sizeof(header.magic)) != 0) {
        return RECORD_LOG_ERR_FLASH;
    }
    if (log->pending == 0U) {
        log->oldest_slot = slot;
    }
    log->pending++;
    return RECORD_LOG_OK;
}

static record_log_status_t mark_slot_sent(record_log_t *log, uint32_t slot)
{
    const uint32_t sent = 0U;
    return log->flash.write(log->flash.context,
                            slot_offset(log, slot) + SENT_OFFSET, &sent,
                            sizeof(sent)) == 0
               ? RECORD_LOG_OK
               : RECORD_LOG_ERR_FLASH;
}

void record_log_pending_begin(const record_log_t *log,
                              record_log_cursor_t *cursor)
{
    if (log == NULL || cursor == NULL) {
        return;
    }
    cursor->slot = log->oldest_slot;
    cursor->visited = 0U;
}

record_log_status_t record_log_pending_next(record_log_t *log,
                                            record_log_cursor_t *cursor,
                                            void *out,
                                            uint32_t out_size,
                                            uint32_t *length,
                                            uint32_t *id)
{
    if (log == NULL || cursor == NULL || out == NULL || length == NULL ||
        id == NULL) {
        return RECORD_LOG_ERR_ARG;
    }
    /* oldest_slot == next_slot with records pending means a full ring, so
     * the first slot is always visited. */
    while (log->pending > 0U && cursor->visited < log->slot_count &&
           (cursor->visited == 0U || cursor->slot != log->next_slot)) {
        const uint32_t slot = cursor->slot;
        cursor->slot = next(log, slot);
        cursor->visited++;
        slot_header_t header;
        if (!read_header(log, slot, &header)) {
            return RECORD_LOG_ERR_FLASH;
        }
        if (!header_valid(log, &header) || header.sent != NOT_SENT) {
            continue;
        }
        if (header.length > out_size) {
            return RECORD_LOG_ERR_SIZE;
        }
        if (log->flash.read(log->flash.context,
                            slot_offset(log, slot) + RECORD_LOG_HEADER_SIZE,
                            out, header.length) != 0) {
            return RECORD_LOG_ERR_FLASH;
        }
        if (record_log_crc32(out, header.length) != header.crc) {
            if (mark_slot_sent(log, slot) != RECORD_LOG_OK) {
                return RECORD_LOG_ERR_FLASH;
            }
            log->pending--;
            log->corrupt++;
            continue;
        }
        *length = header.length;
        *id = header.id;
        return RECORD_LOG_OK;
    }
    return RECORD_LOG_EMPTY;
}

record_log_status_t record_log_mark_sent_through(record_log_t *log,
                                                 uint32_t last_id)
{
    if (log == NULL || log->slot_count == 0U) {
        return RECORD_LOG_ERR_ARG;
    }
    uint32_t slot = log->oldest_slot;
    uint32_t visited = 0U;
    while (log->pending > 0U && visited < log->slot_count &&
           (visited == 0U || slot != log->next_slot)) {
        slot_header_t header;
        if (!read_header(log, slot, &header)) {
            return RECORD_LOG_ERR_FLASH;
        }
        if (header_valid(log, &header) && header.sent == NOT_SENT) {
            if (header.id > last_id) {
                break;
            }
            if (mark_slot_sent(log, slot) != RECORD_LOG_OK) {
                return RECORD_LOG_ERR_FLASH;
            }
            log->pending--;
        }
        slot = next(log, slot);
        visited++;
    }
    if (log->pending == 0U) {
        log->oldest_slot = log->next_slot;
    } else {
        if (visited > 0U && slot == log->next_slot) {
            /* Counter drifted from flash (should not happen); trust flash. */
            log->pending = 0U;
        }
        log->oldest_slot = slot;
    }
    return RECORD_LOG_OK;
}

record_log_status_t record_log_recent(record_log_t *log,
                                      uint32_t max_records,
                                      void *buffer,
                                      uint32_t buffer_size,
                                      record_log_visitor_t visitor,
                                      void *context)
{
    if (log == NULL || buffer == NULL || visitor == NULL ||
        log->slot_count == 0U) {
        return RECORD_LOG_ERR_ARG;
    }
    /* Walk back from the newest slot to find where the window starts. */
    uint32_t start = log->next_slot;
    uint32_t found = 0U;
    uint32_t slot = log->next_slot;
    for (uint32_t visited = 0U;
         visited < log->slot_count && found < max_records; visited++) {
        slot = previous(log, slot);
        slot_header_t header;
        if (!read_header(log, slot, &header)) {
            return RECORD_LOG_ERR_FLASH;
        }
        if (header_valid(log, &header) && header.id < log->next_id) {
            found++;
            start = slot;
        }
    }
    slot = start;
    for (uint32_t emitted = 0U; emitted < found;) {
        slot_header_t header;
        if (!read_header(log, slot, &header)) {
            return RECORD_LOG_ERR_FLASH;
        }
        if (header_valid(log, &header) && header.id < log->next_id) {
            emitted++;
            if (header.length > buffer_size) {
                return RECORD_LOG_ERR_SIZE;
            }
            if (log->flash.read(log->flash.context,
                                slot_offset(log, slot) + RECORD_LOG_HEADER_SIZE,
                                buffer, header.length) != 0) {
                return RECORD_LOG_ERR_FLASH;
            }
            if (record_log_crc32(buffer, header.length) == header.crc &&
                !visitor(context, buffer, header.length, header.id,
                         header.sent != NOT_SENT)) {
                break;
            }
        }
        slot = next(log, slot);
    }
    return RECORD_LOG_OK;
}

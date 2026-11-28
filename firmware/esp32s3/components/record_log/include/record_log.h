#pragma once

/*
 * Power-safe ring log of fixed-size records on raw NOR flash.
 *
 * Pure C, no ESP-IDF dependency: the flash is reached through callbacks so
 * host tests can run it on a simulated flash. Used by storage_manager to
 * keep telemetry while the server is unreachable.
 *
 * Layout: the area is split into slots of slot_size bytes; a flash sector
 * holds a whole number of slots. Each slot is
 *
 *   [id u32][length u16][reserved u16][crc32 u32][sent u32][magic u32][payload]
 *
 * Writing order makes a slot valid only once it is complete: payload, then
 * the header fields, then the magic word last. Marking a record as sent
 * clears the "sent" word in place (1 -> 0 bits only, no erase). A sector is
 * erased just before the first slot in it is written; if that sector still
 * holds unsent records they are dropped (oldest data goes first) and
 * counted. Records already sent stay readable as history until overwritten.
 *
 * Records are numbered by a monotonic id; the newest valid id found when the
 * log is opened tells where writing continues, so no separate index or
 * metadata sector is needed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RECORD_LOG_HEADER_SIZE 20U
#define RECORD_LOG_MAGIC 0x524C4753U /* "SGLR" */

typedef enum {
    RECORD_LOG_OK = 0,
    RECORD_LOG_EMPTY,      /* no (more) records */
    RECORD_LOG_ERR_ARG,
    RECORD_LOG_ERR_SIZE,   /* payload does not fit a slot / output buffer */
    RECORD_LOG_ERR_FLASH,  /* a flash callback failed */
} record_log_status_t;

typedef struct {
    /* Each returns 0 on success. Offsets are relative to the log area. */
    int (*read)(void *context, uint32_t offset, void *data, uint32_t size);
    int (*write)(void *context, uint32_t offset, const void *data, uint32_t size);
    int (*erase)(void *context, uint32_t offset, uint32_t size);
    void *context;
    uint32_t size;
    uint32_t sector_size;
    uint32_t slot_size;
} record_log_flash_t;

typedef struct {
    record_log_flash_t flash;
    uint32_t slot_count;
    uint32_t slots_per_sector;
    uint32_t next_slot;   /* where the next record is written */
    uint32_t next_id;     /* id of the next record */
    uint32_t oldest_slot; /* first slot that may hold an unsent record */
    uint32_t pending;     /* unsent records */
    uint32_t dropped;     /* unsent records lost to wrap-around since open */
    uint32_t corrupt;     /* records skipped because of a bad CRC since open */
} record_log_t;

/* Position-independent part of record_log_t, for caching across Deep Sleep. */
typedef struct {
    uint32_t next_slot;
    uint32_t next_id;
    uint32_t oldest_slot;
    uint32_t pending;
} record_log_state_t;

/* Validate the geometry and scan every slot header to rebuild the state. */
record_log_status_t record_log_open(record_log_t *log,
                                    const record_log_flash_t *flash);

/* Re-open from a cached state without scanning (trusted after Deep Sleep). */
record_log_status_t record_log_restore(record_log_t *log,
                                       const record_log_flash_t *flash,
                                       const record_log_state_t *state);
void record_log_save_state(const record_log_t *log, record_log_state_t *state);

/* Maximum payload length for the configured slot size. */
uint32_t record_log_max_payload(const record_log_t *log);

/* Append one record; *dropped_now (optional) gets the unsent records lost. */
record_log_status_t record_log_append(record_log_t *log,
                                      const void *payload,
                                      uint32_t length,
                                      uint32_t *dropped_now);

typedef struct {
    uint32_t slot;
    uint32_t visited;
} record_log_cursor_t;

/* Iterate unsent records from the oldest. */
void record_log_pending_begin(const record_log_t *log,
                              record_log_cursor_t *cursor);

/*
 * Copy the next unsent record into out (not NUL terminated) and return its
 * id. A record with a bad CRC (torn write) is marked sent and skipped.
 */
record_log_status_t record_log_pending_next(record_log_t *log,
                                            record_log_cursor_t *cursor,
                                            void *out,
                                            uint32_t out_size,
                                            uint32_t *length,
                                            uint32_t *id);

/* Mark every unsent record with id <= last_id as sent (FIFO acknowledge). */
record_log_status_t record_log_mark_sent_through(record_log_t *log,
                                                 uint32_t last_id);

/*
 * Visit up to max_records most recent valid records (sent or not) in
 * chronological order. The callback returns false to stop early.
 */
typedef bool (*record_log_visitor_t)(void *context,
                                     const void *payload,
                                     uint32_t length,
                                     uint32_t id,
                                     bool sent);
record_log_status_t record_log_recent(record_log_t *log,
                                      uint32_t max_records,
                                      void *buffer,
                                      uint32_t buffer_size,
                                      record_log_visitor_t visitor,
                                      void *context);

uint32_t record_log_crc32(const void *data, uint32_t length);

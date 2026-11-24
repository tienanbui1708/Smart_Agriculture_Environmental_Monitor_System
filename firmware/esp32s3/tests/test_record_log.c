/*
 * Host test for the record_log flash ring buffer on a simulated NOR flash
 * (writes can only clear bits; erase sets a whole sector to 0xFF).
 * Build from firmware/esp32s3:
 *   gcc -std=c11 -Wall -Wextra -Werror -Icomponents/record_log/include \
 *       tests/test_record_log.c components/record_log/record_log.c \
 *       -o test_record_log
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "record_log.h"

static int checks;
#define CHECK(condition) \
    do {                 \
        assert(condition); \
        checks++;        \
    } while (0)

#define SECTOR 4096U
#define SLOT 1024U
#define SECTORS 4U
#define AREA (SECTOR * SECTORS)

typedef struct {
    uint8_t bytes[AREA];
    int writes_left; /* < 0: unlimited; 0: fail (simulated power loss) */
    int erases;
} sim_flash_t;

static int sim_read(void *context, uint32_t offset, void *data, uint32_t size)
{
    sim_flash_t *flash = context;
    if (offset + size > AREA) {
        return -1;
    }
    memcpy(data, flash->bytes + offset, size);
    return 0;
}

static int sim_write(void *context, uint32_t offset, const void *data,
                     uint32_t size)
{
    sim_flash_t *flash = context;
    if (offset + size > AREA) {
        return -1;
    }
    if (flash->writes_left == 0) {
        return -1;
    }
    if (flash->writes_left > 0) {
        flash->writes_left--;
    }
    const uint8_t *source = data;
    for (uint32_t index = 0; index < size; index++) {
        flash->bytes[offset + index] &= source[index]; /* NOR: 1 -> 0 only */
    }
    return 0;
}

static int sim_erase(void *context, uint32_t offset, uint32_t size)
{
    sim_flash_t *flash = context;
    if (offset % SECTOR != 0 || size % SECTOR != 0 || offset + size > AREA) {
        return -1;
    }
    memset(flash->bytes + offset, 0xFF, size);
    flash->erases++;
    return 0;
}

static record_log_flash_t flash_of(sim_flash_t *flash)
{
    const record_log_flash_t config = {
        sim_read, sim_write, sim_erase, flash, AREA, SECTOR, SLOT,
    };
    return config;
}

static sim_flash_t *new_flash(uint8_t fill)
{
    sim_flash_t *flash = calloc(1, sizeof(*flash));
    assert(flash != NULL);
    memset(flash->bytes, fill, sizeof(flash->bytes));
    flash->writes_left = -1;
    return flash;
}

static void append_text(record_log_t *log, const char *text,
                        uint32_t *dropped)
{
    CHECK(record_log_append(log, text, (uint32_t)strlen(text), dropped) ==
          RECORD_LOG_OK);
}

/* Read all pending records into "a,b,c" form; return the last id. */
static uint32_t pending_list(record_log_t *log, char *out, size_t out_size)
{
    record_log_cursor_t cursor;
    record_log_pending_begin(log, &cursor);
    char record[SLOT];
    uint32_t length = 0;
    uint32_t id = 0;
    uint32_t last = 0;
    out[0] = '\0';
    while (record_log_pending_next(log, &cursor, record, sizeof(record) - 1,
                                   &length, &id) == RECORD_LOG_OK) {
        record[length] = '\0';
        if (out[0] != '\0') {
            strncat(out, ",", out_size - strlen(out) - 1);
        }
        strncat(out, record, out_size - strlen(out) - 1);
        last = id;
    }
    return last;
}

static void test_geometry(void)
{
    sim_flash_t *flash = new_flash(0xFF);
    record_log_t log;
    record_log_flash_t config = flash_of(flash);
    config.slot_size = 1000U; /* sector not a multiple of the slot */
    CHECK(record_log_open(&log, &config) == RECORD_LOG_ERR_ARG);
    config = flash_of(flash);
    config.size = SECTOR; /* need two sectors */
    CHECK(record_log_open(&log, &config) == RECORD_LOG_ERR_ARG);
    config = flash_of(flash);
    CHECK(record_log_open(&log, &config) == RECORD_LOG_OK);
    CHECK(log.slot_count == 16U);
    CHECK(record_log_max_payload(&log) == SLOT - RECORD_LOG_HEADER_SIZE);
    char big[SLOT];
    memset(big, 'x', sizeof(big));
    CHECK(record_log_append(&log, big, SLOT - RECORD_LOG_HEADER_SIZE + 1U,
                            NULL) == RECORD_LOG_ERR_SIZE);
    CHECK(record_log_append(&log, big, 0U, NULL) == RECORD_LOG_ERR_SIZE);
    free(flash);
}

static void test_append_send_ack(void)
{
    /* Garbage from an older image must not be taken for records. */
    sim_flash_t *flash = new_flash(0x5A);
    record_log_flash_t config = flash_of(flash);
    record_log_t log;
    CHECK(record_log_open(&log, &config) == RECORD_LOG_OK);
    CHECK(log.pending == 0U);
    append_text(&log, "a", NULL);
    append_text(&log, "b", NULL);
    append_text(&log, "c", NULL);
    CHECK(log.pending == 3U);

    char list[128];
    record_log_cursor_t cursor;
    record_log_pending_begin(&log, &cursor);
    char record[SLOT];
    uint32_t length = 0;
    uint32_t id = 0;
    CHECK(record_log_pending_next(&log, &cursor, record, sizeof(record),
                                  &length, &id) == RECORD_LOG_OK);
    CHECK(length == 1U && record[0] == 'a' && id == 1U);
    CHECK(record_log_pending_next(&log, &cursor, record, sizeof(record),
                                  &length, &id) == RECORD_LOG_OK);
    CHECK(id == 2U);
    /* Acknowledge the first two (one batch). */
    CHECK(record_log_mark_sent_through(&log, 2U) == RECORD_LOG_OK);
    CHECK(log.pending == 1U);
    CHECK(pending_list(&log, list, sizeof(list)) == 3U);
    CHECK(strcmp(list, "c") == 0);
    CHECK(record_log_mark_sent_through(&log, 3U) == RECORD_LOG_OK);
    CHECK(log.pending == 0U);
    record_log_pending_begin(&log, &cursor);
    CHECK(record_log_pending_next(&log, &cursor, record, sizeof(record),
                                  &length, &id) == RECORD_LOG_EMPTY);

    /* Reopen (cold boot): state is rebuilt from flash headers. */
    record_log_t reopened;
    CHECK(record_log_open(&reopened, &config) == RECORD_LOG_OK);
    CHECK(reopened.pending == 0U);
    CHECK(reopened.next_id == 4U);
    CHECK(reopened.next_slot == 3U);
    append_text(&reopened, "d", NULL);
    CHECK(pending_list(&reopened, list, sizeof(list)) == 4U);
    CHECK(strcmp(list, "d") == 0);
    free(flash);
}

static void test_wrap_drops_oldest(void)
{
    sim_flash_t *flash = new_flash(0xFF);
    record_log_flash_t config = flash_of(flash);
    record_log_t log;
    CHECK(record_log_open(&log, &config) == RECORD_LOG_OK);
    char text[16];
    uint32_t dropped = 0;
    uint32_t total_dropped = 0;
    /* 16 slots; nothing acknowledged. Record 17 recycles sector 0. */
    for (int index = 1; index <= 16; index++) {
        snprintf(text, sizeof(text), "r%d", index);
        append_text(&log, text, &dropped);
        total_dropped += dropped;
    }
    CHECK(total_dropped == 0U);
    CHECK(log.pending == 16U);
    append_text(&log, "r17", &dropped);
    CHECK(dropped == 4U); /* r1..r4 lost with sector 0 */
    CHECK(log.pending == 13U);
    CHECK(log.dropped == 4U);

    char list[256];
    CHECK(pending_list(&log, list, sizeof(list)) == 17U);
    CHECK(strncmp(list, "r5,r6,", 6) == 0);
    CHECK(strstr(list, "r16,r17") != NULL);

    /* Reopen finds the same oldest/newest after wrap-around. */
    record_log_t reopened;
    CHECK(record_log_open(&reopened, &config) == RECORD_LOG_OK);
    CHECK(reopened.pending == 13U);
    CHECK(reopened.next_id == 18U);
    char again[256];
    CHECK(pending_list(&reopened, again, sizeof(again)) == 17U);
    CHECK(strcmp(list, again) == 0);

    /* Acknowledge everything, then wrapping drops nothing. */
    CHECK(record_log_mark_sent_through(&reopened, 17U) == RECORD_LOG_OK);
    CHECK(reopened.pending == 0U);
    total_dropped = 0;
    for (int index = 18; index <= 30; index++) {
        snprintf(text, sizeof(text), "r%d", index);
        append_text(&reopened, text, &dropped);
        total_dropped += dropped;
        CHECK(record_log_mark_sent_through(&reopened, (uint32_t)index) ==
              RECORD_LOG_OK);
    }
    CHECK(total_dropped == 0U);
    free(flash);
}

static void test_inflight_batch_dropped_by_wrap(void)
{
    sim_flash_t *flash = new_flash(0xFF);
    record_log_flash_t config = flash_of(flash);
    record_log_t log;
    CHECK(record_log_open(&log, &config) == RECORD_LOG_OK);
    char text[16];
    for (int index = 1; index <= 16; index++) {
        snprintf(text, sizeof(text), "r%d", index);
        append_text(&log, text, NULL);
    }
    /* r1..r3 are in flight when r17 recycles their sector. */
    append_text(&log, "r17", NULL);
    /* The late acknowledgement must not consume r5 or later. */
    CHECK(record_log_mark_sent_through(&log, 3U) == RECORD_LOG_OK);
    CHECK(log.pending == 13U);
    char list[256];
    pending_list(&log, list, sizeof(list));
    CHECK(strncmp(list, "r5,", 3) == 0);
    free(flash);
}

static void test_power_loss(void)
{
    sim_flash_t *flash = new_flash(0xFF);
    record_log_flash_t config = flash_of(flash);
    record_log_t log;
    CHECK(record_log_open(&log, &config) == RECORD_LOG_OK);
    append_text(&log, "ok1", NULL);
    /* Payload written, header lost: slot must not count after reboot. */
    flash->writes_left = 1;
    CHECK(record_log_append(&log, "torn", 4U, NULL) == RECORD_LOG_ERR_FLASH);
    flash->writes_left = -1;

    record_log_t reopened;
    CHECK(record_log_open(&reopened, &config) == RECORD_LOG_OK);
    CHECK(reopened.pending == 1U);
    CHECK(reopened.next_slot == 2U); /* skips the dirty slot */
    append_text(&reopened, "ok2", NULL);
    char list[64];
    /* Ids continue from the newest complete record. */
    CHECK(pending_list(&reopened, list, sizeof(list)) == 2U);
    CHECK(strcmp(list, "ok1,ok2") == 0);

    /* Corrupt a payload byte: CRC mismatch is skipped and marked sent. */
    flash->bytes[SLOT * 2U + RECORD_LOG_HEADER_SIZE] &= 0x00;
    CHECK(pending_list(&reopened, list, sizeof(list)) == 1U);
    CHECK(strcmp(list, "ok1") == 0);
    CHECK(reopened.corrupt == 1U);
    CHECK(reopened.pending == 1U);
    free(flash);
}

static void test_restore_state(void)
{
    sim_flash_t *flash = new_flash(0xFF);
    record_log_flash_t config = flash_of(flash);
    record_log_t log;
    CHECK(record_log_open(&log, &config) == RECORD_LOG_OK);
    append_text(&log, "x", NULL);
    append_text(&log, "y", NULL);
    record_log_state_t state;
    record_log_save_state(&log, &state);
    record_log_t restored;
    CHECK(record_log_restore(&restored, &config, &state) == RECORD_LOG_OK);
    CHECK(restored.pending == 2U && restored.next_id == 3U);
    state.next_slot = 999U;
    CHECK(record_log_restore(&restored, &config, &state) == RECORD_LOG_ERR_ARG);
    free(flash);
}

typedef struct {
    char text[256];
    int sent;
} recent_t;

static bool collect(void *context, const void *payload, uint32_t length,
                    uint32_t id, bool sent)
{
    (void)id;
    recent_t *recent = context;
    if (recent->text[0] != '\0') {
        strcat(recent->text, ",");
    }
    strncat(recent->text, payload, length);
    recent->sent += sent ? 1 : 0;
    return true;
}

static void test_recent_history(void)
{
    sim_flash_t *flash = new_flash(0xFF);
    record_log_flash_t config = flash_of(flash);
    record_log_t log;
    CHECK(record_log_open(&log, &config) == RECORD_LOG_OK);
    char text[16];
    for (int index = 1; index <= 20; index++) {
        snprintf(text, sizeof(text), "h%d", index);
        append_text(&log, text, NULL);
    }
    /* Ring completely full: oldest pending slot == next write slot. */
    CHECK(log.pending == 16U);
    CHECK(log.oldest_slot == log.next_slot);
    char full[256];
    CHECK(pending_list(&log, full, sizeof(full)) == 20U);
    CHECK(strncmp(full, "h5,", 3) == 0);
    CHECK(record_log_mark_sent_through(&log, 18U) == RECORD_LOG_OK);
    CHECK(log.pending == 2U);
    char buffer[SLOT];
    recent_t recent = {{0}, 0};
    CHECK(record_log_recent(&log, 3U, buffer, sizeof(buffer), collect,
                            &recent) == RECORD_LOG_OK);
    CHECK(strcmp(recent.text, "h18,h19,h20") == 0);
    CHECK(recent.sent == 1);
    /* Asking for more than exists returns everything still on flash. */
    memset(&recent, 0, sizeof(recent));
    CHECK(record_log_recent(&log, 100U, buffer, sizeof(buffer), collect,
                            &recent) == RECORD_LOG_OK);
    CHECK(strncmp(recent.text, "h5,h6,", 6) == 0); /* h1..h4 recycled */
    free(flash);
}

/* Random appends, partial acknowledgements and reboots. Invariants: pending
 * ids strictly increase, their count equals log.pending, nothing is lost
 * except what wrap-around reports as dropped, and a reopen sees the same. */
static void test_randomized(void)
{
    sim_flash_t *flash = new_flash(0xFF);
    record_log_flash_t config = flash_of(flash);
    record_log_t log;
    CHECK(record_log_open(&log, &config) == RECORD_LOG_OK);
    srand(12345);
    uint32_t appended = 0U;
    uint32_t acknowledged = 0U;
    uint32_t dropped_total = 0U;
    for (int step = 0; step < 4000; step++) {
        const int action = rand() % 10;
        if (action < 6) {
            char text[32];
            snprintf(text, sizeof(text), "p%u", (unsigned)(appended + 1U));
            uint32_t dropped = 0U;
            CHECK(record_log_append(&log, text, (uint32_t)strlen(text),
                                    &dropped) == RECORD_LOG_OK);
            appended++;
            dropped_total += dropped;
        } else if (action < 9) {
            /* Acknowledge a batch of 1..4 from the oldest pending. */
            record_log_cursor_t cursor;
            record_log_pending_begin(&log, &cursor);
            char record[SLOT];
            uint32_t length = 0U;
            uint32_t id = 0U;
            uint32_t last = 0U;
            const int batch = 1 + rand() % 4;
            for (int index = 0; index < batch; index++) {
                if (record_log_pending_next(&log, &cursor, record,
                                            sizeof(record), &length,
                                            &id) != RECORD_LOG_OK) {
                    break;
                }
                last = id;
            }
            if (last != 0U) {
                const uint32_t before = log.pending;
                CHECK(record_log_mark_sent_through(&log, last) ==
                      RECORD_LOG_OK);
                acknowledged += before - log.pending;
            }
        } else {
            const uint32_t pending = log.pending;
            CHECK(record_log_open(&log, &config) == RECORD_LOG_OK);
            CHECK(log.pending == pending);
        }
        CHECK(appended == acknowledged + dropped_total + log.pending);
        record_log_cursor_t cursor;
        record_log_pending_begin(&log, &cursor);
        char record[SLOT];
        uint32_t length = 0U;
        uint32_t id = 0U;
        uint32_t previous_id = 0U;
        uint32_t count = 0U;
        while (record_log_pending_next(&log, &cursor, record, sizeof(record),
                                       &length, &id) == RECORD_LOG_OK) {
            CHECK(id > previous_id);
            previous_id = id;
            count++;
        }
        CHECK(count == log.pending);
        CHECK(log.pending <= log.slot_count);
    }
    CHECK(dropped_total > 0U && acknowledged > 0U);
    free(flash);
}

static void test_crc(void)
{
    CHECK(record_log_crc32("123456789", 9U) == 0xCBF43926U);
}

int main(void)
{
    test_crc();
    test_geometry();
    test_append_send_ack();
    test_wrap_drops_oldest();
    test_inflight_batch_dropped_by_wrap();
    test_power_loss();
    test_restore_state();
    test_recent_history();
    test_randomized();
    printf("record_log tests: %d checks passed\n", checks);
    return 0;
}

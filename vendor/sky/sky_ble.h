/*
 * Skylanders BLE link, message handling (platform independent; builds on the Flipper and on a PC).
 * The PC "portal" app sends 1024-byte figure dumps; this loads them into the SkyBase portal core
 * and reports back. Frames come from ble_proto.c (protocol version 2, see ble_proto.h).
 *
 * PC -> Flipper: HELLO, LOAD [slot][1024], REMOVE [slot], CLEAR, STATUS_REQ
 * Flipper -> PC: STATUS [version=2][usb_state][present_lo][present_hi][game],
 *                RESULT [request][slot][code], SAVE [slot][1024] (the game changed this figure)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/sky_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes of list lines per DUMP_LIST_ITEM frame, and listing work done per poll. */
#define SKY_BLE_LIST_BATCH 400
#define SKY_BLE_LIST_STEPS 32

typedef bool (*SkyBleSendFn)(void* ctx, uint8_t type, const uint8_t* payload, uint16_t len);
/* Persist data that could not be sent to the PC (the app writes unsent_slotN.bin). */
typedef void (*SkyBleSaveFn)(void* ctx, uint8_t slot, const uint8_t data[SKY_FIGURE_SIZE]);
typedef void (*SkyBleEmitPathFn)(void* emit_ctx, const char* path);
typedef void (*SkyBleListDumpsFn)(void* file_ctx, SkyBleEmitPathFn emit, void* emit_ctx);
/* Incremental dump listing (preferred): begin, then step until it returns false, then end.
 * Each step does a small amount of work and may call emit() for found dumps. This keeps the
 * app loop responsive and lets the link abort the listing the moment a send fails. */
typedef void (*SkyBleListBeginFn)(void* file_ctx);
typedef bool (*SkyBleListStepFn)(void* file_ctx, SkyBleEmitPathFn emit, void* emit_ctx);
typedef void (*SkyBleListEndFn)(void* file_ctx);
typedef bool (*SkyBleReadDumpFn)(void* file_ctx, const char* path, uint8_t data[SKY_FIGURE_SIZE]);
typedef bool (*SkyBleWriteDumpFn)(void* file_ctx, const char* path, const uint8_t data[SKY_FIGURE_SIZE]);

typedef struct {
    SkyBase* base;
    SkyBleSendFn send;
    void* send_ctx;
    void (*lock)(void); /* optional: guards SkyBase access shared with the USB interrupt */
    void (*unlock)(void);
    uint8_t game; /* SkyGame value reported in STATUS */
    uint8_t usb_state; /* UsbSkyState value reported in STATUS */
    bool remote[SKY_SLOT_COUNT]; /* figure in this slot came from the PC */
    uint8_t fails[SKY_SLOT_COUNT]; /* consecutive failed SAVE sends per slot */
    uint16_t saves_sent; /* diagnostics, shown on the Flipper screen */
    uint16_t saves_failed;
    /* Optional SD-card file operations, configured by skybase.c. */
    void* file_ctx;
    SkyBleListDumpsFn list_dumps;
    SkyBleReadDumpFn read_dump;
    SkyBleWriteDumpFn write_dump;
    /* Incremental listing state. Items are batched: one frame carries several '\n'-separated
     * lines, each either ">dir" (sets the current directory) or a bare file name inside it. */
    SkyBleListBeginFn list_begin;
    SkyBleListStepFn list_step;
    SkyBleListEndFn list_end;
    bool list_active;
    bool list_failed;
    uint16_t list_frames; /* item frames sent in this listing */
    uint16_t list_found;  /* dumps found in this listing */
    uint16_t list_len;
    uint16_t list_dir_len;
    uint8_t list_buf[SKY_BLE_LIST_BATCH];
    char list_dir[256];
    char write_path[256];
    uint8_t write_data[SKY_FIGURE_SIZE];
    uint16_t write_offset;
    bool write_active;
} SkyBle;

/* A failed SAVE is retried this many times before the data is parked as unsent_slotN.bin. */
#define SKY_BLE_SAVE_RETRIES 4

void sky_ble_init(SkyBle* b, SkyBase* base, SkyBleSendFn send, void* send_ctx);
void sky_ble_set_file_ops(
    SkyBle* b, void* file_ctx, SkyBleListDumpsFn list_dumps,
    SkyBleReadDumpFn read_dump, SkyBleWriteDumpFn write_dump);

void sky_ble_set_list_ops(
    SkyBle* b, SkyBleListBeginFn begin, SkyBleListStepFn step, SkyBleListEndFn end);
/* Call from the app loop: advances an active dump listing by one radio frame at most. */
void sky_ble_list_tick(SkyBle* b);
/* Stop a running listing and release its resources (BLE going away). */
void sky_ble_list_cancel(SkyBle* b);

/* Matches BleProtoHandler; ctx is the SkyBle*. */
void sky_ble_on_message(void* ctx, uint8_t type, const uint8_t* payload, uint16_t len);

/* Send SAVE for each PC-loaded slot whose data the game changed; if a send fails, hand the data
 * to `unsent` instead so nothing is lost. scratch is SKY_FIGURE_SIZE bytes. */
void sky_ble_poll_saves(
    SkyBle* b,
    uint8_t scratch[SKY_FIGURE_SIZE],
    SkyBleSaveFn unsent,
    void* unsent_ctx);

/* BLE is being switched off: hand every changed PC-loaded slot to `unsent`, then forget that any
 * slot came from the PC (the figures stay on the portal). */
void sky_ble_flush_unsent(
    SkyBle* b,
    uint8_t scratch[SKY_FIGURE_SIZE],
    SkyBleSaveFn unsent,
    void* unsent_ctx);

#ifdef __cplusplus
}
#endif

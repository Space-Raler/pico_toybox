/*
 * LEGO Dimensions BLE link, protocol side (no Flipper dependencies, tested on a PC).
 *
 * The web page names tokens by id; the Flipper builds them itself, like its own menus do.
 * Frames are the shared ones from inf/ble_proto.h (protocol version 3).
 *
 *   PC -> Flipper
 *     HELLO / STATUS_REQ                  -> STATUS
 *     LD_PLACE [box][kind][id lo][id hi]  kind 1 = minifigure, 2 = vehicle; a vehicle may add
 *              ([up0 4 bytes][up1 4 bytes], little endian) after the id. Replaces what is on that box.
 *     REMOVE   [box]
 *     CLEAR                               take every token off the pad
 *     LD_PLACE_VEHICLE [box][uid 7][data 180]   a vehicle exactly as it was saved (upgrades kept)
 *     LD_GET [box]                        -> LD_TOKEN
 *     LD_SAVED_LIST                       -> LD_SAVED_ITEM [name] ..., then an empty one
 *     LD_SAVED_READ [name]                -> LD_TOKEN with box 0xFF
 *     LD_SAVE_VEHICLE [box]               save that vehicle in the Flipper's own saved vehicles
 *   Flipper -> PC
 *     STATUS [version][usb_state][filled mask lo][hi] then 7 x [kind][id lo][id hi]
 *            usb_state: 0 not connected to a console, 1 ready, 2 connected but the Emulate
 *            screen has not been opened yet on the Flipper
 *     RESULT [request type][box][code]
 *     LD_TOKEN [box][uid 7][data 180]     a vehicle's memory, pushed shortly after the game
 *            wrote to it (upgrades, levels) and whenever asked for. Like SAVE on Skylanders.
 *
 * Boxes are the 7 spots of the Flipper's Emulate screen: 0 left-top, 1 centre, 2 right-top,
 * 3 and 4 left-bottom, 5 and 6 right-bottom.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../inf/ble_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LD_BLE_BOXES 7

enum { LdKindEmpty = 0, LdKindMinifig = 1, LdKindVehicle = 2 };
enum { LdUsbNone = 0, LdUsbReady = 1, LdUsbOpenEmulate = 2 };

typedef struct {
    uint8_t (*usb_state)(void* ctx);
    /* Returns a BleResult* code. */
    uint8_t (*place)(void* ctx, uint8_t box, uint8_t kind, uint16_t id, const uint32_t up[2]);
    uint8_t (*remove)(void* ctx, uint8_t box);
    void (*clear)(void* ctx);
    /* What is on `box`: a LdKind*, and its id in *id. */
    uint8_t (*box_info)(void* ctx, uint8_t box, uint16_t* id);
    /* Vehicle memory. All optional (NULL = not supported, answered with "unknown"). */
    /* Copy the uid and the 180 data bytes of the vehicle on `box`. False if not a vehicle. */
    bool (*get_token)(void* ctx, uint8_t box, uint8_t uid[BLE_LD_UID_SIZE], uint8_t data[BLE_LD_TOKEN_SIZE]);
    /* Place a vehicle from saved uid + data. Returns a BleResult* code. */
    uint8_t (*place_vehicle)(void* ctx, uint8_t box, const uint8_t uid[BLE_LD_UID_SIZE], const uint8_t data[BLE_LD_TOKEN_SIZE]);
    /* Save the vehicle on `box` to the Flipper's own saved vehicles. Returns a BleResult* code. */
    uint8_t (*save_box)(void* ctx, uint8_t box);
    /* Call emit() once per saved vehicle file name (at most LD_BLE_SAVED_MAX). */
    void (*saved_list)(void* ctx, void (*emit)(void* emit_ctx, const char* name), void* emit_ctx);
    /* Load a saved vehicle file by name. False if missing or not a vehicle. */
    bool (*saved_read)(void* ctx, const char* name, uint8_t uid[BLE_LD_UID_SIZE], uint8_t data[BLE_LD_TOKEN_SIZE]);
} LdBleOps;

#define LD_BLE_SAVED_MAX 40
#define LD_BLE_NAME_MAX 96

typedef bool (*LdBleSendFn)(void* ctx, uint8_t type, const uint8_t* payload, uint16_t len);

typedef struct {
    LdBleOps ops;
    void* ops_ctx;
    LdBleSendFn send;
    void* send_ctx;
    BleProtoParser parser;
    uint8_t last_status[4 + 3 * LD_BLE_BOXES]; /* what the PC was last told */
    bool status_known;
} LdBle;

void ld_ble_init(LdBle* b, const LdBleOps* ops, void* ops_ctx, LdBleSendFn send, void* send_ctx);
void ld_ble_feed(LdBle* b, const uint8_t* data, size_t size);
void ld_ble_resync(LdBle* b);
/* Send STATUS if the pad changed since the last one (the Flipper's own buttons move tokens too). */
void ld_ble_push_status_if_changed(LdBle* b);
/* Send the vehicle on `box` to the PC as LD_TOKEN (the game changed its data). False if there is
 * no vehicle there or the send failed (try again later). */
bool ld_ble_push_token(LdBle* b, uint8_t box);
void ld_ble_on_message(void* ctx, uint8_t type, const uint8_t* payload, uint16_t len);

#ifdef __cplusplus
}
#endif

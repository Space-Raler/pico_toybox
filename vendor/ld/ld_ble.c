/* LEGO Dimensions BLE link, protocol side. See ld_ble.h. SPDX-License-Identifier: GPL-2.0-only */
#include "ld_ble.h"

#include <string.h>

void ld_ble_init(LdBle* b, const LdBleOps* ops, void* ops_ctx, LdBleSendFn send, void* send_ctx) {
    memset(b, 0, sizeof(*b));
    b->ops = *ops;
    b->ops_ctx = ops_ctx;
    b->send = send;
    b->send_ctx = send_ctx;
    ble_proto_init(&b->parser);
}

void ld_ble_feed(LdBle* b, const uint8_t* data, size_t size) {
    ble_proto_feed(&b->parser, data, size, ld_ble_on_message, b);
}

void ld_ble_resync(LdBle* b) {
    if(ble_proto_busy(&b->parser)) ble_proto_resync(&b->parser, ld_ble_on_message, b);
}

static void build_status(LdBle* b, uint8_t out[4 + 3 * LD_BLE_BOXES]) {
    uint16_t mask = 0;
    for(uint8_t i = 0; i < LD_BLE_BOXES; i++) {
        uint16_t id = 0;
        const uint8_t kind = b->ops.box_info(b->ops_ctx, i, &id);
        if(kind != LdKindMinifig && kind != LdKindVehicle) {
            out[4 + 3 * i] = LdKindEmpty;
            out[5 + 3 * i] = 0;
            out[6 + 3 * i] = 0;
            continue;
        }
        mask |= (uint16_t)(1u << i);
        out[4 + 3 * i] = kind;
        out[5 + 3 * i] = (uint8_t)(id & 0xFF);
        out[6 + 3 * i] = (uint8_t)(id >> 8);
    }
    out[0] = BLE_PROTO_VERSION_LD;
    out[1] = b->ops.usb_state(b->ops_ctx);
    out[2] = (uint8_t)(mask & 0xFF);
    out[3] = (uint8_t)(mask >> 8);
}

static void send_status(LdBle* b) {
    uint8_t status[4 + 3 * LD_BLE_BOXES];
    build_status(b, status);
    if(b->send(b->send_ctx, BleMsgStatus, status, sizeof(status))) {
        memcpy(b->last_status, status, sizeof(status));
        b->status_known = true;
    }
}

void ld_ble_push_status_if_changed(LdBle* b) {
    uint8_t status[4 + 3 * LD_BLE_BOXES];
    build_status(b, status);
    if(b->status_known && memcmp(status, b->last_status, sizeof(status)) == 0) return;
    if(b->send(b->send_ctx, BleMsgStatus, status, sizeof(status))) {
        memcpy(b->last_status, status, sizeof(status));
        b->status_known = true;
    }
}

static void send_result(LdBle* b, uint8_t request, uint8_t box, uint8_t code) {
    const uint8_t payload[3] = {request, box, code};
    b->send(b->send_ctx, BleMsgResult, payload, sizeof(payload));
}


static bool send_token_frame(LdBle* b, uint8_t box, const uint8_t uid[BLE_LD_UID_SIZE], const uint8_t data[BLE_LD_TOKEN_SIZE]) {
    uint8_t frame[BLE_LD_TOKEN_FRAME];
    frame[0] = box;
    memcpy(frame + 1, uid, BLE_LD_UID_SIZE);
    memcpy(frame + 1 + BLE_LD_UID_SIZE, data, BLE_LD_TOKEN_SIZE);
    return b->send(b->send_ctx, BleMsgLdToken, frame, sizeof(frame));
}

bool ld_ble_push_token(LdBle* b, uint8_t box) {
    uint8_t uid[BLE_LD_UID_SIZE], data[BLE_LD_TOKEN_SIZE];
    if(box >= LD_BLE_BOXES || !b->ops.get_token || !b->ops.get_token(b->ops_ctx, box, uid, data)) return false;
    return send_token_frame(b, box, uid, data);
}

/* A saved-vehicle file name from the PC: a plain "*.toy" name, never a path. */
static bool name_ok(const char* n, size_t len) {
    if(len < 5 || len > LD_BLE_NAME_MAX) return false;
    for(size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)n[i];
        if(c < 0x20 || c == '/' || c == '\\' || c == 0x7F) return false;
    }
    if(n[0] == '.') return false;
    return memcmp(n + len - 4, ".toy", 4) == 0;
}

static void list_emit(void* ctx, const char* name) {
    LdBle* b = ctx;
    const size_t n = name ? strlen(name) : 0;
    if(n && n <= LD_BLE_NAME_MAX) b->send(b->send_ctx, BleMsgLdSavedItem, (const uint8_t*)name, (uint16_t)n);
}

void ld_ble_on_message(void* ctx, uint8_t type, const uint8_t* p, uint16_t len) {
    LdBle* b = ctx;
    switch(type) {
    case BleMsgHello:
    case BleMsgStatusReq:
        b->status_known = false; /* the PC just (re)connected: it needs the full picture */
        send_status(b);
        break;
    case BleMsgLdPlace: {
        const uint8_t box = len ? p[0] : 0;
        if(len != 4 && len != 12) {
            send_result(b, type, box, BleResultBadLength);
            break;
        }
        const uint8_t kind = p[1];
        const uint16_t id = (uint16_t)(p[2] | (p[3] << 8));
        if(box >= LD_BLE_BOXES) {
            send_result(b, type, box, BleResultBadSlot);
            break;
        }
        if((kind != LdKindMinifig && kind != LdKindVehicle) || id == 0 ||
           (len == 12 && kind != LdKindVehicle)) {
            send_result(b, type, box, BleResultUnknown);
            break;
        }
        uint32_t up[2] = {0, 0};
        if(len == 12) {
            for(uint8_t i = 0; i < 2; i++) {
                const uint8_t* q = p + 4 + 4 * i;
                up[i] = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) |
                        ((uint32_t)q[3] << 24);
            }
        }
        send_result(b, type, box, b->ops.place(b->ops_ctx, box, kind, id, up));
        send_status(b);
        break;
    }
    case BleMsgRemove: {
        const uint8_t box = len ? p[0] : 0;
        uint8_t code;
        if(len != 1) code = BleResultBadLength;
        else if(box >= LD_BLE_BOXES) code = BleResultBadSlot;
        else code = b->ops.remove(b->ops_ctx, box);
        send_result(b, type, box, code);
        send_status(b);
        break;
    }
    case BleMsgClear:
        b->ops.clear(b->ops_ctx);
        send_result(b, type, 0, BleResultOk);
        send_status(b);
        break;
    case BleMsgLdPlaceVehicle: {
        const uint8_t box = len ? p[0] : 0;
        if(len != BLE_LD_TOKEN_FRAME) {
            send_result(b, type, box, BleResultBadLength);
            break;
        }
        if(box >= LD_BLE_BOXES) {
            send_result(b, type, box, BleResultBadSlot);
            break;
        }
        if(!b->ops.place_vehicle) {
            send_result(b, type, box, BleResultUnknown);
            break;
        }
        send_result(b, type, box, b->ops.place_vehicle(b->ops_ctx, box, p + 1, p + 1 + BLE_LD_UID_SIZE));
        send_status(b);
        break;
    }
    case BleMsgLdGet: {
        const uint8_t box = len ? p[0] : 0;
        if(len != 1) send_result(b, type, box, BleResultBadLength);
        else if(box >= LD_BLE_BOXES) send_result(b, type, box, BleResultBadSlot);
        else if(!ld_ble_push_token(b, box)) send_result(b, type, box, BleResultNotPresent);
        break;
    }
    case BleMsgLdSaveVehicle: {
        const uint8_t box = len ? p[0] : 0;
        if(len != 1) send_result(b, type, box, BleResultBadLength);
        else if(box >= LD_BLE_BOXES) send_result(b, type, box, BleResultBadSlot);
        else if(!b->ops.save_box) send_result(b, type, box, BleResultUnknown);
        else send_result(b, type, box, b->ops.save_box(b->ops_ctx, box));
        break;
    }
    case BleMsgLdSavedList:
        if(b->ops.saved_list) b->ops.saved_list(b->ops_ctx, list_emit, b);
        b->send(b->send_ctx, BleMsgLdSavedItem, NULL, 0); /* empty frame ends the list */
        break;
    case BleMsgLdSavedRead: {
        uint8_t uid[BLE_LD_UID_SIZE], data[BLE_LD_TOKEN_SIZE];
        if(!name_ok((const char*)p, len)) {
            send_result(b, type, BLE_LD_BOX_FILE, BleResultBadLength);
            break;
        }
        char name[LD_BLE_NAME_MAX + 1];
        memcpy(name, p, len);
        name[len] = '\0';
        if(!b->ops.saved_read || !b->ops.saved_read(b->ops_ctx, name, uid, data) ||
           !send_token_frame(b, BLE_LD_BOX_FILE, uid, data))
            send_result(b, type, BLE_LD_BOX_FILE, BleResultNotPresent);
        break;
    }
    default:
        send_result(b, type, 0, BleResultUnknown);
        break;
    }
}

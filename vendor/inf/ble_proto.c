/*
 * FlipBase BLE link: frame parser and encoder. See ble_proto.h.
 *
 * The parser buffers raw bytes and rescans from the byte after a bad start marker,
 * so line noise that happens to contain 0xA5 can never swallow the frame behind it.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "ble_proto.h"

#include <string.h>

static uint8_t crc8_update(uint8_t crc, uint8_t byte) {
    crc ^= byte;
    for(int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

void ble_proto_init(BleProtoParser* p) {
    memset(p, 0, sizeof(*p));
}

int ble_proto_busy(const BleProtoParser* p) {
    return p->n > 0;
}

static void drop(BleProtoParser* p, uint16_t count) {
    if(count >= p->n) {
        p->n = 0;
    } else {
        memmove(p->buf, p->buf + count, (size_t)(p->n - count));
        p->n = (uint16_t)(p->n - count);
    }
}

/* Deliver every complete good frame at the front of the buffer. */
static void extract(BleProtoParser* p, BleProtoHandler handler, void* ctx) {
    for(;;) {
        uint16_t skip = 0;
        while(skip < p->n && p->buf[skip] != BLE_PROTO_SOF) skip++;
        if(skip) drop(p, skip);
        if(p->n < 4) return;

        const uint16_t len = (uint16_t)(p->buf[2] | (p->buf[3] << 8));
        if(len > BLE_PROTO_MAX_PAYLOAD) {
            p->bad_frames++;
            drop(p, 1);
            continue;
        }
        const uint16_t total = (uint16_t)(5 + len);
        if(p->n < total) return; /* wait for the rest */

        uint8_t crc = 0;
        for(uint16_t i = 1; i < (uint16_t)(total - 1); i++) crc = crc8_update(crc, p->buf[i]);
        if(crc == p->buf[total - 1]) {
            if(handler) handler(ctx, p->buf[1], p->buf + 4, len);
            drop(p, total);
        } else {
            p->bad_frames++;
            drop(p, 1); /* not a real frame: look for the next start marker */
        }
    }
}

void ble_proto_feed(
    BleProtoParser* p,
    const uint8_t* data,
    size_t size,
    BleProtoHandler handler,
    void* ctx) {
    for(size_t i = 0; i < size; i++) {
        if(p->n >= BLE_PROTO_MAX_FRAME) drop(p, 1); /* cannot happen; keeps us safe */
        p->buf[p->n++] = data[i];
        extract(p, handler, ctx);
    }
}

void ble_proto_resync(BleProtoParser* p, BleProtoHandler handler, void* ctx) {
    if(p->n == 0) return;
    p->bad_frames++;
    drop(p, 1);
    extract(p, handler, ctx);
}

size_t ble_proto_encode(uint8_t type, const uint8_t* payload, uint16_t len, uint8_t* out) {
    size_t n = 0;
    out[n++] = BLE_PROTO_SOF;
    out[n++] = type;
    out[n++] = (uint8_t)(len & 0xFF);
    out[n++] = (uint8_t)(len >> 8);
    uint8_t crc = crc8_update(crc8_update(crc8_update(0, type), (uint8_t)(len & 0xFF)), (uint8_t)(len >> 8));
    for(uint16_t i = 0; i < len; i++) {
        out[n++] = payload[i];
        crc = crc8_update(crc, payload[i]);
    }
    out[n++] = crc;
    return n;
}

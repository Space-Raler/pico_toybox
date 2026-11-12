#include "sky_core.h"
#include <string.h>

static void status_push(SkySlot* s, uint8_t v) {
    if(s->q_count == SKY_STATUS_QUEUE_DEPTH) { /* host not polling: drop oldest */
        s->q_head = (uint8_t)((s->q_head + 1) % SKY_STATUS_QUEUE_DEPTH);
        s->q_count--;
    }
    s->q[(s->q_head + s->q_count) % SKY_STATUS_QUEUE_DEPTH] = v;
    s->q_count++;
}

static bool status_pop(SkySlot* s, uint8_t* v) {
    if(s->q_count == 0) return false;
    *v = s->q[s->q_head];
    s->q_head = (uint8_t)((s->q_head + 1) % SKY_STATUS_QUEUE_DEPTH);
    s->q_count--;
    return true;
}

static void reply_push(SkyBase* b, const uint8_t* pkt) {
    if(b->r_count == SKY_REPLY_QUEUE_DEPTH) return; /* full: drop newest */
    uint8_t tail = (uint8_t)((b->r_head + b->r_count) % SKY_REPLY_QUEUE_DEPTH);
    memcpy(b->r_queue[tail], pkt, SKY_PACKET_SIZE);
    b->r_count++;
}

static bool size_is(size_t len, size_t a, size_t b) {
    return len == a || len == b;
}

static void lights_ring(SkyBase* b, uint8_t r, uint8_t g, uint8_t blue, uint32_t fade_ms) {
    SkyLights* l = &b->lights;
    l->ring[0] = r;
    l->ring[1] = g;
    l->ring[2] = blue;
    l->fade_ms = (uint16_t)(fade_ms > SKY_LIGHT_FADE_MAX_MS ? SKY_LIGHT_FADE_MAX_MS : fade_ms);
    l->seq++;
}

static void lights_backlight(SkyBase* b, uint8_t level) {
    SkyLights* l = &b->lights;
    l->backlight = level;
    l->backlight_set = true;
    l->seq++;
}

void sky_base_init(SkyBase* b) {
    memset(b, 0, sizeof(*b));
    b->activated = true; /* matches RPCS3: portal starts active */
    b->fw_version = 0x1B;
}

static void activate(SkyBase* b) {
    if(b->activated) return;
    for(int i = 0; i < SKY_SLOT_COUNT; i++) {
        SkySlot* s = &b->slots[i];
        if(s->status & 1) {
            status_push(s, 3);
            status_push(s, 1);
        }
    }
    b->activated = true;
}

/* Reset (the host's 'R'): drop pending transitions and settle every slot at its final
 * presence, clearing the "changed" bit. The next activate() re-announces present figures. */
static void deactivate(SkyBase* b) {
    for(int i = 0; i < SKY_SLOT_COUNT; i++) {
        SkySlot* s = &b->slots[i];
        if(s->q_count > 0) {
            s->status = s->q[(s->q_head + s->q_count - 1) % SKY_STATUS_QUEUE_DEPTH];
            s->q_count = 0;
            s->q_head = 0;
        }
        s->status &= 1;
    }
    b->activated = false;
}

static uint32_t serial_of(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void place_figure(SkyBase* b, int slot, const uint8_t* data) {
    SkySlot* s = &b->slots[slot];
    memcpy(s->data, data, SKY_FIGURE_SIZE);
    s->status = 3;
    /* Pushed on top of anything still pending; the reference never clears it. */
    status_push(s, 3);
    status_push(s, 1);
    s->last_id = serial_of(data);
    s->dirty = false;
}

int sky_base_load_figure(SkyBase* b, const uint8_t data[SKY_FIGURE_SIZE]) {
    const uint32_t serial = serial_of(data);
    int found = -1;
    /* Mimic slot retention: prefer a free slot that last held this serial,
     * otherwise the lowest free slot. A slot mid-removal (status 2) is free. */
    for(int i = 0; i < SKY_SLOT_COUNT; i++) {
        if((b->slots[i].status & 1) == 0) {
            if(b->slots[i].last_id == serial) {
                found = i;
                break;
            }
            if(found < 0) found = i;
        }
    }
    if(found < 0) return -1;
    place_figure(b, found, data);
    return found;
}

int sky_base_load_figure_at(SkyBase* b, uint8_t slot, const uint8_t data[SKY_FIGURE_SIZE]) {
    if(slot >= SKY_SLOT_COUNT || (b->slots[slot].status & 1)) return -1;
    place_figure(b, slot, data);
    return slot;
}

bool sky_base_remove_figure(SkyBase* b, uint8_t slot) {
    if(slot >= SKY_SLOT_COUNT) return false;
    SkySlot* s = &b->slots[slot];
    if(!(s->status & 1)) return false;
    s->status = 2;
    status_push(s, 2);
    status_push(s, 0);
    return true;
}

bool sky_base_slot_present(const SkyBase* b, uint8_t slot) {
    return slot < SKY_SLOT_COUNT && (b->slots[slot].status & 1);
}

bool sky_base_slot_present_settled(const SkyBase* b, uint8_t slot) {
    if(slot >= SKY_SLOT_COUNT) return false;
    const SkySlot* s = &b->slots[slot];
    if(s->q_count == 0) return s->status & 1;
    return s->q[(s->q_head + s->q_count - 1) % SKY_STATUS_QUEUE_DEPTH] & 1;
}

bool sky_base_take_dirty(SkyBase* b, uint8_t slot, uint8_t out[SKY_FIGURE_SIZE]) {
    if(slot >= SKY_SLOT_COUNT || !b->slots[slot].dirty) return false;
    memcpy(out, b->slots[slot].data, SKY_FIGURE_SIZE);
    b->slots[slot].dirty = false;
    return true;
}

void sky_base_mark_dirty(SkyBase* b, uint8_t slot) {
    if(slot < SKY_SLOT_COUNT) b->slots[slot].dirty = true;
}

static void build_status(SkyBase* b, uint8_t* r) {
    uint16_t st = 0;
    for(int i = SKY_SLOT_COUNT - 1; i >= 0; i--) {
        SkySlot* s = &b->slots[i];
        uint8_t v;
        if(status_pop(s, &v)) s->status = v;
        st = (uint16_t)(st << 2);
        st = (uint16_t)(st | (s->status & 3));
    }
    memset(r, 0, SKY_PACKET_SIZE);
    r[0] = 0x53;
    r[1] = (uint8_t)(st & 0xFF);
    r[2] = (uint8_t)(st >> 8);
    r[5] = b->interrupt_counter++;
    r[6] = 0x01;
}

void sky_base_handle_command(SkyBase* b, const uint8_t* buf, size_t len) {
    if(!b || !buf || len == 0) return;
    uint8_t r[SKY_PACKET_SIZE];
    memset(r, 0, sizeof(r));

    switch(buf[0]) {
    case 'A': /* activate: any length (hosts differ: 1, 2, 32 or 33 bytes) */
        r[0] = 0x41;
        r[1] = buf[1];
        r[2] = 0xFF;
        r[3] = 0x77;
        activate(b);
        reply_push(b, r);
        break;
    case 'C': /* ring colour: no reply */
        if(len >= 4) lights_ring(b, buf[1], buf[2], buf[3], 0);
        break;
    case 'J': /* Swap Force light fade: shown by the app, acked */
        if(len != 7) return;
        if(buf[1] == 0) {
            lights_ring(b, buf[2], buf[3], buf[4], (uint32_t)buf[5] | ((uint32_t)buf[6] << 8));
        }
        r[0] = buf[0];
        reply_push(b, r);
        break;
    case 'L': /* light: no reply */
        if(len >= 3) {
            if(buf[1] == 0 && len >= 5) {
                lights_ring(b, buf[2], buf[3], buf[4], 0);
            } else if(buf[1] == 1) {
                lights_backlight(b, buf[2]);
            } else if(buf[1] == 3) {
                lights_backlight(b, 0xFF);
            }
        }
        break;
    case 'S': /* status request: status flows via the interrupt stream */
    case 'V': /* no reply */
        break;
    case 'M':
        if(len >= 2) b->speaker_on = buf[1] != 0; /* audio itself arrives on the OUT endpoint */
        if(len != 2) return;
        r[0] = 0x4D;
        r[1] = buf[1];
        r[3] = 0x19;
        reply_push(b, r);
        break;
    case 'R': /* ready / firmware: any length */
        r[0] = 0x52;
        r[1] = 0x02;
        r[2] = b->fw_version;
        reply_push(b, r);
        b->speaker_on = false;
        deactivate(b); /* as the reference emulator: R resets, the following A re-announces */
        break;
    case 'Q': { /* read block */
        if(!size_is(len, 3, 32)) return;
        const uint8_t n = buf[1] & 0x0F;
        const uint8_t block = buf[2];
        if(n >= SKY_SLOT_COUNT || block >= SKY_BLOCK_COUNT) return;
        SkySlot* s = &b->slots[n];
        r[0] = 'Q';
        r[2] = block;
        if(s->status & 1) {
            r[1] = (uint8_t)(0x10 | n);
            memcpy(r + 3, s->data + SKY_BLOCK_SIZE * block, SKY_BLOCK_SIZE);
        } else {
            r[1] = n;
        }
        reply_push(b, r);
        break;
    }
    case 'W': { /* write block */
        if(!size_is(len, 19, 32)) return;
        const uint8_t n = buf[1] & 0x0F;
        const uint8_t block = buf[2];
        if(n >= SKY_SLOT_COUNT || block >= SKY_BLOCK_COUNT) return;
        SkySlot* s = &b->slots[n];
        r[0] = 'W';
        r[2] = block;
        if(s->status & 1) {
            r[1] = (uint8_t)(0x10 | n);
            uint8_t* dst = s->data + SKY_BLOCK_SIZE * block;
            if(memcmp(dst, buf + 3, SKY_BLOCK_SIZE) != 0) {
                memcpy(dst, buf + 3, SKY_BLOCK_SIZE);
                s->dirty = true;
                b->block_writes++;
            }
        } else {
            r[1] = n;
        }
        reply_push(b, r);
        break;
    }
    default:
        break;
    }
}

bool sky_base_next_packet(SkyBase* b, uint8_t out[SKY_PACKET_SIZE]) {
    if(b->r_count > 0) {
        memcpy(out, b->r_queue[b->r_head], SKY_PACKET_SIZE);
        b->r_head = (uint8_t)((b->r_head + 1) % SKY_REPLY_QUEUE_DEPTH);
        b->r_count--;
        return true;
    }
    build_status(b, out);
    return false;
}

void sky_base_slot_info(const SkyBase* b, uint8_t slot, SkySlotInfo* out) {
    memset(out, 0, sizeof(*out));
    if(slot >= SKY_SLOT_COUNT || !(b->slots[slot].status & 1)) return;
    const SkySlot* s = &b->slots[slot];
    out->present = true;
    out->dirty = s->dirty;
    out->serial = (uint32_t)s->data[0] | ((uint32_t)s->data[1] << 8) |
                  ((uint32_t)s->data[2] << 16) | ((uint32_t)s->data[3] << 24);
    out->type_id = (uint16_t)(s->data[SKY_BLOCK_SIZE] | (s->data[SKY_BLOCK_SIZE + 1] << 8));
}

void sky_base_clear_replies(SkyBase* b) {
    b->r_head = 0;
    b->r_count = 0;
}

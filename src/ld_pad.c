#include "ld_pad.h"

#include <string.h>

#include "log.h"
#include "plat.h"
#include "../vendor/ld/burtle.h"
#include "../vendor/ld/bytes.h"
#include "../vendor/ld/minifigures.h"
#include "../vendor/ld/tea.h"
#include "../vendor/ld/usb/frame.h"

#define CMD_WAKE   0xB0
#define CMD_READ   0xD2
#define CMD_MODEL  0xD4
#define CMD_SEED   0xB1
#define CMD_CHAL   0xB3
#define CMD_COL    0xC0
#define CMD_GETCOL 0xC1
#define CMD_FADE   0xC2
#define CMD_FLASH  0xC3
#define CMD_FADRD  0xC4
#define CMD_FADAL  0xC6
#define CMD_FLSAL  0xC7
#define CMD_COLAL  0xC8
#define CMD_TGLST  0xD0
#define CMD_WRITE  0xD3
#define CMD_PWD    0xE1
#define CMD_ACTIVE 0xE5
#define CMD_LEDSQ  0xFF

#define IN_QUEUE 40 /* events and responses, in order */

static LdToken tokens[LD_TOKENS];
static int box_index[LD_BOXES]; /* token index in each box, -1 = empty */
static uint8_t tea_key[16];
static Burtle burtle;
static uint8_t q[IN_QUEUE][HID_EP_SZ];
static uint8_t q_head, q_count;
static bool awake = false, wake_flag = false;
static uint8_t dirty_mask = 0;
static uint32_t commands = 0;
static uint8_t last_cmd = 0;
static uint32_t rng = 0;

static const uint8_t default_tea_key[16] = {0x55, 0xFE, 0xF6, 0xB0, 0x62, 0xBF, 0x0B, 0x41,
                                            0xC9, 0xB3, 0x7C, 0xB4, 0x97, 0x3E, 0x29, 0x7B};

void ld_pad_init(void) {
    memset(tokens, 0, sizeof(tokens));
    for(int i = 0; i < LD_BOXES; i++) box_index[i] = -1;
    memcpy(tea_key, default_tea_key, sizeof(tea_key));
    memset(&burtle, 0, sizeof(burtle));
    q_head = q_count = 0;
    awake = wake_flag = false;
    dirty_mask = 0;
    commands = 0;
    last_cmd = 0;
}

void ld_pad_forget_all(void) {
    memset(tokens, 0, sizeof(tokens));
    for(int i = 0; i < LD_BOXES; i++) box_index[i] = -1;
    dirty_mask = 0;
}

uint16_t ld_vehicle_id(const LdToken* t) {
    return (uint16_t)(t->token[0x24 * 4] | (t->token[0x24 * 4 + 1] << 8));
}

/* ---- tokens ---- */

static uint32_t uid_hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

static uint32_t next_rand(void) {
    if(rng == 0) rng = plat_ms() * 2654435761u + 0x9E3779B9u;
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int count_by_id(uint32_t id) {
    int n = 0;
    for(int i = 0; i < LD_TOKENS; i++) {
        if(tokens[i].used && tokens[i].id == id) n++;
    }
    return n;
}

static void create_uid(LdToken* t, uint32_t id) {
    t->uid[0] = 0x04; /* first uid byte always 0x04 */
    t->uid[6] = 0x80; /* last uid byte always 0x80 */
    if(ld_is_vehicle(t)) {
        for(int i = 1; i <= 5; i++) t->uid[i] = (uint8_t)(next_rand() & 0xFF);
        return;
    }
    /* Stable per figure id (and per copy), so the game sees the same toy every time. */
    const uint32_t h = uid_hash(id) ^ uid_hash((uint32_t)count_by_id(id)) ^ uid_hash(0x4F434950u);
    /* The Flipper code shifted by 32 for the last byte (undefined in C, 0 on its ARM core): keep 0. */
    for(int i = 1; i <= 5; i++) t->uid[i] = i <= 4 ? (uint8_t)((h >> ((i - 1) * 8)) & 0xFF) : 0;
}

void ld_make_minifig(LdToken* t, uint16_t id) {
    memset(t, 0, sizeof(*t));
    t->id = id;
    create_uid(t, id);
}

void ld_make_vehicle(LdToken* t, uint16_t id, const uint32_t upgrades[2]) {
    memset(t, 0, sizeof(*t)); /* id stays 0: that is what makes it a vehicle */
    create_uid(t, id);
    writeUInt32LE(&t->token[0x23 * 4], upgrades[0], 0);
    writeUInt16LE(&t->token[0x24 * 4], id, 0);
    writeUInt32LE(&t->token[0x25 * 4], upgrades[1], 0);
    writeUInt16BE(&t->token[0x26 * 4], 1, 0);
}

void ld_make_vehicle_from_saved(LdToken* t, const uint8_t uid[7], const uint8_t data[LD_TOKEN_BYTES]) {
    memset(t, 0, sizeof(*t));
    memcpy(t->uid, uid, 7);
    memcpy(t->token, data, LD_TOKEN_BYTES);
}

/* ---- IN queue ---- */

static void q_push(const uint8_t pkt[HID_EP_SZ]) {
    if(q_count == IN_QUEUE) {
        LOGE("ld: IN queue full, packet dropped");
        return;
    }
    memcpy(q[(q_head + q_count) % IN_QUEUE], pkt, HID_EP_SZ);
    q_count++;
}

bool ld_pad_in_peek(uint8_t out[32]) {
    if(!q_count) return false;
    memcpy(out, q[q_head], HID_EP_SZ);
    return true;
}

void ld_pad_in_drop(void) {
    if(q_count) {
        q_head = (uint8_t)((q_head + 1) % IN_QUEUE);
        q_count--;
    }
}

void ld_pad_in_clear(void) {
    q_head = q_count = 0;
}

uint8_t ld_pad_in_count(void) {
    return q_count;
}

static uint8_t checksum(const uint8_t* p, size_t len) {
    uint8_t r = 0;
    for(size_t i = 0; i < len; i++) r = (uint8_t)(r + p[i]);
    return r;
}

static void send_token_event(const LdToken* t, bool placed) {
    uint8_t b[HID_EP_SZ] = {0};
    b[0] = FRAME_TYPE_REQUEST;
    b[1] = 0x0b; /* size always 11 */
    b[2] = t->pad;
    b[3] = 0x00;
    b[4] = t->index;
    b[5] = placed ? 0x00 : 0x01; /* 0 = placed, 1 = removed */
    memcpy(&b[6], t->uid, 7);
    b[13] = checksum(b, 13);
    q_push(b);
}

/* ---- pad ---- */

static uint8_t pad_for_box(uint8_t box) {
    static const uint8_t box_to_pad[LD_BOXES] = {2, 1, 3, 2, 2, 3, 3};
    return box_to_pad[box < LD_BOXES ? box : 0];
}

bool ld_pad_box_filled(uint8_t box) {
    return box < LD_BOXES && box_index[box] >= 0;
}

LdToken* ld_pad_box_token(uint8_t box) {
    if(!ld_pad_box_filled(box)) return NULL;
    return &tokens[box_index[box]];
}

uint8_t ld_pad_token_count(void) {
    uint8_t n = 0;
    for(int i = 0; i < LD_TOKENS; i++) n += tokens[i].used;
    return n;
}

bool ld_pad_uid_present(const uint8_t uid[7], uint8_t* box_out) {
    for(int i = 0; i < LD_TOKENS; i++) {
        if(tokens[i].used && memcmp(tokens[i].uid, uid, 7) == 0) {
            if(box_out) *box_out = tokens[i].box;
            return true;
        }
    }
    return false;
}

bool ld_pad_place_now(const LdToken* proto, uint8_t box) {
    if(box >= LD_BOXES || box_index[box] >= 0) return false;
    int slot = -1;
    for(int i = 0; i < LD_TOKENS; i++) {
        if(!tokens[i].used) {
            slot = i;
            break;
        }
    }
    if(slot < 0) return false;
    tokens[slot] = *proto;
    tokens[slot].used = true;
    tokens[slot].index = (uint8_t)slot;
    tokens[slot].pad = pad_for_box(box);
    tokens[slot].box = box;
    box_index[box] = slot;
    send_token_event(&tokens[slot], true);
    return true;
}

bool ld_pad_remove_box_now(uint8_t box) {
    if(!ld_pad_box_filled(box)) return false;
    const int idx = box_index[box];
    send_token_event(&tokens[idx], false);
    memset(&tokens[idx], 0, sizeof(tokens[idx]));
    box_index[box] = -1;
    return true;
}

bool ld_pad_take_wake(void) {
    const bool w = wake_flag;
    wake_flag = false;
    return w;
}

bool ld_pad_awake(void) {
    return awake;
}

void ld_pad_set_awake(bool a) {
    awake = a;
}

uint8_t ld_pad_take_dirty_mask(void) {
    const uint8_t m = dirty_mask;
    dirty_mask = 0;
    return m;
}

uint32_t ld_pad_commands(void) {
    return commands;
}

uint8_t ld_pad_last_cmd(void) {
    return last_cmd;
}

LdToken* ld_pad_token_at(uint8_t index) {
    return index < LD_TOKENS && tokens[index].used ? &tokens[index] : NULL;
}

static LdToken* find_by_index(int index) {
    if(index < 0 || index >= LD_TOKENS) return NULL;
    return tokens[index].used ? &tokens[index] : NULL;
}

/* ---- USB command handling (from usb_toypad.c hid_out_callback) ---- */

void ld_pad_handle_out(const uint8_t* buf_in, uint16_t len) {
    if(len < 2) return;
    unsigned char req_buf[HID_EP_SZ] = {0};
    memcpy(req_buf, buf_in, len > HID_EP_SZ ? HID_EP_SZ : len);

    /* The Flipper code trusted these lengths; a bad one would overrun its buffers. */
    if(req_buf[1] < 2 || req_buf[1] > HID_EP_SZ - 2) return;

    Frame frame;
    parse_frame(&frame, req_buf, len);

    Request request;
    memset(&request, 0, sizeof(request));
    parse_request(&request, &frame);

    Response response;
    memset(&response, 0, sizeof(response));
    response.cid = request.cid;
    response.payload_len = 0;

    commands++;
    last_cmd = request.cmd;

    uint32_t conf;
    LdToken* token;
    int ind, page;

    switch(request.cmd) {
    case CMD_WAKE: {
        static const uint8_t wake_payload[13] = {0x28, 0x63, 0x29, 0x20, 0x4C, 0x45, 0x47,
                                                 0x4F, 0x20, 0x32, 0x30, 0x31, 0x34};
        memcpy(response.payload, wake_payload, sizeof(wake_payload));
        response.payload_len = sizeof(wake_payload);
        awake = true;
        wake_flag = true;
        break;
    }
    case CMD_READ:
        ind = request.payload[0];
        page = request.payload[1];
        response.payload_len = 17;
        response.payload[0] = 0;
        token = find_by_index(ind);
        if(token && page >= 0 && page * 4 < LD_TOKEN_BYTES) {
            int n = LD_TOKEN_BYTES - page * 4; /* near the end there are fewer than 16 bytes left */
            if(n > 16) n = 16;
            memcpy(response.payload + 1, token->token + page * 4, (size_t)n);
        }
        break;
    case CMD_MODEL: {
        tea_decrypt(request.payload, tea_key, request.payload);
        const int index = request.payload[0];
        conf = readUInt32BE(request.payload, 4);
        unsigned char b8[8] = {0};
        writeUInt32BE(b8, conf, 4);
        token = find_by_index(index);
        memset(response.payload, 0, sizeof(response.payload));
        if(token) {
            if(!ld_is_vehicle(token)) {
                response.payload[0] = 0x00;
                writeUInt32LE(b8, token->id, 0);
                tea_encrypt(b8, tea_key, response.payload + 1);
                response.payload_len = 9;
            } else {
                response.payload[0] = 0xF9;
                response.payload_len = 1;
            }
        } else {
            response.payload[0] = 0xF2;
            response.payload_len = 1;
        }
        break;
    }
    case CMD_SEED: {
        tea_decrypt(request.payload, tea_key, request.payload);
        const uint32_t seed = readUInt32LE(request.payload, 0);
        conf = readUInt32BE(request.payload, 4);
        burtle_init(&burtle, seed);
        memset(response.payload, 0, 8);
        writeUInt32BE(response.payload, conf, 0);
        tea_encrypt(response.payload, tea_key, response.payload);
        response.payload_len = 8;
        break;
    }
    case CMD_WRITE: {
        ind = request.payload[0];
        page = request.payload[1];
        const uint8_t* data = request.payload + 2;
        token = find_by_index(ind);
        if(token && page >= 0 && page < 45) { /* 45 pages * 4 = 180 bytes */
            memcpy(token->token + 4 * page, data, 4);
            if(ind >= 0 && ind < 8) dirty_mask |= (uint8_t)(1u << ind);
        }
        response.payload[0] = 0x00;
        response.payload_len = 1;
        break;
    }
    case CMD_CHAL: {
        tea_decrypt(request.payload, tea_key, request.payload);
        conf = readUInt32BE(request.payload, 0);
        memset(response.payload, 0, 8);
        const uint32_t r = burtle_rand(&burtle);
        writeUInt32LE(response.payload, r, 0);
        writeUInt32BE(response.payload + 4, conf, 0);
        tea_encrypt(response.payload, tea_key, response.payload);
        response.payload_len = 8;
        break;
    }
    case CMD_COL:
    case CMD_GETCOL:
    case CMD_FADE:
    case CMD_FLASH:
    case CMD_FADRD:
    case CMD_FADAL:
    case CMD_FLSAL:
    case CMD_COLAL:
    case CMD_TGLST:
    case CMD_PWD:
    case CMD_ACTIVE:
    case CMD_LEDSQ:
        /* Lights and the rest: the Flipper app answers these with an empty response too. */
        break;
    default:
        LOGD("ld: unknown command %02x", request.cmd);
        return; /* no response, like the Flipper app */
    }

    if(response.payload_len > HID_EP_SZ) return;

    unsigned char res_buf[HID_EP_SZ];
    memset(res_buf, 0, sizeof(res_buf));
    const int res_len = build_response(&response, res_buf);
    if(res_len <= 0) return;
    q_push(res_buf);
}

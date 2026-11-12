/*
 * LEGO Dimensions toy pad (VID 0E6F PID 0241).
 * USB commands: ld_pad.c (port of the Flipper's usb_toypad.c / ToyPadEmu.c).
 * Phone protocol: vendor/ld/ld_ble.c, unchanged; the "ops" below are the Pico's version of the
 * Flipper's ld_ble_app.c.
 *
 * Differences from the Flipper app, all because there is no screen here:
 *  - there is no "Emulate" screen to open: the pad is ready as soon as the console wakes it;
 *  - the Flipper blocked for 150 ms between pad changes; here a small queue does the same pacing;
 *  - the token layout (which minifigure/vehicle is on which of the 7 boxes) is saved to flash and
 *    put back on the pad automatically after every console wake-up.
 */
#include <string.h>

#include "ld_pad.h"
#include "log.h"
#include "mode.h"
#include "persist.h"
#include "plat.h"
#include "store.h"
#include "toybox_proto.h"
#include "../vendor/inf/ble_proto.h"
#include "../vendor/ld/ld_ble.h"
#include "../vendor/ld/minifigures.h"

#define EP_IN  0x81
#define EP_OUT 0x01

#define TOKEN_DELAY_MS 150     /* pause between pad changes, same as the Flipper's own screen */
#define STATUS_PUSH_MS 250
#define VEHICLE_SETTLE_MS 400  /* the game writes a vehicle a few bytes at a time: wait for quiet */
#define RESTORE_DELAY_MS 1500  /* after a console wake-up, before the saved tokens are put back */

static const uint8_t report_desc[29] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x19, 0x01, 0x29, 0x20, 0x15, 0x00, 0x26, 0xFF,
    0x00, 0x75, 0x08, 0x95, 0x20, 0x81, 0x00, 0x19, 0x01, 0x29, 0x20, 0x91, 0x00, 0xC0};

static const uint8_t dev_desc[18] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, /* USB 2.0, EP0 64 (the real pad uses 64) */
    0x6F, 0x0E,                                     /* VID 0x0E6F */
    0x41, 0x02,                                     /* PID 0x0241 */
    0x00, 0x01,                                     /* bcdDevice 1.00 */
    0x01, 0x02, 0x03,                               /* manufacturer, product, serial strings */
    0x01};

static const uint8_t cfg_desc[41] = {
    0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x00, 0x01, 0x00, 0x01, 0x22, 0x1D, 0x00, /* HID 1.00 */
    0x07, 0x05, EP_IN, 0x03, 0x20, 0x00, 0x01,
    0x07, 0x05, EP_OUT, 0x03, 0x20, 0x00, 0x01};

/* ---- the saved layout: what should be on the pad after a wake-up ---- */
typedef struct {
    bool valid;
    bool unsynced; /* a vehicle the game changed and the phone has not received */
    LdToken tok;
} Saved;

static Saved saved[LD_BOXES];
static bool lay_pending[LD_BOXES];
static uint32_t lay_due[LD_BOXES];

/* ---- timed pad changes ---- */
typedef enum { ActRemove, ActPlace } ActKind;
typedef struct {
    ActKind kind;
    uint8_t box;
    LdToken tok;
} Action;
#define ACT_MAX 24
static Action acts[ACT_MAX];
static uint8_t act_head, act_count;
static uint32_t next_due = 0;

static LdBle ldble;
static bool configured = false;
static uint32_t enum_configs = 0;
static uint32_t restore_at = 0; /* 0 = nothing to restore */
static uint32_t dirty_at[LD_BOXES];
static uint32_t last_push = 0;
static bool replay_pending = false;

static void mark_layout(uint8_t box, uint32_t delay_ms) {
    lay_pending[box] = true;
    lay_due[box] = plat_ms() + delay_ms;
}

static void write_layout(uint8_t box) {
    lay_pending[box] = false;
    const Saved* s = &saved[box];
    if(!s->valid) {
        store_erase_key(KEY_LD(box));
        LOGI("persist: box %u erased", box);
        return;
    }
    uint8_t rec[1 + 7 + LD_TOKEN_BYTES];
    size_t n;
    if(ld_is_vehicle(&s->tok)) {
        rec[0] = 2;
        memcpy(rec + 1, s->tok.uid, 7);
        memcpy(rec + 8, s->tok.token, LD_TOKEN_BYTES);
        n = sizeof(rec);
    } else {
        rec[0] = 1;
        rec[1] = (uint8_t)(s->tok.id & 0xFF);
        rec[2] = (uint8_t)(s->tok.id >> 8);
        n = 3;
    }
    const bool ok = store_save(KEY_LD(box), rec, (uint16_t)n, s->unsynced ? STORE_FLAG_UNSYNCED : 0);
    LOGI("persist: box %u %s", box, ok ? "saved" : "FAILED");
}

static void load_layout(void) {
    memset(saved, 0, sizeof(saved));
    uint8_t restored = 0;
    for(uint8_t b = 0; b < LD_BOXES; b++) {
        uint8_t rec[1 + 7 + LD_TOKEN_BYTES];
        uint16_t flags = 0;
        const int n = store_load(KEY_LD(b), rec, sizeof(rec), &flags);
        if(n == 3 && rec[0] == 1) {
            const uint16_t id = (uint16_t)(rec[1] | (rec[2] << 8));
            if(strcmp(get_minifigure_name(id), "?") != 0) {
                ld_make_minifig(&saved[b].tok, id);
                saved[b].valid = true;
                restored++;
            }
        } else if(n == (int)sizeof(rec) && rec[0] == 2) {
            ld_make_vehicle_from_saved(&saved[b].tok, rec + 1, rec + 8);
            saved[b].valid = true;
            saved[b].unsynced = (flags & STORE_FLAG_UNSYNCED) != 0;
            restored++;
        }
    }
    LOGI("lego: %u saved token(s) in the layout", restored);
}

/* ---- action queue ---- */
static Action* act_push(ActKind kind, uint8_t box) {
    if(act_count == ACT_MAX) return NULL;
    Action* a = &acts[(act_head + act_count) % ACT_MAX];
    memset(a, 0, sizeof(*a));
    a->kind = kind;
    a->box = box;
    act_count++;
    return a;
}

static void act_pop(void) {
    act_head = (uint8_t)((act_head + 1) % ACT_MAX);
    act_count--;
}

static void run_actions(uint32_t now) {
    if(!act_count || (int32_t)(now - next_due) < 0) return;
    Action* a = &acts[act_head];
    if(a->kind == ActRemove) {
        /* A vehicle the game changed must reach the phone before it leaves the pad. */
        const LdToken* gone = ld_pad_box_token(a->box);
        if(gone && ld_is_vehicle(gone) && (dirty_at[a->box] || saved[a->box].unsynced) &&
           tb_phone_connected()) {
            ld_ble_push_token(&ldble, a->box);
        }
        dirty_at[a->box] = 0;
        if(ld_pad_remove_box_now(a->box)) {
            LOGI("lego: box %u cleared", a->box);
            next_due = now + TOKEN_DELAY_MS;
        }
        saved[a->box].valid = false;
        saved[a->box].unsynced = false;
        mark_layout(a->box, PERSIST_DELAY_MS);
        act_pop();
        return;
    }
    /* Place. Same UID already on the pad (another box)? Take that one off first, wait, retry. */
    uint8_t other;
    if(ld_pad_uid_present(a->tok.uid, &other)) {
        ld_pad_remove_box_now(other);
        saved[other].valid = false;
        mark_layout(other, PERSIST_DELAY_MS);
        next_due = now + TOKEN_DELAY_MS;
        return;
    }
    if(ld_pad_box_filled(a->box)) { /* should not happen (removal is queued first) */
        ld_pad_remove_box_now(a->box);
        next_due = now + TOKEN_DELAY_MS;
        return;
    }
    if(ld_pad_place_now(&a->tok, a->box)) {
        LOGI(
            "lego: box %u <- %s %u",
            a->box,
            ld_is_vehicle(&a->tok) ? "vehicle" : "minifig",
            ld_is_vehicle(&a->tok) ? ld_vehicle_id(&a->tok) : (unsigned)a->tok.id);
        saved[a->box].valid = true;
        saved[a->box].unsynced = false;
        saved[a->box].tok = a->tok;
        mark_layout(a->box, PERSIST_DELAY_MS);
        dirty_at[a->box] = 0;
        next_due = now + TOKEN_DELAY_MS;
    } else {
        LOGE("lego: pad is full, box %u not placed", a->box);
    }
    act_pop();
    usbm_kick();
}

/* ---- ops used by the shared phone protocol (vendor/ld/ld_ble.c) ---- */
static bool ready(void) {
    return configured && ld_pad_awake();
}

static uint8_t ops_usb_state(void* ctx) {
    (void)ctx;
    return ready() ? LdUsbReady : LdUsbNone;
}

static uint8_t ops_box_info(void* ctx, uint8_t box, uint16_t* id) {
    (void)ctx;
    *id = 0;
    const LdToken* t = ld_pad_box_token(box);
    if(!t) return LdKindEmpty;
    if(ld_is_vehicle(t)) {
        *id = ld_vehicle_id(t);
        return LdKindVehicle;
    }
    *id = (uint16_t)t->id;
    return LdKindMinifig;
}

static bool box_busy(uint8_t box) {
    if(ld_pad_box_filled(box)) return true;
    for(uint8_t i = 0; i < act_count; i++) {
        if(acts[(act_head + i) % ACT_MAX].box == box) return true;
    }
    return false;
}

/* Queue "take the old one off, then put this one on". */
static uint8_t queue_place(uint8_t box, const LdToken* tok) {
    if(act_count + 2 > ACT_MAX) return BleResultUnknown;
    if(box_busy(box)) act_push(ActRemove, box);
    Action* a = act_push(ActPlace, box);
    a->tok = *tok;
    return BleResultOk;
}

static uint8_t ops_place(void* ctx, uint8_t box, uint8_t kind, uint16_t id, const uint32_t up[2]) {
    (void)ctx;
    if(!ready()) return BleResultNotReady;
    if(box >= LD_BOXES) return BleResultBadSlot;
    const bool vehicle = kind == LdKindVehicle;
    const char* name = vehicle ? get_vehicle_name(id) : get_minifigure_name(id);
    if(strcmp(name, "?") == 0) return BleResultNotPresent; /* unknown id */
    static LdToken tok;
    if(vehicle) ld_make_vehicle(&tok, id, up);
    else ld_make_minifig(&tok, id);
    return queue_place(box, &tok);
}

static uint8_t ops_place_vehicle(void* ctx, uint8_t box, const uint8_t uid[7], const uint8_t data[180]) {
    (void)ctx;
    if(!ready()) return BleResultNotReady;
    if(box >= LD_BOXES) return BleResultBadSlot;
    static LdToken tok;
    ld_make_vehicle_from_saved(&tok, uid, data);
    if(strcmp(get_vehicle_name(ld_vehicle_id(&tok)), "?") == 0) return BleResultNotPresent;
    return queue_place(box, &tok);
}

static uint8_t ops_remove(void* ctx, uint8_t box) {
    (void)ctx;
    if(!ready()) return BleResultNotReady;
    if(!box_busy(box)) return BleResultNotPresent;
    if(act_count + 1 > ACT_MAX) return BleResultUnknown;
    act_push(ActRemove, box);
    return BleResultOk;
}

static void ops_clear(void* ctx) {
    (void)ctx;
    /* Drop anything still queued, then take every token off, one at a time. */
    act_head = act_count = 0;
    for(uint8_t b = 0; b < LD_BOXES; b++) {
        if(ld_pad_box_filled(b)) act_push(ActRemove, b);
        else if(!saved[b].unsynced) { /* not on the pad (console asleep): forget the saved one */
            saved[b].valid = false;
            mark_layout(b, PERSIST_DELAY_MS);
        }
    }
}

static bool ops_get_token(void* ctx, uint8_t box, uint8_t uid[7], uint8_t data[180]) {
    (void)ctx;
    const LdToken* t = ld_pad_box_token(box);
    if(!t || !ld_is_vehicle(t)) return false;
    memcpy(uid, t->uid, 7);
    memcpy(data, t->token, LD_TOKEN_BYTES);
    return true;
}

static bool ld_send_fn(void* ctx, uint8_t type, const uint8_t* payload, uint16_t len) {
    (void)ctx;
    return tb_send(type, payload, len);
}

/* ---- mode hooks ---- */
static void activate(uint8_t game) {
    (void)game;
    static const LdBleOps ops = {
        .usb_state = ops_usb_state,
        .place = ops_place,
        .remove = ops_remove,
        .clear = ops_clear,
        .box_info = ops_box_info,
        .get_token = ops_get_token,
        .place_vehicle = ops_place_vehicle,
        /* save_box / saved_list / saved_read: no SD card here; the web page keeps its own copies */
    };
    ld_pad_init();
    ld_ble_init(&ldble, &ops, NULL, ld_send_fn, NULL);
    act_head = act_count = 0;
    next_due = 0;
    configured = false;
    enum_configs = 0;
    restore_at = 0;
    memset(dirty_at, 0, sizeof(dirty_at));
    memset(lay_pending, 0, sizeof(lay_pending));
    replay_pending = false;
    load_layout();
}

static void deactivate(void) {
    for(uint8_t b = 0; b < LD_BOXES; b++) {
        if(lay_pending[b]) write_layout(b);
    }
}

static uint8_t usb_state(void) {
    if(!configured) return 0;
    return ld_pad_awake() ? 3 : 2;
}

static void ble_msg(uint8_t type, const uint8_t* p, uint16_t len) {
    if(type == BleMsgHello) replay_pending = true;
    ld_ble_on_message(&ldble, type, p, len);
}

static void ble_connected(void) {
    ldble.status_known = false;
}

static void replay_unsynced(void) {
    for(uint8_t b = 0; b < LD_BOXES; b++) {
        if(!saved[b].valid || !saved[b].unsynced || !ld_is_vehicle(&saved[b].tok)) continue;
        uint8_t frame[BLE_LD_TOKEN_FRAME];
        frame[0] = b;
        memcpy(frame + 1, saved[b].tok.uid, 7);
        memcpy(frame + 8, saved[b].tok.token, LD_TOKEN_BYTES);
        if(!tb_send(BleMsgLdToken, frame, sizeof(frame))) return;
        saved[b].unsynced = false;
        mark_layout(b, PERSIST_DELAY_MS);
        LOGI("lego: sent saved vehicle data of box %u to the phone", b);
    }
    replay_pending = false;
}

static void queue_restore(void) {
    uint8_t n = 0;
    for(uint8_t b = 0; b < LD_BOXES; b++) {
        if(!saved[b].valid || box_busy(b)) continue;
        Action* a = act_push(ActPlace, b);
        if(!a) break;
        a->tok = saved[b].tok;
        n++;
    }
    LOGI("lego: console is ready, putting %u saved token(s) back on the pad", n);
}

static void tick(uint32_t now) {
    /* The console woke the pad: it knows nothing about tokens any more. */
    if(ld_pad_take_wake()) {
        ld_pad_forget_all();
        act_head = act_count = 0;
        memset(dirty_at, 0, sizeof(dirty_at));
        restore_at = now + RESTORE_DELAY_MS;
        LOGI("lego: console woke the pad (#%lu commands so far)", (unsigned long)ld_pad_commands());
    }
    if(restore_at && (int32_t)(now - restore_at) >= 0) {
        restore_at = 0;
        queue_restore();
    }

    run_actions(now);

    /* The game wrote to a vehicle (upgrades, levels): keep it, and send it to the phone once it
     * goes quiet. */
    uint8_t mask = ld_pad_take_dirty_mask();
    for(uint8_t idx = 0; idx < LD_TOKENS && mask; idx++) {
        if(!(mask & (1u << idx))) continue;
        mask &= (uint8_t)~(1u << idx);
        const LdToken* t = ld_pad_token_at(idx);
        if(t && ld_is_vehicle(t)) dirty_at[t->box] = now ? now : 1;
    }
    for(uint8_t b = 0; b < LD_BOXES; b++) {
        if(!dirty_at[b] || now - dirty_at[b] < VEHICLE_SETTLE_MS) continue;
        const LdToken* t = ld_pad_box_token(b);
        if(!t) {
            dirty_at[b] = 0;
            continue;
        }
        saved[b].tok = *t; /* the layout keeps the vehicle's new upgrades */
        saved[b].valid = true;
        saved[b].unsynced = true;
        if(tb_phone_connected() && ld_ble_push_token(&ldble, b)) saved[b].unsynced = false;
        mark_layout(b, PERSIST_DELAY_MS);
        dirty_at[b] = 0;
        LOGI("lego: vehicle in box %u changed by the game", b);
    }

    if(replay_pending && tb_phone_connected()) replay_unsynced();

    if(tb_phone_connected() && now - last_push >= STATUS_PUSH_MS) {
        last_push = now;
        ld_ble_push_status_if_changed(&ldble);
    }

    /* One flash write per pass at most (each can stall for tens of ms). */
    for(uint8_t b = 0; b < LD_BOXES; b++) {
        if(lay_pending[b] && (int32_t)(now - lay_due[b]) >= 0) {
            write_layout(b);
            break;
        }
    }
}

static void usb_configured(void) {
    configured = true;
    enum_configs++;
    ld_pad_set_awake(false);
    ld_pad_forget_all();
    ld_pad_in_clear();
    act_head = act_count = 0;
    restore_at = 0;
    LOGI("lego: USB configured (#%lu), waiting for the console to wake the pad", (unsigned long)enum_configs);
}

static void usb_unconfigured(void) {
    configured = false;
    ld_pad_set_awake(false);
}

static void usb_suspend(bool s) {
    LOGI("lego: USB %s", s ? "suspended" : "resumed");
    if(s) { /* console went to sleep / cable pulled: same cleanup the Flipper app does */
        ld_pad_set_awake(false);
        ld_pad_forget_all();
        act_head = act_count = 0;
    }
}

static void usb_out(const uint8_t* buf, uint16_t len) {
    ld_pad_handle_out(buf, len);
    LOGD("ld OUT cmd %02x", ld_pad_last_cmd());
}

static bool usb_peek_in(uint8_t out[32]) {
    return ld_pad_in_peek(out);
}

static void usb_drop_in(void) {
    ld_pad_in_drop();
}

const tb_mode_def_t mode_ld = {
    .id = TbModeLego,
    .name = "LEGO Dimensions",
    .dev_desc = dev_desc,
    .cfg_desc = cfg_desc,
    .cfg_len = sizeof(cfg_desc),
    .hid_desc_off = 18,
    .report_desc = report_desc,
    .report_len = sizeof(report_desc),
    .ep_in = EP_IN,
    .ep_out = EP_OUT,
    .manuf = "Raspberry Pi",
    .product = "Pico ToyBox",
    .serial = NULL, /* NULL: usb_modes.c uses the board's unique id */
    .activate = activate,
    .deactivate = deactivate,
    .tick = tick,
    .usb_configured = usb_configured,
    .usb_unconfigured = usb_unconfigured,
    .usb_suspend = usb_suspend,
    .usb_out = usb_out,
    .usb_peek_in = usb_peek_in,
    .usb_drop_in = usb_drop_in,
    .usb_set_report = NULL,
    .usb_get_report = NULL,
    .ble_msg = ble_msg,
    .ble_connected = ble_connected,
    .usb_state = usb_state,
};

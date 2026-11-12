/*
 * Skylanders Portal of Power (VID 1430 PID 0150).
 * Descriptor values and behaviour follow the Flipper app's sky/usb_skylander.c; the portal
 * itself is vendor/sky/core/sky_core.c and the phone protocol is vendor/sky/sky_ble.c, unchanged.
 *
 * Commands arrive as HID SET_REPORT on the control endpoint, NOT on the OUT endpoint (the OUT
 * endpoint only carries Trap Team speaker audio, which is dropped here).
 */
#include <string.h>

#include "log.h"
#include "mode.h"
#include "persist.h"
#include "plat.h"
#include "store.h"
#include "toybox_proto.h"
#include "../vendor/inf/ble_proto.h"
#include "../vendor/sky/core/sky_core.h"
#include "../vendor/sky/sky_ble.h"

#define EP_IN  0x81
#define EP_OUT 0x02

/* How often the host polls the IN endpoint. 1 ms buried command replies between status
 * packets on the Flipper; RPCS3 paces at ~22 ms. If a game still misses replies, go slower. */
#ifndef SKY_IN_INTERVAL_MS
#define SKY_IN_INTERVAL_MS 22
#endif

static const uint8_t dev_desc[18] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, /* USB 2.0, class in interface, EP0 64 */
    0x30, 0x14,                                     /* VID 0x1430 */
    0x50, 0x01,                                     /* PID 0x0150 */
    0x00, 0x01,                                     /* bcdDevice 1.00 */
    0x00, 0x00, 0x00,                               /* no strings */
    0x01};

static const uint8_t cfg_desc[41] = {
    0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x1D, 0x00,
    /* EP IN 0x81 interrupt 32 B, SKY_IN_INTERVAL_MS */
    0x07, 0x05, EP_IN, 0x03, 0x20, 0x00, SKY_IN_INTERVAL_MS,
    /* EP OUT 0x02 interrupt 32 B, 1 ms */
    0x07, 0x05, EP_OUT, 0x03, 0x20, 0x00, 0x01};

static const uint8_t report_desc[29] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x19, 0x01, 0x29, 0x40, 0x15, 0x00, 0x26, 0xFF,
    0x00, 0x75, 0x08, 0x95, 0x20, 0x81, 0x00, 0x19, 0x01, 0x29, 0x40, 0x91, 0x00, 0xC0};

static SkyBase base;
static SkyBle ble;
static fig_persist_t fp;
static uint8_t game_id = 0;

static bool configured = false;
static bool polling = false;
static bool suspended = false;
static bool cmd_seen = false;
static uint32_t last_in_ms = 0, last_cmd_ms = 0;
static uint32_t cmd_count = 0, session_cmds = 0;
static uint32_t enum_configs = 0;
static uint8_t last_s1 = 0xFF, last_s2 = 0xFF;

static bool staged_valid = false;
static uint8_t staged[SKY_PACKET_SIZE];
static bool replay_pending = false;

static const uint8_t* fp_data(uint8_t slot) {
    return base.slots[slot].data;
}

static bool fp_present(uint8_t slot) {
    return sky_base_slot_present(&base, slot);
}

/* Every frame the shared Skylanders handler sends goes through here so we can see which SAVEs
 * really left (a SAVE that was queued means the phone has that progress). */
static bool sky_send(void* ctx, uint8_t type, const uint8_t* payload, uint16_t len) {
    (void)ctx;
    const bool ok = tb_send(type, payload, len);
    if(ok && type == BleMsgSave && len >= 1 && payload[0] < SKY_SLOT_COUNT) {
        fp.unsynced[payload[0]] = false;
    }
    return ok;
}

static void unsent_cb(void* ctx, uint8_t slot, const uint8_t data[SKY_FIGURE_SIZE]) {
    (void)ctx;
    (void)data;
    /* The phone could not take it: it stays flagged and is replayed on the next HELLO. */
    fp.unsynced[slot] = true;
    fp_mark(&fp, slot, plat_ms(), PERSIST_DELAY_MS);
}

static void activate(uint8_t game) {
    game_id = game;
    sky_base_init(&base);
    if(game == 5) base.fw_version = 0x18; /* Spyro's Adventure reports an older portal */
    sky_ble_init(&ble, &base, sky_send, NULL);
    ble.game = game;
    fp_init(&fp, KEY_SKY(0), SKY_SLOT_COUNT, SKY_FIGURE_SIZE, fp_data, fp_present);
    configured = polling = suspended = cmd_seen = false;
    staged_valid = false;
    enum_configs = cmd_count = 0;
    last_s1 = last_s2 = 0xFF;

    static uint8_t buf[SKY_FIGURE_SIZE];
    uint8_t restored = 0;
    for(uint8_t s = 0; s < SKY_SLOT_COUNT; s++) {
        bool unsynced = false;
        if(fp_load(&fp, s, buf, &unsynced)) {
            sky_base_load_figure_at(&base, s, buf);
            fp.unsynced[s] = unsynced;
            restored++;
        }
    }
    LOGI("skylanders: ready (game %u), %u saved figure(s) restored", game, restored);
}

static void deactivate(void) {
    fp_flush(&fp);
}

static uint8_t usb_state(void) {
    const uint32_t now = plat_ms();
    if(!configured || suspended) return 0;
    if(!polling) return 1;
    if(now - last_in_ms > 600) return 0; /* host stopped polling: cable pulled or asleep */
    if(cmd_seen && now - last_cmd_ms < 3000) return 3;
    return 2;
}

static void ble_msg(uint8_t type, const uint8_t* p, uint16_t len) {
    if(type == BleMsgHello) replay_pending = true;
    ble.usb_state = usb_state();
    sky_ble_on_message(&ble, type, p, len);

    switch(type) {
    case BleMsgLoad:
        if(len == 1 + BLE_PROTO_SKY_FIGURE && p[0] < SKY_SLOT_COUNT && ble.remote[p[0]]) {
            fp.unsynced[p[0]] = false; /* the phone's copy is the newest now */
            fp_mark(&fp, p[0], plat_ms(), 500);
            LOGI("skylanders: slot %u loaded from phone", p[0]);
            usbm_kick();
        }
        break;
    case BleMsgRemove:
        if(len == 1 && p[0] < SKY_SLOT_COUNT) {
            fp_forget(&fp, p[0]);
            LOGI("skylanders: slot %u removed", p[0]);
            usbm_kick();
        }
        break;
    case BleMsgClear:
        for(uint8_t s = 0; s < SKY_SLOT_COUNT; s++) fp_forget(&fp, s);
        LOGI("skylanders: portal cleared");
        usbm_kick();
        break;
    default:
        break;
    }
}

static void replay_unsynced(void) {
    static uint8_t frame[1 + SKY_FIGURE_SIZE];
    for(uint8_t s = 0; s < SKY_SLOT_COUNT; s++) {
        if(!fp.unsynced[s]) continue; /* data stays in the core even after a CLEAR */
        frame[0] = s;
        memcpy(frame + 1, base.slots[s].data, SKY_FIGURE_SIZE);
        if(!tb_send(BleMsgSave, frame, sizeof(frame))) return; /* queue full: next tick */
        fp.unsynced[s] = false;
        fp_mark(&fp, s, plat_ms(), PERSIST_DELAY_MS);
        LOGI("skylanders: sent saved progress of slot %u to the phone", s);
    }
    replay_pending = false;
}

static void tick(uint32_t now) {
    /* Game changed a figure: persist it, and let the shared handler send SAVE for figures the
     * phone loaded. Figures that came from flash are only flagged as unsynced. */
    bool any_dirty = false;
    for(uint8_t s = 0; s < SKY_SLOT_COUNT; s++) {
        if(!base.slots[s].dirty) continue;
        any_dirty = true;
        fp.unsynced[s] = true;
        fp_mark(&fp, s, now, PERSIST_DELAY_MS);
        if(!ble.remote[s] || !tb_phone_connected()) {
            static uint8_t scratch_local[SKY_FIGURE_SIZE];
            sky_base_take_dirty(&base, s, scratch_local); /* the flash copy is what we keep */
        }
    }
    if(any_dirty && tb_phone_connected()) {
        static uint8_t scratch[SKY_FIGURE_SIZE];
        ble.usb_state = usb_state();
        sky_ble_poll_saves(&ble, scratch, unsent_cb, NULL);
    }
    if(replay_pending && tb_phone_connected()) replay_unsynced();
    fp_tick(&fp, now);
}

static void usb_configured(void) {
    configured = true;
    suspended = false;
    polling = false;
    cmd_seen = false;
    staged_valid = false;
    session_cmds = 0;
    enum_configs++;
    sky_base_clear_replies(&base); /* drop stale replies from a previous session */
    LOGI("skylanders: USB configured (#%lu)", (unsigned long)enum_configs);
}

static void usb_unconfigured(void) {
    configured = false;
    polling = false;
}

static void usb_suspend(bool s) {
    suspended = s;
    if(s) polling = false;
    LOGI("skylanders: USB %s", s ? "suspended" : "resumed");
}

static void usb_out(const uint8_t* buf, uint16_t len) {
    /* Speaker audio (Trap Team): dropped. */
    (void)buf;
    (void)len;
}

static bool usb_peek_in(uint8_t out[32]) {
    if(!staged_valid) {
        const bool reply = sky_base_next_packet(&base, staged);
        staged_valid = true;
        if(reply) {
            log_hex(2, "sky reply", staged, 12);
        } else if(staged[1] != last_s1 || staged[2] != last_s2) {
            last_s1 = staged[1];
            last_s2 = staged[2];
            log_hex(2, "sky status", staged, 8);
        }
    }
    memcpy(out, staged, SKY_PACKET_SIZE);
    return true;
}

static void usb_drop_in(void) {
    staged_valid = false;
    polling = true;
    last_in_ms = plat_ms();
}

static bool is_cmd_letter(uint8_t c) {
    return c == 'A' || c == 'C' || c == 'J' || c == 'L' || c == 'M' || c == 'Q' || c == 'R' ||
           c == 'S' || c == 'V' || c == 'W' || c == 'Z';
}

static bool usb_set_report(const uint8_t* buf, uint16_t len) {
    last_cmd_ms = plat_ms();
    cmd_seen = true;
    cmd_count++;
    session_cmds++;
    const uint8_t* d = buf;
    size_t n = len;
    /* Some hosts prefix the payload with a HID report ID of 0: skip it. */
    if(n > 1 && d[0] == 0x00 && is_cmd_letter(d[1])) {
        d++;
        n--;
    }
    if(session_cmds <= 12) {
        LOGI("sky cmd '%c' len %u", (d[0] >= 0x20 && d[0] < 0x7F) ? d[0] : '?', len);
    } else {
        LOGD("sky cmd '%c' len %u", (d[0] >= 0x20 && d[0] < 0x7F) ? d[0] : '?', len);
    }
    if(n > 0) sky_base_handle_command(&base, d, n);
    return true;
}

static uint16_t usb_get_report(uint8_t* buf, uint16_t max) {
    uint8_t pkt[SKY_PACKET_SIZE];
    sky_base_next_packet(&base, pkt);
    const uint16_t n = max < SKY_PACKET_SIZE ? max : SKY_PACKET_SIZE;
    memcpy(buf, pkt, n);
    return n;
}

static void ble_disconnected(void) {
    replay_pending = false;
}

const tb_mode_def_t mode_sky = {
    .id = TbModeSkylanders,
    .name = "Skylanders",
    .dev_desc = dev_desc,
    .cfg_desc = cfg_desc,
    .cfg_len = sizeof(cfg_desc),
    .hid_desc_off = 18,
    .report_desc = report_desc,
    .report_len = sizeof(report_desc),
    .ep_in = EP_IN,
    .ep_out = EP_OUT,
    .serial = NULL,
    .activate = activate,
    .deactivate = deactivate,
    .tick = tick,
    .usb_configured = usb_configured,
    .usb_unconfigured = usb_unconfigured,
    .usb_suspend = usb_suspend,
    .usb_out = usb_out,
    .usb_peek_in = usb_peek_in,
    .usb_drop_in = usb_drop_in,
    .usb_set_report = usb_set_report,
    .usb_get_report = usb_get_report,
    .ble_msg = ble_msg,
    .ble_disconnected = ble_disconnected,
    .usb_state = usb_state,
};

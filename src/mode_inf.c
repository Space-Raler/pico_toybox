/*
 * Disney Infinity base (VID 0E6F PID 0129).
 * Descriptor values and behaviour follow the Flipper app's inf/usb_infinity.c and
 * inf/flipbase_app.c; the protocol itself is vendor/inf/infinity_core.c, unchanged.
 */
#include <string.h>

#include "log.h"
#include "mode.h"
#include "persist.h"
#include "plat.h"
#include "store.h"
#include "toybox_proto.h"
#include "../vendor/inf/ble_proto.h"
#include "../vendor/inf/infinity_core.h"

#define EP_IN  0x81
#define EP_OUT 0x01

/* Vendor-defined, 32 bytes in and out (29 bytes, 0x1D, as the real base reports). */
static const uint8_t report_desc[29] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x19, 0x01, 0x29, 0x20, 0x15, 0x00, 0x26, 0xFF,
    0x00, 0x75, 0x08, 0x95, 0x20, 0x81, 0x00, 0x19, 0x01, 0x29, 0x20, 0x91, 0x00, 0xC0};

static const uint8_t dev_desc[18] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, /* USB 2.0, class per interface, EP0 64 */
    0x6F, 0x0E,                                     /* VID 0x0E6F */
    0x29, 0x01,                                     /* PID 0x0129 */
    0x00, 0x02,                                     /* bcdDevice 2.00 */
    0x00, 0x00, 0x00,                               /* no strings: asking for one would stall */
    0x01};

static const uint8_t cfg_desc[41] = {
    /* configuration: 1 interface, bus powered, 500 mA */
    0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA,
    /* interface 0: HID, 2 endpoints, non-boot */
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    /* HID 1.11, one report descriptor of 29 bytes */
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x1D, 0x00,
    /* EP IN 0x81, interrupt, 32 bytes, 1 ms */
    0x07, 0x05, EP_IN, 0x03, 0x20, 0x00, 0x01,
    /* EP OUT 0x01, interrupt, 32 bytes, 1 ms */
    0x07, 0x05, EP_OUT, 0x03, 0x20, 0x00, 0x01};

static InfBase base;
static fig_persist_t fp;
static bool slot_remote[INF_SLOT_COUNT]; /* the phone loaded this figure this session */
static bool replay_pending = false;
static bool usb_configured_flag = false;
static bool usb_suspended_flag = false;
static uint32_t enum_configs = 0;
static uint32_t last_rx_ms = 0;
static uint32_t out_packets = 0, in_packets = 0;

#define LINK_SILENT_MS 10000

static const uint8_t* fp_data(uint8_t slot) {
    return base.figures[slot].data;
}

static bool fp_present(uint8_t slot) {
    return inf_base_slot_present(&base, slot);
}

static void activate(uint8_t game) {
    (void)game;
    inf_base_init(&base);
    memset(slot_remote, 0, sizeof(slot_remote));
    fp_init(&fp, KEY_INF(0), INF_SLOT_COUNT, INF_FIGURE_SIZE, fp_data, fp_present);
    usb_configured_flag = false;
    usb_suspended_flag = false;
    enum_configs = 0;
    out_packets = in_packets = 0;
    uint8_t buf[INF_FIGURE_SIZE];
    uint8_t restored = 0;
    for(uint8_t s = 0; s < INF_SLOT_COUNT; s++) {
        bool unsynced = false;
        if(fp_load(&fp, s, buf, &unsynced)) {
            inf_base_load_figure(&base, s, buf);
            fp.unsynced[s] = unsynced;
            restored++;
        }
    }
    LOGI("infinity: ready, %u saved figure(s) restored", restored);
}

static void deactivate(void) {
    fp_flush(&fp);
}

static uint8_t usb_state(void) {
    /* Same ladder as the Flipper app's link_state(): waiting, starting, connected, silent, gone. */
    if(enum_configs == 0) return 0;
    if(usb_suspended_flag) return 0;
    if(!base.handshake_seen) return 1;
    if(plat_ms() - last_rx_ms > LINK_SILENT_MS) return 1;
    return 3;
}

static void send_result(uint8_t request, uint8_t slot, uint8_t code) {
    const uint8_t p[3] = {request, slot, code};
    tb_send(BleMsgResult, p, sizeof(p));
}

static void send_status(void) {
    uint16_t mask = 0;
    for(uint8_t s = 0; s < INF_SLOT_COUNT; s++) {
        if(inf_base_slot_present(&base, s)) mask |= (uint16_t)(1u << s);
    }
    const uint8_t p[4] = {BLE_PROTO_VERSION, usb_state(), (uint8_t)(mask & 0xFF), (uint8_t)(mask >> 8)};
    tb_send(BleMsgStatus, p, sizeof(p));
}

/* Send SAVE for any figure whose game progress the phone has not received yet. */
static void replay_unsynced(void) {
    for(uint8_t s = 0; s < INF_SLOT_COUNT; s++) {
        if(!fp.unsynced[s]) continue; /* data stays in the core even after a CLEAR */
        uint8_t frame[1 + INF_FIGURE_SIZE];
        frame[0] = s;
        memcpy(frame + 1, base.figures[s].data, INF_FIGURE_SIZE);
        if(!tb_send(BleMsgSave, frame, sizeof(frame))) return; /* queue full: try again next tick */
        fp.unsynced[s] = false;
        fp_mark(&fp, s, plat_ms(), PERSIST_DELAY_MS);
        LOGI("infinity: sent saved progress of slot %u to the phone", s);
    }
    replay_pending = false;
}

static void ble_msg(uint8_t type, const uint8_t* p, uint16_t len) {
    switch(type) {
    case BleMsgHello:
        replay_pending = true;
        /* fall through */
    case BleMsgStatusReq:
        send_status();
        break;
    case BleMsgLoad: {
        if(len != 1 + BLE_PROTO_FIGURE) {
            send_result(type, len ? p[0] : 0, BleResultBadLength);
        } else if(p[0] >= INF_SLOT_COUNT) {
            send_result(type, p[0], BleResultBadSlot);
        } else {
            const uint8_t slot = p[0];
            if(inf_base_slot_present(&base, slot)) inf_base_remove_figure(&base, slot);
            inf_base_load_figure(&base, slot, p + 1);
            slot_remote[slot] = true;
            fp.unsynced[slot] = false; /* the phone's copy is the newest now */
            fp_mark(&fp, slot, plat_ms(), 500);
            LOGI("infinity: slot %u loaded from phone", slot);
            usbm_kick();
            send_result(type, slot, BleResultOk);
            send_status();
        }
        break;
    }
    case BleMsgRemove:
        if(len < 1 || p[0] >= INF_SLOT_COUNT) {
            send_result(type, len ? p[0] : 0, BleResultBadSlot);
        } else if(!inf_base_slot_present(&base, p[0])) {
            send_result(type, p[0], BleResultNotPresent);
        } else {
            inf_base_remove_figure(&base, p[0]);
            slot_remote[p[0]] = false;
            fp_forget(&fp, p[0]);
            LOGI("infinity: slot %u removed", p[0]);
            usbm_kick();
            send_result(type, p[0], BleResultOk);
            send_status();
        }
        break;
    case BleMsgClear:
        for(uint8_t s = 0; s < INF_SLOT_COUNT; s++) {
            if(inf_base_slot_present(&base, s)) {
                inf_base_remove_figure(&base, s);
                slot_remote[s] = false;
                fp_forget(&fp, s);
            }
        }
        LOGI("infinity: base cleared");
        usbm_kick();
        send_result(type, 0, BleResultOk);
        send_status();
        break;
    default:
        send_result(type, 0, BleResultUnknown);
        break;
    }
}

static void tick(uint32_t now) {
    /* Figures the game changed (progress, unlocks). */
    for(uint8_t s = 0; s < INF_SLOT_COUNT; s++) {
        uint8_t copy[INF_FIGURE_SIZE];
        if(!inf_base_take_dirty(&base, s, copy)) continue;
        LOGI("infinity: game changed slot %u", s);
        fp.unsynced[s] = true;
        fp_mark(&fp, s, now, PERSIST_DELAY_MS);
        if(slot_remote[s] && tb_phone_connected()) {
            uint8_t frame[1 + INF_FIGURE_SIZE];
            frame[0] = s;
            memcpy(frame + 1, copy, INF_FIGURE_SIZE);
            if(tb_send(BleMsgSave, frame, sizeof(frame))) {
                fp.unsynced[s] = false;
            } else {
                /* queue full: leave it flagged, the replay on the next HELLO sends it */
                LOGE("infinity: SAVE for slot %u could not be queued", s);
            }
        }
    }
    if(replay_pending && tb_phone_connected()) replay_unsynced();
    fp_tick(&fp, now);
}

static void usb_configured(void) {
    usb_configured_flag = true;
    usb_suspended_flag = false;
    enum_configs++;
    last_rx_ms = plat_ms();
    /* A new session: forget queued packets and the old handshake. Figures stay. */
    inf_base_reset_link(&base);
    LOGI("infinity: USB configured (#%lu)", (unsigned long)enum_configs);
}

static void usb_unconfigured(void) {
    usb_configured_flag = false;
}

static void usb_suspend(bool s) {
    usb_suspended_flag = s;
    LOGI("infinity: USB %s", s ? "suspended" : "resumed");
}

static void usb_out(const uint8_t* buf, uint16_t len) {
    uint8_t pkt[INF_PACKET_SIZE] = {0};
    memcpy(pkt, buf, len > INF_PACKET_SIZE ? INF_PACKET_SIZE : len);
    out_packets++;
    last_rx_ms = plat_ms();
    usb_suspended_flag = false;
    const bool had_handshake = base.handshake_seen;
    inf_base_handle_out(&base, pkt);
    if(!had_handshake && base.handshake_seen) LOGI("infinity: console handshake seen");
    LOGD("inf OUT cmd %02x seq %02x", pkt[2], pkt[3]);
}

static bool usb_peek_in(uint8_t out[32]) {
    return inf_base_peek_in(&base, out);
}

static void usb_drop_in(void) {
    inf_base_drop_in(&base);
    in_packets++;
}

const tb_mode_def_t mode_inf = {
    .id = TbModeInfinity,
    .name = "Infinity",
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
    .usb_set_report = NULL,
    .usb_get_report = NULL,
    .ble_msg = ble_msg,
    .usb_state = usb_state,
};

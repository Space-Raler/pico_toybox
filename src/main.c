/*
 * Pico ToyBox: a Raspberry Pi Pico 2 W that plugs into a console over USB and pretends to be a
 * Disney Infinity base, a Skylanders portal or a LEGO Dimensions toy pad (one at a time), with
 * the figures managed from a phone over Bluetooth LE. Log output goes to the phone too.
 */
#include <stdio.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "ble_link.h"
#include "ctl.h"
#include "log.h"
#include "mode.h"
#include "plat.h"
#include "store.h"
#include "toybox_proto.h"
#include "usb_modes.h"
#include "../vendor/inf/ble_proto.h"

extern char __flash_binary_end;

static BleProtoParser parser;
static uint32_t seen_epoch = 0;
static uint32_t last_rx_ms = 0;
static bool phone_was = false;
static uint32_t boot_ms = 0;

bool tb_send(uint8_t type, const uint8_t* payload, uint16_t len) {
    return ble_link_send(type, payload, len);
}

bool tb_phone_connected(void) {
    return ble_link_connected();
}

/* ---- hooks for ctl.c ---- */
static uint8_t h_cur_mode(void) { return usbm_mode() ? usbm_mode()->id : 0; }
static uint8_t h_cur_game(void) { return usbm_game(); }
static uint8_t h_usb_state(void) {
    if(usbm_switching() || !usbm_mode()) return 0;
    return usbm_mode()->usb_state();
}
static void h_request_mode(uint8_t m, uint8_t g) { usbm_request_mode(m, g); }

static void h_info(char* out, unsigned max) {
    const usbm_stats_t* u = usbm_stats();
    snprintf(
        out, max,
        "fw=%d.%d\nuptime_s=%lu\nmode=%s\nusb_configured=%d\nusb_in=%lu\nusb_in_busy=%lu\nusb_out=%lu\n"
        "usb_ctrl=%lu\nusb_ctrl_stall=%lu\nusb_switches=%lu\nble_mtu=%u\nble_rx_dropped=%lu\n"
        "flash_saves=%lu\nstore_base=0x%lx\nfw_end=0x%lx\n",
        TB_FW_MAJOR, TB_FW_MINOR, (unsigned long)(plat_ms() / 1000),
        usbm_mode() ? usbm_mode()->name : "?", usbm_is_configured(), (unsigned long)u->in_ok,
        (unsigned long)u->in_busy_fail, (unsigned long)u->out_packets, (unsigned long)u->ctrl_requests,
        (unsigned long)u->ctrl_stalled, (unsigned long)u->switches, (unsigned)ble_link_mtu(),
        (unsigned long)ble_link_rx_dropped(), (unsigned long)store_saves(), (unsigned long)store_base(),
        (unsigned long)(uintptr_t)&__flash_binary_end - XIP_BASE);
}

static void h_factory_reset(void) {
    store_wipe_all();
    watchdog_reboot(0, 0, 100);
    while(true) tight_loop_contents();
}

/* ---- phone frames ---- */
static void on_frame(void* ctx, uint8_t type, const uint8_t* p, uint16_t len) {
    (void)ctx;
    if(ctl_handles(type)) {
        ctl_message(type, p, len);
        return;
    }
    const tb_mode_def_t* m = usbm_mode();
    if(!m || usbm_switching()) {
        /* USB is being re-plugged: the mode's state is being rebuilt, so answer "not ready". */
        const uint8_t r[3] = {type, 0, BleResultNotReady};
        tb_send(BleMsgResult, r, sizeof(r));
        return;
    }
    m->ble_msg(type, p, len);
}

static void pump_ble(uint32_t now) {
    const uint32_t ep = ble_link_epoch();
    if(ep != seen_epoch) {
        seen_epoch = ep;
        ble_proto_init(&parser); /* a new connection must not inherit half a frame */
        const bool c = ble_link_connected();
        if(usbm_mode() && !usbm_switching()) {
            if(!c && usbm_mode()->ble_disconnected) usbm_mode()->ble_disconnected();
        }
        ctl_phone_changed(c);
    }
    const bool conn = ble_link_connected();
    if(conn != phone_was) {
        phone_was = conn;
        if(conn) {
            LOGI("ble: link ready (notifications on)");
            if(usbm_mode() && usbm_mode()->ble_connected) usbm_mode()->ble_connected();
            ctl_phone_changed(true);
        } else {
            ctl_phone_changed(false);
            if(usbm_mode() && usbm_mode()->ble_disconnected) usbm_mode()->ble_disconnected();
        }
    }
    uint8_t chunk[128];
    size_t n;
    while((n = ble_link_read(chunk, sizeof(chunk))) > 0) {
        last_rx_ms = now;
        ble_proto_feed(&parser, chunk, n, on_frame, NULL);
    }
    /* Quiet with half a frame buffered: stop waiting on a false start marker. */
    if(ble_proto_busy(&parser) && now - last_rx_ms > 1000) {
        ble_proto_resync(&parser, on_frame, NULL);
        last_rx_ms = now;
    }
}

/* LED on the CYW43: blinks N times (N = mode number) until the console enumerates the device,
 * then stays on. */
static void update_led(uint32_t now) {
    static bool last = false;
    bool on;
    if(usbm_is_configured() && !usbm_switching()) {
        on = true;
    } else {
        const uint32_t t = now % 2000;
        const uint8_t n = usbm_mode() ? usbm_mode()->id : 1;
        on = t < (uint32_t)n * 300 && (t % 300) < 150;
        if(usbm_switching()) on = (now / 100) & 1;
    }
    if(on != last) {
        last = on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    }
}

int main(void) {
    boot_ms = plat_ms();
    log_init();
    LOGI("Pico ToyBox fw %d.%d starting", TB_FW_MAJOR, TB_FW_MINOR);
    if(watchdog_caused_reboot()) LOGE("last reset was caused by the WATCHDOG (a hang or a crash recovered)");

    if(cyw43_arch_init()) {
        LOGE("cyw43_arch_init failed: no Bluetooth, no LED");
    }

    if(!store_init()) LOGE("flash store unavailable: nothing will be saved");
    const uint32_t fw_end = (uint32_t)(uintptr_t)&__flash_binary_end - XIP_BASE;
    if(store_base() && fw_end > store_base()) {
        LOGE("FIRMWARE OVERLAPS THE SAVE AREA (fw ends 0x%lx, store at 0x%lx)", (unsigned long)fw_end,
             (unsigned long)store_base());
    }

    tb_settings_t s;
    settings_load(&s);
    log_set_level(s.log_level);

    const ctl_hooks_t hooks = {
        .send = tb_send,
        .request_mode = h_request_mode,
        .cur_mode = h_cur_mode,
        .cur_game = h_cur_game,
        .usb_state = h_usb_state,
        .info_text = h_info,
        .factory_reset = h_factory_reset,
    };
    ctl_init(&hooks);
    ble_proto_init(&parser);

    ble_link_init();
    usbm_init(tb_mode_by_id(s.mode), s.game);

    /* Recover from a hang (stuck flash write, USB lock-up): reboot and re-enumerate. */
    watchdog_enable(8000, true);

    while(true) {
        const uint32_t now = plat_ms();
        usbm_task();
        pump_ble(now);
        ctl_task();
        ble_link_task();
        update_led(now);
        watchdog_update();
    }
}

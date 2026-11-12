/*
 * One custom TinyUSB class driver that serves the descriptors of whichever base is active
 * (Infinity, Skylanders, LEGO toy pad). Switching bases = tud_disconnect(), wait, swap the
 * descriptors, tud_connect(): to the console that is one device unplugged and another plugged in.
 *
 * A custom driver (instead of TinyUSB's HID class) because these devices need exact control of
 * endpoint addresses and of the HID class requests on EP0 (the Skylanders portal takes its
 * commands as SET_REPORT on the control pipe, not on the OUT endpoint).
 *
 * Everything here runs on the main thread inside tud_task(); the USB interrupt only queues events.
 */
#include "usb_modes.h"

#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

#include "log.h"
#include "plat.h"

#define HID_DESC_TYPE_HID    0x21
#define HID_DESC_TYPE_REPORT 0x22
#define HID_REQ_GET_REPORT   0x01
#define HID_REQ_GET_IDLE     0x02
#define HID_REQ_GET_PROTOCOL 0x03
#define HID_REQ_SET_REPORT   0x09
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

#define SWITCH_GAP_MS 1000 /* how long the console sees "nothing plugged in" between two bases */

static const tb_mode_def_t* cur = NULL;
static uint8_t cur_game = 0;
static usbm_stats_t stats;

static bool switch_pending = false;
static uint8_t pend_mode = 0, pend_game = 0;
static bool down = false;
static uint32_t reconnect_at = 0;

static uint8_t s_rhport = 0;
static uint8_t ep_in_addr = 0, ep_out_addr = 0;
static bool open_ok = false;

static uint8_t in_buf[32] TU_ATTR_ALIGNED(4);
static uint8_t out_buf[32] TU_ATTR_ALIGNED(4);
static uint8_t ctl_buf[64] TU_ATTR_ALIGNED(4);

const tb_mode_def_t* tb_mode_by_id(uint8_t id) {
    switch(id) {
    case 1: return &mode_inf;
    case 2: return &mode_sky;
    case 3: return &mode_ld;
    default: return NULL;
    }
}

const tb_mode_def_t* usbm_mode(void) { return cur; }
uint8_t usbm_game(void) { return cur_game; }
bool usbm_switching(void) { return down || switch_pending; }
bool usbm_is_configured(void) { return open_ok; }
const usbm_stats_t* usbm_stats(void) { return &stats; }

/* ---- IN endpoint pump ---- */

static void pump_in(void) {
    if(!open_ok || down || !cur || !cur->usb_peek_in) return;
    if(usbd_edpt_busy(s_rhport, ep_in_addr)) return;
    if(!cur->usb_peek_in(in_buf)) return;
    if(usbd_edpt_xfer(s_rhport, ep_in_addr, in_buf, sizeof(in_buf))) {
        cur->usb_drop_in();
        stats.in_ok++;
    } else {
        stats.in_busy_fail++; /* the packet stays queued and is offered again next time */
    }
}

void usbm_kick(void) {
    pump_in();
}

/* ---- descriptors (TinyUSB callbacks) ---- */

uint8_t const* tud_descriptor_device_cb(void) {
    return cur ? cur->dev_desc : mode_inf.dev_desc;
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return cur ? cur->cfg_desc : mode_inf.cfg_desc;
}

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc[34];
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char* s = NULL;
    uint8_t n;
    if(index == 0) {
        desc[1] = 0x0409; /* English (US) */
        n = 1;
    } else {
        if(!cur) return NULL;
        if(index == 1) s = cur->manuf;
        else if(index == 2) s = cur->product;
        else if(index == 3) {
            if(cur->serial) {
                s = cur->serial;
            } else {
                pico_get_unique_board_id_string(serial, sizeof(serial));
                s = serial;
            }
        }
        if(!s) return NULL;
        n = (uint8_t)strlen(s);
        if(n > 31) n = 31;
        for(uint8_t i = 0; i < n; i++) desc[1 + i] = (uint16_t)(uint8_t)s[i];
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc;
}

/* ---- the class driver ---- */

static void drv_init(void) {
    open_ok = false;
}

static void drv_reset(uint8_t rhport) {
    (void)rhport;
    if(open_ok && cur) {
        cur->usb_unconfigured();
        LOGI("usb: unconfigured/reset");
    }
    open_ok = false;
}

static uint16_t drv_open(uint8_t rhport, tusb_desc_interface_t const* desc_itf, uint16_t max_len) {
    (void)max_len;
    TU_VERIFY(cur != NULL && desc_itf->bInterfaceClass == TUSB_CLASS_HID, 0);

    uint8_t const* p = (uint8_t const*)desc_itf;
    uint16_t drv_len = desc_itf->bLength;
    p += desc_itf->bLength;
    TU_VERIFY(p[1] == HID_DESC_TYPE_HID, 0);
    drv_len = (uint16_t)(drv_len + p[0]);
    p += p[0];

    ep_in_addr = ep_out_addr = 0;
    for(uint8_t i = 0; i < desc_itf->bNumEndpoints; i++) {
        tusb_desc_endpoint_t const* ep = (tusb_desc_endpoint_t const*)p;
        TU_VERIFY(ep->bDescriptorType == TUSB_DESC_ENDPOINT, 0);
        TU_VERIFY(usbd_edpt_open(rhport, ep), 0);
        if(ep->bEndpointAddress & 0x80) ep_in_addr = ep->bEndpointAddress;
        else ep_out_addr = ep->bEndpointAddress;
        drv_len = (uint16_t)(drv_len + ep->bLength);
        p += ep->bLength;
    }
    TU_VERIFY(ep_in_addr && ep_out_addr, 0);

    s_rhport = rhport;
    open_ok = true;
    stats.configured_count++;
    usbd_edpt_xfer(rhport, ep_out_addr, out_buf, sizeof(out_buf)); /* listen for packets */
    LOGI("usb: %s configured by host", cur->name);
    cur->usb_configured();
    return drv_len;
}

static bool drv_control_xfer(uint8_t rhport, uint8_t stage, tusb_control_request_t const* req) {
    if(!cur) return false;
    const uint8_t type = req->bmRequestType_bit.type;

    if(stage == CONTROL_STAGE_SETUP) {
        stats.ctrl_requests++;
        if(type == TUSB_REQ_TYPE_STANDARD) {
            if(req->bRequest == TUSB_REQ_GET_DESCRIPTOR) {
                const uint8_t dtype = tu_u16_high(req->wValue);
                if(dtype == HID_DESC_TYPE_HID) {
                    return tud_control_xfer(rhport, req, (void*)(uintptr_t)(cur->cfg_desc + cur->hid_desc_off), 9);
                }
                if(dtype == HID_DESC_TYPE_REPORT) {
                    return tud_control_xfer(rhport, req, (void*)(uintptr_t)cur->report_desc, cur->report_len);
                }
            }
            stats.ctrl_stalled++;
            return false;
        }
        if(type == TUSB_REQ_TYPE_CLASS) {
            switch(req->bRequest) {
            case HID_REQ_SET_REPORT:
                if(!cur->usb_set_report || req->wLength > sizeof(ctl_buf)) break;
                if(req->wLength == 0) return tud_control_status(rhport, req);
                return tud_control_xfer(rhport, req, ctl_buf, req->wLength); /* handled at the ACK stage */
            case HID_REQ_GET_REPORT:
                if(!cur->usb_get_report) break;
                {
                    uint16_t max = req->wLength < 32 ? req->wLength : 32;
                    const uint16_t n = cur->usb_get_report(ctl_buf, max);
                    return tud_control_xfer(rhport, req, ctl_buf, n);
                }
            case HID_REQ_GET_IDLE:
                ctl_buf[0] = 0;
                return tud_control_xfer(rhport, req, ctl_buf, 1);
            case HID_REQ_GET_PROTOCOL:
                ctl_buf[0] = 1; /* report protocol */
                return tud_control_xfer(rhport, req, ctl_buf, 1);
            case HID_REQ_SET_IDLE:
            case HID_REQ_SET_PROTOCOL:
                return tud_control_status(rhport, req);
            default:
                break;
            }
        }
        stats.ctrl_stalled++;
        LOGD("usb: stalled ctrl type %02x req %02x", req->bmRequestType, req->bRequest);
        return false;
    }

    if(stage == CONTROL_STAGE_ACK && type == TUSB_REQ_TYPE_CLASS && req->bRequest == HID_REQ_SET_REPORT &&
       req->wLength > 0 && cur->usb_set_report) {
        cur->usb_set_report(ctl_buf, req->wLength); /* data stage finished */
        pump_in();
    }
    return true;
}

static bool drv_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)result;
    if(ep_addr == ep_out_addr) {
        stats.out_packets++;
        if(cur && xferred_bytes) cur->usb_out(out_buf, (uint16_t)xferred_bytes);
        usbd_edpt_xfer(rhport, ep_out_addr, out_buf, sizeof(out_buf)); /* keep listening */
    }
    pump_in(); /* a reply may have been queued by the OUT packet, or the IN one just left */
    return true;
}

static const usbd_class_driver_t toy_driver = {
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    .name = "ToyBox",
#endif
    .init = drv_init,
    .reset = drv_reset,
    .open = drv_open,
    .control_xfer_cb = drv_control_xfer,
    .xfer_cb = drv_xfer_cb,
    .sof = NULL,
};

usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return &toy_driver;
}

/* ---- bus events ---- */

void tud_mount_cb(void) {}
void tud_umount_cb(void) {
    LOGI("usb: unmounted");
}
void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    if(cur && open_ok) cur->usb_suspend(true);
}
void tud_resume_cb(void) {
    if(cur && open_ok) cur->usb_suspend(false);
}

/* ---- init, task, switching ---- */

void usbm_init(const tb_mode_def_t* mode, uint8_t game) {
    cur = mode;
    cur_game = game;
    memset(&stats, 0, sizeof(stats));
    cur->activate(game);
    tusb_init();
    LOGI("usb: started as %s", cur->name);
}

void usbm_request_mode(uint8_t mode_id, uint8_t game) {
    if(!tb_mode_by_id(mode_id)) return;
    pend_mode = mode_id;
    pend_game = game;
    switch_pending = true;
}

static void begin_switch(void) {
    switch_pending = false;
    LOGI("usb: switching to %s", tb_mode_by_id(pend_mode)->name);
    cur->deactivate(); /* write unsaved figures to flash */
    open_ok = false;
    tud_disconnect();
    down = true;
    reconnect_at = plat_ms() + SWITCH_GAP_MS;
    stats.switches++;
}

static void finish_switch(void) {
    down = false;
    cur = tb_mode_by_id(pend_mode);
    cur_game = pend_game;
    cur->activate(cur_game);
    tud_connect();
    LOGI("usb: now plugged in as %s", cur->name);
}

void usbm_task(void) {
    tud_task();
    if(down) {
        if((int32_t)(plat_ms() - reconnect_at) >= 0) finish_switch();
        return;
    }
    if(switch_pending) {
        begin_switch();
        return;
    }
    if(cur) cur->tick(plat_ms());
    pump_in();
}

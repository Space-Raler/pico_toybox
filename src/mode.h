/*
 * One "base" the Pico can pretend to be. usb_modes.c owns the USB side (descriptors,
 * re-enumeration, endpoints) and calls into the active mode; main.c routes Bluetooth frames
 * to it. Everything here runs on the main thread, never in an interrupt, so no locking.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t id; /* TbMode* */
    const char* name;

    /* USB description of the device */
    const uint8_t* dev_desc; /* 18 bytes */
    const uint8_t* cfg_desc; /* whole configuration: config + interface + HID + 2 endpoints */
    uint16_t cfg_len;
    uint16_t hid_desc_off; /* offset of the 9-byte HID descriptor inside cfg_desc */
    const uint8_t* report_desc;
    uint16_t report_len;
    uint8_t ep_in, ep_out;
    const char* manuf;   /* USB string descriptors; NULL = the device has none of that string */
    const char* product;
    const char* serial;

    /* life cycle */
    void (*activate)(uint8_t game);  /* fresh state, then restore saved figures from flash */
    void (*deactivate)(void);        /* write pending changes to flash */
    void (*tick)(uint32_t now);      /* called every main-loop pass */

    /* USB events */
    void (*usb_configured)(void);    /* host set the configuration: a new session */
    void (*usb_unconfigured)(void);
    void (*usb_suspend)(bool suspended);
    void (*usb_out)(const uint8_t* buf, uint16_t len); /* packet on the OUT endpoint */
    bool (*usb_peek_in)(uint8_t out[32]); /* next IN packet without removing it; false = none */
    void (*usb_drop_in)(void);            /* the one just peeked was accepted by the hardware */
    bool (*usb_set_report)(const uint8_t* buf, uint16_t len); /* class request on EP0 (may be NULL) */
    uint16_t (*usb_get_report)(uint8_t* buf, uint16_t max);   /* class request on EP0 (may be NULL) */

    /* Bluetooth */
    void (*ble_msg)(uint8_t type, const uint8_t* payload, uint16_t len);
    void (*ble_connected)(void);     /* a phone connected / disconnected (called on change) */
    void (*ble_disconnected)(void);
    uint8_t (*usb_state)(void);      /* 0..3, see toybox_proto.h */
} tb_mode_def_t;

extern const tb_mode_def_t mode_inf;
extern const tb_mode_def_t mode_sky;
extern const tb_mode_def_t mode_ld;

const tb_mode_def_t* tb_mode_by_id(uint8_t id);

/* Services the modes use, provided by main.c / the Bluetooth layer. */
bool tb_send(uint8_t type, const uint8_t* payload, uint16_t len); /* false: no phone or queue full */
bool tb_phone_connected(void);

/* Ask the USB layer to wake up right away (a mode queued something to send). */
void usbm_kick(void);

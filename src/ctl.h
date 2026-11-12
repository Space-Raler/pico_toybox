/* Pico-only phone messages (mode switch, log, info): see toybox_proto.h. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t mode;
    uint8_t game;
    uint8_t log_level;
} tb_settings_t;

/* Hooks the platform provides so this file stays testable on a PC. */
typedef struct {
    bool (*send)(uint8_t type, const uint8_t* p, uint16_t len);
    void (*request_mode)(uint8_t mode, uint8_t game);
    uint8_t (*cur_mode)(void);
    uint8_t (*cur_game)(void);
    uint8_t (*usb_state)(void);
    void (*info_text)(char* out, unsigned max); /* "key=value\n" lines */
    void (*factory_reset)(void);
} ctl_hooks_t;

void ctl_init(const ctl_hooks_t* hooks);
bool ctl_handles(uint8_t type);
void ctl_message(uint8_t type, const uint8_t* p, uint16_t len);
void ctl_task(void); /* log streaming and chunked LOG_GET replies */
void ctl_phone_changed(bool connected);
void ctl_send_mode(void);

void settings_load(tb_settings_t* s);
void settings_save(const tb_settings_t* s);

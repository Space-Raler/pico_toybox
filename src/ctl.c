#include "ctl.h"

#include <string.h>

#include "log.h"
#include "store.h"
#include "toybox_proto.h"

#define LOG_CHUNK 180

static ctl_hooks_t hk;
static bool streaming = false;
static uint32_t stream_cursor = 0;
static bool dump_active = false;
static uint32_t dump_cursor = 0, dump_end = 0;
static bool dump_end_pending = false;
static bool persist_ok = true;

void ctl_init(const ctl_hooks_t* hooks) {
    hk = *hooks;
}

void settings_load(tb_settings_t* s) {
    s->mode = TbModeInfinity;
    s->game = 1; /* Trap Team: just the label layout on the Skylanders page */
    s->log_level = 1;
    uint8_t rec[4];
    uint16_t flags;
    if(store_load(KEY_SETTINGS, rec, sizeof(rec), &flags) == 4) {
        if(rec[0] >= 1 && rec[0] <= 3) s->mode = rec[0];
        if(rec[1] <= 5) s->game = rec[1];
        if(rec[2] <= 2) s->log_level = rec[2];
    }
}

void settings_save(const tb_settings_t* s) {
    const uint8_t rec[4] = {s->mode, s->game, s->log_level, 0};
    persist_ok = store_save(KEY_SETTINGS, rec, sizeof(rec), 0);
}

bool ctl_handles(uint8_t type) {
    return type >= TbMsgModeGet && type <= TbMsgFactoryReset;
}

void ctl_send_mode(void) {
    const uint8_t p[8] = {
        hk.cur_mode(), hk.cur_game(), hk.usb_state(), log_get_level(), streaming,
        TB_FW_MAJOR, TB_FW_MINOR, persist_ok};
    hk.send(TbMsgMode, p, sizeof(p));
}

void ctl_phone_changed(bool connected) {
    streaming = false;
    dump_active = false;
    if(connected) stream_cursor = log_total();
}

static void save_current(void) {
    tb_settings_t s = {hk.cur_mode(), hk.cur_game(), log_get_level()};
    settings_save(&s);
}

void ctl_message(uint8_t type, const uint8_t* p, uint16_t len) {
    switch(type) {
    case TbMsgModeGet:
        ctl_send_mode();
        break;
    case TbMsgModeSet:
        if(len >= 1 && p[0] >= 1 && p[0] <= 3) {
            const uint8_t game = len >= 2 && p[1] <= 5 ? p[1] : hk.cur_game();
            LOGI("ctl: phone asked for mode %u game %u", p[0], game);
            tb_settings_t s = {p[0], game, log_get_level()};
            settings_save(&s);
            hk.request_mode(p[0], game);
        } else {
            LOGE("ctl: bad MODE_SET");
        }
        ctl_send_mode();
        break;
    case TbMsgLogGet:
        dump_cursor = log_total() > LOG_BUF_SIZE ? log_total() - LOG_BUF_SIZE : 0;
        dump_end = log_total();
        dump_active = true;
        dump_end_pending = false;
        break;
    case TbMsgLogCtl:
        if(len >= 1) {
            if(p[0] <= 1) {
                streaming = p[0];
                if(streaming) stream_cursor = log_total();
            }
        }
        if(len >= 2 && p[1] <= 2) {
            log_set_level(p[1]);
            save_current();
        }
        ctl_send_mode();
        break;
    case TbMsgLogClear:
        log_clear();
        stream_cursor = log_total();
        dump_active = false;
        break;
    case TbMsgInfoGet: {
        char text[400];
        hk.info_text(text, sizeof(text));
        hk.send(TbMsgInfo, (const uint8_t*)text, (uint16_t)strlen(text));
        break;
    }
    case TbMsgFactoryReset:
        if(len == 1 && p[0] == 0xA5) {
            LOGE("ctl: factory reset requested");
            hk.factory_reset();
        }
        break;
    default:
        break;
    }
}

void ctl_task(void) {
    uint8_t buf[LOG_CHUNK];
    if(dump_active) {
        /* a few chunks per pass so the radio queue is not flooded */
        for(int i = 0; i < 4; i++) {
            if(dump_cursor == dump_end) {
                if(hk.send(TbMsgLogText, NULL, 0)) dump_active = false;
                break;
            }
            uint32_t c = dump_cursor;
            size_t want = dump_end - c;
            if(want > sizeof(buf)) want = sizeof(buf);
            const size_t n = log_read(&c, buf, want);
            if(!n) { dump_cursor = dump_end; continue; }
            if(!hk.send(TbMsgLogText, buf, (uint16_t)n)) break; /* queue full: retry next pass */
            dump_cursor = c;
        }
        return;
    }
    if(streaming) {
        for(int i = 0; i < 2; i++) {
            uint32_t c = stream_cursor;
            const size_t n = log_read(&c, buf, sizeof(buf));
            if(!n) break;
            if(!hk.send(TbMsgLogText, buf, (uint16_t)n)) break;
            stream_cursor = c;
        }
    }
}

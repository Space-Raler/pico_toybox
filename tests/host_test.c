/* Host-side tests: the portable logic (store, persistence, three modes, LEGO commands, control
 * messages) compiled with gcc against a RAM "flash". No USB/BLE hardware involved. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ctl.h"
#include "../src/ld_pad.h"
#include "../src/log.h"
#include "../src/mode.h"
#include "../src/persist.h"
#include "../src/plat.h"
#include "../src/store.h"
#include "../src/toybox_proto.h"
#include "../vendor/inf/ble_proto.h"
#include "../vendor/inf/infinity_core.h"
#include "../vendor/ld/bytes.h"
#include "../vendor/ld/tea.h"
#include "../vendor/sky/core/sky_core.h"

/* ---------- fake platform ---------- */
#define FLASH_SZ (4u * 1024 * 1024)
static uint8_t flash[FLASH_SZ];
static uint32_t now_ms = 1000;
static int fail_after = -1; /* simulate power cut: prog stops after N bytes */
uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_irq_save(void) { return 0; }
void plat_irq_restore(uint32_t s) { (void)s; }
uint32_t plat_flash_size(void) { return FLASH_SZ; }
void plat_flash_read(uint32_t o, void* d, uint32_t l) { memcpy(d, flash + o, l); }
bool plat_flash_erase(uint32_t o, uint32_t l) { assert(o % 4096 == 0 && l % 4096 == 0); memset(flash + o, 0xFF, l); return true; }
bool plat_flash_prog(uint32_t o, const void* s, uint32_t l) {
    assert(o % 256 == 0 && l % 256 == 0);
    if(fail_after >= 0) { memcpy(flash + o, s, (size_t)fail_after < l ? (size_t)fail_after : l); return false; }
    memcpy(flash + o, s, l);
    return true;
}

/* ---------- fake services ---------- */
typedef struct { uint8_t type; uint16_t len; uint8_t data[1100]; } Sent;
static Sent sent[256];
static int nsent = 0;
static bool phone = true;
bool tb_send(uint8_t type, const uint8_t* p, uint16_t len) {
    if(!phone) return false;
    assert(nsent < 256);
    sent[nsent].type = type; sent[nsent].len = len;
    if(len) memcpy(sent[nsent].data, p, len);
    nsent++;
    return true;
}
bool tb_phone_connected(void) { return phone; }
static int kicks = 0;
void usbm_kick(void) { kicks++; }

static const Sent* find_sent(uint8_t type) {
    for(int i = nsent - 1; i >= 0; i--) if(sent[i].type == type) return &sent[i];
    return NULL;
}

#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while(0)
#define PASS(n) printf("ok  %s\n", n)

static void fresh_flash(void) { memset(flash, 0xFF, sizeof(flash)); CHECK(store_init()); }

/* ---------- store ---------- */
static void test_store(void) {
    fresh_flash();
    uint8_t buf[1100], a[1024], b[1024];
    memset(a, 0xA1, sizeof(a)); memset(b, 0xB2, sizeof(b));
    CHECK(store_load(5, buf, sizeof(buf), NULL) == -1);
    CHECK(store_save(5, a, sizeof(a), 0));
    uint16_t fl = 9;
    CHECK(store_load(5, buf, sizeof(buf), &fl) == 1024 && memcmp(buf, a, 1024) == 0 && fl == 0);
    CHECK(store_save(5, b, sizeof(b), STORE_FLAG_UNSYNCED));
    CHECK(store_load(5, buf, sizeof(buf), &fl) == 1024 && memcmp(buf, b, 1024) == 0 && fl == STORE_FLAG_UNSYNCED);
    /* power cut while writing a third version: the previous one must survive */
    uint8_t c[1024]; memset(c, 0xC3, sizeof(c));
    for(int cut = 0; cut <= 1280; cut += 256) {
        fail_after = cut;
        store_save(5, c, sizeof(c), 0);
        fail_after = -1;
        CHECK(store_init());
        int n = store_load(5, buf, sizeof(buf), &fl);
        CHECK(n == 1024);
        CHECK(memcmp(buf, b, 1024) == 0 || memcmp(buf, c, 1024) == 0);
        if(memcmp(buf, c, 1024) == 0) { /* cut landed after the full record: fine, make b current again */
            CHECK(store_save(5, b, sizeof(b), STORE_FLAG_UNSYNCED));
        }
    }
    /* erase key, keys are independent */
    CHECK(store_save(6, a, 320, 0));
    CHECK(store_erase_key(5));
    CHECK(store_load(5, buf, sizeof(buf), NULL) == 0);
    CHECK(store_load(6, buf, sizeof(buf), NULL) == 320);
    for(int i = 0; i < 50; i++) CHECK(store_save(6, a, 320, 0));
    CHECK(store_load(6, buf, sizeof(buf), NULL) == 320);
    PASS("store: save/load/ping-pong/power-cut/erase");
}

/* ---------- LEGO USB commands ---------- */
static const uint8_t key[16] = {0x55,0xFE,0xF6,0xB0,0x62,0xBF,0x0B,0x41,0xC9,0xB3,0x7C,0xB4,0x97,0x3E,0x29,0x7B};
static void ld_cmd(uint8_t cmd, uint8_t cid, const uint8_t* pl, int n, uint8_t out[32]) {
    uint8_t req[32] = {0};
    req[0] = 0x55; req[1] = (uint8_t)(2 + n); req[2] = cmd; req[3] = cid;
    if(n) memcpy(req + 4, pl, (size_t)n);
    uint8_t s = 0; for(int i = 0; i < 2 + 2 + n; i++) s = (uint8_t)(s + req[i]);
    req[4 + n] = s;
    ld_pad_handle_out(req, 32);
    CHECK(ld_pad_in_peek(out));
    ld_pad_in_drop();
}
static void test_lego(void) {
    ld_pad_init();
    uint8_t out[32];
    ld_cmd(0xB0, 7, NULL, 0, out);
    CHECK(out[0] == 0x55 && out[1] == 14 && out[2] == 7 && memcmp(out + 3, "(c) LEGO 2014", 13) == 0);
    CHECK(ld_pad_take_wake() && ld_pad_awake());
    /* checksum of response */
    uint8_t s = 0; for(int i = 0; i < 16; i++) s = (uint8_t)(s + out[i]);
    CHECK(out[16] == s);

    LdToken t; ld_make_minifig(&t, 1);
    CHECK(ld_pad_place_now(&t, 1));
    uint8_t ev[32]; CHECK(ld_pad_in_peek(ev)); ld_pad_in_drop();
    CHECK(ev[0] == 0x56 && ev[1] == 0x0b && ev[2] == 1 /*centre pad*/ && ev[4] == 0 && ev[5] == 0);
    s = 0; for(int i = 0; i < 13; i++) s = (uint8_t)(s + ev[i]); CHECK(ev[13] == s);
    CHECK(ld_pad_box_filled(1) && ld_pad_token_count() == 1);

    /* MODEL: encrypted request {index, 0,0,0, conf BE}, answer = tea(id LE, conf BE) */
    uint8_t plain[8] = {0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44}, enc[8];
    tea_encrypt(plain, key, enc);
    ld_cmd(0xD4, 9, enc, 8, out);
    CHECK(out[3] == 0x00);
    uint8_t dec[8]; tea_decrypt(out + 4, key, dec);
    CHECK(readUInt32LE(dec, 0) == 1 && dec[4] == 0x11 && dec[7] == 0x44);
    /* unknown index -> 0xF2 */
    plain[0] = 5; tea_encrypt(plain, key, enc); ld_cmd(0xD4, 9, enc, 8, out);
    CHECK(out[3] == 0xF2);

    /* vehicle: READ returns token pages, WRITE changes it and flags dirty */
    uint32_t up[2] = {0x01020304, 0x05060708};
    LdToken v; ld_make_vehicle(&v, 1000, up);
    CHECK(ld_pad_place_now(&v, 3)); ld_pad_in_peek(ev); ld_pad_in_drop();
    uint8_t rd[2] = {1, 0x23};
    ld_cmd(0xD2, 3, rd, 2, out);
    CHECK(out[3] == 0 && readUInt32LE(out, 4) == 0x01020304);
    uint8_t wr[6] = {1, 0x23, 9, 9, 9, 9};
    ld_cmd(0xD3, 4, wr, 6, out);
    CHECK(out[3] == 0 && (ld_pad_take_dirty_mask() & 2));
    CHECK(ld_pad_box_token(3)->token[0x23 * 4] == 9);
    /* model of vehicle -> 0xF9 */
    plain[0] = 1; tea_encrypt(plain, key, enc); ld_cmd(0xD4, 9, enc, 8, out);
    CHECK(out[3] == 0xF9);
    /* SEED/CHAL deterministic pair */
    uint8_t seedp[8] = {1, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD};
    tea_encrypt(seedp, key, enc); ld_cmd(0xB1, 1, enc, 8, out);
    tea_decrypt(out + 3, key, dec); CHECK(dec[0] == 0xAA && dec[3] == 0xDD);
    uint8_t chal[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0, 0, 0, 0};
    tea_encrypt(chal, key, enc); ld_cmd(0xB3, 2, enc, 8, out);
    tea_decrypt(out + 3, key, dec); CHECK(dec[4] == 0xAA);
    /* removal */
    CHECK(ld_pad_remove_box_now(1)); ld_pad_in_peek(ev); ld_pad_in_drop(); CHECK(ev[5] == 1);
    CHECK(!ld_pad_remove_box_now(1));
    /* hostile frames must not crash */
    uint8_t bad[32]; memset(bad, 0xFF, 32); ld_pad_handle_out(bad, 32);
    bad[1] = 1; ld_pad_handle_out(bad, 32); bad[1] = 0; ld_pad_handle_out(bad, 32);
    ld_pad_handle_out(bad, 1);
    PASS("lego: wake/model/read/write/seed/chal/events/hostile input");
}

/* ---------- helper to drive a mode's ble_msg ---------- */
static void ble(const tb_mode_def_t* m, uint8_t type, const uint8_t* p, uint16_t len) { m->ble_msg(type, p, len); }

static void test_infinity(void) {
    fresh_flash(); nsent = 0; phone = true;
    mode_inf.activate(0);
    mode_inf.usb_configured();
    ble(&mode_inf, BleMsgHello, NULL, 0);
    const Sent* st = find_sent(BleMsgStatus);
    CHECK(st && st->len == 4 && st->data[0] == 1 && st->data[2] == 0);

    uint8_t fig[1 + 320]; memset(fig, 0, sizeof(fig)); fig[0] = 3; for(int i = 1; i < 321; i++) fig[i] = (uint8_t)i;
    ble(&mode_inf, BleMsgLoad, fig, sizeof(fig));
    st = find_sent(BleMsgStatus); CHECK(st->data[2] == (1 << 3));
    const Sent* rs = find_sent(BleMsgResult); CHECK(rs->data[2] == BleResultOk);
    uint8_t pk[32]; CHECK(mode_inf.usb_peek_in(pk)); mode_inf.usb_drop_in(); /* 'added' event */
    /* bad slot / bad length */
    fig[0] = 12; ble(&mode_inf, BleMsgLoad, fig, sizeof(fig)); CHECK(find_sent(BleMsgResult)->data[2] == BleResultBadSlot);
    ble(&mode_inf, BleMsgLoad, fig, 10); CHECK(find_sent(BleMsgResult)->data[2] == BleResultBadLength);

    /* debounce: nothing in flash yet, then written after the delay */
    uint8_t buf[400];
    CHECK(store_load(KEY_INF(3), buf, sizeof(buf), NULL) == -1);
    now_ms += 600; mode_inf.tick(now_ms);
    CHECK(store_load(KEY_INF(3), buf, sizeof(buf), NULL) == 320 && buf[0] == 1);

    /* reboot: figure comes back without the phone */
    mode_inf.activate(0);
    st = NULL; nsent = 0; ble(&mode_inf, BleMsgStatusReq, NULL, 0);
    CHECK(find_sent(BleMsgStatus)->data[2] == (1 << 3));

    /* remove erases it */
    uint8_t slot = 3; ble(&mode_inf, BleMsgRemove, &slot, 1);
    now_ms += 600; mode_inf.tick(now_ms);
    CHECK(store_load(KEY_INF(3), buf, sizeof(buf), NULL) == 0);
    PASS("infinity: status/load/validation/persist/restore/remove");
}

static void test_skylanders(void) {
    fresh_flash(); nsent = 0; phone = true;
    mode_sky.activate(1);
    mode_sky.usb_configured();
    static uint8_t fig[1 + 1024];
    fig[0] = 2; for(int i = 1; i < 1025; i++) fig[i] = (uint8_t)(i * 7);
    ble(&mode_sky, BleMsgHello, NULL, 0);
    const Sent* st = find_sent(BleMsgStatus);
    CHECK(st && st->len == 5 && st->data[0] == 2 && st->data[4] == 1);
    ble(&mode_sky, BleMsgLoad, fig, sizeof(fig));
    CHECK(find_sent(BleMsgStatus)->data[2] == (1 << 2));

    /* console talks: 'R' (reset) then 'A' (activate) via SET_REPORT, queue gives a reply packet */
    uint8_t cmd[32] = {0}; cmd[0] = 'R';
    CHECK(mode_sky.usb_set_report(cmd, 32));
    uint8_t pk[32]; CHECK(mode_sky.usb_peek_in(pk)); mode_sky.usb_drop_in();
    CHECK(pk[0] == 'R');
    cmd[0] = 0; cmd[1] = 'A'; cmd[2] = 1; /* report-id-prefixed activate */
    CHECK(mode_sky.usb_set_report(cmd, 32));
    CHECK(mode_sky.usb_peek_in(pk)); mode_sky.usb_drop_in();
    CHECK(pk[0] == 'A');
    /* state ladder */
    CHECK(mode_sky.usb_state() == 3);

    /* game writes block 8 (W command) -> dirty -> SAVE to phone; unsynced bookkeeping */
    nsent = 0;
    uint8_t w[32] = {0}; w[0] = 'W'; w[1] = 0x20 | 2; w[2] = 8; for(int i = 0; i < 16; i++) w[3 + i] = 0xEE;
    mode_sky.usb_set_report(w, 32);
    mode_sky.tick(now_ms);
    const Sent* sv = find_sent(BleMsgSave);
    CHECK(sv && sv->len == 1 + 1024 && sv->data[0] == 2);
    CHECK(sv->data[1 + 8 * 16] == 0xEE);

    /* phone absent when the game writes: progress is kept in flash and replayed on HELLO */
    phone = false; nsent = 0;
    memset(w + 3, 0xDD, 16); mode_sky.usb_set_report(w, 32);
    mode_sky.tick(now_ms);
    now_ms += PERSIST_DELAY_MS + 10; mode_sky.tick(now_ms);
    uint8_t buf[1100]; uint16_t fl = 0;
    CHECK(store_load(KEY_SKY(2), buf, sizeof(buf), &fl) == 1024);
    CHECK(buf[8 * 16] == 0xDD && (fl & STORE_FLAG_UNSYNCED));
    mode_sky.activate(1); /* power cycle */
    phone = true; nsent = 0;
    ble(&mode_sky, BleMsgHello, NULL, 0);
    mode_sky.tick(now_ms);
    sv = find_sent(BleMsgSave);
    CHECK(sv && sv->data[0] == 2 && sv->data[1 + 8 * 16] == 0xDD);
    /* a CLEAR from the page must not lose unsent progress */
    phone = false; nsent = 0;
    mode_sky.activate(1);
    ble(&mode_sky, BleMsgClear, NULL, 0);
    now_ms += PERSIST_DELAY_MS + 10; mode_sky.tick(now_ms);
    CHECK(store_load(KEY_SKY(2), buf, sizeof(buf), &fl) == 1024 && buf[8 * 16] == 0xDD);
    PASS("skylanders: load/handshake/report-id/dirty->SAVE/offline progress/replay/CLEAR safety");
}

static void test_lego_mode(void) {
    fresh_flash(); nsent = 0; phone = true;
    mode_ld.activate(0);
    mode_ld.usb_configured();
    /* not awake yet: placing is refused */
    uint8_t p[4] = {1, 1, 1, 0}; /* box 1, minifig, id 1 */
    ble(&mode_ld, BleMsgLdPlace, p, 4);
    CHECK(find_sent(BleMsgResult)->data[2] == BleResultNotReady);
    /* console wakes the pad */
    uint8_t wake[32] = {0x55, 2, 0xB0, 1, 0}; uint8_t s = 0; for(int i = 0; i < 4; i++) s = (uint8_t)(s + wake[i]); wake[4] = s;
    mode_ld.usb_out(wake, 32);
    uint8_t pk[32]; CHECK(mode_ld.usb_peek_in(pk)); mode_ld.usb_drop_in();
    now_ms += 10; mode_ld.tick(now_ms);
    CHECK(mode_ld.usb_state() == 3);
    nsent = 0; ble(&mode_ld, BleMsgLdPlace, p, 4);
    CHECK(find_sent(BleMsgResult)->data[2] == BleResultOk);
    mode_ld.tick(now_ms + 1500); now_ms += 1500; /* action queue runs */
    mode_ld.tick(now_ms);
    CHECK(mode_ld.usb_peek_in(pk) && pk[0] == 0x56 && pk[2] == 1); mode_ld.usb_drop_in();
    /* unknown id refused */
    uint8_t bad[4] = {2, 1, 0xFF, 0x7F}; nsent = 0; ble(&mode_ld, BleMsgLdPlace, bad, 4);
    CHECK(find_sent(BleMsgResult)->data[2] == BleResultNotPresent);
    /* persisted, and restored after the console wakes again (e.g. after power cycle) */
    now_ms += PERSIST_DELAY_MS + 200; mode_ld.tick(now_ms);
    uint8_t buf[300]; CHECK(store_load(KEY_LD(1), buf, sizeof(buf), NULL) == 3 && buf[0] == 1 && buf[1] == 1);
    mode_ld.activate(0); mode_ld.usb_configured();
    mode_ld.usb_out(wake, 32); mode_ld.usb_peek_in(pk); mode_ld.usb_drop_in();
    for(int i = 0; i < 40; i++) { now_ms += 100; mode_ld.tick(now_ms); }
    CHECK(mode_ld.usb_peek_in(pk) && pk[0] == 0x56 && pk[2] == 1 && pk[5] == 0);
    PASS("lego mode: not-ready/wake/place/validate/persist/auto-restore");
}

static uint8_t g_mode = 1, g_game = 1; static int g_req = 0, g_req_game = 0;
static uint8_t c_mode(void) { return g_mode; }
static uint8_t c_game(void) { return g_game; }
static uint8_t c_usb(void) { return 2; }
static void c_req(uint8_t m, uint8_t g) { g_req = m; g_req_game = g; }
static void c_info(char* o, unsigned n) { snprintf(o, n, "fw=0.1\n"); }
static int resets = 0;
static void c_reset(void) { resets++; }

static void test_ctl2(void) {
    fresh_flash(); nsent = 0; phone = true; log_init();
    ctl_hooks_t h = {.send = tb_send, .request_mode = c_req, .cur_mode = c_mode, .cur_game = c_game,
                     .usb_state = c_usb, .info_text = c_info, .factory_reset = c_reset};
    ctl_init(&h);
    ctl_message(TbMsgModeGet, NULL, 0);
    const Sent* m = find_sent(TbMsgMode);
    CHECK(m && m->len == 8 && m->data[0] == 1 && m->data[2] == 2);
    uint8_t set[2] = {3, 0};
    ctl_message(TbMsgModeSet, set, 2);
    CHECK(g_req == 3 && g_req_game == 0);
    tb_settings_t s; settings_load(&s); CHECK(s.mode == 3 && s.game == 0);
    uint8_t badset[1] = {9}; g_req = 0; ctl_message(TbMsgModeSet, badset, 1); CHECK(g_req == 0);
    /* log dump: every byte arrives, terminated by an empty frame */
    for(int i = 0; i < 300; i++) LOGI("line number %d of the test log", i);
    nsent = 0; ctl_message(TbMsgLogGet, NULL, 0);
    size_t total = 0; bool ended = false;
    for(int pass = 0; pass < 500 && !ended; pass++) {
        ctl_task();
        for(int i = 0; i < nsent; i++) if(sent[i].type == TbMsgLogText) { if(sent[i].len == 0) ended = true; else total += sent[i].len; }
        nsent = 0;
    }
    CHECK(ended && total > 5000);
    /* streaming */
    uint8_t on[2] = {1, 2}; ctl_message(TbMsgLogCtl, on, 2); CHECK(log_get_level() == 2);
    nsent = 0; LOGD("streamed line"); ctl_task();
    const Sent* ls = find_sent(TbMsgLogText);
    CHECK(ls && memmem(ls->data, ls->len, "streamed line", 13));
    uint8_t fr[1] = {0xA5}; ctl_message(TbMsgFactoryReset, fr, 1); CHECK(resets == 1);
    fr[0] = 1; ctl_message(TbMsgFactoryReset, fr, 1); CHECK(resets == 1);
    PASS("ctl: mode get/set/validation/persist, log dump + stream, factory reset guard");
}

static int got = 0;
static void cb(void* c, uint8_t t, const uint8_t* d, uint16_t l) { (void)c; (void)d; if(t == TbMsgMode && l == 8) got++; }
static void test_proto_framing(void) {
    /* every message the Pico sends must parse with the shared frame parser */
    uint8_t frame[BLE_PROTO_MAX_FRAME]; uint8_t payload[8] = {1,2,3,4,5,6,7,8};
    size_t n = ble_proto_encode(TbMsgMode, payload, 8, frame);
    BleProtoParser p; ble_proto_init(&p);
    got = 0;
    ble_proto_feed(&p, frame, n, cb, NULL);
    CHECK(got == 1);
    PASS("framing");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    log_init();
    test_store();
    test_lego();
    test_infinity();
    test_skylanders();
    test_lego_mode();
    test_ctl2();
    test_proto_framing();
    printf("ALL PASSED\n");
    return 0;
}

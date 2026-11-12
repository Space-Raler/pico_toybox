#include "store.h"

#include <string.h>

#include "log.h"
#include "plat.h"

#define STORE_MAGIC 0x31584254u /* "TBX1" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint32_t crc; /* over seq, key, len, flags, then the payload */
    uint16_t key;
    uint16_t len;
    uint16_t flags;
    uint16_t reserved;
} StoreHdr; /* 20 bytes */

static uint32_t base = 0;
static uint32_t seq_now = 0;
static uint32_t saves = 0;
static bool ready = false;

uint32_t store_crc32(const void* data, uint32_t len, uint32_t crc) {
    const uint8_t* p = data;
    crc = ~crc;
    for(uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for(int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

static uint32_t sector_off(uint16_t key, int which) {
    return base + ((uint32_t)key * 2u + (uint32_t)which) * STORE_SECTOR;
}

static uint32_t rec_crc(const StoreHdr* h, const uint8_t* payload) {
    uint32_t c = store_crc32(&h->seq, 4, 0);
    c = store_crc32(&h->key, 2, c);
    c = store_crc32(&h->len, 2, c);
    c = store_crc32(&h->flags, 2, c);
    if(h->len) c = store_crc32(payload, h->len, c);
    return c;
}

/* Is sector `which` of `key` holding a valid record? Fills hdr on success. */
static bool sector_valid(uint16_t key, int which, StoreHdr* hdr) {
    plat_flash_read(sector_off(key, which), hdr, sizeof(*hdr));
    if(hdr->magic != STORE_MAGIC || hdr->key != key || hdr->len > STORE_MAX_PAYLOAD) return false;
    static uint8_t tmp[STORE_MAX_PAYLOAD];
    if(hdr->len) plat_flash_read(sector_off(key, which) + sizeof(StoreHdr), tmp, hdr->len);
    return rec_crc(hdr, tmp) == hdr->crc;
}

/* Newest valid sector for a key: 0 or 1, or -1 if none. */
static int newest(uint16_t key, StoreHdr* out) {
    StoreHdr h[2];
    const bool v0 = sector_valid(key, 0, &h[0]);
    const bool v1 = sector_valid(key, 1, &h[1]);
    int pick = -1;
    if(v0 && v1) pick = (int32_t)(h[1].seq - h[0].seq) > 0 ? 1 : 0;
    else if(v0) pick = 0;
    else if(v1) pick = 1;
    if(pick >= 0 && out) *out = h[pick];
    return pick;
}

uint32_t store_base(void) {
    return base;
}

uint32_t store_saves(void) {
    return saves;
}

bool store_init(void) {
    const uint32_t size = plat_flash_size();
    if(size < STORE_BYTES + 512 * 1024) {
        LOGE("store: flash too small (%lu)", (unsigned long)size);
        return false;
    }
    base = size - STORE_BYTES;
    seq_now = 0;
    uint32_t used = 0;
    for(uint16_t k = 0; k < STORE_KEYS; k++) {
        StoreHdr h;
        if(newest(k, &h) >= 0) {
            used++;
            if((int32_t)(h.seq - seq_now) > 0) seq_now = h.seq;
        }
    }
    ready = true;
    LOGI("store: base 0x%lx, %lu keys in use, seq %lu", (unsigned long)base, (unsigned long)used, (unsigned long)seq_now);
    return true;
}

int store_load(uint16_t key, void* dst, uint16_t max, uint16_t* flags) {
    if(!ready || key >= STORE_KEYS) return -1;
    StoreHdr h;
    const int which = newest(key, &h);
    if(which < 0) return -1;
    if(h.len > max) return -1;
    if(h.len) plat_flash_read(sector_off(key, which) + sizeof(StoreHdr), dst, h.len);
    if(flags) *flags = h.flags;
    return h.len;
}

bool store_save(uint16_t key, const void* data, uint16_t len, uint16_t flags) {
    if(!ready || key >= STORE_KEYS || len > STORE_MAX_PAYLOAD) return false;
    StoreHdr cur;
    const int old = newest(key, &cur);
    const int target = old == 0 ? 1 : 0; /* the other one (or sector 0 if none) */

    static uint8_t buf[(sizeof(StoreHdr) + STORE_MAX_PAYLOAD + 255) / 256 * 256];
    const uint32_t total = ((uint32_t)sizeof(StoreHdr) + len + 255u) / 256u * 256u;
    memset(buf, 0xFF, total);
    StoreHdr* h = (StoreHdr*)buf;
    h->magic = STORE_MAGIC;
    h->seq = ++seq_now;
    h->key = key;
    h->len = len;
    h->flags = flags;
    h->reserved = 0xFFFF;
    if(len) memcpy(buf + sizeof(StoreHdr), data, len);
    h->crc = rec_crc(h, buf + sizeof(StoreHdr));

    const uint32_t off = sector_off(key, target);
    if(!plat_flash_erase(off, STORE_SECTOR)) {
        LOGE("store: erase failed key %u", key);
        return false;
    }
    if(!plat_flash_prog(off, buf, total)) {
        LOGE("store: program failed key %u", key);
        return false;
    }
    saves++;
    return true;
}

bool store_erase_key(uint16_t key) {
    return store_save(key, NULL, 0, 0);
}

void store_wipe_all(void) {
    if(!ready) return;
    for(uint32_t off = 0; off < STORE_BYTES; off += STORE_SECTOR) plat_flash_erase(base + off, STORE_SECTOR);
    seq_now = 0;
}

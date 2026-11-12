#include "persist.h"

#include <string.h>

#include "log.h"
#include "store.h"

void fp_init(fig_persist_t* fp, uint16_t key_base, uint8_t slots, uint16_t size,
             const uint8_t* (*data)(uint8_t), bool (*present)(uint8_t)) {
    memset(fp, 0, sizeof(*fp));
    fp->key_base = key_base;
    fp->slots = slots;
    fp->size = size;
    fp->data = data;
    fp->present = present;
}

void fp_mark(fig_persist_t* fp, uint8_t slot, uint32_t now, uint32_t delay_ms) {
    if(slot >= fp->slots) return;
    fp->pending[slot] = true;
    fp->due[slot] = now + delay_ms;
}

static void write_slot(fig_persist_t* fp, uint8_t slot) {
    fp->pending[slot] = false;
    if(fp->present(slot) || fp->unsynced[slot]) { /* unsent game progress is kept even if the slot was cleared */
        const bool ok = store_save(
            (uint16_t)(fp->key_base + slot), fp->data(slot), fp->size,
            fp->unsynced[slot] ? STORE_FLAG_UNSYNCED : 0);
        LOGI("persist: key %u %s", (unsigned)(fp->key_base + slot), ok ? "saved" : "FAILED");
    } else {
        store_erase_key((uint16_t)(fp->key_base + slot));
        LOGI("persist: key %u erased", (unsigned)(fp->key_base + slot));
    }
}

void fp_forget(fig_persist_t* fp, uint8_t slot) {
    if(slot >= fp->slots) return;
    if(fp->unsynced[slot]) return; /* never throw away game progress the phone has not got */
    fp->pending[slot] = true; /* write_slot erases it, since the slot is empty now */
    fp->due[slot] = 0;
}

void fp_tick(fig_persist_t* fp, uint32_t now) {
    for(uint8_t i = 0; i < fp->slots; i++) {
        if(fp->pending[i] && (int32_t)(now - fp->due[i]) >= 0) {
            write_slot(fp, i); /* one per call: each write can stall for tens of ms */
            return;
        }
    }
}

void fp_flush(fig_persist_t* fp) {
    for(uint8_t i = 0; i < fp->slots; i++) {
        if(fp->pending[i]) write_slot(fp, i);
    }
}

bool fp_load(fig_persist_t* fp, uint8_t slot, uint8_t* dst, bool* unsynced) {
    uint16_t flags = 0;
    const int n = store_load((uint16_t)(fp->key_base + slot), dst, fp->size, &flags);
    if(n != fp->size) return false;
    if(unsynced) *unsynced = (flags & STORE_FLAG_UNSYNCED) != 0;
    return true;
}

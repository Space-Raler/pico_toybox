/*
 * Debounced saving of fixed-size figures (Infinity 320 B, Skylanders 1 KiB) to the flash store.
 * A game writes a figure many times in a row; we wait until it has been quiet for a while,
 * then write once. Flash erases stall the CPU for tens of milliseconds, so at most one
 * slot is written per call to fp_tick().
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PERSIST_DELAY_MS 5000 /* quiet time before a changed figure is written to flash */
#define FP_MAX_SLOTS 9

typedef struct {
    uint16_t key_base;
    uint8_t slots;
    uint16_t size;
    const uint8_t* (*data)(uint8_t slot); /* the figure's bytes in the running core */
    bool (*present)(uint8_t slot);

    bool pending[FP_MAX_SLOTS];
    uint32_t due[FP_MAX_SLOTS];
    bool unsynced[FP_MAX_SLOTS]; /* changed by the game, phone has not seen it */
} fig_persist_t;

void fp_init(fig_persist_t* fp, uint16_t key_base, uint8_t slots, uint16_t size,
             const uint8_t* (*data)(uint8_t), bool (*present)(uint8_t));
/* Slot content changed (loaded from the phone, or the game wrote it). delay_ms 0 = soon. */
void fp_mark(fig_persist_t* fp, uint8_t slot, uint32_t now, uint32_t delay_ms);
/* Slot was removed on purpose: forget the saved copy (unless it holds unsent game progress). */
void fp_forget(fig_persist_t* fp, uint8_t slot);
void fp_tick(fig_persist_t* fp, uint32_t now);
void fp_flush(fig_persist_t* fp);
/* Read one saved figure into dst (size bytes). Returns true if there is one. Sets *unsynced. */
bool fp_load(fig_persist_t* fp, uint8_t slot, uint8_t* dst, bool* unsynced);

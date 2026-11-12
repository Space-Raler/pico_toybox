/*
 * Persistent key/value store in the last part of the Pico's flash.
 *
 * Each key owns two 4 KiB sectors used ping-pong: a save goes to the sector that is NOT the
 * newest valid one, so a power cut in the middle of a write leaves the previous copy intact.
 * Records carry a sequence number and a CRC32; at boot the newest valid copy wins.
 * Wear: each save erases one sector, and saves alternate between two, so 100k erase cycles
 * means ~200k saves per key. Saves are debounced (see persist.c) so that is years of use.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define STORE_KEYS        32
#define STORE_SECTOR      4096u
#define STORE_BYTES       (STORE_KEYS * 2u * STORE_SECTOR) /* 256 KiB at the end of flash */
#define STORE_MAX_PAYLOAD 1100u

/* Key layout */
#define KEY_SETTINGS 0
#define KEY_INF(slot) (1 + (slot)) /* 9 slots: 1..9   */
#define KEY_SKY(slot) (10 + (slot)) /* 8 slots: 10..17 */
#define KEY_LD(box)   (18 + (box)) /* 7 boxes: 18..24 */

/* Record flag bits */
#define STORE_FLAG_UNSYNCED 0x0001 /* the game changed this and the phone has not seen it */

bool store_init(void);
/* Returns payload length (0 = key empty/erased), or -1 if nothing valid is stored. */
int store_load(uint16_t key, void* dst, uint16_t max, uint16_t* flags);
bool store_save(uint16_t key, const void* data, uint16_t len, uint16_t flags);
bool store_erase_key(uint16_t key); /* saves an empty record */
void store_wipe_all(void);          /* erases every sector (factory reset) */
uint32_t store_saves(void);         /* number of saves since boot (diagnostics) */
uint32_t store_base(void);          /* flash offset where the store starts */

uint32_t store_crc32(const void* data, uint32_t len, uint32_t crc);

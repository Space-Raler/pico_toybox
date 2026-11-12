#pragma once
/* Portable Skylanders Portal of Power state machine.
 * No OS, USB or filesystem dependencies, so it builds on the host for testing. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SKY_SLOT_COUNT 8
#define SKY_BLOCK_SIZE 16
#define SKY_BLOCK_COUNT 0x40
#define SKY_FIGURE_SIZE (SKY_BLOCK_SIZE * SKY_BLOCK_COUNT) /* 1024-byte dump */
#define SKY_PACKET_SIZE 32
#define SKY_STATUS_QUEUE_DEPTH 16
#define SKY_REPLY_QUEUE_DEPTH 64

typedef struct {
    uint8_t status; /* bit0 = present, bit1 = changed */
    uint8_t q[SKY_STATUS_QUEUE_DEPTH]; /* pending status transitions */
    uint8_t q_head;
    uint8_t q_count;
    bool dirty; /* data modified by the game, not yet saved */
    uint32_t last_id; /* serial last seen here (slot retention) */
    uint8_t data[SKY_FIGURE_SIZE];
} SkySlot;

/* Longest fade the app will honour. Caps a bad or unexpected duration so a fade can never stick. */
#define SKY_LIGHT_FADE_MAX_MS 5000

/* What the game has asked the portal lights to do ('C', 'L' and 'J' commands). The core only
 * records it; the app shows it on the Flipper LED and backlight from its own thread, because
 * the LED driver must not be touched from the USB interrupt. `seq` goes up on every change.
 *
 * Command layouts as parsed here (the first byte is the command letter). 'C' is the common one;
 * the L and J layouts are assumed, so check them against the C lines in debug.txt:
 *   C r g b                 ring colour, immediately
 *   L 0 r g b               ring colour, immediately (same as C)
 *   L 1 level               backlight brightness 0..255
 *   L 3 ...                 backlight to full
 *   J 0 r g b ms_lo ms_hi   fade the ring to r,g,b over ms (16-bit little endian)
 * Other sides are ignored. */
typedef struct {
    uint8_t ring[3]; /* target ring colour R, G, B */
    uint16_t fade_ms; /* time to reach `ring` (0 = at once) */
    uint8_t backlight; /* 0..255, meaningful when backlight_set */
    bool backlight_set;
    uint32_t seq;
} SkyLights;

typedef struct {
    SkySlot slots[SKY_SLOT_COUNT];
    SkyLights lights;
    bool activated;
    uint8_t fw_version; /* byte 2 of the reply to 'R' (default 0x1B) */
    volatile bool speaker_on; /* the host's last 'M' (speaker on/off); read from other contexts */
    uint8_t interrupt_counter;
    uint8_t r_queue[SKY_REPLY_QUEUE_DEPTH][SKY_PACKET_SIZE];
    uint8_t r_head;
    uint8_t r_count;
    uint32_t block_writes; /* game block writes that changed a figure (diagnostics) */
} SkyBase;

void sky_base_init(SkyBase* b);

/* Load a 1024-byte dump. Returns slot index 0..7, or -1 if the portal is full. */
int sky_base_load_figure(SkyBase* b, const uint8_t data[SKY_FIGURE_SIZE]);
/* Load into a specific free slot. Returns slot, or -1 if the slot is invalid or occupied. */
int sky_base_load_figure_at(SkyBase* b, uint8_t slot, const uint8_t data[SKY_FIGURE_SIZE]);
/* Remove a figure. Returns false if the slot was empty. */
bool sky_base_remove_figure(SkyBase* b, uint8_t slot);
bool sky_base_slot_present(const SkyBase* b, uint8_t slot);
/* Presence once every pending transition has been delivered to the game. While the game is still
 * reading the status queue, sky_base_slot_present() can show a figure as gone or back mid-swap;
 * this is what the PC page should be told. */
bool sky_base_slot_present_settled(const SkyBase* b, uint8_t slot);

/* If the slot has unsaved changes, copy its data to out, clear the flag, return true. */
bool sky_base_take_dirty(SkyBase* b, uint8_t slot, uint8_t out[SKY_FIGURE_SIZE]);
void sky_base_mark_dirty(SkyBase* b, uint8_t slot);

typedef struct {
    bool present;
    bool dirty;
    uint32_t serial; /* first 4 bytes of block 0 */
    uint16_t type_id; /* first 2 bytes of block 1 (character/toy id) */
} SkySlotInfo;
void sky_base_slot_info(const SkyBase* b, uint8_t slot, SkySlotInfo* out);

/* Drop queued command replies (call when the host (re)configures the device). */
void sky_base_clear_replies(SkyBase* b);

/* Handle one output report / SET_REPORT payload from the host. Malformed input is ignored. */
void sky_base_handle_command(SkyBase* b, const uint8_t* buf, size_t len);

/* Produce the next 32-byte interrupt-IN packet: a queued reply if any,
 * otherwise a status packet. Returns true if it was a queued reply. */
bool sky_base_next_packet(SkyBase* b, uint8_t out[SKY_PACKET_SIZE]);

#ifdef __cplusplus
}
#endif

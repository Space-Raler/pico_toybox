/*
 * LEGO Dimensions toy pad: token table and USB command handling.
 * A port of the Flipper app's ld/ToyPadEmu.c and the command switch in ld/usb/usb_toypad.c,
 * with no Flipper calls and no blocking delays (the timed placing lives in mode_ld.c).
 * Pure C, no hardware: it is unit-tested on a PC.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LD_TOKENS 7
#define LD_BOXES 7
#define LD_TOKEN_BYTES 180

typedef struct {
    bool used;
    uint8_t index; /* the index the console uses for this token */
    uint32_t id;   /* minifigure id; 0 = vehicle (its id is inside the token data) */
    uint8_t pad;   /* 1 centre, 2 left, 3 right */
    uint8_t uid[7];
    uint8_t token[LD_TOKEN_BYTES];
    uint8_t box;   /* which of the 7 boxes the phone/UI put it in */
} LdToken;

static inline bool ld_is_vehicle(const LdToken* t) {
    return t->id == 0;
}
uint16_t ld_vehicle_id(const LdToken* t);

void ld_pad_init(void);
/* Forget every token without telling the console (it just woke up and knows nothing). */
void ld_pad_forget_all(void);

/* Build tokens the way the Flipper app does. */
void ld_make_minifig(LdToken* t, uint16_t id);
void ld_make_vehicle(LdToken* t, uint16_t id, const uint32_t upgrades[2]);
void ld_make_vehicle_from_saved(LdToken* t, const uint8_t uid[7], const uint8_t data[LD_TOKEN_BYTES]);

/* Immediate operations; each queues the "placed"/"removed" event for the console. */
bool ld_pad_place_now(const LdToken* proto, uint8_t box);   /* false: no free token slot */
bool ld_pad_remove_box_now(uint8_t box);                    /* false: box empty */
bool ld_pad_uid_present(const uint8_t uid[7], uint8_t* box_out);

bool ld_pad_box_filled(uint8_t box);
LdToken* ld_pad_box_token(uint8_t box); /* NULL if empty */
uint8_t ld_pad_token_count(void);
LdToken* ld_pad_token_at(uint8_t index); /* by the console's token index */

/* USB side */
void ld_pad_handle_out(const uint8_t* buf, uint16_t len);
bool ld_pad_in_peek(uint8_t out[32]);
void ld_pad_in_drop(void);
void ld_pad_in_clear(void);
uint8_t ld_pad_in_count(void);

/* Console woke the pad (CMD_WAKE) since the last call? Cleared by reading. */
bool ld_pad_take_wake(void);
bool ld_pad_awake(void);
void ld_pad_set_awake(bool awake);
/* Bit n set = the game wrote to token index n since the last call (cleared by reading). */
uint8_t ld_pad_take_dirty_mask(void);
uint32_t ld_pad_commands(void);
uint8_t ld_pad_last_cmd(void);

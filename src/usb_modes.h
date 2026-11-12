/* USB device side: one TinyUSB class driver that serves whichever base is active. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "mode.h"

typedef struct {
    uint32_t in_ok, in_busy_fail, out_packets, ctrl_requests, ctrl_stalled;
    uint32_t configured_count, switches;
} usbm_stats_t;

void usbm_init(const tb_mode_def_t* mode, uint8_t game);
void usbm_task(void); /* call every main-loop pass: runs TinyUSB, the IN pump, mode switches */
/* Disconnect USB and come back as another base. Same mode again = just re-plug. */
void usbm_request_mode(uint8_t mode_id, uint8_t game);
const tb_mode_def_t* usbm_mode(void);
uint8_t usbm_game(void);
bool usbm_switching(void);
bool usbm_is_configured(void);
const usbm_stats_t* usbm_stats(void);

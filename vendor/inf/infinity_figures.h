/*
 * Disney Infinity figure files: name table and number decoding. (The blank-figure
 * generator below exists for tests only.)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "infinity_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    uint8_t series; /* 1 = Infinity 1.0, 2 = 2.0, 3 = 3.0 */
    const char* name;
} InfFigureInfo;

extern const InfFigureInfo inf_figure_table[];
extern const size_t inf_figure_table_count;

const InfFigureInfo* inf_figure_lookup(uint32_t id);

/* Figure number stored (encrypted) in a 320-byte figure image. */
uint32_t inf_figure_decode_number(const uint8_t data[INF_FIGURE_SIZE]);

#ifdef INF_ENABLE_BLANK_FIGURES
/* Test-only (build with -DINF_ENABLE_BLANK_FIGURES; the app does not define it).
 * Build a blank figure image. uid7 is the 7-byte tag UID. Returns false for a
 * figure number the format cannot hold. */
bool inf_figure_create_blank(
    uint8_t out[INF_FIGURE_SIZE],
    uint32_t figure_number,
    uint8_t series,
    const uint8_t uid7[7]);
#endif

#ifdef __cplusplus
}
#endif

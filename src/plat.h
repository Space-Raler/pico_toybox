/* Thin platform layer so the logic files build on a PC for tests. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t plat_ms(void);                 /* milliseconds since boot */
uint32_t plat_irq_save(void);           /* disable interrupts, return old state */
void plat_irq_restore(uint32_t state);

/* Flash (the persistent store lives in the last part of it). Offsets are from the
 * start of flash. erase: multiples of 4096; prog: multiples of 256. */
void plat_flash_read(uint32_t off, void* dst, uint32_t len);
bool plat_flash_erase(uint32_t off, uint32_t len);
bool plat_flash_prog(uint32_t off, const void* src, uint32_t len);
uint32_t plat_flash_size(void);

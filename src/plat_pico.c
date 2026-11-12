#include "plat.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

uint32_t plat_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

uint32_t plat_irq_save(void) {
    return save_and_disable_interrupts();
}

void plat_irq_restore(uint32_t state) {
    restore_interrupts(state);
}

uint32_t plat_flash_size(void) {
    return PICO_FLASH_SIZE_BYTES;
}

void plat_flash_read(uint32_t off, void* dst, uint32_t len) {
    memcpy(dst, (const void*)(XIP_BASE + off), len);
}

typedef struct {
    uint32_t off, len;
    const void* src;
} flash_job_t;

static void do_erase(void* p) {
    const flash_job_t* j = p;
    flash_range_erase(j->off, j->len);
}

static void do_prog(void* p) {
    const flash_job_t* j = p;
    flash_range_program(j->off, j->src, j->len);
}

bool plat_flash_erase(uint32_t off, uint32_t len) {
    flash_job_t j = {off, len, NULL};
    return flash_safe_execute(do_erase, &j, 1000) == PICO_OK;
}

bool plat_flash_prog(uint32_t off, const void* src, uint32_t len) {
    flash_job_t j = {off, len, src};
    return flash_safe_execute(do_prog, &j, 1000) == PICO_OK;
}

/*
 * RAM log. Everything the firmware wants to say goes here; the phone reads it over the
 * same Bluetooth link as the figures (see web/pico.html), so no serial cable is needed.
 *
 * Levels: 0 errors only, 1 events (default), 2 every USB packet / command (chatty).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define LOG_BUF_SIZE 12288

void log_init(void);
void logf_(uint8_t level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void log_hex(uint8_t level, const char* label, const uint8_t* data, size_t len);

#define LOGE(...) logf_(0, __VA_ARGS__)
#define LOGI(...) logf_(1, __VA_ARGS__)
#define LOGD(...) logf_(2, __VA_ARGS__)

void log_set_level(uint8_t level);
uint8_t log_get_level(void);
void log_clear(void);

/* Reader: bytes are numbered from boot. *cursor is where the reader is; if it fell more than
 * LOG_BUF_SIZE behind, it is moved forward (the lost part is gone). Returns bytes copied. */
uint32_t log_total(void);
size_t log_read(uint32_t* cursor, uint8_t* dst, size_t max);

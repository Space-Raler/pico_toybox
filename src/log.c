#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "plat.h"

static uint8_t ring[LOG_BUF_SIZE];
static volatile uint32_t total = 0; /* bytes ever written */
static uint8_t level_now = 1;

void log_init(void) {
    total = 0;
    level_now = 1;
}

void log_set_level(uint8_t level) {
    level_now = level > 2 ? 2 : level;
}

uint8_t log_get_level(void) {
    return level_now;
}

void log_clear(void) {
    /* Keep numbering monotonic so readers' cursors stay valid. */
    const uint32_t irq = plat_irq_save();
    memset(ring, 0, sizeof(ring));
    total += LOG_BUF_SIZE; /* readers jump to the (empty) end */
    plat_irq_restore(irq);
}

static void put(const char* s, size_t n) {
    const uint32_t irq = plat_irq_save();
    uint32_t t = total;
    for(size_t i = 0; i < n; i++) ring[(t + i) % LOG_BUF_SIZE] = (uint8_t)s[i];
    total = t + (uint32_t)n;
    plat_irq_restore(irq);
}

void logf_(uint8_t level, const char* fmt, ...) {
    if(level > level_now) return;
    char line[160];
    const uint32_t ms = plat_ms();
    int n = snprintf(
        line,
        sizeof(line),
        "%lu.%03lu %c ",
        (unsigned long)(ms / 1000),
        (unsigned long)(ms % 1000),
        level == 0 ? 'E' : (level == 1 ? 'I' : 'D'));
    if(n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if(m < 0) m = 0;
    size_t len = (size_t)n + (size_t)m;
    if(len > sizeof(line) - 2) len = sizeof(line) - 2;
    line[len++] = '\n';
    put(line, len);
}

void log_hex(uint8_t level, const char* label, const uint8_t* data, size_t len) {
    if(level > level_now) return;
    char line[160];
    size_t pos = 0;
    const size_t show = len > 24 ? 24 : len;
    pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%s(%u):", label, (unsigned)len);
    for(size_t i = 0; i < show && pos + 4 < sizeof(line); i++) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " %02x", data[i]);
    }
    if(show < len && pos + 4 < sizeof(line)) pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " ..");
    logf_(level, "%s", line);
}

uint32_t log_total(void) {
    return total;
}

size_t log_read(uint32_t* cursor, uint8_t* dst, size_t max) {
    const uint32_t irq = plat_irq_save();
    const uint32_t t = total;
    if(t - *cursor > LOG_BUF_SIZE) *cursor = t - LOG_BUF_SIZE; /* fell behind */
    size_t n = t - *cursor;
    if(n > max) n = max;
    for(size_t i = 0; i < n; i++) dst[i] = ring[(*cursor + i) % LOG_BUF_SIZE];
    *cursor += (uint32_t)n;
    plat_irq_restore(irq);
    return n;
}

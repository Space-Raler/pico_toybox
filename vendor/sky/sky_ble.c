/* Skylanders BLE link, message handling. See sky_ble.h. SPDX-License-Identifier: GPL-2.0-only */
#include "sky_ble.h"

#include <string.h>

#include "../inf/ble_proto.h"

static void lock(SkyBle* b) {
    if(b->lock) b->lock();
}

static void unlock(SkyBle* b) {
    if(b->unlock) b->unlock();
}

void sky_ble_init(SkyBle* b, SkyBase* base, SkyBleSendFn send, void* send_ctx) {
    memset(b, 0, sizeof(*b));
    b->base = base;
    b->send = send;
    b->send_ctx = send_ctx;
}

void sky_ble_set_file_ops(
    SkyBle* b, void* file_ctx, SkyBleListDumpsFn list_dumps,
    SkyBleReadDumpFn read_dump, SkyBleWriteDumpFn write_dump) {
    b->file_ctx = file_ctx;
    b->list_dumps = list_dumps;
    b->read_dump = read_dump;
    b->write_dump = write_dump;
}

void sky_ble_set_list_ops(
    SkyBle* b, SkyBleListBeginFn begin, SkyBleListStepFn step, SkyBleListEndFn end) {
    b->list_begin = begin;
    b->list_step = step;
    b->list_end = end;
}

/* ---- incremental, batched dump listing ----
 * One DUMP_LIST_ITEM frame carries several lines separated by '\n'. A line starting with '>'
 * sets the current directory; any other line is a file name inside it (a line that starts with
 * '/' is a complete path). Grouping by directory keeps the stream small, which is what makes
 * the listing finish quickly over the slow BLE serial link. An empty frame ends the listing. */
static bool list_flush(SkyBle* b) {
    if(!b->list_len) return true;
    const bool ok = b->send(b->send_ctx, BleMsgDumpListItem, b->list_buf, b->list_len);
    b->list_len = 0;
    if(ok) b->list_frames++;
    return ok;
}

static void list_emit(void* ctx, const char* path) {
    SkyBle* b = ctx;
    if(!b->list_active || b->list_failed || !path) return;
    const char* slash = strrchr(path, '/');
    if(!slash || slash == path) return;
    const size_t dir_len = (size_t)(slash - path);
    const char* name = slash + 1;
    const size_t name_len = strlen(name);
    if(!name_len || dir_len >= sizeof(b->list_dir) || name_len > 120) return;
    const bool new_dir = dir_len != b->list_dir_len || memcmp(b->list_dir, path, dir_len) != 0;
    const size_t need = (new_dir ? 1 + dir_len + 1 : 0) + name_len + 1;
    if(b->list_len + need > SKY_BLE_LIST_BATCH) {
        if(!list_flush(b)) { b->list_failed = true; return; }
    }
    if(new_dir) {
        b->list_buf[b->list_len++] = '>';
        memcpy(b->list_buf + b->list_len, path, dir_len);
        b->list_len = (uint16_t)(b->list_len + dir_len);
        b->list_buf[b->list_len++] = '\n';
        memcpy(b->list_dir, path, dir_len);
        b->list_dir_len = (uint16_t)dir_len;
    }
    memcpy(b->list_buf + b->list_len, name, name_len);
    b->list_len = (uint16_t)(b->list_len + name_len);
    b->list_buf[b->list_len++] = '\n';
    b->list_found++;
}

static void list_close(SkyBle* b) {
    if(b->list_active && b->list_end) b->list_end(b->file_ctx);
    b->list_active = false;
}

/* Send what is left, then the empty terminator, and release the lister. A failed send is not
 * retried over and over: the PC side has its own timeout and keeps what it already received. */
static void list_finish(SkyBle* b) {
    if(!b->list_failed) list_flush(b);
    list_close(b);
    b->send(b->send_ctx, BleMsgDumpListItem, NULL, 0);
}

void sky_ble_list_tick(SkyBle* b) {
    if(!b->list_active || !b->list_step) return;
    const uint16_t frames_before = b->list_frames;
    for(uint8_t i = 0; i < SKY_BLE_LIST_STEPS; i++) {
        const bool more = b->list_step(b->file_ctx, list_emit, b);
        if(b->list_failed) {
            /* Radio stopped taking data: give up now instead of timing out on every file. */
            list_close(b);
            b->send(b->send_ctx, BleMsgDumpListItem, NULL, 0);
            return;
        }
        if(!more) { list_finish(b); return; }
        if(b->list_frames != frames_before) return; /* one radio frame per poll */
    }
}

void sky_ble_list_cancel(SkyBle* b) {
    list_close(b);
}

static void send_list_item(void* ctx, const char* path) {
    SkyBle* b = ctx;
    const size_t n = path ? strlen(path) : 0;
    if(n <= 250) b->send(b->send_ctx, BleMsgDumpListItem, (const uint8_t*)path, (uint16_t)n);
}

static void send_result(SkyBle* b, uint8_t request, uint8_t slot, uint8_t code) {
    const uint8_t payload[3] = {request, slot, code};
    b->send(b->send_ctx, BleMsgResult, payload, sizeof(payload));
}

static void send_status(SkyBle* b) {
    uint16_t mask = 0;
    lock(b);
    for(uint8_t i = 0; i < SKY_SLOT_COUNT; i++) {
        if(sky_base_slot_present_settled(b->base, i)) mask |= (uint16_t)(1u << i);
    }
    unlock(b);
    const uint8_t payload[5] = {
        BLE_PROTO_VERSION_SKY, b->usb_state, (uint8_t)(mask & 0xFF), (uint8_t)(mask >> 8), b->game};
    b->send(b->send_ctx, BleMsgStatus, payload, sizeof(payload));
}

void sky_ble_on_message(void* ctx, uint8_t type, const uint8_t* p, uint16_t len) {
    SkyBle* b = ctx;
    switch(type) {
    case BleMsgHello:
    case BleMsgStatusReq:
        send_status(b);
        break;
    case BleMsgLoad: {
        const uint8_t slot = len ? p[0] : 0;
        if(len != 1 + BLE_PROTO_SKY_FIGURE) {
            send_result(b, type, slot, BleResultBadLength);
        } else if(slot >= SKY_SLOT_COUNT) {
            send_result(b, type, slot, BleResultBadSlot);
        } else {
            int at;
            lock(b);
            if(sky_base_slot_present(b->base, slot)) sky_base_remove_figure(b->base, slot);
            at = sky_base_load_figure_at(b->base, slot, p + 1);
            unlock(b);
            b->remote[slot] = at == slot;
            send_result(b, type, slot, at == slot ? BleResultOk : BleResultUnknown);
            send_status(b);
        }
        break;
    }
    case BleMsgRemove: {
        const uint8_t slot = len ? p[0] : 0;
        if(len != 1 || slot >= SKY_SLOT_COUNT) {
            send_result(b, type, slot, BleResultBadSlot);
        } else {
            lock(b);
            const bool removed = sky_base_remove_figure(b->base, slot);
            unlock(b);
            b->remote[slot] = false;
            send_result(b, type, slot, removed ? BleResultOk : BleResultNotPresent);
            send_status(b);
        }
        break;
    }
    case BleMsgClear:
        lock(b);
        for(uint8_t i = 0; i < SKY_SLOT_COUNT; i++) {
            if(sky_base_slot_present(b->base, i)) sky_base_remove_figure(b->base, i);
        }
        unlock(b);
        memset(b->remote, 0, sizeof(b->remote));
        send_result(b, type, 0, BleResultOk);
        send_status(b);
        break;
    case BleMsgDumpList:
        if(b->list_begin && b->list_step) {
            list_close(b); /* a new request restarts the listing */
            b->list_len = 0; b->list_dir_len = 0; b->list_dir[0] = '\0';
            b->list_frames = 0; b->list_found = 0; b->list_failed = false;
            b->list_begin(b->file_ctx);
            b->list_active = true; /* sky_ble_list_tick() does the work */
            break;
        }
        if(b->list_dumps) b->list_dumps(b->file_ctx, send_list_item, b);
        /* Empty list item terminates the stream. */
        b->send(b->send_ctx, BleMsgDumpListItem, NULL, 0);
        break;
    case BleMsgDumpRead: {
        char path[256];
        uint8_t data[SKY_FIGURE_SIZE];
        if(!len || len >= sizeof(path) || !b->read_dump) {
            send_result(b, type, 0, BleResultBadLength);
            break;
        }
        memcpy(path, p, len); path[len] = '\0';
        if(b->read_dump(b->file_ctx, path, data)) b->send(b->send_ctx, BleMsgDumpData, data, sizeof(data));
        else send_result(b, type, 0, BleResultNotPresent);
        break;
    }
    case BleMsgDumpWriteBegin:
        if(!len || len >= sizeof(b->write_path)) {
            b->write_active = false; send_result(b, type, 0, BleResultBadLength); break;
        }
        memcpy(b->write_path, p, len); b->write_path[len] = '\0';
        b->write_offset = 0; b->write_active = true;
        send_result(b, type, 0, BleResultOk);
        break;
    case BleMsgDumpWriteChunk: {
        if(!b->write_active || len < 3) { send_result(b, type, 0, BleResultBadLength); break; }
        const uint16_t offset = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        const uint16_t n = (uint16_t)(len - 2);
        if(offset != b->write_offset || (uint32_t)offset + n > SKY_FIGURE_SIZE) {
            b->write_active = false; send_result(b, type, 0, BleResultBadLength); break;
        }
        memcpy(b->write_data + offset, p + 2, n); b->write_offset = (uint16_t)(offset + n);
        break;
    }
    case BleMsgDumpWriteEnd: {
        const bool complete = b->write_active && b->write_offset == SKY_FIGURE_SIZE;
        const bool ok = complete && b->write_dump && b->write_dump(b->file_ctx, b->write_path, b->write_data);
        b->write_active = false; b->write_offset = 0; b->write_path[0] = '\0';
        send_result(b, type, 0, ok ? BleResultOk : (complete ? BleResultNotPresent : BleResultBadLength));
        break;
    }
    default:
        send_result(b, type, 0, BleResultUnknown);
        break;
    }
}

void sky_ble_poll_saves(
    SkyBle* b,
    uint8_t scratch[SKY_FIGURE_SIZE],
    SkyBleSaveFn unsent,
    void* unsent_ctx) {
    static uint8_t frame[1 + SKY_FIGURE_SIZE];
    for(uint8_t i = 0; i < SKY_SLOT_COUNT; i++) {
        if(!b->remote[i]) continue;
        lock(b);
        const bool dirty = sky_base_take_dirty(b->base, i, scratch);
        unlock(b);
        if(!dirty) continue;
        frame[0] = i;
        memcpy(frame + 1, scratch, SKY_FIGURE_SIZE);
        if(b->send(b->send_ctx, BleMsgSave, frame, sizeof(frame))) {
            b->fails[i] = 0;
            b->saves_sent++;
            continue;
        }
        b->saves_failed++;
        if(b->fails[i] + 1 < SKY_BLE_SAVE_RETRIES) {
            /* Probably a busy radio, not a lost link: keep the data dirty and try again on the
             * next poll (the game may change it again meanwhile; the newest copy is sent). */
            b->fails[i]++;
            lock(b);
            sky_base_mark_dirty(b->base, i);
            unlock(b);
        } else {
            b->fails[i] = 0;
            if(unsent) unsent(unsent_ctx, i, scratch);
        }
    }
}

void sky_ble_flush_unsent(
    SkyBle* b,
    uint8_t scratch[SKY_FIGURE_SIZE],
    SkyBleSaveFn unsent,
    void* unsent_ctx) {
    for(uint8_t i = 0; i < SKY_SLOT_COUNT; i++) {
        if(!b->remote[i]) continue;
        lock(b);
        const bool dirty = sky_base_take_dirty(b->base, i, scratch);
        unlock(b);
        if(dirty && unsent) unsent(unsent_ctx, i, scratch);
    }
    memset(b->remote, 0, sizeof(b->remote));
}

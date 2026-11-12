#include "ble_link.h"

#include <string.h>

#include "btstack.h"
#include "pico/async_context.h"
#include "pico/cyw43_arch.h"

#include "log.h"
#include "toybox.h" /* generated from toybox.gatt */
#include "../vendor/inf/ble_proto.h"

#define RX_SIZE 4096
#define TX_SIZE 16384 /* eight 1 KiB Skylanders SAVE frames fit */

static uint8_t rx[RX_SIZE];
static volatile uint32_t rx_head, rx_tail; /* head: written by BTstack ctx, tail: main loop */
static uint8_t tx[TX_SIZE];
static volatile uint32_t tx_head, tx_tail; /* head: main loop, tail: BTstack ctx */
static volatile uint32_t rx_dropped = 0;

static volatile hci_con_handle_t con = HCI_CON_HANDLE_INVALID;
static volatile bool notify_on = false;
static volatile bool send_requested = false;
static volatile uint32_t epoch = 0;
static volatile uint16_t mtu = 23;

static btstack_packet_callback_registration_t hci_cb;

static const uint8_t adv_data[] = {
    2, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    11, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME, 'P', 'i', 'c', 'o', 'T', 'o', 'y', 'B', 'o', 'x',
};
static const uint8_t scan_resp[] = {
    17, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E,
};

static inline uint32_t ring_used(uint32_t head, uint32_t tail, uint32_t size) {
    return (head - tail + size) % size;
}

static void reset_rings(void) {
    rx_head = rx_tail = 0;
    tx_head = tx_tail = 0;
}

static int att_write_cb(
    hci_con_handle_t con_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset,
    uint8_t* buffer, uint16_t buffer_size) {
    (void)transaction_mode;
    (void)offset;
    if(att_handle == ATT_CHARACTERISTIC_6E400003_B5A3_F393_E0A9_E50E24DCCA9E_01_CLIENT_CONFIGURATION_HANDLE) {
        notify_on = little_endian_read_16(buffer, 0) == GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION;
        con = con_handle;
        send_requested = false;
        return 0;
    }
    if(att_handle == ATT_CHARACTERISTIC_6E400002_B5A3_F393_E0A9_E50E24DCCA9E_01_VALUE_HANDLE) {
        uint32_t head = rx_head;
        for(uint16_t i = 0; i < buffer_size; i++) {
            const uint32_t next = (head + 1) % RX_SIZE;
            if(next == rx_tail) {
                rx_dropped += (uint32_t)(buffer_size - i);
                break;
            }
            rx[head] = buffer[i];
            head = next;
        }
        __sync_synchronize();
        rx_head = head;
    }
    return 0;
}

/* Runs in the BTstack context: send one notification, then ask for the next slot. */
static void send_chunk(void) {
    send_requested = false;
    if(con == HCI_CON_HANDLE_INVALID || !notify_on) return;
    uint32_t tail = tx_tail;
    const uint32_t used = ring_used(tx_head, tail, TX_SIZE);
    if(!used) return;
    uint16_t chunk = (uint16_t)(att_server_get_mtu(con) - 3);
    if(chunk < 20) chunk = 20;
    if(chunk > 244) chunk = 244;
    if(chunk > used) chunk = (uint16_t)used;
    uint8_t buf[244];
    for(uint16_t i = 0; i < chunk; i++) buf[i] = tx[(tail + i) % TX_SIZE];
    const uint8_t r = att_server_notify(
        con, ATT_CHARACTERISTIC_6E400003_B5A3_F393_E0A9_E50E24DCCA9E_01_VALUE_HANDLE, buf, chunk);
    if(r == ERROR_CODE_SUCCESS) {
        __sync_synchronize();
        tx_tail = (tail + chunk) % TX_SIZE;
    }
    if(ring_used(tx_head, tx_tail, TX_SIZE)) {
        send_requested = true;
        att_server_request_can_send_now_event(con);
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    (void)channel;
    (void)size;
    if(packet_type != HCI_EVENT_PACKET) return;
    switch(hci_event_packet_get_type(packet)) {
    case HCI_EVENT_LE_META:
        if(hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
            con = hci_subevent_le_connection_complete_get_connection_handle(packet);
            notify_on = false;
            send_requested = false;
            mtu = 23;
            reset_rings();
            epoch++;
            LOGI("ble: phone connected");
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        con = HCI_CON_HANDLE_INVALID;
        notify_on = false;
        send_requested = false;
        epoch++;
        LOGI("ble: phone disconnected");
        gap_advertisements_enable(1);
        break;
    case ATT_EVENT_MTU_EXCHANGE_COMPLETE:
        mtu = att_event_mtu_exchange_complete_get_MTU(packet);
        LOGI("ble: MTU %u", (unsigned)mtu);
        break;
    case ATT_EVENT_CAN_SEND_NOW:
        send_chunk();
        break;
    default:
        break;
    }
}

void ble_link_init(void) {
    l2cap_init();
    sm_init();
    att_server_init(profile_data, NULL, att_write_cb);

    hci_cb.callback = &packet_handler;
    hci_add_event_handler(&hci_cb);
    att_server_register_packet_handler(packet_handler);

    bd_addr_t null_addr;
    memset(null_addr, 0, 6);
    gap_advertisements_set_params(0x0030, 0x0030, 0, 0, null_addr, 0x07, 0x00); /* ~30 ms */
    gap_advertisements_set_data(sizeof(adv_data), (uint8_t*)adv_data);
    gap_scan_response_set_data(sizeof(scan_resp), (uint8_t*)scan_resp);
    gap_advertisements_enable(1);

    hci_power_control(HCI_POWER_ON);
    LOGI("ble: advertising as PicoToyBox");
}

bool ble_link_connected(void) {
    return con != HCI_CON_HANDLE_INVALID && notify_on;
}

uint32_t ble_link_epoch(void) { return epoch; }
uint32_t ble_link_rx_dropped(void) { return rx_dropped; }
uint16_t ble_link_mtu(void) { return mtu; }

size_t ble_link_read(uint8_t* dst, size_t max) {
    size_t n = 0;
    uint32_t tail = rx_tail;
    const uint32_t head = rx_head;
    while(tail != head && n < max) {
        dst[n++] = rx[tail];
        tail = (tail + 1) % RX_SIZE;
    }
    __sync_synchronize();
    rx_tail = tail;
    return n;
}

size_t ble_link_tx_free(void) {
    return TX_SIZE - 1 - ring_used(tx_head, tx_tail, TX_SIZE);
}

bool ble_link_send(uint8_t type, const uint8_t* payload, uint16_t len) {
    if(!ble_link_connected() || len > BLE_PROTO_MAX_PAYLOAD) return false;
    static uint8_t frame[BLE_PROTO_MAX_FRAME];
    const size_t total = ble_proto_encode(type, payload, len, frame);
    if(total > ble_link_tx_free()) return false; /* all or nothing, caller retries later */
    uint32_t head = tx_head;
    for(size_t i = 0; i < total; i++) tx[(head + i) % TX_SIZE] = frame[i];
    __sync_synchronize();
    tx_head = (head + (uint32_t)total) % TX_SIZE;
    return true;
}

static uint32_t kick_in_ctx(void* param) {
    (void)param;
    if(!send_requested && con != HCI_CON_HANDLE_INVALID && notify_on &&
       ring_used(tx_head, tx_tail, TX_SIZE)) {
        send_requested = true;
        att_server_request_can_send_now_event(con);
    }
    return 0;
}

void ble_link_task(void) {
    if(!send_requested && ring_used(tx_head, tx_tail, TX_SIZE)) {
        async_context_execute_sync(cyw43_arch_async_context(), kick_in_ctx, NULL);
    }
}

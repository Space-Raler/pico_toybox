/*
 * Bluetooth LE serial link (Nordic UART service) on the Pico 2 W, using BTstack.
 * BTstack callbacks run in a low-priority interrupt context, so they only move bytes in and out
 * of two lock-free ring buffers; the main loop does all the protocol work.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ble_link_init(void);
bool ble_link_connected(void);
uint32_t ble_link_epoch(void);                       /* changes on every new connection */
size_t ble_link_read(uint8_t* dst, size_t max);      /* bytes the phone sent */
bool ble_link_send(uint8_t type, const uint8_t* payload, uint16_t len); /* queue one frame */
size_t ble_link_tx_free(void);
void ble_link_task(void);                            /* main loop: starts radio sends */
uint32_t ble_link_rx_dropped(void);
uint16_t ble_link_mtu(void);

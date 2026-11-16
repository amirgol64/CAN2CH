#pragma once

/* The forwarding core: CAN1 receive task, CAN2 task, counters. */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "can_frame.h"

typedef struct {
    atomic_uint can1_rx;      /* frames received on CAN1 */
    atomic_uint can1_tx;      /* test frames sent on CAN1 */
    atomic_uint forwarded;    /* frames queued for CAN2 */
    atomic_uint blocked;      /* frames stopped by the blocklist */
    atomic_uint not_forwarded; /* frames received while forwarding was off */
    atomic_uint dropped;      /* frames lost: queue full or CAN2 not sending */
    atomic_uint can2_tx;      /* frames sent on CAN2 */
    atomic_uint can2_tx_fail; /* transmissions given up after a timeout */
    atomic_uint can2_rx;      /* frames received on CAN2 */
    atomic_uint can2_rx_ovf;  /* CAN2 receive buffer overflows */
    atomic_uint can2_busoff;  /* CAN2 bus-off events */
    atomic_uint bits[2];      /* approximate bits seen per bus, for bus load */
} gateway_counters_t;

typedef struct {
    const char *state; /* "absent", "active", "warning", "passive", "bus-off" */
    uint8_t tec;
    uint8_t rec;
    uint8_t xtal_mhz; /* crystal frequency in use */
} can2_status_t;

extern gateway_counters_t g_counters;

/* Start both tasks on core 1. Returns once CAN1 has started (or failed). */
bool gateway_start(void);

void gateway_get_can2_status(can2_status_t *st);
unsigned gateway_queue_depth(void);
/* Most frames ever waiting for CAN2, and the capacity of that queue. */
unsigned gateway_queue_peak(void);
unsigned gateway_queue_size(void);
/* Reset the high-water marks of both queues. */
void gateway_reset_peaks(void);

/*
 * Settings. Each updates g_cfg, applies the change to the running bus and
 * schedules a save. Returns false if a value is not valid.
 */
bool gateway_set_bus1(uint16_t kbps, bool listen_only);
bool gateway_set_bus2(uint16_t kbps, uint8_t xtal_mhz);
void gateway_set_forwarding(bool enabled);
/* Re-apply g_cfg to both buses (after a config import). */
void gateway_apply_config(void);

/* Send a test frame. bus: 1 or 2. */
esp_err_t gateway_send(int bus, const can_frame_t *f);

#pragma once

/* CAN1: the ESP32-S3 native TWAI controller. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "can_frame.h"

#define CAN1_TX_GPIO 15
#define CAN1_RX_GPIO 16

typedef struct {
    const char *state; /* "stopped", "running", "bus-off", "recovering" */
    uint32_t tec;
    uint32_t rec;
    uint32_t bus_errors;
    uint32_t rx_missed; /* frames lost in the driver (queue full or FIFO overrun) */
    uint32_t arb_lost;
    uint32_t busoff_count;
    uint32_t rx_queue_peak; /* most frames ever waiting in the driver queue */
} can1_status_t;

/* Frames the driver can hold while the receive task cannot run. */
#define CAN1_RX_QUEUE_LEN 1024

/*
 * Install and start the driver. The driver's interrupt runs on the calling
 * core, so call this (and can1_stop) from the receive task only.
 */
esp_err_t can1_start(uint16_t kbps, bool listen_only);
void can1_stop(void);
bool can1_running(void);

/* Receive task only. */
bool can1_receive(can_frame_t *f, TickType_t timeout);
/* Receive task only: handles bus-off and recovery. */
void can1_poll_alerts(void);
/* Receive task only: record how full the driver queue is. */
void can1_sample_queue(void);
void can1_reset_peak(void);

/* Any task. */
esp_err_t can1_transmit(const can_frame_t *f);
void can1_get_status(can1_status_t *st);

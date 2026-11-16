#pragma once

/*
 * CAN2: XL2515 (MCP2515-compatible) stand-alone CAN controller on SPI.
 * Every function here must be called from one task only (the CAN2 task).
 */

#include <stdbool.h>
#include <stdint.h>

#include "can_frame.h"

#define XL2515_CS_GPIO 17
#define XL2515_INT_GPIO 18
#define XL2515_SCLK_GPIO 21
#define XL2515_MOSI_GPIO 41
#define XL2515_MISO_GPIO 42
#define XL2515_SPI_HZ (10 * 1000 * 1000)

/* CANINTF bits */
#define XL_INT_RX0 0x01
#define XL_INT_RX1 0x02
#define XL_INT_TX0 0x04
#define XL_INT_TX1 0x08
#define XL_INT_TX2 0x10
#define XL_INT_ERR 0x20
#define XL_INT_WAK 0x40
#define XL_INT_MERR 0x80

/* EFLG bits */
#define XL_EFLG_EWARN 0x01
#define XL_EFLG_RXWAR 0x02
#define XL_EFLG_TXWAR 0x04
#define XL_EFLG_RXEP 0x08
#define XL_EFLG_TXEP 0x10
#define XL_EFLG_TXBO 0x20
#define XL_EFLG_RX0OVR 0x40
#define XL_EFLG_RX1OVR 0x80

typedef struct {
    uint8_t brp;    /* 1..64 */
    uint8_t prseg;  /* 1..8 */
    uint8_t phseg1; /* 1..8 */
    uint8_t phseg2; /* 1..8 */
    uint8_t sjw;    /* 1..4 */
} xl2515_timing_t;

/* Pure calculation, also used by the host unit tests. */
bool xl2515_calc_timing(uint32_t fosc_hz, uint32_t bitrate_bps, xl2515_timing_t *t);

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Set up the SPI bus and the interrupt pin. `notify` is woken on each INT edge. */
esp_err_t xl2515_init(TaskHandle_t notify);
/* Reset the chip and check that it answers. Leaves it in configuration mode. */
bool xl2515_reset(void);
/* Program bit timing and receive setup, then enter normal mode. */
bool xl2515_start(uint32_t fosc_hz, uint16_t kbps);
/*
 * Work out the crystal frequency by timing a frame in loopback mode.
 * Returns the frequency in MHz (8, 12, 16 or 20) or 0 if it could not be
 * measured. Leaves the chip in configuration mode.
 */
int xl2515_detect_xtal(void);

bool xl2515_int_active(void);
void xl2515_read_flags(uint8_t *intf, uint8_t *eflg);
void xl2515_clear_intf(uint8_t mask);
void xl2515_clear_eflg(uint8_t mask);
void xl2515_read_errors(uint8_t *tec, uint8_t *rec, uint8_t *eflg);
/* True while the chip is still in normal mode (false after a brown-out reset). */
bool xl2515_in_normal_mode(void);

/* Read receive buffer n (0 or 1); this also clears its interrupt flag. */
void xl2515_read_rx(int n, can_frame_t *f);
/* Load TXB0 and request transmission. */
void xl2515_send_tx0(const can_frame_t *f);
void xl2515_abort_tx0(void);

#endif /* ESP_PLATFORM */

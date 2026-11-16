#include "xl2515.h"

#include <string.h>

/* ---------- bit timing (portable) ---------- */

bool xl2515_calc_timing(uint32_t fosc_hz, uint32_t bitrate_bps, xl2515_timing_t *t)
{
    if (!fosc_hz || !bitrate_bps || fosc_hz % (2 * bitrate_bps) != 0) {
        return false;
    }
    /* Time quanta per bit times prescaler: TQ = 2 * BRP / fosc. */
    uint32_t total = fosc_hz / (2 * bitrate_bps);

    /* Pick the bit length closest to 16 TQ that divides evenly. */
    uint32_t best_n = 0;
    for (uint32_t n = 5; n <= 25; n++) {
        if (total % n != 0 || total / n < 1 || total / n > 64) {
            continue;
        }
        uint32_t d = n > 16 ? n - 16 : 16 - n;
        uint32_t best_d = best_n > 16 ? best_n - 16 : 16 - best_n;
        if (best_n == 0 || d < best_d) {
            best_n = n;
        }
    }
    if (best_n == 0) {
        if (total == 4) {
            /* 1 Mbit/s from an 8 MHz crystal: the usual 4 TQ compromise. */
            *t = (xl2515_timing_t){.brp = 1, .prseg = 1, .phseg1 = 1, .phseg2 = 1, .sjw = 1};
            return true;
        }
        return false;
    }

    /* Sample point: 87.5 % up to 500k, 80 % at 800k, 75 % at 1M (CiA). */
    uint32_t sp_permille = bitrate_bps > 800000 ? 750 : bitrate_bps > 500000 ? 800 : 875;
    uint32_t n = best_n;
    uint32_t phseg2 = n - (n * sp_permille + 500) / 1000;
    if (phseg2 < 2) phseg2 = 2;
    if (phseg2 > 8) phseg2 = 8;
    while (n - 1 - phseg2 > 16) { /* prseg + phseg1 can cover at most 16 TQ */
        phseg2++;
    }
    uint32_t tseg1 = n - 1 - phseg2;
    if (tseg1 < 2 || phseg2 > 8) {
        return false;
    }
    uint32_t phseg1 = tseg1 / 2;
    if (phseg1 > 8) phseg1 = 8;
    uint32_t prseg = tseg1 - phseg1;
    if (prseg > 8) {
        prseg = 8;
        phseg1 = tseg1 - prseg;
    }

    t->brp = (uint8_t)(total / n);
    t->prseg = (uint8_t)prseg;
    t->phseg1 = (uint8_t)phseg1;
    t->phseg2 = (uint8_t)phseg2;
    t->sjw = (uint8_t)(phseg2 > 4 ? 4 : phseg2 - 1);
    return true;
}

#ifdef ESP_PLATFORM

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "xl2515";

/* SPI instructions */
#define CMD_RESET 0xC0
#define CMD_READ 0x03
#define CMD_WRITE 0x02
#define CMD_BIT_MODIFY 0x05
#define CMD_READ_STATUS 0xA0
#define CMD_LOAD_TX0 0x40
#define CMD_RTS_TX0 0x81
#define CMD_READ_RX0 0x90
#define CMD_READ_RX1 0x94

/* Registers */
#define REG_BFPCTRL 0x0C
#define REG_TXRTSCTRL 0x0D
#define REG_CANSTAT 0x0E
#define REG_CANCTRL 0x0F
#define REG_TEC 0x1C
#define REG_REC 0x1D
#define REG_CNF3 0x28
#define REG_CNF2 0x29
#define REG_CNF1 0x2A
#define REG_CANINTE 0x2B
#define REG_CANINTF 0x2C
#define REG_EFLG 0x2D
#define REG_TXB0CTRL 0x30
#define REG_TXB1CTRL 0x40
#define REG_TXB2CTRL 0x50
#define REG_RXB0CTRL 0x60
#define REG_RXB1CTRL 0x70

#define MODE_NORMAL 0x00
#define MODE_LOOPBACK 0x40
#define MODE_CONFIG 0x80
#define MODE_MASK 0xE0

#define TXREQ 0x08
#define SIDL_EXIDE 0x08
#define SIDL_SRR 0x10
#define DLC_RTR 0x40

static spi_device_handle_t s_dev;
static TaskHandle_t s_notify;

static void IRAM_ATTR int_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    if (s_notify) {
        vTaskNotifyGiveFromISR(s_notify, &woken);
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static inline void xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(s_dev, &t);
}

static uint8_t read_reg(uint8_t reg)
{
    uint8_t tx[3] = {CMD_READ, reg, 0}, rx[3];
    xfer(tx, rx, 3);
    return rx[2];
}

static void write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[3] = {CMD_WRITE, reg, val};
    xfer(tx, NULL, 3);
}

static void bit_modify(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t tx[4] = {CMD_BIT_MODIFY, reg, mask, val};
    xfer(tx, NULL, 4);
}

static bool set_mode(uint8_t mode)
{
    bit_modify(REG_CANCTRL, MODE_MASK, mode);
    for (int i = 0; i < 50; i++) {
        if ((read_reg(REG_CANSTAT) & MODE_MASK) == mode) {
            return true;
        }
        esp_rom_delay_us(200);
    }
    return false;
}

esp_err_t xl2515_init(TaskHandle_t notify)
{
    s_notify = notify;

    spi_bus_config_t bus = {
        .mosi_io_num = XL2515_MOSI_GPIO,
        .miso_io_num = XL2515_MISO_GPIO,
        .sclk_io_num = XL2515_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        return err;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = XL2515_SPI_HZ,
        .mode = 0,
        .spics_io_num = XL2515_CS_GPIO,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &dev, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    /* This is the only device on the bus: keep it so transfers start faster. */
    spi_device_acquire_bus(s_dev, portMAX_DELAY);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << XL2515_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return gpio_isr_handler_add(XL2515_INT_GPIO, int_isr, NULL);
}

bool xl2515_reset(void)
{
    uint8_t cmd = CMD_RESET;
    xfer(&cmd, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    /* After reset the chip is in configuration mode with CANCTRL = 0x87. */
    uint8_t stat = read_reg(REG_CANSTAT);
    uint8_t ctrl = read_reg(REG_CANCTRL);
    if ((stat & MODE_MASK) != MODE_CONFIG || (ctrl & MODE_MASK) != MODE_CONFIG) {
        ESP_LOGE(TAG, "no response (CANSTAT=0x%02X CANCTRL=0x%02X)", stat, ctrl);
        return false;
    }
    return true;
}

static bool configure(uint32_t fosc_hz, uint32_t bitrate_bps, uint8_t mode)
{
    xl2515_timing_t t;
    if (!xl2515_calc_timing(fosc_hz, bitrate_bps, &t)) {
        ESP_LOGE(TAG, "no bit timing for %lu bit/s with a %lu Hz crystal",
                 (unsigned long)bitrate_bps, (unsigned long)fosc_hz);
        return false;
    }
    if (!set_mode(MODE_CONFIG)) {
        return false;
    }
    write_reg(REG_CNF1, (uint8_t)(((t.sjw - 1) << 6) | (t.brp - 1)));
    write_reg(REG_CNF2, (uint8_t)(0x80 | ((t.phseg1 - 1) << 3) | (t.prseg - 1)));
    write_reg(REG_CNF3, (uint8_t)(t.phseg2 - 1));

    write_reg(REG_RXB0CTRL, 0x64); /* accept everything, roll over into RXB1 */
    write_reg(REG_RXB1CTRL, 0x60); /* accept everything */
    write_reg(REG_BFPCTRL, 0x00);
    write_reg(REG_TXRTSCTRL, 0x00);
    write_reg(REG_TXB0CTRL, 0x00);
    write_reg(REG_TXB1CTRL, 0x00);
    write_reg(REG_TXB2CTRL, 0x00);
    write_reg(REG_CANINTF, 0x00);
    write_reg(REG_EFLG, 0x00);
    write_reg(REG_CANINTE, XL_INT_RX0 | XL_INT_RX1 | XL_INT_TX0 | XL_INT_ERR);

    /* Requested mode, CLKOUT off, retransmit until sent. */
    write_reg(REG_CANCTRL, mode);
    for (int i = 0; i < 50; i++) {
        if ((read_reg(REG_CANSTAT) & MODE_MASK) == mode) {
            return true;
        }
        esp_rom_delay_us(200);
    }
    ESP_LOGE(TAG, "mode change to 0x%02X timed out", mode);
    return false;
}

bool xl2515_start(uint32_t fosc_hz, uint16_t kbps)
{
    return configure(fosc_hz, (uint32_t)kbps * 1000, MODE_NORMAL);
}

int xl2515_detect_xtal(void)
{
    /*
     * Configure for 125 kbit/s as if the crystal were 16 MHz, then time one
     * frame in loopback mode. ID 0x555 with data 0x55 has almost no stuff
     * bits, so the frame is about 110 bits: 0.88 ms if the guess is right,
     * and proportionally longer or shorter otherwise.
     */
    const uint32_t assumed_hz = 16000000;
    const int64_t nominal_us = 880;
    if (!configure(assumed_hz, 125000, MODE_LOOPBACK)) {
        return 0;
    }
    write_reg(REG_CANINTE, 0x00);

    can_frame_t f = {.id = 0x555, .dlc = 8};
    memset(f.data, 0x55, 8);

    int64_t best = 0;
    for (int run = 0; run < 3; run++) {
        write_reg(REG_CANINTF, 0x00);
        xl2515_send_tx0(&f);
        int64_t start = esp_timer_get_time();
        int64_t elapsed = 0;
        bool done = false;
        while (elapsed < 20000) {
            uint8_t tx = CMD_READ_STATUS, rx[2], txb[2] = {tx, 0};
            xfer(txb, rx, 2);
            elapsed = esp_timer_get_time() - start;
            if (rx[1] & 0x08) { /* TX0IF */
                done = true;
                break;
            }
        }
        if (!done) {
            xl2515_abort_tx0();
            best = 0;
            break;
        }
        if (best == 0 || elapsed < best) {
            best = elapsed;
        }
    }
    set_mode(MODE_CONFIG);
    write_reg(REG_CANINTF, 0x00);
    if (best == 0) {
        ESP_LOGW(TAG, "crystal detection failed (loopback frame never completed)");
        return 0;
    }

    uint32_t est_khz = (uint32_t)((int64_t)(assumed_hz / 1000) * nominal_us / best);
    static const int candidates[] = {8, 12, 16, 20};
    int pick = 0;
    uint32_t pick_err = UINT32_MAX;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        uint32_t c = (uint32_t)candidates[i] * 1000;
        uint32_t e = c > est_khz ? c - est_khz : est_khz - c;
        if (e < pick_err) {
            pick_err = e;
            pick = candidates[i];
        }
    }
    ESP_LOGI(TAG, "loopback frame took %lld us, crystal is about %lu kHz -> %d MHz",
             (long long)best, (unsigned long)est_khz, pick);
    if (pick_err > 1500) {
        ESP_LOGW(TAG, "measurement is not close to a standard crystal value");
        return 0;
    }
    return pick;
}

bool xl2515_int_active(void)
{
    return gpio_get_level(XL2515_INT_GPIO) == 0;
}

void xl2515_read_flags(uint8_t *intf, uint8_t *eflg)
{
    uint8_t tx[4] = {CMD_READ, REG_CANINTF, 0, 0}, rx[4];
    xfer(tx, rx, 4);
    *intf = rx[2];
    *eflg = rx[3];
}

void xl2515_clear_intf(uint8_t mask)
{
    bit_modify(REG_CANINTF, mask, 0);
}

void xl2515_clear_eflg(uint8_t mask)
{
    bit_modify(REG_EFLG, mask, 0);
}

void xl2515_read_errors(uint8_t *tec, uint8_t *rec, uint8_t *eflg)
{
    uint8_t tx[4] = {CMD_READ, REG_TEC, 0, 0}, rx[4];
    xfer(tx, rx, 4);
    *tec = rx[2];
    *rec = rx[3];
    *eflg = read_reg(REG_EFLG);
}

bool xl2515_in_normal_mode(void)
{
    return (read_reg(REG_CANSTAT) & MODE_MASK) == MODE_NORMAL;
}

void xl2515_read_rx(int n, can_frame_t *f)
{
    uint8_t tx[14] = {n ? CMD_READ_RX1 : CMD_READ_RX0}, rx[14];
    xfer(tx, rx, 14);
    const uint8_t *b = &rx[1]; /* SIDH SIDL EID8 EID0 DLC D0..D7 */
    uint32_t sid = ((uint32_t)b[0] << 3) | (b[1] >> 5);
    f->flags = 0;
    if (b[1] & SIDL_EXIDE) {
        f->id = (sid << 18) | ((uint32_t)(b[1] & 0x03) << 16) | ((uint32_t)b[2] << 8) | b[3];
        f->flags |= CAN_FLAG_EXT;
        if (b[4] & DLC_RTR) {
            f->flags |= CAN_FLAG_RTR;
        }
    } else {
        f->id = sid;
        if (b[1] & SIDL_SRR) {
            f->flags |= CAN_FLAG_RTR;
        }
    }
    f->dlc = b[4] & 0x0F;
    if (f->dlc > 8) {
        f->dlc = 8;
    }
    memcpy(f->data, &b[5], 8);
}

void xl2515_send_tx0(const can_frame_t *f)
{
    uint8_t tx[14];
    tx[0] = CMD_LOAD_TX0;
    if (f->flags & CAN_FLAG_EXT) {
        uint32_t id = f->id & CAN_EXT_ID_MAX;
        tx[1] = (uint8_t)(id >> 21);
        tx[2] = (uint8_t)(((id >> 13) & 0xE0) | SIDL_EXIDE | ((id >> 16) & 0x03));
        tx[3] = (uint8_t)(id >> 8);
        tx[4] = (uint8_t)id;
    } else {
        uint32_t id = f->id & CAN_STD_ID_MAX;
        tx[1] = (uint8_t)(id >> 3);
        tx[2] = (uint8_t)(id << 5);
        tx[3] = 0;
        tx[4] = 0;
    }
    uint8_t dlc = f->dlc > 8 ? 8 : f->dlc;
    tx[5] = dlc | ((f->flags & CAN_FLAG_RTR) ? DLC_RTR : 0);
    memcpy(&tx[6], f->data, 8);
    xfer(tx, NULL, 14);

    uint8_t rts = CMD_RTS_TX0;
    xfer(&rts, NULL, 1);
}

void xl2515_abort_tx0(void)
{
    bit_modify(REG_TXB0CTRL, TXREQ, 0);
}

#endif /* ESP_PLATFORM */

#include "gateway.h"

#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "can1_twai.h"
#include "config.h"
#include "filter.h"
#include "ring.h"
#include "stats.h"
#include "xl2515.h"

#define CAN1_TASK_PRIO 20
#define CAN2_TASK_PRIO 19
#define RT_CORE 1
#define TX_TIMEOUT_US 100000 /* give up on a frame CAN2 cannot send */
#define HEALTH_PERIOD_US 100000

static const char *TAG = "gateway";

gateway_counters_t g_counters;

static ring_t s_ring;            /* CAN1 receive task -> CAN2 task */
static QueueHandle_t s_inject2;  /* test frames for CAN2 */
static TaskHandle_t s_can1_task;
static TaskHandle_t s_can2_task;
static SemaphoreHandle_t s_started;
static atomic_bool s_reconf1;
static atomic_bool s_reconf2;
static atomic_bool s_forwarding;

static atomic_uint s_queue_peak;
static atomic_uint s_can2_state; /* index into k_can2_states */
static atomic_uint s_can2_tec, s_can2_rec, s_can2_xtal;
static int s_detected_xtal; /* CAN2 task only: measured once, then reused */
static const char *const k_can2_states[] = {"absent", "active", "warning", "passive", "bus-off"};
enum { C2_ABSENT, C2_ACTIVE, C2_WARNING, C2_PASSIVE, C2_BUSOFF };

/* ---------------- CAN1: receive, filter, queue ---------------- */

static void can1_task(void *arg)
{
    can1_start(g_cfg.bitrate1, g_cfg.listen_only1);
    xSemaphoreGive(s_started);
    esp_task_wdt_add(NULL);

    can_frame_t f;
    unsigned n = 0;
    int64_t last_retry = 0;
    for (;;) {
        esp_task_wdt_reset();
        if (atomic_exchange(&s_reconf1, false)) {
            can1_stop();
            can1_start(g_cfg.bitrate1, g_cfg.listen_only1);
        } else if (!can1_running()) {
            /* The driver failed to start: keep trying rather than stay deaf. */
            int64_t now = esp_timer_get_time();
            if (now - last_retry > 1000000) {
                last_retry = now;
                can1_start(g_cfg.bitrate1, g_cfg.listen_only1);
            }
        }
        if (!can1_receive(&f, pdMS_TO_TICKS(20))) {
            can1_poll_alerts();
            continue;
        }
        atomic_fetch_add_explicit(&g_counters.can1_rx, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_counters.bits[0], can_frame_bits(&f), memory_order_relaxed);
        stats_update(0, &f);

        if (!atomic_load_explicit(&s_forwarding, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&g_counters.not_forwarded, 1, memory_order_relaxed);
        } else if (filter_blocked(f.id, f.flags & CAN_FLAG_EXT)) {
            atomic_fetch_add_explicit(&g_counters.blocked, 1, memory_order_relaxed);
        } else if (ring_push(&s_ring, &f)) {
            atomic_fetch_add_explicit(&g_counters.forwarded, 1, memory_order_relaxed);
            xTaskNotifyGive(s_can2_task);
            unsigned depth = ring_count(&s_ring);
            if (depth > atomic_load_explicit(&s_queue_peak, memory_order_relaxed)) {
                atomic_store_explicit(&s_queue_peak, depth, memory_order_relaxed);
            }
        } else {
            /* Never wait here: CAN1 reception must not stall. */
            atomic_fetch_add_explicit(&g_counters.dropped, 1, memory_order_relaxed);
        }

        if ((++n & 0x0F) == 0) {
            can1_sample_queue();
            if ((n & 0x3F) == 0) {
                can1_poll_alerts();
            }
        }
    }
}

/* ---------------- CAN2: transmit in order, monitor receive ---------------- */

static void can2_discard_queue(void)
{
    unsigned n = ring_flush(&s_ring);
    if (n) {
        atomic_fetch_add(&g_counters.dropped, n);
    }
}

static bool can2_bringup(void)
{
    atomic_store(&s_can2_state, C2_ABSENT);
    if (!xl2515_reset()) {
        return false;
    }
    int xtal = g_cfg.xtal_mhz;
    if (xtal == 0) {
        /*
         * Measure only once per boot. A restart after a fault then uses the
         * same value, and a measurement disturbed by other work on this core
         * cannot change the bit timing mid-operation.
         */
        if (s_detected_xtal == 0) {
            s_detected_xtal = xl2515_detect_xtal();
            if (s_detected_xtal == 0) {
                s_detected_xtal = 16;
                ESP_LOGW(TAG, "assuming a 16 MHz crystal on the XL2515");
            }
        }
        xtal = s_detected_xtal;
    }
    atomic_store(&s_can2_xtal, (unsigned)xtal);
    if (!xl2515_start((uint32_t)xtal * 1000000u, g_cfg.bitrate2)) {
        return false;
    }
    ESP_LOGI(TAG, "CAN2 started at %u kbit/s (%d MHz crystal)", g_cfg.bitrate2, xtal);
    atomic_store(&s_can2_state, C2_ACTIVE);
    return true;
}

static void can2_task(void *arg)
{
    esp_task_wdt_add(NULL);
    esp_err_t err = xl2515_init(xTaskGetCurrentTaskHandle());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "XL2515 SPI setup failed: %s", esp_err_to_name(err));
        for (;;) { /* stay alive: CAN1 keeps notifying this task */
            esp_task_wdt_reset();
            can2_discard_queue();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    bool up = can2_bringup();
    bool tx_busy = false;
    int64_t tx_start = 0;
    int64_t last_health = 0;
    can_frame_t f;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(tx_busy ? 2 : 10));
        esp_task_wdt_reset();

        if (atomic_exchange(&s_reconf2, false)) {
            s_detected_xtal = 0; /* settings changed: measure again */
            up = false;
        }
        if (!up) {
            up = can2_bringup();
            tx_busy = false;
            if (!up) {
                can2_discard_queue();
                while (xQueueReceive(s_inject2, &f, 0)) {
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

        bool work;
        do {
            work = false;
            int64_t now = esp_timer_get_time();

            if (xl2515_int_active()) {
                uint8_t intf, eflg;
                xl2515_read_flags(&intf, &eflg);
                if (intf & XL_INT_RX0) {
                    xl2515_read_rx(0, &f);
                    atomic_fetch_add_explicit(&g_counters.can2_rx, 1, memory_order_relaxed);
                    atomic_fetch_add_explicit(&g_counters.bits[1], can_frame_bits(&f), memory_order_relaxed);
                    stats_update(1, &f);
                    work = true;
                }
                if (intf & XL_INT_RX1) {
                    xl2515_read_rx(1, &f);
                    atomic_fetch_add_explicit(&g_counters.can2_rx, 1, memory_order_relaxed);
                    atomic_fetch_add_explicit(&g_counters.bits[1], can_frame_bits(&f), memory_order_relaxed);
                    stats_update(1, &f);
                    work = true;
                }
                if (intf & XL_INT_TX0) {
                    xl2515_clear_intf(XL_INT_TX0);
                    if (tx_busy) {
                        tx_busy = false;
                        atomic_fetch_add_explicit(&g_counters.can2_tx, 1, memory_order_relaxed);
                    }
                    work = true;
                }
                uint8_t other = intf & (XL_INT_ERR | XL_INT_MERR | XL_INT_WAK | XL_INT_TX1 | XL_INT_TX2);
                if (other) {
                    if (eflg & (XL_EFLG_RX0OVR | XL_EFLG_RX1OVR)) {
                        atomic_fetch_add(&g_counters.can2_rx_ovf, 1);
                        xl2515_clear_eflg(XL_EFLG_RX0OVR | XL_EFLG_RX1OVR);
                    }
                    if (eflg & XL_EFLG_TXBO) {
                        /* The chip rejoins the bus by itself; drop what is stale. */
                        atomic_fetch_add(&g_counters.can2_busoff, 1);
                        ESP_LOGW(TAG, "CAN2 bus-off");
                        if (tx_busy) {
                            xl2515_abort_tx0();
                            tx_busy = false;
                            atomic_fetch_add(&g_counters.can2_tx_fail, 1);
                        }
                        can2_discard_queue();
                    }
                    xl2515_clear_intf(other);
                    work = true;
                }
            }

            if (tx_busy && now - tx_start > TX_TIMEOUT_US) {
                /* Nothing is acknowledging on CAN2 (unplugged, wrong bitrate). */
                xl2515_abort_tx0();
                xl2515_clear_intf(XL_INT_TX0);
                tx_busy = false;
                atomic_fetch_add(&g_counters.can2_tx_fail, 1);
                can2_discard_queue();
            }

            /*
             * Only TXB0 is used, and the next frame is loaded when the
             * previous one has gone out, so CAN1 arrival order is kept.
             */
            if (!tx_busy && (xQueueReceive(s_inject2, &f, 0) || ring_pop(&s_ring, &f))) {
                xl2515_send_tx0(&f);
                atomic_fetch_add_explicit(&g_counters.bits[1], can_frame_bits(&f), memory_order_relaxed);
                tx_busy = true;
                tx_start = now;
                work = true;
            }
        } while (work);

        int64_t now = esp_timer_get_time();
        if (now - last_health > HEALTH_PERIOD_US) {
            last_health = now;
            uint8_t tec, rec, eflg;
            xl2515_read_errors(&tec, &rec, &eflg);
            atomic_store(&s_can2_tec, tec);
            atomic_store(&s_can2_rec, rec);
            unsigned state = C2_ACTIVE;
            if (eflg & XL_EFLG_TXBO) {
                state = C2_BUSOFF;
            } else if (eflg & (XL_EFLG_TXEP | XL_EFLG_RXEP)) {
                state = C2_PASSIVE;
            } else if (eflg & XL_EFLG_EWARN) {
                state = C2_WARNING;
            }
            atomic_store(&s_can2_state, state);
            if (!xl2515_in_normal_mode()) {
                ESP_LOGW(TAG, "XL2515 left normal mode, restarting it");
                up = false;
            }
        }
    }
}

/* ---------------- public ---------------- */

bool gateway_start(void)
{
    atomic_store(&s_forwarding, g_cfg.forwarding);
    s_inject2 = xQueueCreate(16, sizeof(can_frame_t));
    s_started = xSemaphoreCreateBinary();
    /* CAN2 first, so its handle exists before CAN1 starts notifying it. */
    xTaskCreatePinnedToCore(can2_task, "can2", 4096, NULL, CAN2_TASK_PRIO, &s_can2_task, RT_CORE);
    xTaskCreatePinnedToCore(can1_task, "can1_rx", 4096, NULL, CAN1_TASK_PRIO, &s_can1_task, RT_CORE);
    xSemaphoreTake(s_started, pdMS_TO_TICKS(3000));
    return can1_running();
}

void gateway_get_can2_status(can2_status_t *st)
{
    st->state = k_can2_states[atomic_load(&s_can2_state)];
    st->tec = (uint8_t)atomic_load(&s_can2_tec);
    st->rec = (uint8_t)atomic_load(&s_can2_rec);
    st->xtal_mhz = (uint8_t)atomic_load(&s_can2_xtal);
}

unsigned gateway_queue_depth(void)
{
    return ring_count(&s_ring);
}

unsigned gateway_queue_peak(void)
{
    return atomic_load(&s_queue_peak);
}

unsigned gateway_queue_size(void)
{
    return RING_SIZE;
}

void gateway_reset_peaks(void)
{
    atomic_store(&s_queue_peak, 0);
    can1_reset_peak();
}

bool gateway_set_bus1(uint16_t kbps, bool listen_only)
{
    if (!config_bitrate_valid(kbps)) {
        return false;
    }
    if (g_cfg.bitrate1 != kbps || g_cfg.listen_only1 != listen_only) {
        g_cfg.bitrate1 = kbps;
        g_cfg.listen_only1 = listen_only;
        atomic_store(&s_reconf1, true);
        config_mark_dirty();
    }
    return true;
}

bool gateway_set_bus2(uint16_t kbps, uint8_t xtal_mhz)
{
    if (!config_bitrate_valid(kbps) || !config_xtal_valid(xtal_mhz)) {
        return false;
    }
    if (g_cfg.bitrate2 != kbps || g_cfg.xtal_mhz != xtal_mhz) {
        g_cfg.bitrate2 = kbps;
        g_cfg.xtal_mhz = xtal_mhz;
        atomic_store(&s_reconf2, true);
        if (s_can2_task) {
            xTaskNotifyGive(s_can2_task);
        }
        config_mark_dirty();
    }
    return true;
}

void gateway_set_forwarding(bool enabled)
{
    if (g_cfg.forwarding != enabled) {
        g_cfg.forwarding = enabled;
        config_mark_dirty();
    }
    atomic_store(&s_forwarding, enabled);
}

void gateway_apply_config(void)
{
    atomic_store(&s_forwarding, g_cfg.forwarding);
    atomic_store(&s_reconf1, true);
    atomic_store(&s_reconf2, true);
    if (s_can2_task) {
        xTaskNotifyGive(s_can2_task);
    }
}

esp_err_t gateway_send(int bus, const can_frame_t *f)
{
    if (bus == 1) {
        esp_err_t err = can1_transmit(f);
        if (err == ESP_OK) {
            atomic_fetch_add(&g_counters.can1_tx, 1);
            atomic_fetch_add(&g_counters.bits[0], can_frame_bits(f));
        }
        return err;
    }
    if (bus == 2) {
        if (atomic_load(&s_can2_state) == C2_ABSENT) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!xQueueSend(s_inject2, f, 0)) {
            return ESP_ERR_TIMEOUT;
        }
        xTaskNotifyGive(s_can2_task);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

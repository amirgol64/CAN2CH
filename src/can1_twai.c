#include "can1_twai.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/twai.h"
#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "can1";

static SemaphoreHandle_t s_lock; /* guards install/uninstall against other tasks */
static bool s_installed;
static bool s_listen_only;
static atomic_uint s_busoff_count;
static atomic_uint s_queue_peak;

static bool timing_for(uint16_t kbps, twai_timing_config_t *t)
{
    switch (kbps) {
    case 25: *t = (twai_timing_config_t)TWAI_TIMING_CONFIG_25KBITS(); return true;
    case 50: *t = (twai_timing_config_t)TWAI_TIMING_CONFIG_50KBITS(); return true;
    case 100: *t = (twai_timing_config_t)TWAI_TIMING_CONFIG_100KBITS(); return true;
    case 125: *t = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS(); return true;
    case 250: *t = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS(); return true;
    case 500: *t = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS(); return true;
    case 800: *t = (twai_timing_config_t)TWAI_TIMING_CONFIG_800KBITS(); return true;
    case 1000: *t = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS(); return true;
    default: return false;
    }
}

esp_err_t can1_start(uint16_t kbps, bool listen_only)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    twai_timing_config_t timing;
    if (!timing_for(kbps, &timing)) {
        return ESP_ERR_INVALID_ARG;
    }
    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
        CAN1_TX_GPIO, CAN1_RX_GPIO, listen_only ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
    /*
     * Flash writes (settings, DBC upload, firmware update) stop both CPU
     * cores for tens of milliseconds. The interrupt handler keeps filling
     * this queue meanwhile: 1024 frames last about 450 ms at full load at
     * 250 kbit/s and about 130 ms at 1 Mbit/s.
     */
    general.rx_queue_len = CAN1_RX_QUEUE_LEN;
    general.tx_queue_len = 16;
    general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED;
#ifdef CONFIG_TWAI_ISR_IN_IRAM
    general.intr_flags = ESP_INTR_FLAG_IRAM;
#endif
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = twai_driver_install(&general, &timing, &filter);
    if (err == ESP_OK) {
        err = twai_start();
        if (err != ESP_OK) {
            twai_driver_uninstall();
        }
    }
    s_installed = (err == ESP_OK);
    s_listen_only = listen_only;
    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "started at %u kbit/s%s", kbps, listen_only ? " (listen-only)" : "");
    } else {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
    }
    return err;
}

void can1_stop(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_installed) {
        twai_stop(); /* fails harmlessly if the bus is off */
        twai_driver_uninstall();
        s_installed = false;
    }
    xSemaphoreGive(s_lock);
}

bool can1_running(void)
{
    return s_installed;
}

bool can1_receive(can_frame_t *f, TickType_t timeout)
{
    if (!s_installed) {
        vTaskDelay(timeout ? timeout : 1);
        return false;
    }
    twai_message_t m;
    if (twai_receive(&m, timeout) != ESP_OK) {
        return false;
    }
    f->id = m.identifier;
    f->flags = (m.extd ? CAN_FLAG_EXT : 0) | (m.rtr ? CAN_FLAG_RTR : 0);
    f->dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
    memcpy(f->data, m.data, 8);
    return true;
}

void can1_poll_alerts(void)
{
    uint32_t alerts;
    if (!s_installed || twai_read_alerts(&alerts, 0) != ESP_OK) {
        return;
    }
    if (alerts & TWAI_ALERT_BUS_OFF) {
        atomic_fetch_add(&s_busoff_count, 1);
        ESP_LOGW(TAG, "bus-off, starting recovery");
        twai_initiate_recovery();
    }
    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        ESP_LOGI(TAG, "bus recovered");
        twai_start();
    }
}

void can1_sample_queue(void)
{
    twai_status_info_t info;
    if (s_installed && twai_get_status_info(&info) == ESP_OK) {
        unsigned peak = atomic_load_explicit(&s_queue_peak, memory_order_relaxed);
        if (info.msgs_to_rx > peak) {
            atomic_store_explicit(&s_queue_peak, info.msgs_to_rx, memory_order_relaxed);
        }
    }
}

void can1_reset_peak(void)
{
    atomic_store(&s_queue_peak, 0);
}

esp_err_t can1_transmit(const can_frame_t *f)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    twai_message_t m = {0};
    m.identifier = f->id;
    m.extd = (f->flags & CAN_FLAG_EXT) ? 1 : 0;
    m.rtr = (f->flags & CAN_FLAG_RTR) ? 1 : 0;
    m.data_length_code = f->dlc > 8 ? 8 : f->dlc;
    memcpy(m.data, f->data, 8);

    esp_err_t err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_installed) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_listen_only) {
        err = ESP_ERR_NOT_SUPPORTED;
    } else {
        err = twai_transmit(&m, pdMS_TO_TICKS(10));
    }
    xSemaphoreGive(s_lock);
    return err;
}

void can1_get_status(can1_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->state = "stopped";
    st->busoff_count = atomic_load(&s_busoff_count);
    st->rx_queue_peak = atomic_load(&s_queue_peak);
    if (!s_lock) {
        return;
    }
    twai_status_info_t info;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_installed && twai_get_status_info(&info) == ESP_OK;
    xSemaphoreGive(s_lock);
    if (!ok) {
        return;
    }
    switch (info.state) {
    case TWAI_STATE_RUNNING: st->state = "running"; break;
    case TWAI_STATE_BUS_OFF: st->state = "bus-off"; break;
    case TWAI_STATE_RECOVERING: st->state = "recovering"; break;
    default: st->state = "stopped"; break;
    }
    st->tec = info.tx_error_counter;
    st->rec = info.rx_error_counter;
    st->bus_errors = info.bus_error_count;
    st->rx_missed = info.rx_missed_count + info.rx_overrun_count;
    st->arb_lost = info.arb_lost_count;
}

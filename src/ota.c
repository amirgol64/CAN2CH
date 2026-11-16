#include "ota.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "ota";

static void reboot_cb(void *arg)
{
    esp_restart();
}

void app_reboot_later(void)
{
    static esp_timer_handle_t timer;
    if (timer) {
        return;
    }
    const esp_timer_create_args_t args = {.callback = reboot_cb, .name = "reboot"};
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 1500 * 1000);
    }
}

esp_err_t ota_handle_upload(httpd_req_t *req, char *err, size_t err_len)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        snprintf(err, err_len, "no spare app partition");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        snprintf(err, err_len, "image size %u does not fit (max %u)",
                 (unsigned)req->content_len, (unsigned)part->size);
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        snprintf(err, err_len, "out of memory");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle = 0;
    bool begun = false;
    esp_err_t result = ESP_FAIL;
    size_t remaining = req->content_len;
    bool first = true;

    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            snprintf(err, err_len, "upload interrupted");
            goto out;
        }
        if (first) {
            first = false;
            if ((uint8_t)buf[0] != 0xE9) {
                snprintf(err, err_len, "not an ESP32 firmware image");
                goto out;
            }
            /*
             * Erase one 4 KB sector at a time as the image arrives, instead
             * of the whole 3 MB slot up front. Flash erases pause both CPU
             * cores, so this keeps each pause short enough for the CAN
             * receive queue to bridge it.
             */
            esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            if (e != ESP_OK) {
                snprintf(err, err_len, "esp_ota_begin: %s", esp_err_to_name(e));
                goto out;
            }
            begun = true;
        }
        esp_err_t e = esp_ota_write(handle, buf, (size_t)n);
        if (e != ESP_OK) {
            snprintf(err, err_len, "flash write: %s", esp_err_to_name(e));
            goto out;
        }
        remaining -= (size_t)n;
    }

    esp_err_t e = esp_ota_end(handle);
    begun = false;
    if (e != ESP_OK) {
        snprintf(err, err_len, "image check failed: %s", esp_err_to_name(e));
        goto out;
    }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        snprintf(err, err_len, "set boot partition: %s", esp_err_to_name(e));
        goto out;
    }
    ESP_LOGI(TAG, "image written to %s, rebooting", part->label);
    result = ESP_OK;

out:
    if (begun) {
        esp_ota_abort(handle);
    }
    free(buf);
    return result;
}

void ota_confirm_boot(bool ok)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    if (ok) {
        ESP_LOGI(TAG, "new image confirmed");
        esp_ota_mark_app_valid_cancel_rollback();
    } else {
        ESP_LOGE(TAG, "new image failed its start-up check, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

/*
 * On this board the first boot attempt after a software restart fails in the
 * ROM loader, and the RTC watchdog that ESP-IDF arms for every restart resets
 * the chip again; that second boot works. The chip then reports "watchdog"
 * even for a deliberate restart. To tell the two apart, every planned restart
 * (esp_restart) leaves a marker in RTC memory, which survives that reset.
 */
#define PLANNED_RESTART_MAGIC 0x52425421u
static RTC_NOINIT_ATTR uint32_t s_planned_restart;
static bool s_last_was_planned;

static void mark_planned_restart(void)
{
    s_planned_restart = PLANNED_RESTART_MAGIC;
}

/*
 * The base MAC is burned into eFuse at the factory and is unique per chip.
 * The first 3 bytes are an Espressif vendor prefix (OUI); the last 3 are the
 * per-chip part. Set once in app_restart_init, read-only after that.
 */
static char s_serial[16] = "CF-000000";

static void serial_init(void)
{
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(s_serial, sizeof(s_serial), "CF-%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
}

const char *app_serial_number(void)
{
    return s_serial;
}

void app_restart_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    /* RTC memory holds garbage after power-on or a brownout. */
    s_last_was_planned = s_planned_restart == PLANNED_RESTART_MAGIC &&
                         r != ESP_RST_POWERON && r != ESP_RST_BROWNOUT;
    s_planned_restart = 0;
    esp_register_shutdown_handler(mark_planned_restart);
    serial_init();
}

const char *app_reset_reason(void)
{
    if (s_last_was_planned) {
        return "software restart";
    }
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog or crash";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout (supply dip)";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    default: return "unknown";
    }
}

const char *ota_running_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running ? running->label : "?";
}

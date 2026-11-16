/*
 * esp32s3-can-gateway
 * One-way CAN gateway (CAN1 -> CAN2) with an ID blocklist, for the
 * Waveshare ESP32-S3-CAN-2CH.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "can_frame.h"
#include "cli.h"
#include "config.h"
#include "gateway.h"
#include "ota.h"
#include "stats.h"
#include "web.h"
#include "wifi.h"

#define BOOT_BUTTON_GPIO 0
#define BUTTON_HOLD_MS 5000
#define BUTTON_POLL_MS 50

static const char *TAG = "main";

/* Holding BOOT for 5 s while running restores the factory defaults. */
static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += BUTTON_POLL_MS;
            if (held_ms >= BUTTON_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT held for 5 s: restoring factory defaults");
                config_factory_reset();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
    }
}

void app_main(void)
{
    app_restart_init();
    ESP_LOGI(TAG, "esp32s3-can-gateway %s, serial %s, last restart: %s", FW_VERSION,
             app_serial_number(), app_reset_reason());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    config_init();
    stats_init();

    /* The gateway runs first: forwarding must not wait for Wi-Fi. */
    bool can_ok = gateway_start();

    xTaskCreatePinnedToCore(button_task, "button", 3072, NULL, 2, NULL, 0);
    cli_start();

    web_fs_mount();
    wifi_start();
    bool web_ok = web_start();

    ota_confirm_boot(can_ok && web_ok);
}

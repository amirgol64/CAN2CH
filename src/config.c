#include "config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "filter.h"

#define NVS_NS "cangw"
/*
 * Storage layout version 2: one NVS key per setting, so firmware updates can
 * add settings without losing the existing ones (a missing key keeps its
 * default). Version 1 stored config_v1_t as a single blob and is migrated on
 * first boot.
 */
#define NVS_VERSION 2
#define SAVE_DEBOUNCE_MS 1000

/* Frozen copy of the version 1 layout. Never change it. */
typedef struct {
    uint16_t bitrate1;
    uint16_t bitrate2;
    bool listen_only1;
    bool forwarding;
    uint8_t xtal_mhz;
    char wifi_ssid[33];
    char wifi_pass[65];
    char admin_salt[17];
    char admin_hash[65];
} config_v1_t;

static const char *TAG = "config";

config_t g_cfg;
static TaskHandle_t s_task;

static bool s_frozen; /* set by a factory reset so nothing is written back */

static void save(void)
{
    if (s_frozen) {
        return;
    }
    static uint32_t std_bits[FILTER_STD_WORDS];
    static uint32_t ext[FILTER_MAX_EXT];
    uint16_t ext_count = 0;
    filter_export_raw(std_bits, ext, &ext_count);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return;
    }
    config_t copy = g_cfg;
    err = nvs_set_u16(h, "br1", copy.bitrate1);
    if (err == ESP_OK) err = nvs_set_u16(h, "br2", copy.bitrate2);
    if (err == ESP_OK) err = nvs_set_u8(h, "lo1", copy.listen_only1);
    if (err == ESP_OK) err = nvs_set_u8(h, "fwd", copy.forwarding);
    if (err == ESP_OK) err = nvs_set_u8(h, "xtal", copy.xtal_mhz);
    if (err == ESP_OK) err = nvs_set_str(h, "ssid", copy.wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", copy.wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, "salt", copy.admin_salt);
    if (err == ESP_OK) err = nvs_set_str(h, "hash", copy.admin_hash);
    if (err == ESP_OK) err = nvs_set_blob(h, "fstd", std_bits, sizeof(std_bits));
    if (err == ESP_OK) err = nvs_set_u16(h, "fextn", ext_count);
    if (err == ESP_OK) {
        /* NVS rejects zero-length blobs, so always store at least one word. */
        err = nvs_set_blob(h, "fext", ext, (ext_count ? ext_count : 1) * sizeof(uint32_t));
    }
    /* Written last: a power cut before this point leaves the old version
     * marker, and the next boot reads whatever is complete. */
    if (err == ESP_OK) err = nvs_set_u8(h, "ver", NVS_VERSION);
    if (err == ESP_OK) {
        esp_err_t e = nvs_erase_key(h, "cfg"); /* version 1 blob, if still there */
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) err = e;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "saved");
    }
}

static void get_str(nvs_handle_t h, const char *key, char *buf, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
        return; /* keep the default */
    }
    buf[size - 1] = '\0';
}

static bool load(bool *migrated)
{
    static uint32_t std_bits[FILTER_STD_WORDS];
    static uint32_t ext[FILTER_MAX_EXT];
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = false;
    uint8_t ver = 0;
    config_t tmp;
    config_defaults(&tmp);
    size_t len;
    uint16_t ext_count = 0;
    if (nvs_get_u8(h, "ver", &ver) != ESP_OK) goto out;
    if (ver == 1) {
        config_v1_t v1;
        len = sizeof(v1);
        if (nvs_get_blob(h, "cfg", &v1, &len) != ESP_OK || len != sizeof(v1)) goto out;
        tmp.bitrate1 = v1.bitrate1;
        tmp.bitrate2 = v1.bitrate2;
        tmp.listen_only1 = v1.listen_only1;
        tmp.forwarding = v1.forwarding;
        tmp.xtal_mhz = v1.xtal_mhz;
        memcpy(tmp.wifi_ssid, v1.wifi_ssid, sizeof(tmp.wifi_ssid));
        memcpy(tmp.wifi_pass, v1.wifi_pass, sizeof(tmp.wifi_pass));
        memcpy(tmp.admin_salt, v1.admin_salt, sizeof(tmp.admin_salt));
        memcpy(tmp.admin_hash, v1.admin_hash, sizeof(tmp.admin_hash));
        *migrated = true;
    } else if (ver == NVS_VERSION) {
        uint8_t u8;
        /* Each key is optional: anything missing keeps its default. */
        nvs_get_u16(h, "br1", &tmp.bitrate1);
        nvs_get_u16(h, "br2", &tmp.bitrate2);
        if (nvs_get_u8(h, "lo1", &u8) == ESP_OK) tmp.listen_only1 = u8 != 0;
        if (nvs_get_u8(h, "fwd", &u8) == ESP_OK) tmp.forwarding = u8 != 0;
        nvs_get_u8(h, "xtal", &tmp.xtal_mhz);
        get_str(h, "ssid", tmp.wifi_ssid, sizeof(tmp.wifi_ssid));
        get_str(h, "pass", tmp.wifi_pass, sizeof(tmp.wifi_pass));
        get_str(h, "salt", tmp.admin_salt, sizeof(tmp.admin_salt));
        get_str(h, "hash", tmp.admin_hash, sizeof(tmp.admin_hash));
    } else {
        goto out; /* written by a newer firmware with an unknown layout */
    }
    len = sizeof(std_bits);
    if (nvs_get_blob(h, "fstd", std_bits, &len) != ESP_OK || len != sizeof(std_bits)) goto out;
    if (nvs_get_u16(h, "fextn", &ext_count) != ESP_OK || ext_count > FILTER_MAX_EXT) goto out;
    len = sizeof(ext);
    if (nvs_get_blob(h, "fext", ext, &len) != ESP_OK || len < ext_count * sizeof(uint32_t)) goto out;

    /* Never trust stored strings to be terminated, or stored values to be sane. */
    tmp.wifi_ssid[sizeof(tmp.wifi_ssid) - 1] = '\0';
    tmp.wifi_pass[sizeof(tmp.wifi_pass) - 1] = '\0';
    tmp.admin_salt[sizeof(tmp.admin_salt) - 1] = '\0';
    tmp.admin_hash[sizeof(tmp.admin_hash) - 1] = '\0';
    if (!config_bitrate_valid(tmp.bitrate1)) tmp.bitrate1 = 250;
    if (!config_bitrate_valid(tmp.bitrate2)) tmp.bitrate2 = 250;
    if (!config_xtal_valid(tmp.xtal_mhz)) tmp.xtal_mhz = 0;

    g_cfg = tmp;
    filter_import_raw(std_bits, ext, ext_count);
    ok = true;
out:
    nvs_close(h);
    return ok;
}

static void config_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Restart the wait every time another change arrives. */
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SAVE_DEBOUNCE_MS))) {
        }
        save();
    }
}

void config_init(void)
{
    filter_init();
    bool migrated = false;
    if (load(&migrated)) {
        ESP_LOGI(TAG, "loaded: CAN1 %u kbit/s, CAN2 %u kbit/s, %u blocked IDs",
                 g_cfg.bitrate1, g_cfg.bitrate2, (unsigned)filter_count());
        if (migrated) {
            ESP_LOGI(TAG, "converting stored settings to the new layout");
            save();
        }
    } else {
        ESP_LOGW(TAG, "no stored settings, using factory defaults");
        config_defaults(&g_cfg);
        config_default_filters();
    }
    xTaskCreatePinnedToCore(config_task, "config", 4096, NULL, 3, &s_task, 0);
}

void config_mark_dirty(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void config_save_now(void)
{
    save();
}

void config_factory_reset(void)
{
    nvs_handle_t h;
    s_frozen = true;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "settings erased");
}

static void hash_password(const char *salt, const char *password, char out_hex[65])
{
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)salt, strlen(salt));
    mbedtls_sha256_update(&ctx, (const uint8_t *)password, strlen(password));
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    for (int i = 0; i < 32; i++) {
        sprintf(out_hex + i * 2, "%02x", digest[i]);
    }
}

void config_set_admin_password(const char *password)
{
    if (!password || !*password) {
        g_cfg.admin_hash[0] = '\0';
        g_cfg.admin_salt[0] = '\0';
    } else {
        uint8_t salt[8];
        esp_fill_random(salt, sizeof(salt));
        for (int i = 0; i < 8; i++) {
            sprintf(g_cfg.admin_salt + i * 2, "%02x", salt[i]);
        }
        hash_password(g_cfg.admin_salt, password, g_cfg.admin_hash);
    }
    config_mark_dirty();
}

bool config_admin_enabled(void)
{
    return g_cfg.admin_hash[0] != '\0';
}

bool config_check_admin_password(const char *password)
{
    if (!config_admin_enabled() || !password) {
        return false;
    }
    char hex[65];
    hash_password(g_cfg.admin_salt, password, hex);
    uint8_t diff = 0;
    for (int i = 0; i < 64; i++) {
        diff |= (uint8_t)(hex[i] ^ g_cfg.admin_hash[i]);
    }
    return diff == 0;
}

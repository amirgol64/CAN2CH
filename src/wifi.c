#include "wifi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"

#include "config.h"

/*
 * With saved credentials the gateway is a station and never gives up on the
 * network: it retries with a growing delay (5 s up to 60 s). While the network
 * has been unreachable for FALLBACK_MS, the access point CANGW-XXXX is opened
 * as well, so the gateway stays reachable on site; it closes again as soon as
 * the station connection is back. Without credentials only the access point
 * runs.
 */
#define FALLBACK_MS 20000
#define RETRY_MIN_MS 5000
#define RETRY_MAX_MS 60000
#define SUPERVISE_MS 1000
#define MDNS_HOSTNAME "cangw"

static const char *TAG = "wifi";

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_timer;
static char s_ap_ssid[33];
static bool s_have_sta;

/* Shared between the event handler and the supervisor timer. */
static atomic_bool s_ap_on;
static atomic_bool s_connected;  /* has an IP address */
static atomic_bool s_associated; /* joined the access point (may still wait for DHCP) */
static atomic_uint s_down_since_ms;
static atomic_uint s_next_try_ms;
static atomic_uint s_retry_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Signed difference, correct across the 49-day wrap of the millisecond clock. */
static int32_t since(uint32_t t)
{
    return (int32_t)(now_ms() - t);
}

static void ap_config(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "CANGW-%02X%02X", mac[4], mac[5]);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.ap.ssid, s_ap_ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    wc.ap.channel = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &wc);
}

static void ap_open(void)
{
    if (atomic_exchange(&s_ap_on, true)) {
        return;
    }
    esp_wifi_set_mode(s_have_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    ap_config();
    ESP_LOGW(TAG, "access point \"%s\" open at http://192.168.4.1", s_ap_ssid);
}

static void ap_close(void)
{
    if (!atomic_exchange(&s_ap_on, false)) {
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "network is back, access point closed");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        atomic_store(&s_associated, true);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&s_associated, false);
        if (atomic_exchange(&s_connected, false)) {
            const wifi_event_sta_disconnected_t *ev = data;
            ESP_LOGW(TAG, "lost \"%s\" (reason %d), reconnecting", g_cfg.wifi_ssid, ev->reason);
            atomic_store(&s_down_since_ms, now_ms());
            atomic_store(&s_retry_ms, RETRY_MIN_MS);
            atomic_store(&s_next_try_ms, now_ms() + 1000);
        }
        /* Retries are made by the supervisor, with backoff. */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "connected to \"%s\", IP " IPSTR, g_cfg.wifi_ssid, IP2STR(&ev->ip_info.ip));
        atomic_store(&s_connected, true);
        atomic_store(&s_retry_ms, RETRY_MIN_MS);
    }
}

/* Runs every second in the esp_timer task. */
static void supervise(void *arg)
{
    if (!s_have_sta) {
        return;
    }
    if (atomic_load(&s_connected)) {
        ap_close();
        return;
    }
    if (since(atomic_load(&s_down_since_ms)) >= FALLBACK_MS) {
        ap_open();
    }
    /* Every attempt ends in a CONNECTED or DISCONNECTED event, so only start
     * a new one while not associated; never cut short a DHCP exchange. */
    if (!atomic_load(&s_associated) && since(atomic_load(&s_next_try_ms)) >= 0) {
        uint32_t delay = atomic_load(&s_retry_ms);
        atomic_store(&s_next_try_ms, now_ms() + delay);
        atomic_store(&s_retry_ms, delay * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : delay * 2);
        esp_wifi_connect();
    }
}

void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, MDNS_HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);

    s_have_sta = g_cfg.wifi_ssid[0] != '\0';
    if (s_have_sta) {
        wifi_config_t wc = {0};
        strlcpy((char *)wc.sta.ssid, g_cfg.wifi_ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, g_cfg.wifi_pass, sizeof(wc.sta.password));
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        atomic_store(&s_down_since_ms, now_ms());
        atomic_store(&s_retry_ms, RETRY_MIN_MS);
        atomic_store(&s_next_try_ms, now_ms() + RETRY_MIN_MS);
        ESP_LOGI(TAG, "connecting to \"%s\"", g_cfg.wifi_ssid);
    } else {
        atomic_store(&s_ap_on, true);
        esp_wifi_set_mode(WIFI_MODE_AP);
        ap_config();
        ESP_LOGI(TAG, "no network configured, access point \"%s\" at http://192.168.4.1", s_ap_ssid);
    }
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);

    const esp_timer_create_args_t args = {.callback = supervise, .name = "wifi_sup"};
    if (esp_timer_create(&args, &s_timer) == ESP_OK) {
        esp_timer_start_periodic(s_timer, SUPERVISE_MS * 1000);
    }

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(MDNS_HOSTNAME);
        mdns_instance_name_set("ESP32-S3 CAN gateway");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
}

void wifi_get_info(wifi_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->ap_mode = atomic_load(&s_ap_on);
    info->connected = atomic_load(&s_connected);
    esp_netif_ip_info_t ip = {0};
    if (info->connected || !info->ap_mode) {
        strlcpy(info->ssid, g_cfg.wifi_ssid, sizeof(info->ssid));
        esp_netif_get_ip_info(s_sta_netif, &ip);
        wifi_ap_record_t ap;
        if (info->connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            info->rssi = ap.rssi;
        }
        info->ap_mode = false;
    } else {
        strlcpy(info->ssid, s_ap_ssid, sizeof(info->ssid));
        esp_netif_get_ip_info(s_ap_netif, &ip);
    }
    if (s_have_sta) {
        strlcpy(info->sta_ssid, g_cfg.wifi_ssid, sizeof(info->sta_ssid));
    }
    snprintf(info->ip, sizeof(info->ip), IPSTR, IP2STR(&ip.ip));
}

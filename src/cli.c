#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "can1_twai.h"
#include "config.h"
#include "filter.h"
#include "gateway.h"
#include "ota.h"
#include "stats.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "cli";

/* "182" is a standard ID; "18FF50E5" or "182x" is an extended one. */
static bool parse_id_arg(const char *arg, uint32_t *id, bool *ext)
{
    char tmp[16];
    size_t len = strlen(arg);
    if (len == 0 || len >= sizeof(tmp)) {
        return false;
    }
    strcpy(tmp, arg);
    *ext = false;
    if (tmp[len - 1] == 'x' || tmp[len - 1] == 'X') {
        *ext = true;
        tmp[len - 1] = '\0';
    }
    if (!config_parse_hex_id(tmp, id)) {
        return false;
    }
    if (*id > CAN_STD_ID_MAX) {
        *ext = true;
    }
    return true;
}

static bool parse_on_off(const char *arg, bool *on)
{
    if (!strcmp(arg, "on") || !strcmp(arg, "1")) {
        *on = true;
        return true;
    }
    if (!strcmp(arg, "off") || !strcmp(arg, "0")) {
        *on = false;
        return true;
    }
    return false;
}

static int cmd_status(int argc, char **argv)
{
    wifi_info_t wi;
    can1_status_t c1;
    can2_status_t c2;
    wifi_get_info(&wi);
    can1_get_status(&c1);
    gateway_get_can2_status(&c2);
    const gateway_counters_t *g = &g_counters;

    printf("esp32s3-can-gateway %s, serial %s, up %llu s, running from %s\n", FW_VERSION,
           app_serial_number(), (unsigned long long)(esp_timer_get_time() / 1000000),
           ota_running_partition());
    printf("Reset   last cause: %s; free RAM %u bytes (lowest %u)\n", app_reset_reason(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    if (wi.ap_mode && wi.sta_ssid[0]) {
        printf("Wi-Fi   not connected to \"%s\" (retrying); access point \"%s\" ip %s\n",
               wi.sta_ssid, wi.ssid, wi.ip);
    } else if (wi.ap_mode) {
        printf("Wi-Fi   access point \"%s\" ip %s (no network configured)\n", wi.ssid, wi.ip);
    } else {
        printf("Wi-Fi   %s \"%s\" ip %s\n", wi.connected ? "connected to" : "connecting to", wi.ssid, wi.ip);
    }
    printf("CAN1    %u kbit/s%s, %s, load %u.%u%%, rx %u, tx %u, TEC %u REC %u, bus errors %u, missed %u, bus-off %u\n",
           g_cfg.bitrate1, g_cfg.listen_only1 ? " listen-only" : "", c1.state,
           web_bus_load(0) / 10, web_bus_load(0) % 10,
           (unsigned)atomic_load(&g->can1_rx), (unsigned)atomic_load(&g->can1_tx),
           (unsigned)c1.tec, (unsigned)c1.rec, (unsigned)c1.bus_errors, (unsigned)c1.rx_missed,
           (unsigned)c1.busoff_count);
    printf("CAN2    %u kbit/s, %u MHz crystal, %s, load %u.%u%%, tx %u, tx failed %u, rx %u, TEC %u REC %u, rx overflow %u, bus-off %u\n",
           g_cfg.bitrate2, c2.xtal_mhz, c2.state, web_bus_load(1) / 10, web_bus_load(1) % 10,
           (unsigned)atomic_load(&g->can2_tx), (unsigned)atomic_load(&g->can2_tx_fail),
           (unsigned)atomic_load(&g->can2_rx), c2.tec, c2.rec,
           (unsigned)atomic_load(&g->can2_rx_ovf), (unsigned)atomic_load(&g->can2_busoff));
    printf("Gateway forwarding %s, forwarded %u, blocked %u, dropped %u, queued %u, blocklist %u IDs\n",
           g_cfg.forwarding ? "on" : "off", (unsigned)atomic_load(&g->forwarded),
           (unsigned)atomic_load(&g->blocked), (unsigned)atomic_load(&g->dropped),
           gateway_queue_depth(), (unsigned)filter_count());
    printf("Queues  peak fill: CAN1 receive %u/%u, CAN2 send %u/%u (\"clear\" resets)\n",
           (unsigned)c1.rx_queue_peak, (unsigned)CAN1_RX_QUEUE_LEN,
           gateway_queue_peak(), gateway_queue_size());
    printf("Admin password %s\n", config_admin_enabled() ? "set" : "not set");
    return 0;
}

static int cmd_block(int argc, char **argv)
{
    uint32_t id;
    bool ext;
    if (argc != 2 || !parse_id_arg(argv[1], &id, &ext)) {
        printf("usage: %s <hex id>   (append x for an extended ID, e.g. 182x)\n", argv[0]);
        return 1;
    }
    bool add = strcmp(argv[0], "block") == 0;
    filter_err_t err = add ? filter_add(id, ext) : filter_remove(id, ext);
    if (err != FILTER_OK) {
        printf("%s\n", filter_strerror(err));
        return 1;
    }
    config_mark_dirty();
    printf("%s 0x%lX (%s)\n", add ? "blocked" : "unblocked", (unsigned long)id, ext ? "ext" : "std");
    return 0;
}

static int cmd_list(int argc, char **argv)
{
    filter_id_t *ids = malloc(FILTER_MAX_TOTAL * sizeof(filter_id_t));
    if (!ids) {
        return 1;
    }
    size_t n = filter_list(ids, FILTER_MAX_TOTAL);
    printf("%u blocked ID%s\n", (unsigned)n, n == 1 ? "" : "s");
    for (size_t i = 0; i < n; i++) {
        printf(ids[i].ext ? "  0x%08lX ext\n" : "  0x%03lX std\n", (unsigned long)ids[i].id);
    }
    free(ids);
    return 0;
}

static int cmd_bitrate(int argc, char **argv)
{
    int bus = argc == 3 ? atoi(argv[1]) : 0;
    int kbps = argc == 3 ? atoi(argv[2]) : 0;
    if ((bus != 1 && bus != 2) || !config_bitrate_valid(kbps)) {
        printf("usage: bitrate <1|2> <25|50|100|125|250|500|800|1000>\n");
        return 1;
    }
    if (bus == 1) {
        gateway_set_bus1((uint16_t)kbps, g_cfg.listen_only1);
    } else {
        gateway_set_bus2((uint16_t)kbps, g_cfg.xtal_mhz);
    }
    printf("CAN%d set to %d kbit/s\n", bus, kbps);
    return 0;
}

static int cmd_listen(int argc, char **argv)
{
    bool on;
    if (argc != 2 || !parse_on_off(argv[1], &on)) {
        printf("usage: listen on|off   (CAN1 listen-only mode)\n");
        return 1;
    }
    gateway_set_bus1(g_cfg.bitrate1, on);
    printf("CAN1 listen-only %s\n", on ? "on" : "off");
    return 0;
}

static int cmd_xtal(int argc, char **argv)
{
    int mhz = argc == 2 ? atoi(argv[1]) : -1;
    if (argc != 2 || !config_xtal_valid(mhz)) {
        printf("usage: xtal <0|8|12|16|20>   (XL2515 crystal in MHz, 0 = detect)\n");
        return 1;
    }
    gateway_set_bus2(g_cfg.bitrate2, (uint8_t)mhz);
    printf("XL2515 crystal set to %s\n", mhz ? argv[1] : "auto-detect");
    return 0;
}

static int cmd_fwd(int argc, char **argv)
{
    bool on;
    if (argc != 2 || !parse_on_off(argv[1], &on)) {
        printf("usage: fwd on|off\n");
        return 1;
    }
    gateway_set_forwarding(on);
    printf("forwarding %s\n", on ? "on" : "off");
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3 || strlen(argv[1]) > 32 || (argc == 3 && strlen(argv[2]) > 64)) {
        printf("usage: wifi <ssid> [password]   (wifi \"\" clears the credentials)\n");
        return 1;
    }
    strlcpy(g_cfg.wifi_ssid, argv[1], sizeof(g_cfg.wifi_ssid));
    strlcpy(g_cfg.wifi_pass, argc == 3 ? argv[2] : "", sizeof(g_cfg.wifi_pass));
    config_save_now();
    printf("saved, rebooting\n");
    app_reboot_later();
    return 0;
}

static int cmd_appass(int argc, char **argv)
{
    if (argc != 2 || !config_ap_pass_valid(argv[1])) {
        printf("usage: appass <password>   (8 to 63 characters, no spaces)\n");
        return 1;
    }
    strlcpy(g_cfg.ap_pass, argv[1], sizeof(g_cfg.ap_pass));
    config_save_now();
    printf("access point password saved, rebooting\n");
    app_reboot_later();
    return 0;
}

static int cmd_password(int argc, char **argv)
{
    if (argc != 2 || strlen(argv[1]) > 64) {
        printf("usage: password <new password>|off\n");
        return 1;
    }
    bool off = strcmp(argv[1], "off") == 0;
    config_set_admin_password(off ? "" : argv[1]);
    printf("admin password %s\n", off ? "removed" : "set");
    return 0;
}

static int cmd_send(int argc, char **argv)
{
    can_frame_t f = {0};
    bool ext;
    int bus = argc >= 3 ? atoi(argv[1]) : 0;
    if ((bus != 1 && bus != 2) || argc > 4 || !parse_id_arg(argv[2], &f.id, &ext)) {
        printf("usage: send <1|2> <hex id> [hex data]   e.g. send 2 123 DEADBEEF\n");
        return 1;
    }
    f.flags = ext ? CAN_FLAG_EXT : 0;
    if (argc == 4) {
        const char *s = argv[3];
        size_t len = strlen(s);
        if (len % 2 != 0 || len > 16 || strspn(s, "0123456789abcdefABCDEF") != len) {
            printf("data must be up to 8 hex bytes without spaces\n");
            return 1;
        }
        for (size_t i = 0; i < len / 2; i++) {
            char byte[3] = {s[i * 2], s[i * 2 + 1], '\0'};
            f.data[i] = (uint8_t)strtoul(byte, NULL, 16);
        }
        f.dlc = (uint8_t)(len / 2);
    }
    esp_err_t err = gateway_send(bus, &f);
    if (err != ESP_OK) {
        printf("send failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("queued on CAN%d\n", bus);
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    stats_clear();
    gateway_reset_peaks();
    printf("per-ID table and queue peaks cleared\n");
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    config_factory_reset();
    printf("settings erased, rebooting\n");
    app_reboot_later();
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    config_save_now();
    esp_restart();
    return 0;
}

void cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "cangw> ";
    repl_cfg.max_cmdline_length = 128;

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&hw, &repl_cfg, &repl);
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&hw, &repl_cfg, &repl);
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&hw, &repl_cfg, &repl);
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console not available: %s", esp_err_to_name(err));
        return;
    }

    const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "Show bus, gateway and Wi-Fi status", .func = cmd_status},
        {.command = "block", .help = "Block an ID: block <hex id>[x]", .func = cmd_block},
        {.command = "unblock", .help = "Stop blocking an ID: unblock <hex id>[x]", .func = cmd_block},
        {.command = "list", .help = "List blocked IDs", .func = cmd_list},
        {.command = "bitrate", .help = "Set a bus bitrate: bitrate <1|2> <kbps>", .func = cmd_bitrate},
        {.command = "listen", .help = "CAN1 listen-only mode: listen on|off", .func = cmd_listen},
        {.command = "xtal", .help = "XL2515 crystal: xtal <0|8|12|16|20> (0 = detect)", .func = cmd_xtal},
        {.command = "fwd", .help = "Forwarding master switch: fwd on|off", .func = cmd_fwd},
        {.command = "wifi", .help = "Set Wi-Fi credentials and reboot: wifi <ssid> [password]", .func = cmd_wifi},
        {.command = "appass", .help = "Set the access point password and reboot: appass <8-63 chars>", .func = cmd_appass},
        {.command = "password", .help = "Set or remove the web admin password: password <new>|off", .func = cmd_password},
        {.command = "send", .help = "Send a test frame: send <1|2> <hex id> [hex data]", .func = cmd_send},
        {.command = "clear", .help = "Clear the per-ID table and the queue peak marks", .func = cmd_clear},
        {.command = "factory-reset", .help = "Erase all settings and reboot", .func = cmd_factory_reset},
        {.command = "reboot", .help = "Restart the gateway", .func = cmd_reboot},
    };
    esp_console_register_help_command();
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    esp_console_start_repl(repl);
}

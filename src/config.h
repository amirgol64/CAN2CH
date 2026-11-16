#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_JSON_VERSION 1

typedef struct {
    uint16_t bitrate1; /* kbit/s */
    uint16_t bitrate2; /* kbit/s */
    bool listen_only1;
    bool forwarding;
    uint8_t xtal_mhz; /* XL2515 crystal: 0 = detect, else 8/12/16/20 */
    char wifi_ssid[33];
    char wifi_pass[65];
    char admin_salt[17]; /* hex */
    char admin_hash[65]; /* hex sha256(salt + password), empty = auth off */
    char ap_pass[64];    /* fallback access point WPA2 password, 8-63 chars */
} config_t;

/* Fallback access point: SSID is AP_SSID_PREFIX "-XXXX" (end of the MAC). */
#define AP_SSID_PREFIX "CAN-Filter"
#define AP_PASS_DEFAULT "vtron123"

extern config_t g_cfg;

/* ---- config_json.c: portable, also built for the host unit tests ---- */

void config_defaults(config_t *cfg);
/* Factory blocklist: 0x182 and 0x427. */
void config_default_filters(void);
bool config_bitrate_valid(int kbps);
bool config_xtal_valid(int mhz);
/* WPA2 needs 8 to 63 printable ASCII characters. */
bool config_ap_pass_valid(const char *pass);
/* Parses "0x1A2" or "1A2" (always hex). */
bool config_parse_hex_id(const char *s, uint32_t *id);
/* A CAN ID in JSON: either a number or a hex string. */
struct cJSON;
bool config_json_id(const struct cJSON *item, uint32_t *id);

/* Returns a malloc'd JSON string (config + blocklist, no secrets). */
char *config_to_json(const config_t *cfg);
/*
 * Validate and apply a JSON document to *cfg and the blocklist. Nothing is
 * changed unless the whole document is valid. Missing keys keep their value.
 */
bool config_from_json(const char *json, config_t *cfg, char *err, size_t err_len);

/* ---- config.c: device side ---- */

void config_init(void);
/* Schedule a save 1 s after the last change. */
void config_mark_dirty(void);
void config_save_now(void);
/* Erase the stored settings (DBC files are kept). Caller reboots. */
void config_factory_reset(void);

/* Empty or NULL password turns authentication off. */
void config_set_admin_password(const char *password);
bool config_admin_enabled(void);
bool config_check_admin_password(const char *password);

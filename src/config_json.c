#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "can_frame.h"
#include "config.h"
#include "filter.h"

static const uint16_t k_bitrates[] = {25, 50, 100, 125, 250, 500, 800, 1000};

void config_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->bitrate1 = 250;
    cfg->bitrate2 = 250;
    cfg->listen_only1 = false;
    cfg->forwarding = true;
    cfg->xtal_mhz = 0;
    strcpy(cfg->ap_pass, AP_PASS_DEFAULT);
}

bool config_ap_pass_valid(const char *pass)
{
    size_t len = pass ? strlen(pass) : 0;
    if (len < 8 || len > 63) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (pass[i] < 0x20 || pass[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

void config_default_filters(void)
{
    static const filter_id_t defaults[] = {{0x182, false}, {0x427, false}};
    filter_replace(defaults, sizeof(defaults) / sizeof(defaults[0]));
}

bool config_bitrate_valid(int kbps)
{
    for (size_t i = 0; i < sizeof(k_bitrates) / sizeof(k_bitrates[0]); i++) {
        if (k_bitrates[i] == kbps) {
            return true;
        }
    }
    return false;
}

bool config_xtal_valid(int mhz)
{
    return mhz == 0 || mhz == 8 || mhz == 12 || mhz == 16 || mhz == 20;
}

bool config_parse_hex_id(const char *s, uint32_t *id)
{
    if (!s || !*s) {
        return false;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || *end != '\0' || v > CAN_EXT_ID_MAX) {
        return false;
    }
    *id = (uint32_t)v;
    return true;
}

char *config_to_json(const config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", CONFIG_JSON_VERSION);

    cJSON *b1 = cJSON_AddObjectToObject(root, "bus1");
    cJSON_AddNumberToObject(b1, "bitrate", cfg->bitrate1);
    cJSON_AddBoolToObject(b1, "listen_only", cfg->listen_only1);

    cJSON *b2 = cJSON_AddObjectToObject(root, "bus2");
    cJSON_AddNumberToObject(b2, "bitrate", cfg->bitrate2);
    cJSON_AddNumberToObject(b2, "xtal_mhz", cfg->xtal_mhz);

    cJSON_AddBoolToObject(root, "forwarding", cfg->forwarding);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", cfg->wifi_ssid);

    cJSON *arr = cJSON_AddArrayToObject(root, "filters");
    filter_id_t *ids = malloc(FILTER_MAX_TOTAL * sizeof(filter_id_t));
    if (ids) {
        size_t n = filter_list(ids, FILTER_MAX_TOTAL);
        for (size_t i = 0; i < n; i++) {
            char hex[12];
            snprintf(hex, sizeof(hex), "0x%lX", (unsigned long)ids[i].id);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", hex);
            cJSON_AddBoolToObject(item, "ext", ids[i].ext);
            cJSON_AddItemToArray(arr, item);
        }
        free(ids);
    }

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    return out;
}

static bool fail(char *err, size_t err_len, const char *msg)
{
    if (err && err_len) {
        snprintf(err, err_len, "%s", msg);
    }
    return false;
}

/* An ID may be given as a number or as a hex string. */
bool config_json_id(const cJSON *item, uint32_t *id)
{
    if (cJSON_IsNumber(item)) {
        double v = item->valuedouble;
        if (v < 0 || v > CAN_EXT_ID_MAX || v != (double)(uint32_t)v) {
            return false;
        }
        *id = (uint32_t)v;
        return true;
    }
    if (cJSON_IsString(item)) {
        return config_parse_hex_id(item->valuestring, id);
    }
    return false;
}

bool config_from_json(const char *json, config_t *cfg, char *err, size_t err_len)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return fail(err, err_len, "not a JSON object");
    }

    config_t next = *cfg;
    filter_id_t *ids = NULL;
    size_t n_ids = 0;
    bool have_filters = false;
    bool ok = false;

    const cJSON *b1 = cJSON_GetObjectItem(root, "bus1");
    const cJSON *b2 = cJSON_GetObjectItem(root, "bus2");
    const cJSON *v;

    if ((v = cJSON_GetObjectItem(b1, "bitrate"))) {
        if (!cJSON_IsNumber(v) || !config_bitrate_valid(v->valueint)) {
            fail(err, err_len, "bus1.bitrate is not a supported bitrate");
            goto done;
        }
        next.bitrate1 = (uint16_t)v->valueint;
    }
    if ((v = cJSON_GetObjectItem(b1, "listen_only"))) {
        if (!cJSON_IsBool(v)) {
            fail(err, err_len, "bus1.listen_only must be true or false");
            goto done;
        }
        next.listen_only1 = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(b2, "bitrate"))) {
        if (!cJSON_IsNumber(v) || !config_bitrate_valid(v->valueint)) {
            fail(err, err_len, "bus2.bitrate is not a supported bitrate");
            goto done;
        }
        next.bitrate2 = (uint16_t)v->valueint;
    }
    if ((v = cJSON_GetObjectItem(b2, "xtal_mhz"))) {
        if (!cJSON_IsNumber(v) || !config_xtal_valid(v->valueint)) {
            fail(err, err_len, "bus2.xtal_mhz must be 0, 8, 12, 16 or 20");
            goto done;
        }
        next.xtal_mhz = (uint8_t)v->valueint;
    }
    if ((v = cJSON_GetObjectItem(root, "forwarding"))) {
        if (!cJSON_IsBool(v)) {
            fail(err, err_len, "forwarding must be true or false");
            goto done;
        }
        next.forwarding = cJSON_IsTrue(v);
    }

    const cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if ((v = cJSON_GetObjectItem(wifi, "ssid"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(next.wifi_ssid)) {
            fail(err, err_len, "wifi.ssid must be a string of at most 32 characters");
            goto done;
        }
        if (strcmp(next.wifi_ssid, v->valuestring) != 0) {
            strcpy(next.wifi_ssid, v->valuestring);
            next.wifi_pass[0] = '\0';
        }
    }
    if ((v = cJSON_GetObjectItem(wifi, "password"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(next.wifi_pass)) {
            fail(err, err_len, "wifi.password must be a string of at most 64 characters");
            goto done;
        }
        strcpy(next.wifi_pass, v->valuestring);
    }

    const cJSON *filters = cJSON_GetObjectItem(root, "filters");
    if (filters) {
        if (!cJSON_IsArray(filters)) {
            fail(err, err_len, "filters must be an array");
            goto done;
        }
        int count = cJSON_GetArraySize(filters);
        if (count > FILTER_MAX_TOTAL) {
            fail(err, err_len, "too many filters");
            goto done;
        }
        ids = malloc((count ? count : 1) * sizeof(filter_id_t));
        if (!ids) {
            fail(err, err_len, "out of memory");
            goto done;
        }
        const cJSON *item;
        cJSON_ArrayForEach(item, filters) {
            const cJSON *ext = cJSON_GetObjectItem(item, "ext");
            uint32_t id;
            if (!config_json_id(cJSON_GetObjectItem(item, "id"), &id) || (ext && !cJSON_IsBool(ext))) {
                fail(err, err_len, "filters: each entry needs a valid id and an optional ext flag");
                goto done;
            }
            bool is_ext = cJSON_IsTrue(ext);
            if (!is_ext && id > CAN_STD_ID_MAX) {
                fail(err, err_len, "filters: standard ID above 0x7FF");
                goto done;
            }
            ids[n_ids].id = id;
            ids[n_ids].ext = is_ext;
            n_ids++;
        }
        have_filters = true;
    }

    if (have_filters) {
        filter_err_t ferr = filter_replace(ids, n_ids);
        if (ferr != FILTER_OK) {
            fail(err, err_len, filter_strerror(ferr));
            goto done;
        }
    }
    *cfg = next;
    ok = true;

done:
    free(ids);
    cJSON_Delete(root);
    return ok;
}

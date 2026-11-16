#include "web.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "can1_twai.h"
#include "config.h"
#include "filter.h"
#include "gateway.h"
#include "ota.h"
#include "stats.h"
#include "web_assets.h"
#include "wifi.h"

#define FS_BASE "/fs"
#define DBC_MAX_BYTES (8 * 1024 * 1024)
#define BODY_MAX_BYTES (128 * 1024)
#define PUSH_PERIOD_MS 250
#define WS_CHUNK_BYTES 32768
#define MAX_SESSIONS 4
#define MAX_CLIENTS 7

static const char *TAG = "web";

static httpd_handle_t s_httpd;
static atomic_int s_ws_pending;
static atomic_int s_ws_clients;
static atomic_bool s_ws_full;
static atomic_uint s_load[2];
static char s_sessions[MAX_SESSIONS][33];
static int s_session_next;

/* ---------------- helpers ---------------- */

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (; *in && o + 7 < out_len; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, 7, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char esc[160], body[192];
    json_escape(msg, esc, sizeof(esc));
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", esc);
    return send_json(req, status, body);
}

static bool req_authed(httpd_req_t *req)
{
    if (!config_admin_enabled()) {
        return true;
    }
    char sid[40];
    size_t len = sizeof(sid);
    if (httpd_req_get_cookie_val(req, "sid", sid, &len) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i][0] && strcmp(s_sessions[i], sid) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Every request that changes something must carry the X-CANGW header (a
 * browser will not add it to a cross-site request without a CORS preflight,
 * which this server never approves) and, if a password is set, a session.
 */
static bool guard_write(httpd_req_t *req, bool need_auth)
{
    if (httpd_req_get_hdr_value_len(req, "X-CANGW") == 0) {
        send_error(req, "403 Forbidden", "missing X-CANGW header");
        return false;
    }
    if (need_auth && !req_authed(req)) {
        send_error(req, "401 Unauthorized", "login required");
        return false;
    }
    return true;
}

static void new_session(httpd_req_t *req)
{
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    char *sid = s_sessions[s_session_next];
    s_session_next = (s_session_next + 1) % MAX_SESSIONS;
    for (int i = 0; i < 16; i++) {
        sprintf(sid + i * 2, "%02x", rnd[i]);
    }
    static char cookie[MAX_SESSIONS][96];
    static int slot;
    char *c = cookie[slot];
    slot = (slot + 1) % MAX_SESSIONS;
    snprintf(c, 96, "sid=%s; Path=/; HttpOnly; SameSite=Strict", sid);
    httpd_resp_set_hdr(req, "Set-Cookie", c);
}

/* Returns a malloc'd, NUL-terminated copy of the request body, or NULL. */
static char *read_body(httpd_req_t *req, size_t max)
{
    if (req->content_len == 0 || req->content_len > max) {
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        got += (size_t)n;
    }
    buf[got] = '\0';
    return buf;
}

static cJSON *read_json(httpd_req_t *req)
{
    char *body = read_body(req, BODY_MAX_BYTES);
    if (!body) {
        send_error(req, "400 Bad Request", "missing or oversized request body");
        return NULL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        send_error(req, "400 Bad Request", "request body is not a JSON object");
        return NULL;
    }
    return root;
}

/* Bus number from the last character of the URI path ("/api/bus/2"). */
static int uri_bus(httpd_req_t *req)
{
    const char *uri = req->uri;
    size_t len = strcspn(uri, "?");
    if (len == 0) {
        return 0;
    }
    char c = uri[len - 1];
    return (c == '1' || c == '2') ? c - '0' : 0;
}

/* ---------------- live data ---------------- */

static int build_live(char *buf, size_t size)
{
    can1_status_t c1;
    can2_status_t c2;
    can1_get_status(&c1);
    gateway_get_can2_status(&c2);
    const gateway_counters_t *g = &g_counters;
    return snprintf(buf, size,
        "\"up\":%llu,"
        "\"b1\":{\"st\":\"%s\",\"br\":%u,\"lo\":%s,\"rx\":%u,\"tx\":%u,\"tec\":%u,\"rec\":%u,"
        "\"err\":%u,\"miss\":%u,\"boff\":%u,\"load\":%u,\"untracked\":%u,\"qpk\":%u,\"qsz\":%u},"
        "\"b2\":{\"st\":\"%s\",\"br\":%u,\"xtal\":%u,\"rx\":%u,\"tx\":%u,\"fail\":%u,\"tec\":%u,"
        "\"rec\":%u,\"ovf\":%u,\"boff\":%u,\"load\":%u,\"untracked\":%u},"
        "\"gw\":{\"on\":%s,\"fwd\":%u,\"blk\":%u,\"off\":%u,\"drop\":%u,\"q\":%u,\"qpk\":%u,\"qsz\":%u,"
        "\"fg\":%u,\"cg\":%u}",
        (unsigned long long)(esp_timer_get_time() / 1000),
        c1.state, (unsigned)g_cfg.bitrate1, g_cfg.listen_only1 ? "true" : "false",
        (unsigned)atomic_load(&g->can1_rx), (unsigned)atomic_load(&g->can1_tx),
        (unsigned)c1.tec, (unsigned)c1.rec, (unsigned)c1.bus_errors, (unsigned)c1.rx_missed,
        (unsigned)c1.busoff_count, (unsigned)atomic_load(&s_load[0]), (unsigned)stats_untracked(0),
        (unsigned)c1.rx_queue_peak, (unsigned)CAN1_RX_QUEUE_LEN,
        c2.state, (unsigned)g_cfg.bitrate2, (unsigned)c2.xtal_mhz,
        (unsigned)atomic_load(&g->can2_rx), (unsigned)atomic_load(&g->can2_tx),
        (unsigned)atomic_load(&g->can2_tx_fail), (unsigned)c2.tec, (unsigned)c2.rec,
        (unsigned)atomic_load(&g->can2_rx_ovf), (unsigned)atomic_load(&g->can2_busoff),
        (unsigned)atomic_load(&s_load[1]), (unsigned)stats_untracked(1),
        g_cfg.forwarding ? "true" : "false",
        (unsigned)atomic_load(&g->forwarded), (unsigned)atomic_load(&g->blocked),
        (unsigned)atomic_load(&g->not_forwarded), (unsigned)atomic_load(&g->dropped),
        gateway_queue_depth(), gateway_queue_peak(), gateway_queue_size(),
        (unsigned)filter_generation(), (unsigned)stats_clear_generation());
}

typedef struct {
    size_t len;
    char data[];
} ws_msg_t;

static ws_msg_t *ws_msg_alloc(size_t cap)
{
    return heap_caps_malloc_prefer(sizeof(ws_msg_t) + cap, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

/* Runs in the HTTP server task. */
static void ws_broadcast_work(void *arg)
{
    ws_msg_t *m = arg;
    int fds[MAX_CLIENTS];
    size_t n = MAX_CLIENTS;
    int clients = 0;
    if (httpd_get_client_list(s_httpd, &n, fds) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_httpd, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
                continue;
            }
            clients++;
            httpd_ws_frame_t frame = {
                .final = true,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)m->data,
                .len = m->len,
            };
            if (httpd_ws_send_frame_async(s_httpd, fds[i], &frame) != ESP_OK) {
                httpd_sess_trigger_close(s_httpd, fds[i]);
            }
        }
    }
    atomic_store(&s_ws_clients, clients);
    free(m);
    atomic_fetch_sub(&s_ws_pending, 1);
}

static void ws_queue(ws_msg_t *m)
{
    atomic_fetch_add(&s_ws_pending, 1);
    if (httpd_queue_work(s_httpd, ws_broadcast_work, m) != ESP_OK) {
        free(m);
        atomic_fetch_sub(&s_ws_pending, 1);
    }
}

static void ws_queue_str(const char *s)
{
    size_t len = strlen(s);
    ws_msg_t *m = ws_msg_alloc(len + 1);
    if (m) {
        memcpy(m->data, s, len + 1);
        m->len = len;
        ws_queue(m);
    }
}

static void push_task(void *arg)
{
    uint32_t *last[STATS_BUSES];
    for (int b = 0; b < STATS_BUSES; b++) {
        last[b] = heap_caps_calloc_prefer(STATS_SLOTS, sizeof(uint32_t), 2,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
        assert(last[b]);
    }
    uint32_t prev_bits[2] = {0, 0};
    int64_t prev_t = esp_timer_get_time();
    uint32_t seen_clear = stats_clear_generation();
    static char live[1024];
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(PUSH_PERIOD_MS));

        /* Bus load, in tenths of a percent of the configured bitrate. */
        int64_t now = esp_timer_get_time();
        int64_t dt = now - prev_t;
        prev_t = now;
        const uint16_t kbps[2] = {g_cfg.bitrate1, g_cfg.bitrate2};
        for (int b = 0; b < 2; b++) {
            uint32_t bits = atomic_load(&g_counters.bits[b]);
            uint32_t delta = bits - prev_bits[b];
            prev_bits[b] = bits;
            uint64_t load = dt > 0 && kbps[b] ? (uint64_t)delta * 1000000ull / ((uint64_t)kbps[b] * (uint64_t)dt) : 0;
            atomic_store(&s_load[b], load > 1000 ? 1000u : (unsigned)load);
        }

        /* Fall behind rather than queue up behind a slow client. */
        if (atomic_load(&s_ws_pending) > 0) {
            continue;
        }

        int n = snprintf(live, sizeof(live), "{\"t\":\"s\",");
        n += build_live(live + n, sizeof(live) - (size_t)n);
        if (n < (int)sizeof(live) - 2) {
            strcpy(live + n, "}");
            ws_queue_str(live);
        }

        bool full = atomic_exchange(&s_ws_full, false);
        if (atomic_load(&s_ws_clients) == 0 && !full) {
            continue;
        }
        uint32_t clear_gen = stats_clear_generation();
        if (clear_gen != seen_clear) {
            seen_clear = clear_gen;
            for (int b = 0; b < STATS_BUSES; b++) {
                memset(last[b], 0, STATS_SLOTS * sizeof(uint32_t));
            }
            ws_queue_str("{\"t\":\"clr\"}");
            full = true;
        }

        /* Per-ID rows that changed since the last push (all rows if `full`). */
        uint32_t now_ms = (uint32_t)(now / 1000);
        ws_msg_t *m = NULL;
        for (int b = 0; b < STATS_BUSES; b++) {
            for (int slot = 0; slot < STATS_SLOTS; slot++) {
                stats_row_t row;
                if (!stats_read(b, slot, &row)) {
                    continue;
                }
                if (!full && row.count == last[b][slot]) {
                    continue;
                }
                last[b][slot] = row.count;
                bool first = false;
                if (!m) {
                    m = ws_msg_alloc(WS_CHUNK_BYTES);
                    if (!m) {
                        break;
                    }
                    m->len = (size_t)snprintf(m->data, WS_CHUNK_BYTES, "{\"t\":\"i\",\"e\":[");
                    first = true;
                }
                char hex[17] = "";
                if (!row.rtr) {
                    for (int i = 0; i < row.dlc && i < 8; i++) {
                        sprintf(hex + i * 2, "%02X", row.data[i]);
                    }
                }
                m->len += (size_t)snprintf(m->data + m->len, WS_CHUNK_BYTES - m->len,
                                           "%s[%d,%u,%d,%u,%u,%d,\"%s\",%u]",
                                           first ? "" : ",", b + 1, (unsigned)row.id, row.ext ? 1 : 0,
                                           (unsigned)row.count, (unsigned)row.dlc, row.rtr ? 1 : 0, hex,
                                           (unsigned)(now_ms - row.last_ms));
                if (m->len > WS_CHUNK_BYTES - 128) {
                    m->len += (size_t)snprintf(m->data + m->len, WS_CHUNK_BYTES - m->len, "]}");
                    ws_queue(m);
                    m = NULL;
                }
            }
        }
        if (m) {
            m->len += (size_t)snprintf(m->data + m->len, WS_CHUNK_BYTES - m->len, "]}");
            ws_queue(m);
        }
    }
}

unsigned web_bus_load(int bus)
{
    return atomic_load(&s_load[bus & 1]);
}

/* ---------------- handlers ---------------- */

static long dbc_size(int bus)
{
    char path[32];
    snprintf(path, sizeof(path), FS_BASE "/can%d.dbc", bus);
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static esp_err_t status_get(httpd_req_t *req)
{
    char *buf = malloc(2048);
    if (!buf) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    wifi_info_t wi;
    wifi_get_info(&wi);
    char ssid[80], sta[80];
    json_escape(wi.ssid, ssid, sizeof(ssid));
    json_escape(wi.sta_ssid, sta, sizeof(sta));

    int n = snprintf(buf, 2048, "{\"fw\":\"%s\",\"serial\":\"%s\",", FW_VERSION,
                     app_serial_number());
    n += build_live(buf + n, 2048 - (size_t)n);
    n += snprintf(buf + n, 2048 - (size_t)n,
        ",\"wifi\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"sta\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"connected\":%s},"
        "\"auth\":{\"required\":%s,\"authed\":%s},"
        "\"dbc\":[%ld,%ld],\"heap\":%u,\"minheap\":%u,\"psram\":%u,\"partition\":\"%s\",\"filters\":%u,"
        "\"reset\":\"%s\"}",
        wi.ap_mode ? "ap" : "sta", ssid, sta, wi.ip, wi.rssi, wi.connected ? "true" : "false",
        config_admin_enabled() ? "true" : "false", req_authed(req) ? "true" : "false",
        dbc_size(1), dbc_size(2),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        ota_running_partition(), (unsigned)filter_count(), app_reset_reason());
    esp_err_t err = send_json(req, "200 OK", buf);
    free(buf);
    return err;
}

static esp_err_t config_get(httpd_req_t *req)
{
    char *json = config_to_json(&g_cfg);
    if (!json) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    esp_err_t err = send_json(req, "200 OK", json);
    free(json);
    return err;
}

static esp_err_t config_put(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    char *body = read_body(req, BODY_MAX_BYTES);
    if (!body) {
        return send_error(req, "400 Bad Request", "missing or oversized request body");
    }
    char err[96];
    bool ok = config_from_json(body, &g_cfg, err, sizeof(err));
    free(body);
    if (!ok) {
        return send_error(req, "400 Bad Request", err);
    }
    gateway_apply_config();
    config_mark_dirty();
    return send_ok(req);
}

static esp_err_t filters_get(httpd_req_t *req)
{
    filter_id_t *ids = malloc(FILTER_MAX_TOTAL * sizeof(filter_id_t));
    if (!ids) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t n = filter_list(ids, FILTER_MAX_TOTAL);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[512];
    size_t len = 0;
    buf[len++] = '[';
    for (size_t i = 0; i < n; i++) {
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s{\"id\":%u,\"ext\":%s}",
                                i ? "," : "", (unsigned)ids[i].id, ids[i].ext ? "true" : "false");
        if (len > sizeof(buf) - 48) {
            httpd_resp_send_chunk(req, buf, (ssize_t)len);
            len = 0;
        }
    }
    buf[len++] = ']';
    httpd_resp_send_chunk(req, buf, (ssize_t)len);
    free(ids);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool json_filter_id(httpd_req_t *req, const cJSON *root, uint32_t *id, bool *ext)
{
    const cJSON *e = cJSON_GetObjectItem(root, "ext");
    *ext = cJSON_IsTrue(e);
    if (!config_json_id(cJSON_GetObjectItem(root, "id"), id) || (!*ext && *id > CAN_STD_ID_MAX)) {
        send_error(req, "400 Bad Request", "invalid id (standard IDs go up to 0x7FF)");
        return false;
    }
    return true;
}

static esp_err_t filters_post(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (!root) {
        return ESP_OK;
    }
    uint32_t id;
    bool ext;
    bool ok = json_filter_id(req, root, &id, &ext);
    cJSON_Delete(root);
    if (!ok) {
        return ESP_OK;
    }
    filter_err_t err = filter_add(id, ext);
    if (err != FILTER_OK && err != FILTER_EXISTS) {
        return send_error(req, "400 Bad Request", filter_strerror(err));
    }
    config_mark_dirty();
    return send_ok(req);
}

static esp_err_t filters_delete(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (!root) {
        return ESP_OK;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "all"))) {
        cJSON_Delete(root);
        filter_clear();
        config_mark_dirty();
        return send_ok(req);
    }
    uint32_t id;
    bool ext;
    bool ok = json_filter_id(req, root, &id, &ext);
    cJSON_Delete(root);
    if (!ok) {
        return ESP_OK;
    }
    filter_err_t err = filter_remove(id, ext);
    if (err != FILTER_OK && err != FILTER_NOT_FOUND) {
        return send_error(req, "400 Bad Request", filter_strerror(err));
    }
    config_mark_dirty();
    return send_ok(req);
}

static esp_err_t bus_put(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    int bus = uri_bus(req);
    if (!bus) {
        return send_error(req, "404 Not Found", "bus must be 1 or 2");
    }
    cJSON *root = read_json(req);
    if (!root) {
        return ESP_OK;
    }
    const cJSON *br = cJSON_GetObjectItem(root, "bitrate");
    int kbps = cJSON_IsNumber(br) ? br->valueint : (bus == 1 ? g_cfg.bitrate1 : g_cfg.bitrate2);
    bool ok;
    if (bus == 1) {
        const cJSON *lo = cJSON_GetObjectItem(root, "listen_only");
        bool listen = cJSON_IsBool(lo) ? cJSON_IsTrue(lo) : g_cfg.listen_only1;
        ok = config_bitrate_valid(kbps) && gateway_set_bus1((uint16_t)kbps, listen);
    } else {
        const cJSON *x = cJSON_GetObjectItem(root, "xtal_mhz");
        int xtal = cJSON_IsNumber(x) ? x->valueint : g_cfg.xtal_mhz;
        ok = config_bitrate_valid(kbps) && config_xtal_valid(xtal) &&
             gateway_set_bus2((uint16_t)kbps, (uint8_t)xtal);
    }
    cJSON_Delete(root);
    return ok ? send_ok(req) : send_error(req, "400 Bad Request", "unsupported bitrate or crystal value");
}

static esp_err_t forwarding_put(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (!root) {
        return ESP_OK;
    }
    const cJSON *en = cJSON_GetObjectItem(root, "enabled");
    bool ok = cJSON_IsBool(en);
    if (ok) {
        gateway_set_forwarding(cJSON_IsTrue(en));
    }
    cJSON_Delete(root);
    return ok ? send_ok(req) : send_error(req, "400 Bad Request", "enabled must be true or false");
}

static bool parse_hex_bytes(const char *s, uint8_t *out, uint8_t *count)
{
    uint8_t n = 0;
    int nibbles = 0;
    unsigned cur = 0;
    for (; *s; s++) {
        char c = *s;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == ' ' || c == ',' || c == ':' || c == '-') {
            if (nibbles == 1) return false;
            continue;
        } else return false;
        cur = (cur << 4) | (unsigned)v;
        if (++nibbles == 2) {
            if (n >= 8) return false;
            out[n++] = (uint8_t)cur;
            nibbles = 0;
            cur = 0;
        }
    }
    if (nibbles != 0) return false;
    *count = n;
    return true;
}

static esp_err_t tx_post(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (!root) {
        return ESP_OK;
    }
    can_frame_t f = {0};
    const char *problem = NULL;
    const cJSON *bus = cJSON_GetObjectItem(root, "bus");
    const cJSON *data = cJSON_GetObjectItem(root, "data");
    const cJSON *dlc = cJSON_GetObjectItem(root, "dlc");
    bool ext = cJSON_IsTrue(cJSON_GetObjectItem(root, "ext"));
    bool rtr = cJSON_IsTrue(cJSON_GetObjectItem(root, "rtr"));

    if (!cJSON_IsNumber(bus) || (bus->valueint != 1 && bus->valueint != 2)) {
        problem = "bus must be 1 or 2";
    } else if (!config_json_id(cJSON_GetObjectItem(root, "id"), &f.id) || (!ext && f.id > CAN_STD_ID_MAX)) {
        problem = "invalid id";
    } else if (cJSON_IsString(data)) {
        if (!parse_hex_bytes(data->valuestring, f.data, &f.dlc)) {
            problem = "data must be up to 8 hex bytes";
        }
    } else if (cJSON_IsArray(data)) {
        const cJSON *b;
        cJSON_ArrayForEach(b, data) {
            if (!cJSON_IsNumber(b) || b->valueint < 0 || b->valueint > 255 || f.dlc >= 8) {
                problem = "data must be up to 8 bytes";
                break;
            }
            f.data[f.dlc++] = (uint8_t)b->valueint;
        }
    } else if (data) {
        problem = "data must be a hex string or an array of bytes";
    }
    if (!problem && rtr) {
        if (cJSON_IsNumber(dlc) && dlc->valueint >= 0 && dlc->valueint <= 8) {
            f.dlc = (uint8_t)dlc->valueint;
        }
        memset(f.data, 0, sizeof(f.data));
    }
    f.flags = (ext ? CAN_FLAG_EXT : 0) | (rtr ? CAN_FLAG_RTR : 0);
    int bus_no = cJSON_IsNumber(bus) ? bus->valueint : 0;
    cJSON_Delete(root);
    if (problem) {
        return send_error(req, "400 Bad Request", problem);
    }

    esp_err_t err = gateway_send(bus_no, &f);
    switch (err) {
    case ESP_OK: return send_ok(req);
    case ESP_ERR_NOT_SUPPORTED: return send_error(req, "409 Conflict", "CAN1 is in listen-only mode");
    case ESP_ERR_INVALID_STATE: return send_error(req, "409 Conflict", "bus is not running");
    default: return send_error(req, "503 Service Unavailable", "transmit queue is full or the bus is not acknowledging");
    }
}

static esp_err_t dbc_get(httpd_req_t *req)
{
    int bus = uri_bus(req);
    char path[32];
    snprintf(path, sizeof(path), FS_BASE "/can%d.dbc", bus);
    FILE *fp = bus ? fopen(path, "rb") : NULL;
    if (!fp) {
        return send_error(req, "404 Not Found", "no DBC stored for this bus");
    }
    char *buf = malloc(4096);
    if (!buf) {
        fclose(fp);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, 4096, fp)) > 0) {
        err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
    }
    fclose(fp);
    free(buf);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t dbc_put(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    int bus = uri_bus(req);
    if (!bus) {
        return send_error(req, "404 Not Found", "bus must be 1 or 2");
    }
    if (req->content_len == 0 || req->content_len > DBC_MAX_BYTES) {
        return send_error(req, "400 Bad Request", "DBC file is empty or larger than 8 MB");
    }
    char path[32], tmp[32];
    snprintf(path, sizeof(path), FS_BASE "/can%d.dbc", bus);
    snprintf(tmp, sizeof(tmp), FS_BASE "/can%d.tmp", bus);
    FILE *fp = fopen(tmp, "wb");
    char *buf = malloc(4096);
    if (!fp || !buf) {
        if (fp) fclose(fp);
        free(buf);
        return send_error(req, "500 Internal Server Error", "cannot open file for writing");
    }
    size_t remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0 || fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
            ok = false;
            break;
        }
        remaining -= (size_t)n;
    }
    free(buf);
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp);
        return send_error(req, "500 Internal Server Error", "upload failed (storage full or connection lost)");
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        return send_error(req, "500 Internal Server Error", "could not store the file");
    }
    return send_ok(req);
}

static esp_err_t dbc_delete(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    int bus = uri_bus(req);
    if (!bus) {
        return send_error(req, "404 Not Found", "bus must be 1 or 2");
    }
    char path[32];
    snprintf(path, sizeof(path), FS_BASE "/can%d.dbc", bus);
    unlink(path);
    return send_ok(req);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    char err[96] = "";
    if (ota_handle_upload(req, err, sizeof(err)) != ESP_OK) {
        return send_error(req, "400 Bad Request", err);
    }
    send_ok(req);
    app_reboot_later();
    return ESP_OK;
}

static esp_err_t login_post(httpd_req_t *req)
{
    if (!guard_write(req, false)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (!root) {
        return ESP_OK;
    }
    /*
     * Slow down guessing without stalling the server: after each wrong
     * password, refuse further attempts for 1 s more than the last time
     * (up to 30 s).
     */
    static int64_t s_locked_until;
    static int s_failures;
    int64_t now = esp_timer_get_time();
    if (now < s_locked_until) {
        cJSON_Delete(root);
        return send_error(req, "429 Too Many Requests", "too many wrong passwords, wait a few seconds");
    }
    const cJSON *pw = cJSON_GetObjectItem(root, "password");
    bool ok = !config_admin_enabled() ||
              (cJSON_IsString(pw) && config_check_admin_password(pw->valuestring));
    cJSON_Delete(root);
    if (!ok) {
        if (s_failures < 30) {
            s_failures++;
        }
        s_locked_until = now + (int64_t)s_failures * 1000000;
        return send_error(req, "401 Unauthorized", "wrong password");
    }
    s_failures = 0;
    if (config_admin_enabled()) {
        new_session(req);
    }
    return send_ok(req);
}

static esp_err_t logout_post(httpd_req_t *req)
{
    if (!guard_write(req, false)) {
        return ESP_OK;
    }
    char sid[40];
    size_t len = sizeof(sid);
    if (httpd_req_get_cookie_val(req, "sid", sid, &len) == ESP_OK) {
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (strcmp(s_sessions[i], sid) == 0) {
                s_sessions[i][0] = '\0';
            }
        }
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; Max-Age=0");
    return send_ok(req);
}

static esp_err_t password_put(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (!root) {
        return ESP_OK;
    }
    const cJSON *pw = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(pw) || strlen(pw->valuestring) > 64) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "password must be a string of at most 64 characters");
    }
    config_set_admin_password(pw->valuestring);
    bool enabled = pw->valuestring[0] != '\0';
    cJSON_Delete(root);
    memset(s_sessions, 0, sizeof(s_sessions));
    if (enabled) {
        new_session(req);
    }
    return send_ok(req);
}

static esp_err_t wifi_put(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (!root) {
        return ESP_OK;
    }
    const cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(root, "password");
    bool ok = cJSON_IsString(ssid) && strlen(ssid->valuestring) <= 32 &&
              (!pass || (cJSON_IsString(pass) && strlen(pass->valuestring) <= 64));
    if (ok) {
        strlcpy(g_cfg.wifi_ssid, ssid->valuestring, sizeof(g_cfg.wifi_ssid));
        strlcpy(g_cfg.wifi_pass, pass ? pass->valuestring : "", sizeof(g_cfg.wifi_pass));
    }
    cJSON_Delete(root);
    if (!ok) {
        return send_error(req, "400 Bad Request", "ssid (max 32) and password (max 64) must be strings");
    }
    config_save_now();
    send_ok(req);
    app_reboot_later();
    return ESP_OK;
}

static esp_err_t ap_password_put(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    cJSON *root = read_json(req);
    if (!root) {
        return ESP_OK;
    }
    const cJSON *pw = cJSON_GetObjectItem(root, "password");
    bool ok = cJSON_IsString(pw) && config_ap_pass_valid(pw->valuestring);
    if (ok) {
        strlcpy(g_cfg.ap_pass, pw->valuestring, sizeof(g_cfg.ap_pass));
    }
    cJSON_Delete(root);
    if (!ok) {
        return send_error(req, "400 Bad Request", "password must be 8 to 63 characters");
    }
    config_save_now();
    send_ok(req);
    app_reboot_later();
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    config_save_now();
    send_ok(req);
    app_reboot_later();
    return ESP_OK;
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    config_factory_reset();
    send_ok(req);
    app_reboot_later();
    return ESP_OK;
}

static esp_err_t stats_clear_post(httpd_req_t *req)
{
    if (!guard_write(req, true)) {
        return ESP_OK;
    }
    stats_clear();
    gateway_reset_peaks();
    return send_ok(req);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake finished: send the newcomer a complete table. */
        atomic_store(&s_ws_full, true);
        return ESP_OK;
    }
    /* Nothing is expected from the client; read and ignore whatever it sends. */
    uint8_t buf[128];
    httpd_ws_frame_t frame = {.payload = buf};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (frame.len > sizeof(buf)) {
        return ESP_FAIL;
    }
    return frame.len ? httpd_ws_recv_frame(req, &frame, frame.len) : ESP_OK;
}

static esp_err_t static_get(httpd_req_t *req)
{
    const char *uri = req->uri;
    size_t len = strcspn(uri, "?");
    if (len == 1 && uri[0] == '/') {
        uri = "/index.html";
        len = strlen(uri);
    }
    for (size_t i = 0; i < web_assets_count; i++) {
        const web_asset_t *a = &web_assets[i];
        if (strlen(a->path) == len && strncmp(a->path, uri, len) == 0) {
            httpd_resp_set_type(req, a->mime);
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
            return httpd_resp_send(req, (const char *)a->data, (ssize_t)a->len);
        }
    }
    return send_error(req, "404 Not Found", "not found");
}

/* ---------------- start-up ---------------- */

bool web_fs_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = FS_BASE,
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 32;
    cfg.max_open_sockets = MAX_CLIENTS;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.send_wait_timeout = 5;
    cfg.recv_wait_timeout = 10;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return false;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/config", .method = HTTP_PUT, .handler = config_put},
        {.uri = "/api/filters", .method = HTTP_GET, .handler = filters_get},
        {.uri = "/api/filters", .method = HTTP_POST, .handler = filters_post},
        {.uri = "/api/filters", .method = HTTP_DELETE, .handler = filters_delete},
        {.uri = "/api/bus/*", .method = HTTP_PUT, .handler = bus_put},
        {.uri = "/api/forwarding", .method = HTTP_PUT, .handler = forwarding_put},
        {.uri = "/api/tx", .method = HTTP_POST, .handler = tx_post},
        {.uri = "/api/dbc/*", .method = HTTP_GET, .handler = dbc_get},
        {.uri = "/api/dbc/*", .method = HTTP_PUT, .handler = dbc_put},
        {.uri = "/api/dbc/*", .method = HTTP_DELETE, .handler = dbc_delete},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post},
        {.uri = "/api/login", .method = HTTP_POST, .handler = login_post},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = logout_post},
        {.uri = "/api/password", .method = HTTP_PUT, .handler = password_put},
        {.uri = "/api/wifi", .method = HTTP_PUT, .handler = wifi_put},
        {.uri = "/api/ap-password", .method = HTTP_PUT, .handler = ap_password_put},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
        {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_post},
        {.uri = "/api/stats/clear", .method = HTTP_POST, .handler = stats_clear_post},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
        {.uri = "/*", .method = HTTP_GET, .handler = static_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(s_httpd, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "could not register %s", routes[i].uri);
            return false;
        }
    }

    xTaskCreatePinnedToCore(push_task, "ws_push", 6144, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "web server started");
    return true;
}

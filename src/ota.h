#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

/* POST /api/ota handler body: stream the request into the spare app slot. */
esp_err_t ota_handle_upload(httpd_req_t *req, char *err, size_t err_len);

/*
 * Call once CAN and the web server are known to be working (or not). A newly
 * flashed OTA image is confirmed when ok is true; otherwise the device
 * reboots into the previous image.
 */
void ota_confirm_boot(bool ok);

const char *ota_running_partition(void);

/* Restart after a short delay, so a pending HTTP response can go out. */
void app_reboot_later(void);

/* Call first thing in app_main: works out how the last restart happened. */
void app_restart_init(void);
/* Why the chip last restarted, in words ("power on", "software restart", ...). */
const char *app_reset_reason(void);

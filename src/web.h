#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Mount the LittleFS partition that holds the DBC files (at /fs). */
bool web_fs_mount(void);
/* Start the HTTP server and the task that pushes live data to WebSockets. */
bool web_start(void);
/* Estimated bus load in tenths of a percent. bus: 0 = CAN1, 1 = CAN2. */
unsigned web_bus_load(int bus);

#pragma once

#include <stdbool.h>

typedef struct {
    bool ap_mode;   /* reachable through the access point (station not connected) */
    bool connected; /* station has an IP address */
    char ssid[33];  /* network the address below belongs to */
    char sta_ssid[33]; /* saved network, empty if none */
    char ip[16];
    int rssi;
} wifi_info_t;

/*
 * Connect as a station with the saved credentials and keep retrying for as
 * long as it takes. While the network is unreachable, or when there are no
 * credentials, the access point CAN-Filter-XXXX is open. Also starts mDNS
 * (cangw.local). Returns at once; it does not wait for the connection.
 */
void wifi_start(void);
void wifi_get_info(wifi_info_t *info);

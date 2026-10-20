#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stddef.h>
#include "esp_err.h"

#define MDNS_HOSTNAME "cnc-transfer" // http://cnc-transfer.local

void wifi_init(void);

// STA (router) link: state is disabled|connecting|connected|failed; reason is
// set after a disconnect; ip is set while connected.
void wifi_get_sta_status(char *state, size_t state_len, char *reason,
                         size_t reason_len, char *ip, size_t ip_len);

// Recovery mode: AP uses the factory SSID/password for `seconds`, then reverts
// to the saved ones (clients on the AP are disconnected both times).
esp_err_t wifi_recovery_start(int seconds);
int wifi_recovery_seconds_left(void); // 0 when not active

#endif // WIFI_MANAGER_H

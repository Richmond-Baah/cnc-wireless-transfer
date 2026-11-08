#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stddef.h>
#include "esp_err.h"

#define MDNS_HOSTNAME "filexfer"   // http://filexfer.local
#define MDNS_ALIAS "cnc-transfer"  // old name, still answered

esp_err_t wifi_init(void); // ESP_OK once the AP is up

// STA (router) link: state is disabled|connecting|connected|failed; reason is
// set after a disconnect; ip is set while connected.
void wifi_get_sta_status(char *state, size_t state_len, char *reason,
                         size_t reason_len, char *ip, size_t ip_len);

// Router link lost (had an IP, then disconnected) since boot; *ago_s = seconds
// since the last one, -1 if none.
uint32_t wifi_sta_drops(int64_t *ago_s);

// Recovery mode: AP uses the factory SSID/password for `seconds`, then reverts
// to the saved ones (clients on the AP are disconnected both times).
esp_err_t wifi_recovery_start(int seconds);
int wifi_recovery_seconds_left(void); // 0 when not active

// Out-of-box AP SSID: "FILEXFER-" + last 4 hex digits of the AP MAC.
void wifi_default_ap_ssid(char *ssid, size_t len);

// Saved AP SSID/password (not the recovery ones), for pairing new receivers.
void wifi_get_ap_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

#endif // WIFI_MANAGER_H

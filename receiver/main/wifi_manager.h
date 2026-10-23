#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

void wifi_init(void); // DHCP client on the transmitter's AP

bool wifi_manager_get_ip(char *out, size_t len);      // false until DHCP done
bool wifi_manager_get_gateway(char *out, size_t len); // transmitter address
// Saves new AP credentials (from the transmitter) and reconnects if they changed.
void wifi_manager_apply_creds(const char *ssid, const char *pass);
// Stops/resumes connect attempts so ESP-NOW pairing can switch channels
// (esp_wifi_set_channel is not allowed while the STA scans or connects).
void wifi_manager_pause_connect(bool pause);

#endif // WIFI_MANAGER_H

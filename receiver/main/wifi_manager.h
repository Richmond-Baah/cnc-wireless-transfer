#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

void wifi_init(void); // DHCP client on the transmitter's AP

bool wifi_manager_get_ip(char *out, size_t len);      // false until DHCP done
bool wifi_manager_get_gateway(char *out, size_t len); // transmitter address
// Saves new AP credentials (from the transmitter) and reconnects if they changed.
void wifi_manager_apply_creds(const char *ssid, const char *pass);

#endif // WIFI_MANAGER_H

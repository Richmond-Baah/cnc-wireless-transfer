#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stddef.h>

#define MDNS_HOSTNAME "cnc-transfer" // http://cnc-transfer.local

void wifi_init(void);

// STA (router) link: state is disabled|connecting|connected|failed; reason is
// set after a disconnect; ip is set while connected.
void wifi_get_sta_status(char *state, size_t state_len, char *reason,
                         size_t reason_len, char *ip, size_t ip_len);

#endif // WIFI_MANAGER_H

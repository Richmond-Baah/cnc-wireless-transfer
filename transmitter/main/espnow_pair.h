#ifndef ESPNOW_PAIR_H
#define ESPNOW_PAIR_H

// "Add machine": ESP-NOW listens only while a pairing window is open. PAIR_REQ
// from unpaired receivers lands in the registry's pending list; an approved
// receiver gets PAIR_CFG (Wi-Fi settings + its key, encrypted).

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define ESPNOW_PAIR_WINDOW_S 120

esp_err_t espnow_pair_open_window(int seconds);
int espnow_pair_seconds_left(void); // 0 when closed
// Closes the window now (ESP-NOW off, pending PAIR_CFG deliveries dropped).
void espnow_pair_close_window(void);

// Seals PAIR_CFG for rx_id, sends it now and again on each PAIR_REQ from that
// receiver until the window closes (it may be on another channel right now).
// Returns an HTTP status; msg gets the reason on failure.
int espnow_pair_send_cfg(const char *rx_id, const uint8_t mac[6], const uint8_t rx_pub[32],
                         const char *key_hex, const char *name, char *msg, size_t msg_len);

#endif // ESPNOW_PAIR_H

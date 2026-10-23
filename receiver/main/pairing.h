#ifndef PAIRING_H
#define PAIRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define PAIRING_KEY_HEX_LEN 64 // 32-byte key as lowercase hex

// Derives the ID ("rx-" + last 3 MAC bytes) and loads key/name/config version from NVS.
void pairing_init(void);

const char *pairing_get_id(void);          // e.g. "rx-a1b2c3"
void pairing_get_name(char *out, size_t len); // stored name, else "Receiver B2C3"
bool pairing_is_paired(void);
bool pairing_key_matches(const char *hex); // constant time; false when unpaired

// Accepted only while unpaired (ESP_ERR_INVALID_STATE otherwise).
esp_err_t pairing_set(const char *hex_key, const char *name);
// Key from ESP-NOW PAIR_CFG or pair.cfg: used like a pairing, but confirmed only
// when a heartbeat reply says paired; 3 rejected heartbeats drop it again.
esp_err_t pairing_set_provisional(const char *hex_key, const char *name, const char *tx_id);
void pairing_clear(void);

uint32_t pairing_config_version(void);

// Heartbeat to the transmitter every 10 s (+ AP credential pull) and the
// BOOT-button watcher (hold 10 s = clear pairing).
void pairing_start_tasks(void);

#endif // PAIRING_H

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Wi-Fi TX power in 0.25 dBm units. Selectable: 8.5, 11, 13, 15, 17, 19.5 dBm.
// The default stays CONFIG_CNC_WIFI_MAX_TX_POWER (8.5 dBm): higher levels can
// brown out weak supplies, so they are chosen only after the test in the UI.
bool radio_power_valid(int qdbm);
int radio_power_saved(void);       // NVS, else the Kconfig default
esp_err_t radio_power_apply(int qdbm); // now, not saved; logs requested and actual
esp_err_t radio_power_save(int qdbm);
void radio_power_reset(void);      // back to the default (next apply/boot)
int radio_power_actual(void);

// Call once after nvs_flash_init: logs the reset reason, counts brownouts in NVS.
void radio_boot_check(void);
uint32_t radio_brownouts(void);
const char *radio_reset_reason(void);

int radio_sta_rssi(void);                    // station: RSSI of the AP, 0 if unknown
int radio_ap_client_rssi(const char *rx_id); // AP: RSSI of client "rx-aabbcc", 0 if unknown

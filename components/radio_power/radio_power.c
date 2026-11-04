#include "radio_power.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "RADIO";
#define NVS_NS "radio"

static uint32_t s_brownouts;

bool radio_power_valid(int qdbm) {
  static const int levels[] = {34, 44, 52, 60, 68, 78};
  for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
    if (levels[i] == qdbm) return true;
  }
  return false;
}

int radio_power_saved(void) {
  int32_t v = CONFIG_CNC_WIFI_MAX_TX_POWER;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    nvs_get_i32(h, "qdbm", &v);
    nvs_close(h);
  }
  return radio_power_valid(v) ? (int)v : CONFIG_CNC_WIFI_MAX_TX_POWER;
}

esp_err_t radio_power_apply(int qdbm) {
  esp_err_t err = esp_wifi_set_max_tx_power((int8_t)qdbm);
  ESP_LOGI(TAG, "Wi-Fi max TX power: requested %d (%.2f dBm), actual %d (%.2f dBm)%s%s", qdbm, qdbm / 4.0,
           radio_power_actual(), radio_power_actual() / 4.0, err == ESP_OK ? "" : ", set failed: ",
           err == ESP_OK ? "" : esp_err_to_name(err));
  return err;
}

int radio_power_actual(void) {
  int8_t v = 0;
  esp_wifi_get_max_tx_power(&v);
  return v;
}

esp_err_t radio_power_save(int qdbm) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_i32(h, "qdbm", qdbm);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

void radio_power_reset(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_key(h, "qdbm");
    nvs_commit(h);
    nvs_close(h);
  }
}

const char *radio_reset_reason(void) {
  switch (esp_reset_reason()) {
  case ESP_RST_POWERON: return "power-on";
  case ESP_RST_EXT: return "external";
  case ESP_RST_SW: return "software";
  case ESP_RST_PANIC: return "panic";
  case ESP_RST_INT_WDT: return "int-wdt";
  case ESP_RST_TASK_WDT: return "task-wdt";
  case ESP_RST_WDT: return "wdt";
  case ESP_RST_DEEPSLEEP: return "deep-sleep";
  case ESP_RST_BROWNOUT: return "brownout";
  case ESP_RST_SDIO: return "sdio";
  case ESP_RST_USB: return "usb";
  default: return "unknown";
  }
}

uint32_t radio_brownouts(void) { return s_brownouts; }

void radio_boot_check(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_get_u32(h, "brownouts", &s_brownouts);
  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    s_brownouts++;
    nvs_set_u32(h, "brownouts", s_brownouts);
    nvs_commit(h);
    ESP_LOGE(TAG, "BROWNOUT reset (total %lu)", (unsigned long)s_brownouts);
  }
  nvs_close(h);
  ESP_LOGI(TAG, "Reset reason: %s, brownouts so far: %lu", radio_reset_reason(), (unsigned long)s_brownouts);
}

int radio_sta_rssi(void) {
  wifi_ap_record_t ap;
  return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

int radio_ap_client_rssi(const char *rx_id) {
  wifi_sta_list_t list;
  if (rx_id == NULL || esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
  for (int i = 0; i < list.num; i++) {
    char id[16];
    snprintf(id, sizeof(id), "rx-%02x%02x%02x", list.sta[i].mac[3], list.sta[i].mac[4], list.sta[i].mac[5]);
    if (strcmp(id, rx_id) == 0) return list.sta[i].rssi;
  }
  return 0;
}

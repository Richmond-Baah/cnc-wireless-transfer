#include "wifi_manager.h"
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include <string.h>

#ifndef CONFIG_CNC_DEFAULT_WIFI_SSID
#define CONFIG_CNC_DEFAULT_WIFI_SSID "CNC_WIRELESS"
#endif

#ifndef CONFIG_CNC_DEFAULT_WIFI_PASSWORD
#define CONFIG_CNC_DEFAULT_WIFI_PASSWORD "cnc12345"
#endif

#define DEFAULT_WIFI_MODE "AP"
#define DEFAULT_AP_SSID CONFIG_CNC_DEFAULT_WIFI_SSID
#define DEFAULT_AP_PASS CONFIG_CNC_DEFAULT_WIFI_PASSWORD

static const char *TAG = "WIFI_MGR";

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac),
             event->aid);
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac),
             event->aid);
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
    ESP_LOGI(TAG, "Retry to connect to the AP");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
  }
}

void wifi_init(void) {
  ESP_LOGI(TAG, "=== WiFi Init Starting ===");

  esp_err_t err;

  err = esp_netif_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "netif init OK");

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "event loop OK");

  char wifi_mode[8] = "AP";
  char ap_ssid[32] = DEFAULT_AP_SSID;
  char ap_pass[64] = DEFAULT_AP_PASS;
  char sta_ssid[32] = "";
  char sta_pass[64] = "";

  nvs_handle_t nvs_handle;
  err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
  if (err == ESP_OK) {
    int32_t version = 0;
    if (nvs_get_i32(nvs_handle, "cfg_version", &version) != ESP_OK || version != 1) {
        ESP_LOGW(TAG, "NVS version mismatch (or missing). Wiping wifi_cfg for a clean slate!");
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        // Re-open since erase_all might clear things we need to set
        nvs_close(nvs_handle);
        nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
        nvs_set_i32(nvs_handle, "cfg_version", 1);
        nvs_commit(nvs_handle);
    }

    size_t len;
    len = sizeof(wifi_mode);
    if (nvs_get_str(nvs_handle, "wifi_mode", wifi_mode, &len) != ESP_OK) {
      strcpy(wifi_mode, "AP");
    }
    size_t ssid_len = sizeof(ap_ssid);
    if (nvs_get_str(nvs_handle, "ap_ssid", ap_ssid, &ssid_len) != ESP_OK || strlen(ap_ssid) == 0) {
      strcpy(ap_ssid, DEFAULT_AP_SSID);
      ESP_LOGW(TAG, "NVS ap_ssid empty, using default");
    }
    size_t pass_len = sizeof(ap_pass);
    if (nvs_get_str(nvs_handle, "ap_pass", ap_pass, &pass_len) != ESP_OK) {
      strcpy(ap_pass, DEFAULT_AP_PASS);
    }
    if (strlen(ap_pass) < 8 && strlen(ap_pass) > 0) {
        strcpy(ap_pass, DEFAULT_AP_PASS);
    }
    len = sizeof(sta_ssid);
    nvs_get_str(nvs_handle, "sta_ssid", sta_ssid, &len);
    len = sizeof(sta_pass);
    nvs_get_str(nvs_handle, "sta_pass", sta_pass, &len);
    nvs_close(nvs_handle);
  }

  // Sanitize old "CNC WIRELESS" to "CNC_WIRELESS"
  if (strcmp(ap_ssid, "CNC WIRELESS") == 0) {
      ESP_LOGW(TAG, "Sanitizing old SSID format in AP config");
      strcpy(ap_ssid, DEFAULT_AP_SSID);
  }
  
  ESP_LOGI(TAG, "Final AP Config: SSID=[%s], Pass=[%s]", ap_ssid, ap_pass);


  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "WiFi driver init OK");

  esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
  esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

  // Always create AP interface to ensure Receiver can connect
  esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

  // Explicitly set AP IP to 192.168.4.1
  esp_netif_ip_info_t ipInfo;
  IP4_ADDR(&ipInfo.ip, 192, 168, 4, 1);
  IP4_ADDR(&ipInfo.gw, 192, 168, 4, 1);
  IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
  esp_netif_dhcps_stop(ap_netif);
  esp_netif_set_ip_info(ap_netif, &ipInfo);
  esp_netif_dhcps_start(ap_netif);

  wifi_config_t ap_config;
  memset(&ap_config, 0, sizeof(wifi_config_t));
  strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
  ap_config.ap.ssid_len = strlen(ap_ssid);
  strncpy((char *)ap_config.ap.password, ap_pass, sizeof(ap_config.ap.password));
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

  if (strlen(ap_pass) < 8) {
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_LOGW(TAG, "AP password too short (<8 chars), using OPEN auth");
  }

  if (strcmp(wifi_mode, "STA") == 0 && strlen(sta_ssid) > 0) {
    esp_netif_create_default_wifi_sta();
    wifi_config_t sta_config;
    memset(&sta_config, 0, sizeof(wifi_config_t));
    strncpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, sta_pass, sizeof(sta_config.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    ESP_LOGI(TAG, "Starting APSTA mode. AP: %s, STA connecting to: %s", ap_ssid, sta_ssid);
  } else {
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    ESP_LOGI(TAG, "Starting AP mode. SSID: %s, Pass: %s", ap_ssid, 
             strlen(ap_pass) >= 8 ? "****" : "(open)");
  }

  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_start FAILED: %s", esp_err_to_name(err));
  } else {
    // Disable modem sleep so AP beacons are always sent on time
    // and the SSID remains visible to all clients.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "=== WiFi Started Successfully (PS_NONE) ===");
  }
}

#include "wifi_manager.h"
#include "sdkconfig.h"
#include "esp_event.h"
#include "radio_power.h"
#include "file_transfer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "dhcpserver/dhcpserver.h"
#include "mdns.h"
#include "esp_timer.h"
#include <string.h>

#ifndef CONFIG_CNC_DEFAULT_WIFI_SSID
#define CONFIG_CNC_DEFAULT_WIFI_SSID "CNC_WIRELESS"
#endif

#ifndef CONFIG_CNC_DEFAULT_WIFI_PASSWORD
#define CONFIG_CNC_DEFAULT_WIFI_PASSWORD "cnc12345"
#endif

#define DEFAULT_WIFI_MODE "AP"
// Factory SSID: recovery mode and unpaired receivers' fallback (shared by all
// units). The out-of-box AP name is per unit: wifi_default_ap_ssid().
#define DEFAULT_AP_SSID CONFIG_CNC_DEFAULT_WIFI_SSID
#define DEFAULT_AP_PASS CONFIG_CNC_DEFAULT_WIFI_PASSWORD

void wifi_default_ap_ssid(char *ssid, size_t len) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(ssid, len, "FILEXFER-%02X%02X", mac[4], mac[5]);
}

static const char *TAG = "WIFI_MGR";

static void mdns_alias_set(const esp_ip4_addr_t *sta_ip);

// STA (router) link state for /status. Written by the event task, read by httpd.
#define STA_PAUSE_POLL_S 5   // retry check while an upload holds off scanning
#define STA_MAX_RETRIES 5   // fast retries, then slow down so scans don't disturb the AP
#define STA_SLOW_RETRY_S 60 // one attempt per minute after that
static esp_timer_handle_t s_sta_retry_timer;

// Recovery mode: AP temporarily on factory credentials so receivers that
// missed a credential change can rejoin and pull the current ones.
static wifi_config_t s_ap_saved;          // AP config from NVS, restored afterwards
static esp_timer_handle_t s_recovery_timer;
static int64_t s_recovery_end_us;         // 0 = not active
static portMUX_TYPE s_sta_mux = portMUX_INITIALIZER_UNLOCKED;
static const char *s_sta_state = "disabled"; // disabled|connecting|connected|failed
static char s_sta_reason[48] = "";
static char s_sta_ip[16] = "";
static int s_sta_retries = 0;
static bool s_sta_was_connected = false;

static void sta_set(const char *state, const char *reason, const char *ip) {
  portENTER_CRITICAL(&s_sta_mux);
  s_sta_state = state;
  strlcpy(s_sta_reason, reason ? reason : "", sizeof(s_sta_reason));
  strlcpy(s_sta_ip, ip ? ip : "", sizeof(s_sta_ip));
  portEXIT_CRITICAL(&s_sta_mux);
}

void wifi_get_sta_status(char *state, size_t state_len, char *reason,
                         size_t reason_len, char *ip, size_t ip_len) {
  portENTER_CRITICAL(&s_sta_mux);
  strlcpy(state, s_sta_state, state_len);
  strlcpy(reason, s_sta_reason, reason_len);
  strlcpy(ip, s_sta_ip, ip_len);
  portEXIT_CRITICAL(&s_sta_mux);
}

static const char *sta_reason_text(uint8_t reason) {
  switch (reason) {
  case WIFI_REASON_NO_AP_FOUND:
    return "network (SSID) not found";
  case WIFI_REASON_AUTH_FAIL:
  case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
  case WIFI_REASON_HANDSHAKE_TIMEOUT:
  case WIFI_REASON_MIC_FAILURE:
    return "wrong password";
  case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
  case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    return "security mode not supported";
  case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
  case WIFI_REASON_BEACON_TIMEOUT:
    return "signal too weak";
  case WIFI_REASON_ASSOC_FAIL:
  case WIFI_REASON_CONNECTION_FAIL:
    return "router refused connection";
  default:
    return NULL;
  }
}

static void sta_slow_retry(void *arg) {
  if (file_transfer_session_open()) { // a scan would stall the transfer's AP clients
    esp_timer_start_once(s_sta_retry_timer, STA_PAUSE_POLL_S * 1000000ULL);
    return;
  }
  ESP_LOGI(TAG, "STA retry (every %d s until connected)", STA_SLOW_RETRY_S);
  esp_wifi_connect();
}

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
    sta_set("connecting", NULL, NULL);
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *event =
        (wifi_event_sta_disconnected_t *)event_data;
    char reason[48];
    const char *txt = sta_reason_text(event->reason);
    if (txt) {
      snprintf(reason, sizeof(reason), "%s (%d)", txt, event->reason);
    } else {
      snprintf(reason, sizeof(reason), "disconnected (reason %d)", event->reason);
    }
    if (file_transfer_session_open()) {
      sta_set("connecting", reason, NULL);
      ESP_LOGW(TAG, "STA disconnected: %s. No scan until the upload ends.", reason);
      esp_timer_start_once(s_sta_retry_timer, STA_PAUSE_POLL_S * 1000000ULL);
      return;
    }
    // A few quick tries, then one per minute: back-to-back scans keep the radio
    // off the AP channel, so AP clients lose beacons and drop. A link that
    // worked before stays "connecting" because its settings are not at fault.
    if (++s_sta_retries < STA_MAX_RETRIES) {
      sta_set("connecting", reason, NULL);
      ESP_LOGW(TAG, "STA disconnected: %s, retrying (%d/%d)", reason,
               s_sta_retries, STA_MAX_RETRIES);
      esp_wifi_connect();
    } else if (s_sta_was_connected) {
      sta_set("connecting", reason, NULL);
      ESP_LOGW(TAG, "STA lost router: %s. Retrying in %d s; AP stays up.", reason, STA_SLOW_RETRY_S);
      esp_timer_start_once(s_sta_retry_timer, STA_SLOW_RETRY_S * 1000000ULL);
    } else {
      sta_set("failed", reason, NULL);
      ESP_LOGE(TAG, "STA connect FAILED: %s. Retrying in %d s; AP 192.168.4.1 stays up. "
                    "Fix router SSID/password in Settings.", reason, STA_SLOW_RETRY_S);
      esp_timer_start_once(s_sta_retry_timer, STA_SLOW_RETRY_S * 1000000ULL);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_sta_retries = 0;
    s_sta_was_connected = true;
    sta_set("connected", NULL, ip);
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " STA connected to router. STA IP: %s", ip);
    ESP_LOGI(TAG, " Open http://%s or http://" MDNS_HOSTNAME ".local", ip);
    ESP_LOGI(TAG, "==================================================");
    mdns_alias_set(&event->ip_info.ip);
  }
}

static void recovery_end(void *arg) {
  s_recovery_end_us = 0;
  esp_wifi_set_config(WIFI_IF_AP, &s_ap_saved);
  ESP_LOGW(TAG, "Recovery mode over: AP back on [%s]", (char *)s_ap_saved.ap.ssid);
}

esp_err_t wifi_recovery_start(int seconds) {
  if (s_recovery_timer == NULL) {
    const esp_timer_create_args_t args = {.callback = recovery_end, .name = "ap_recovery"};
    esp_err_t err = esp_timer_create(&args, &s_recovery_timer);
    if (err != ESP_OK) return err;
  }
  esp_timer_stop(s_recovery_timer); // restart the window if already active
  wifi_config_t cfg = s_ap_saved;
  memset(cfg.ap.ssid, 0, sizeof(cfg.ap.ssid));
  memset(cfg.ap.password, 0, sizeof(cfg.ap.password));
  strlcpy((char *)cfg.ap.ssid, DEFAULT_AP_SSID, sizeof(cfg.ap.ssid));
  strlcpy((char *)cfg.ap.password, DEFAULT_AP_PASS, sizeof(cfg.ap.password));
  cfg.ap.ssid_len = strlen(DEFAULT_AP_SSID);
  cfg.ap.authmode = strlen(DEFAULT_AP_PASS) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  s_recovery_end_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
  esp_timer_start_once(s_recovery_timer, (uint64_t)seconds * 1000000ULL);
  ESP_LOGW(TAG, "Recovery mode: AP on factory SSID [%s] for %d s", DEFAULT_AP_SSID, seconds);
  return esp_wifi_set_config(WIFI_IF_AP, &cfg);
}

int wifi_recovery_seconds_left(void) {
  int64_t end = s_recovery_end_us;
  int64_t left = end ? (end - esp_timer_get_time()) / 1000000 : 0;
  return left > 0 ? (int)left : 0;
}

void wifi_get_ap_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
  char tmp_ssid[33] = {0};
  memcpy(tmp_ssid, s_ap_saved.ap.ssid, sizeof(s_ap_saved.ap.ssid)); // may lack a NUL
  strlcpy(ssid, tmp_ssid, ssid_len);
  char tmp_pass[65] = {0};
  memcpy(tmp_pass, s_ap_saved.ap.password, sizeof(s_ap_saved.ap.password));
  strlcpy(pass, tmp_pass, pass_len);
}

// MDNS_ALIAS answers with the AP address, plus the router address once known
static void mdns_alias_set(const esp_ip4_addr_t *sta_ip) {
  mdns_ip_addr_t sta = {.addr = {.type = ESP_IPADDR_TYPE_V4}, .next = NULL};
  mdns_ip_addr_t ap = {.addr = ESP_IP4ADDR_INIT(192, 168, 4, 1), .next = NULL};
  if (sta_ip) {
    sta.addr.u_addr.ip4 = *sta_ip;
    ap.next = &sta;
  }
  esp_err_t err = sta_ip ? mdns_delegate_hostname_set_address(MDNS_ALIAS, &ap)
                         : mdns_delegate_hostname_add(MDNS_ALIAS, &ap);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS alias " MDNS_ALIAS ": %s", esp_err_to_name(err));
  }
}

static void mdns_start(void) {
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    return;
  }
  mdns_hostname_set(MDNS_HOSTNAME);
  mdns_instance_name_set("FileXfer");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  mdns_alias_set(NULL);
  ESP_LOGI(TAG, "mDNS: http://" MDNS_HOSTNAME ".local (also " MDNS_ALIAS ".local)");
}

esp_err_t wifi_init(void) {
  ESP_LOGI(TAG, "=== WiFi Init Starting ===");

  esp_err_t err;

  err = esp_netif_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "netif init OK");

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "event loop OK");

  char wifi_mode[8] = "AP";
  char ap_ssid[32];
  wifi_default_ap_ssid(ap_ssid, sizeof(ap_ssid));
  char ap_pass[64] = DEFAULT_AP_PASS;
  char sta_ssid[32] = "";
  char sta_pass[64] = "";

  nvs_handle_t nvs_handle;
  err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
  if (err == ESP_OK) {
    int32_t version = 0;
    // No cfg_version: first boot ever. Otherwise an earlier firmware already ran
    // this unit's AP (on the factory SSID if it never saved one).
    bool first_boot = nvs_get_i32(nvs_handle, "cfg_version", &version) != ESP_OK;
    if (first_boot || version != 1) {
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
    // AP SSID/password are pinned in NVS the first time they are needed, so a
    // later firmware with other defaults never renames a unit already set up.
    size_t ssid_len = sizeof(ap_ssid);
    if (nvs_get_str(nvs_handle, "ap_ssid", ap_ssid, &ssid_len) != ESP_OK || strlen(ap_ssid) == 0) {
      if (first_boot) {
        wifi_default_ap_ssid(ap_ssid, sizeof(ap_ssid));
      } else {
        strlcpy(ap_ssid, DEFAULT_AP_SSID, sizeof(ap_ssid)); // what earlier firmware broadcast
      }
      nvs_set_str(nvs_handle, "ap_ssid", ap_ssid);
      ESP_LOGW(TAG, "NVS ap_ssid empty: saved %s", ap_ssid);
    }
    size_t pass_len = sizeof(ap_pass);
    if (nvs_get_str(nvs_handle, "ap_pass", ap_pass, &pass_len) != ESP_OK) {
      strcpy(ap_pass, DEFAULT_AP_PASS);
      nvs_set_str(nvs_handle, "ap_pass", ap_pass);
      ESP_LOGW(TAG, "NVS ap_pass empty: saved default");
    }
    nvs_commit(nvs_handle);
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
      strlcpy(ap_ssid, DEFAULT_AP_SSID, sizeof(ap_ssid));
  }
  
  ESP_LOGI(TAG, "Final AP Config: SSID=[%s], Pass=[%s]", ap_ssid, ap_pass);


  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
    return err;
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
  ap_config.ap.max_connection = ESP_WIFI_MAX_CONN_NUM; // 15 on ESP32-S3: receivers + phones/laptops
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_config.ap.channel = 1;
  ap_config.ap.beacon_interval = 50;
  ap_config.ap.dtim_period = 1;
  ap_config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;

  if (strlen(ap_pass) < 8) {
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_LOGW(TAG, "AP password too short (<8 chars), using OPEN auth");
  }

  s_ap_saved = ap_config;

  if (strcmp(wifi_mode, "STA") == 0 && strlen(sta_ssid) > 0) {
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, MDNS_HOSTNAME); // name in the router's DHCP list
    const esp_timer_create_args_t retry_args = {
        .callback = sta_slow_retry, .name = "sta_retry"};
    esp_timer_create(&retry_args, &s_sta_retry_timer);
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
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);
    ESP_LOGI(TAG, "Wi-Fi power save: %s", ps == WIFI_PS_NONE ? "none" : "ON");

    // Set country code for proper regulatory domain (improves compatibility)
    wifi_country_t country = {
      .cc = "GH",
      .schan = 1,
      .nchan = 13,
      .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    esp_wifi_set_country(&country);

    // Set bandwidth to 20MHz for better compatibility and range
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);

    {
      // Units of 0.25 dBm (CONFIG_CNC_WIFI_MAX_TX_POWER); max power can brown
      // out small boards and leave the AP invisible
      radio_power_apply(radio_power_saved());
    }

    // Configure DHCP server for faster IP assignment
    dhcps_lease_t lease;
    lease.enable = true;
    lease.start_ip.addr = ipInfo.ip.addr + 1;  // 192.168.4.2
    lease.end_ip.addr = ipInfo.ip.addr + 10;   // 192.168.4.11
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));
    esp_netif_dhcps_start(ap_netif);

    ESP_LOGI(TAG, "=== WiFi Started Successfully (PS_NONE, ch=%d, beacon=%d, BW=HT20) ===",
             ap_config.ap.channel, ap_config.ap.beacon_interval);
    mdns_start();
  }
  return err;
}

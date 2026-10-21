#include "wifi_manager.h"
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

#ifndef CONFIG_CNC_DEFAULT_WIFI_SSID
#define CONFIG_CNC_DEFAULT_WIFI_SSID "CNC_WIRELESS"
#endif

#ifndef CONFIG_CNC_DEFAULT_WIFI_PASSWORD
#define CONFIG_CNC_DEFAULT_WIFI_PASSWORD "cnc12345"
#endif

#define DEFAULT_STA_SSID CONFIG_CNC_DEFAULT_WIFI_SSID
#define DEFAULT_STA_PASS CONFIG_CNC_DEFAULT_WIFI_PASSWORD

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          10

static const char *TAG = "WIFI_MGR";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static esp_netif_t *s_sta_netif = NULL;

static char g_sta_ssid[32] = DEFAULT_STA_SSID;
static char g_sta_pass[64] = DEFAULT_STA_PASS;
static bool s_trying_default = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Connected to AP, waiting for DHCP...");
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "DHCP IP: " IPSTR " (transmitter " IPSTR ")", IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry_num++;
        ESP_LOGW(TAG, "Disconnected. Retry %d/%d", s_retry_num, MAX_RETRY);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        if (s_retry_num >= MAX_RETRY) {
            ESP_LOGW(TAG, "Max retries reached. Switching fallback mode!");
            s_trying_default = !s_trying_default;
            s_retry_num = 0;
            
            wifi_config_t wifi_config = {
                .sta = {
                    .threshold.authmode = WIFI_AUTH_OPEN,
                    .scan_method        = WIFI_FAST_SCAN,
                    .sort_method        = WIFI_CONNECT_AP_BY_SIGNAL,
                },
            };
            
            if (s_trying_default) {
                ESP_LOGI(TAG, "Falling back to DEFAULT SSID: %s", DEFAULT_STA_SSID);
                strncpy((char *)wifi_config.sta.ssid, DEFAULT_STA_SSID, sizeof(wifi_config.sta.ssid) - 1);
                strncpy((char *)wifi_config.sta.password, DEFAULT_STA_PASS, sizeof(wifi_config.sta.password) - 1);
            } else {
                ESP_LOGI(TAG, "Switching back to CUSTOM SSID: %s", g_sta_ssid);
                strncpy((char *)wifi_config.sta.ssid, g_sta_ssid, sizeof(wifi_config.sta.ssid) - 1);
                strncpy((char *)wifi_config.sta.password, g_sta_pass, sizeof(wifi_config.sta.password) - 1);
            }
            
            esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        }
        // Never call vTaskDelay inside event loop handler
        esp_wifi_connect();
    }
}

bool wifi_manager_get_ip(char *out, size_t len) {
    esp_netif_ip_info_t info;
    if (!s_sta_netif || esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    snprintf(out, len, IPSTR, IP2STR(&info.ip));
    return true;
}

bool wifi_manager_get_gateway(char *out, size_t len) {
    esp_netif_ip_info_t info;
    if (!s_sta_netif || esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || info.gw.addr == 0) {
        return false;
    }
    snprintf(out, len, IPSTR, IP2STR(&info.gw));
    return true;
}

void wifi_manager_apply_creds(const char *ssid, const char *pass) {
    if (strcmp(ssid, g_sta_ssid) == 0 && strcmp(pass, g_sta_pass) == 0) {
        return; // already using them
    }
    nvs_handle_t h;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "sta_ssid", ssid);
        nvs_set_str(h, "sta_pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
    strlcpy(g_sta_ssid, ssid, sizeof(g_sta_ssid));
    strlcpy(g_sta_pass, pass, sizeof(g_sta_pass));
    s_trying_default = false;
    s_retry_num = 0;
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
            .scan_method        = WIFI_FAST_SCAN,
            .sort_method        = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    // The transmitter restarts on its new credentials; the disconnect handler reconnects
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "Switched to SSID [%s]", ssid);
}

void wifi_init(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char sta_ssid[32] = DEFAULT_STA_SSID;
    char sta_pass[64] = DEFAULT_STA_PASS;

    nvs_handle_t nvs_handle;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        int32_t version = 0;
        // Bump to version 2 to clean out any old invalid router credentials
        if (nvs_get_i32(nvs_handle, "cfg_version", &version) != ESP_OK || version != 2) {
            ESP_LOGW(TAG, "NVS version mismatch. Resetting wifi_cfg to defaults!");
            nvs_erase_all(nvs_handle);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
            nvs_set_i32(nvs_handle, "cfg_version", 2);
            nvs_set_str(nvs_handle, "sta_ssid", DEFAULT_STA_SSID);
            nvs_set_str(nvs_handle, "sta_pass", DEFAULT_STA_PASS);
            nvs_commit(nvs_handle);
        }

        size_t len = sizeof(sta_ssid);
        if (nvs_get_str(nvs_handle, "sta_ssid", sta_ssid, &len) != ESP_OK || strlen(sta_ssid) == 0) {
            strcpy(sta_ssid, DEFAULT_STA_SSID);
        }
        len = sizeof(sta_pass);
        if (nvs_get_str(nvs_handle, "sta_pass", sta_pass, &len) != ESP_OK) {
            strcpy(sta_pass, DEFAULT_STA_PASS);
        }
        nvs_close(nvs_handle);
    }

    // Sanitize old "CNC WIRELESS" to "CNC_WIRELESS"
    if (strcmp(sta_ssid, "CNC WIRELESS") == 0) {
        ESP_LOGW(TAG, "Sanitizing old SSID format");
        strcpy(sta_ssid, DEFAULT_STA_SSID);
    }

    ESP_LOGI(TAG, "Connecting to SSID=[%s]", sta_ssid);

    // Save custom settings to globals for the fallback loop
    strncpy(g_sta_ssid, sta_ssid, sizeof(g_sta_ssid) - 1);
    strncpy(g_sta_pass, sta_pass, sizeof(g_sta_pass) - 1);

    s_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
            .scan_method        = WIFI_FAST_SCAN,
            .sort_method        = WIFI_CONNECT_AP_BY_SIGNAL,
        },
    };
    strncpy((char *)wifi_config.sta.ssid,     sta_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, sta_pass, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Set country code for proper regulatory domain
    wifi_country_t country = {
      .cc = "GH",
      .schan = 1,
      .nchan = 13,
      .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    esp_wifi_set_country(&country);

    {
        // Units of 0.25 dBm (CONFIG_CNC_WIFI_MAX_TX_POWER); max power can brown
        // out small boards and leave the AP invisible
        esp_err_t txp_err = esp_wifi_set_max_tx_power(CONFIG_CNC_WIFI_MAX_TX_POWER);
        int8_t txp = 0;
        esp_wifi_get_max_tx_power(&txp);
        ESP_LOGI(TAG, "Wi-Fi max TX power: requested %d (%.2f dBm), actual %d (%.2f dBm)%s%s",
                 CONFIG_CNC_WIFI_MAX_TX_POWER, CONFIG_CNC_WIFI_MAX_TX_POWER / 4.0, txp, txp / 4.0,
                 txp_err == ESP_OK ? "" : ", set failed: ", txp_err == ESP_OK ? "" : esp_err_to_name(txp_err));
    }

    // Non-blocking wait: Wait up to 500ms for initial connection bit, then proceed so USB MSC initializes immediately
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
}

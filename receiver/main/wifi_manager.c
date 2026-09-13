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

#ifndef CONFIG_CNC_RECEIVER_IP
#define CONFIG_CNC_RECEIVER_IP "192.168.4.150"
#endif

#ifndef CONFIG_CNC_RECEIVER_GATEWAY
#define CONFIG_CNC_RECEIVER_GATEWAY "192.168.4.1"
#endif

#ifndef CONFIG_CNC_RECEIVER_NETMASK
#define CONFIG_CNC_RECEIVER_NETMASK "255.255.255.0"
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

static void apply_static_ip(void) {
    esp_netif_dhcpc_stop(s_sta_netif);
    esp_netif_ip_info_t ip_info;
    ip4_addr_t ip, gw, mask;
    ip4addr_aton(CONFIG_CNC_RECEIVER_IP, &ip);
    ip4addr_aton(CONFIG_CNC_RECEIVER_GATEWAY, &gw);
    ip4addr_aton(CONFIG_CNC_RECEIVER_NETMASK, &mask);
    ip_info.ip.addr = ip.addr;
    ip_info.gw.addr = gw.addr;
    ip_info.netmask.addr = mask.addr;
    esp_err_t err = esp_netif_set_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_ip_info failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Static IP set: %s", CONFIG_CNC_RECEIVER_IP);
    }
}

static char g_sta_ssid[32] = DEFAULT_STA_SSID;
static char g_sta_pass[64] = DEFAULT_STA_PASS;
static bool s_trying_default = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Connected to AP! Applying static IP...");
        apply_static_ip();
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
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
            
            esp_wifi_disconnect();
            esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    }
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
        if (nvs_get_i32(nvs_handle, "cfg_version", &version) != ESP_OK || version != 1) {
            ESP_LOGW(TAG, "NVS version mismatch (or missing). Wiping wifi_cfg for a clean slate!");
            nvs_erase_all(nvs_handle);
            nvs_commit(nvs_handle);
            // Re-open
            nvs_close(nvs_handle);
            nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
            nvs_set_i32(nvs_handle, "cfg_version", 1);
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
    apply_static_ip();
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

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
}

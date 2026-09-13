#include "mdns_responder.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include <string.h>

#ifndef CONFIG_CNC_MDNS_HOSTNAME
#define CONFIG_CNC_MDNS_HOSTNAME "cnc-receiver"
#endif

#ifndef CONFIG_CNC_MDNS_SERVICE
#define CONFIG_CNC_MDNS_SERVICE "_cnc-wireless"
#endif

static const char *TAG = "MDNS";

void mdns_responder_init(void) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set(CONFIG_CNC_MDNS_HOSTNAME);
    mdns_instance_name_set("CNC Wireless Receiver");

    mdns_txt_item_t txt_records[] = {
        { "role",   "receiver" },
        { "proto",  "http" },
        { "ver",    "1" },
    };

    err = mdns_service_add(
        NULL,
        CONFIG_CNC_MDNS_SERVICE,
        "_tcp",
        80,
        txt_records,
        sizeof(txt_records) / sizeof(txt_records[0])
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "mDNS advertising: %s.local as %s._tcp:80",
             CONFIG_CNC_MDNS_HOSTNAME, CONFIG_CNC_MDNS_SERVICE);
}

#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "file_transfer.h"
#include "rx_registry.h"
#include "led.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting CNC Transmitter Firmware");

    // 1. Initialize NVS (used for Wi-Fi credentials and app state)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize LEDs
    led_init();

    // 3. Initialize Wi-Fi (Moved up to ensure connectivity even if SPIFFS fails)
    wifi_init();

    // 4. (Web UI is embedded in the app image; the spiffs partition is unused)

    // 5. Receiver registry (paired list from NVS) and upload session reaper
    rx_registry_init();
    file_transfer_start();

    // 6. Start Web Server (receivers announce themselves via /rx/heartbeat)
    web_server_start();

    ESP_LOGI(TAG, "Transmitter Init Complete. Free Heap: %u", (unsigned)esp_get_free_heap_size());
}

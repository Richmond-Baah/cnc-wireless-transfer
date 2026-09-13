#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "file_transfer.h"
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

    // 4. Initialize SPIFFS (for web assets)
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = false
    };

    ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s). Web UI may not work.", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        ret = esp_spiffs_info(NULL, &total, &used);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
        }
    }

    // 5. Start Web Server
    web_server_start();

    // 6. Start background receiver status poller
    //    This runs every 2 s and caches the result so /status HTTP calls never block.
    file_transfer_status_task_start();

    ESP_LOGI(TAG, "Transmitter Init Complete. Free Heap: %u", (unsigned)esp_get_free_heap_size());
}

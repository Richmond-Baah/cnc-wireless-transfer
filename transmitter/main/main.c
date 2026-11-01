#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "file_transfer.h"
#include "rx_registry.h"
#include "led.h"
#include "auth.h"
#include "boot_button.h"
#include "fw_update.h"

static const char *TAG = "MAIN";

// BOOT (pinhole) held 10 s: admin login back to factory default only;
// network settings and paired machines stay as they are.
#define BOOT_HOLD_MS 10000

static void boot_pressed(bool pressed) {
  led_set_button(pressed ? LED_BUTTON_HELD : LED_BUTTON_OFF);
}

static void boot_fired(void) {
  ESP_LOGW(TAG, "BOOT held %d s: resetting admin login", BOOT_HOLD_MS / 1000);
  auth_reset_defaults();
  led_set_button(LED_BUTTON_DONE);
}

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
    esp_err_t wifi_err = wifi_init();

    // 4. (Web UI is embedded in the app image; the spiffs partition is unused)

    // 5. Receiver registry (paired list from NVS) and upload session reaper
    rx_registry_init();
    file_transfer_start();

    // 6. Start Web Server (receivers announce themselves via /rx/heartbeat)
    esp_err_t web_err = web_server_start();
    // A freshly updated image is kept only if the AP and web server came up;
    // otherwise (or after a crash before this) the bootloader rolls back.
    fw_update_confirm_boot(wifi_err == ESP_OK && web_err == ESP_OK);
    boot_button_start(BOOT_HOLD_MS, boot_pressed, boot_fired);

    ESP_LOGI(TAG, "Transmitter Init Complete. Free Heap: %u", (unsigned)esp_get_free_heap_size());
}

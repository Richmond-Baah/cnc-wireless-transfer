#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "file_receiver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "nvs_flash.h"
#include "state_machine.h"
#include "usb_msc.h"
#include "wifi_manager.h"
#include "sd_card.h"
#include "storage_manager.h"
#include "tusb.h"
#include "mdns_responder.h"
#include "pairing.h"
#include "espnow_pair.h"
#include <stdio.h>
#include <stdint.h>

static const char *TAG = "MAIN";


static void usb_debug_task(void *param) {
    (void)param;
    uint32_t count = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        count++;
        ESP_LOGI(TAG, "DEBUG #%lu: Heap=%u, USB=%s", 
                 count, (unsigned)esp_get_free_heap_size(),
                 usb_msc_is_host_mounted() ? "MOUNTED" : "NOT_MOUNTED");
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "========== BOOT START ==========");
    
    ESP_LOGI(TAG, "Step 1: NVS init...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "Step 1: DONE");

    ESP_LOGI(TAG, "Step 2: State machine...");
    state_machine_init();
    ESP_LOGI(TAG, "Step 2: DONE");

    ESP_LOGI(TAG, "Step 3: LED init...");
    led_init();
    ESP_LOGI(TAG, "Step 3: DONE");

    ESP_LOGI(TAG, "Step 4: SD card raw init (USB MSC owns card at boot)...");
    sd_card_init_raw();
    ESP_LOGI(TAG, "Step 4: DONE");

    ESP_LOGI(TAG, "Step 4b: Storage manager init...");
    storage_manager_init();
    ESP_LOGI(TAG, "Step 4b: DONE");

    ESP_LOGI(TAG, "Step 4c: Pairing (ID, key)...");
    pairing_init();
    ESP_LOGI(TAG, "Step 4c: DONE");

    ESP_LOGI(TAG, "Step 5: Wi-Fi init...");
    wifi_init();
    ESP_LOGI(TAG, "Step 5: DONE");

    ESP_LOGI(TAG, "Step 6: File receiver init...");
    file_receiver_init();
    ESP_LOGI(TAG, "Step 6: DONE");

    ESP_LOGI(TAG, "Step 7: mDNS responder init...");
    mdns_responder_init();
    ESP_LOGI(TAG, "Step 7: DONE");

    ESP_LOGI(TAG, "Step 7b: Heartbeat + BOOT-button tasks...");
    pairing_start_tasks();
    espnow_pair_start(); // "Add machine" radio pairing while unpaired
    ESP_LOGI(TAG, "Step 7b: DONE");

    ESP_LOGI(TAG, "Step 8: Creating debug task...");
    xTaskCreate(usb_debug_task, "usb_debug", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "Step 8: DONE");

    ESP_LOGI(TAG, "Step 8b: Preparing storage for USB MSC...");
    storage_manager_prepare_for_usb();
    ESP_LOGI(TAG, "Step 8b: DONE");

    // USB is not connected yet: safe to snapshot the file list + free space
    ESP_LOGI(TAG, "Step 8c: Caching file list...");
    file_receiver_refresh_cache();
    ESP_LOGI(TAG, "Step 8c: DONE");

    ESP_LOGI(TAG, "Step 9: USB MSC init (LAST STEP)...");
    usb_msc_init();
    ESP_LOGI(TAG, "Step 9: USB init DONE - should enumerate NOW");

    ESP_LOGI(TAG, "========== ALL INIT COMPLETE ==========");
    ESP_LOGI(TAG, "Wait 3 seconds for USB enumeration...");

    vTaskDelay(pdMS_TO_TICKS(3000));
    
    ESP_LOGI(TAG, "Checking USB connection status...");
    if (!usb_msc_is_host_mounted()) {
        ESP_LOGI(TAG, "WARNING: USB not mounted, attempting reconnect...");
        usb_msc_connect();
    }
    
    ESP_LOGI(TAG, "Entering main loop...");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "MAIN LOOP - Expected USB device on PC!");
        ESP_LOGI(TAG, "Heap: %u bytes", (unsigned)esp_get_free_heap_size());
    }
}
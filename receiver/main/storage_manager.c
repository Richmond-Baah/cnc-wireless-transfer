#include "sdkconfig.h"
#include "storage_manager.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "diskio.h"
#include "diskio_sdmmc.h"
#include "sd_card.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The SD card is the only storage. No card at boot = CARD_ERROR until a
// restart with a card; there is no internal-flash fallback.

static const char *TAG = "STORAGE_MGR";

bool storage_manager_fatfs_available(void) {
    return sd_card_is_mounted();
}

bool storage_manager_is_sd_present(void) {
    return (current_sd_card != NULL);
}

esp_err_t storage_manager_unmount(void) {
    return sd_card_is_mounted() ? sd_card_unmount() : ESP_OK;
}

// Transfer time: mount FatFS on the SD card. A failure is an error for the caller.
esp_err_t storage_manager_remount(void) {
    if (sd_card_is_mounted()) {
        return ESP_OK;
    }
    if (current_sd_card == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t sd_ret = sd_card_remount();
    if (sd_ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card FatFS mounted at /sdcard");
    } else {
        ESP_LOGE(TAG, "SD card FatFS mount failed: %s", esp_err_to_name(sd_ret));
    }
    return sd_ret;
}

esp_err_t storage_manager_init(void) {
    // Boot time: the SD card stays RAW so USB MSC owns it exclusively.
    // storage_manager_remount() mounts FatFS when a Wi-Fi transfer starts.
    if (current_sd_card == NULL) {
        ESP_LOGE(TAG, "No SD card at boot: CARD_ERROR until restarted with a card");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "SD card present (raw for USB MSC)");
    return ESP_OK;
}

esp_err_t storage_manager_format(void) {
    ESP_LOGI(TAG, "Formatting storage...");
    if (current_sd_card == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (sd_card_is_mounted()) {
        sd_card_unmount();
    }
    return sd_card_format_fatfs();
}

esp_err_t storage_manager_get_info(uint64_t *out_total, uint64_t *out_free, const char **out_type) {
    if (out_total) *out_total = 0;
    if (out_free) *out_free = 0;
    if (out_type) *out_type = (current_sd_card != NULL) ? "SDCard" : "None";
    if (!sd_card_is_mounted()) {
        return ESP_OK; // USB owns the card: caller uses its last snapshot
    }
    uint64_t total = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(storage_manager_get_base_path(), &total, &free_bytes);
    if (err == ESP_OK) {
        if (out_total) *out_total = total;
        if (out_free) *out_free = free_bytes;
    }
    return err;
}

esp_err_t storage_manager_prepare_for_usb(void) {
    // Verifies the FAT (never formats); no card or unreadable FAT -> CARD_ERROR
    ESP_LOGI(TAG, "Preparing SD card for USB MSC...");
    esp_err_t err = sd_card_ensure_fatfs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sd_card_ensure_fatfs returned %s (continuing in raw mode)", esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_manager_flush_sync(void) {
    // Unmount and remount filesystem to force FatFS FAT table flush to the card
    if (!sd_card_is_mounted()) {
        ESP_LOGI(TAG, "No mounted FatFS to flush");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Flushing FatFS metadata...");
    esp_err_t err = sd_card_unmount();
    vTaskDelay(pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        return err;
    }
    return sd_card_remount();
}

// Flush and unmount FatFS, leaving the SD card in raw USB MSC mode.
esp_err_t storage_manager_flush_and_release_for_usb(void) {
    if (sd_card_is_mounted()) {
        ESP_LOGI(TAG, "Flushing and releasing SD card for USB MSC...");
        return sd_card_unmount();
    }
    return ESP_OK;
}

esp_err_t storage_manager_release_for_usb(void) {
    return storage_manager_flush_and_release_for_usb();
}

const char* storage_manager_get_base_path(void) {
    return "/sdcard";
}

bool storage_manager_get_fatfs_drive(char *out, size_t len) {
    if (!sd_card_is_mounted()) {
        return false;
    }
    BYTE pdrv = ff_diskio_get_pdrv_card(current_sd_card);
    if (pdrv == 0xFF) {
        return false;
    }
    snprintf(out, len, "%u:", (unsigned)pdrv);
    return true;
}

#include "sdkconfig.h"
#include "storage_manager.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "diskio.h"
#include "diskio_sdmmc.h"
#include "diskio_wl.h"
#include "sd_card.h"
#include <stdio.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "STORAGE_MGR";
static const char *BASE_PATH = "/storage";
static const char *PARTITION_LABEL = "storage";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_flash_mounted = false;

bool storage_manager_is_flash_mounted(void) {
    return s_flash_mounted;
}

// With an SD card detected at boot it is the only storage; flash is never used.
bool storage_manager_fatfs_available(void) {
    return (current_sd_card != NULL) ? sd_card_is_mounted() : s_flash_mounted;
}

bool storage_manager_is_sd_present(void) {
    return (current_sd_card != NULL);
}

esp_err_t storage_manager_unmount(void) {
    if (sd_card_is_mounted()) {
        return sd_card_unmount();
    }
    if (s_flash_mounted) {
        esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(BASE_PATH, s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
        s_flash_mounted = false;
        return err;
    }
    if (s_wl_handle != WL_INVALID_HANDLE) {
        esp_err_t err = wl_unmount(s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
        return err;
    }
    return ESP_OK;
}

esp_err_t storage_manager_remount(void) {
    if (sd_card_is_mounted()) {
        return ESP_OK;
    }
    // Transfer time: remount the SD card if present, else the internal flash.
    // An SD mount failure is an error for the caller, never a switch to flash.
    if (current_sd_card != NULL) {
        esp_err_t sd_ret = sd_card_remount();
        if (sd_ret == ESP_OK) {
            ESP_LOGI(TAG, "SD card FatFS mounted at /sdcard");
        } else {
            ESP_LOGE(TAG, "SD card FatFS mount failed: %s", esp_err_to_name(sd_ret));
        }
        return sd_ret;
    }

    if (s_flash_mounted) {
        return ESP_OK;
    }

    // If flash has raw wear-levelling mounted for USB MSC, unmount it before re-mounting with FatFS
    if (s_wl_handle != WL_INVALID_HANDLE) {
        wl_unmount(s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
    }
    return storage_manager_init();
}

esp_err_t storage_manager_init(void) {
    ESP_LOGI(TAG, "=== storage_manager_init() START ===");

    if (sd_card_is_mounted()) {
        ESP_LOGI(TAG, "SD Card active at /sdcard");
        return ESP_OK;
    }

    // Boot time: the SD card stays RAW so USB MSC owns it exclusively.
    // Do NOT mount FatFS on it here - storage_manager_remount() does that
    // when a Wi-Fi transfer actually starts.
    if (current_sd_card != NULL) {
        // Flash is the storage only when no SD card was detected at boot
        ESP_LOGI(TAG, "SD card present (raw for USB MSC); internal flash not used");
        return ESP_OK;
    }

    if (s_flash_mounted && s_wl_handle != WL_INVALID_HANDLE) {
        ESP_LOGI(TAG, "Already initialized and mounted, handle=%d", s_wl_handle);
        return ESP_OK;
    }

    // If raw wear-levelling is active without FatFS, unmount it before mounting FatFS
    if (s_wl_handle != WL_INVALID_HANDLE && !s_flash_mounted) {
        wl_unmount(s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
    }

    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, PARTITION_LABEL);
    if (!partition) {
        ESP_LOGE(TAG, "FATAL: Cannot find 'storage' partition!");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Found partition: size=%d bytes", partition->size);

    // Mount VFS — format_if_mount_failed=false to NEVER destroy existing files.
    // 512-byte sector size with 4096-byte cluster size (allocation_unit_size = 4096)
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 5,
        .format_if_mount_failed = false,
        .allocation_unit_size = 4096,
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(BASE_PATH, PARTITION_LABEL, &mount_config, &s_wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "VFS Mount failed (%s). Partition may be blank — formatting once...", esp_err_to_name(ret));
        // Only format on the very first use (blank partition). allocation_unit_size=4096
        // ensures the FAT cluster matches the USB sector size for future USB MSC sessions.
        const esp_vfs_fat_mount_config_t init_format_config = {
            .max_files = 5,
            .format_if_mount_failed = true,
            .allocation_unit_size = 4096,
        };
        ret = esp_vfs_fat_spiflash_mount_rw_wl(BASE_PATH, PARTITION_LABEL, &init_format_config, &s_wl_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "VFS Mount after format failed: %s", esp_err_to_name(ret));
            s_flash_mounted = false;
            return ret;
        }
    }
    s_flash_mounted = true;
    ESP_LOGI(TAG, "VFS mounted OK, handle=%d", s_wl_handle);

    return ESP_OK;
}

esp_err_t storage_manager_format(void) {
    ESP_LOGI(TAG, "Formatting storage...");

    // If an SD card is present it is the active storage: format it.
    if (storage_manager_is_sd_present()) {
        if (sd_card_is_mounted()) {
            sd_card_unmount();
        }
        return sd_card_format_fatfs();
    }

    if (s_wl_handle != WL_INVALID_HANDLE) {
        if (s_flash_mounted) {
            esp_vfs_fat_spiflash_unmount_rw_wl(BASE_PATH, s_wl_handle);
            s_flash_mounted = false;
        } else {
            wl_unmount(s_wl_handle);
        }
        s_wl_handle = WL_INVALID_HANDLE;
    }

    const esp_vfs_fat_mount_config_t format_config = {
        .max_files = 5,
        .format_if_mount_failed = true,
        .allocation_unit_size = 4096,  // match wl_sector_size() and USB MSC block_size
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(
        BASE_PATH,
        PARTITION_LABEL,
        &format_config,
        &s_wl_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(ret));
        s_flash_mounted = false;
        return ret;
    }

    s_flash_mounted = true;

    #if CONFIG_FATFS_USE_LABEL
    f_setlabel("FILEXFER");
    #endif

    ESP_LOGI(TAG, "Format complete");
    return ESP_OK;
}

esp_err_t storage_manager_get_info(uint64_t *out_total, uint64_t *out_free, const char **out_type) {
    if (out_total) *out_total = 0;
    if (out_free) *out_free = 0;
    if (out_type) *out_type = (current_sd_card != NULL) ? "SDCard" : "InternalFlash";

    if (!storage_manager_fatfs_available()) {
        const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, PARTITION_LABEL);
        if (partition) {
            if (out_total) *out_total = partition->size;
            if (out_free) *out_free = partition->size;
        }
        ESP_LOGI(TAG, "FatFS unmounted for USB MSC; returning partition capacity info");
        return ESP_OK;
    }

    const char *base = storage_manager_get_base_path();
    uint64_t total = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(base, &total, &free_bytes);
    if (err == ESP_OK) {
        if (out_total) *out_total = total;
        if (out_free) *out_free = free_bytes;
        return ESP_OK;
    }
    return err;
}

esp_err_t storage_manager_unmount_flash_for_usb(void) {
    if (s_flash_mounted) {
        ESP_LOGI(TAG, "Unmounting flash FatFS (flushing to flash)...");
        esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(BASE_PATH, s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
        s_flash_mounted = false;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_vfs_fat_spiflash_unmount_rw_wl returned: %s", esp_err_to_name(err));
        }
    }

    if (s_wl_handle == WL_INVALID_HANDLE) {
        const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, PARTITION_LABEL);
        if (!partition) {
            ESP_LOGE(TAG, "Cannot find 'storage' partition for raw wear-levelling");
            return ESP_ERR_NOT_FOUND;
        }
        esp_err_t err = wl_mount(partition, &s_wl_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount raw wear-levelling for USB MSC: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Raw wear-levelling active for USB MSC (handle=%d)", s_wl_handle);
    }
    return ESP_OK;
}

esp_err_t storage_manager_prepare_for_usb(void) {
    if (current_sd_card != NULL) {
        ESP_LOGI(TAG, "Preparing SD card for USB MSC...");
        esp_err_t err = sd_card_ensure_fatfs();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sd_card_ensure_fatfs returned %s (continuing in raw mode)", esp_err_to_name(err));
        }
        return err;
    }

    ESP_LOGI(TAG, "No SD card: preparing internal flash for USB MSC...");
    if (!s_flash_mounted && s_wl_handle == WL_INVALID_HANDLE) {
        esp_err_t err = storage_manager_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize flash storage: %s", esp_err_to_name(err));
            return err;
        }
    }
    return storage_manager_unmount_flash_for_usb();
}

esp_err_t storage_manager_flush_sync(void) {
    // Unmount and remount filesystem to force FatFS FAT table flush to disk/flash
    ESP_LOGI(TAG, "Flushing FatFS metadata...");

    if (sd_card_is_mounted()) {
        esp_err_t err = sd_card_unmount();
        vTaskDelay(pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            return err;
        }
        return sd_card_remount();
    }

    if (current_sd_card != NULL) {
        // SD present but raw (USB MSC owns it): nothing mounted, nothing to flush.
        ESP_LOGI(TAG, "SD raw (USB) mode: nothing to flush");
        return ESP_OK;
    }

    if (s_flash_mounted) {
        esp_vfs_fat_spiflash_unmount_rw_wl(BASE_PATH, s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
        s_flash_mounted = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        return storage_manager_init();
    }

    ESP_LOGI(TAG, "No mounted FatFS to flush");
    return ESP_OK;
}

// Flush all open file handles and unmount whatever FatFS is currently active,
// leaving the SD card in raw USB MSC mode and internal flash in raw wear-levelling mode.
esp_err_t storage_manager_flush_and_release_for_usb(void) {
    if (sd_card_is_mounted()) {
        ESP_LOGI(TAG, "Flushing and releasing SD card for USB MSC...");
        return sd_card_unmount();
    }
    if (current_sd_card != NULL) {
        // SD card present and already in raw mode
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Flushing and switching internal flash to raw USB MSC mode...");
    return storage_manager_unmount_flash_for_usb();
}

esp_err_t storage_manager_release_for_usb(void) {
    if (sd_card_is_mounted()) {
        ESP_LOGI(TAG, "Releasing SD card for USB MSC...");
        return sd_card_unmount();
    }
    if (current_sd_card != NULL) {
        return ESP_OK;
    }
    if (s_flash_mounted) {
        return storage_manager_unmount_flash_for_usb();
    }
    return ESP_OK;
}

wl_handle_t storage_manager_get_wl_handle(void) {
    return s_wl_handle;
}

const char* storage_manager_get_base_path(void) {
    if (sd_card_is_mounted()) {
        return "/sdcard";
    }
    return BASE_PATH;
}

bool storage_manager_get_fatfs_drive(char *out, size_t len) {
    BYTE pdrv = 0xFF;
    if (sd_card_is_mounted()) {
        pdrv = ff_diskio_get_pdrv_card(current_sd_card);
    } else if (s_flash_mounted && s_wl_handle != WL_INVALID_HANDLE) {
        pdrv = ff_diskio_get_pdrv_wl(s_wl_handle);
    }
    if (pdrv == 0xFF) {
        return false;
    }
    snprintf(out, len, "%u:", (unsigned)pdrv);
    return true;
}
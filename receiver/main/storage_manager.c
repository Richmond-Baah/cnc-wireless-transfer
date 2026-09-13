#include "sdkconfig.h"
#include "storage_manager.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "diskio.h"
#include "sd_card.h"
#include "nvs.h"

static const char *TAG = "STORAGE_MGR";
static const char *BASE_PATH = "/storage";
static const char *PARTITION_LABEL = "storage";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

esp_err_t storage_manager_unmount(void) {
    return ESP_OK;
}

esp_err_t storage_manager_remount(void) {
    return ESP_OK;
}

esp_err_t storage_manager_init(void) {
    ESP_LOGI(TAG, "=== storage_manager_init() START ===");
    
    if (s_wl_handle != WL_INVALID_HANDLE) {
        ESP_LOGI(TAG, "Already initialized, handle=%d", s_wl_handle);
        return ESP_OK;
    }

    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, PARTITION_LABEL);
    if (!partition) {
        ESP_LOGE(TAG, "FATAL: Cannot find 'storage' partition!");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Found partition: size=%d bytes", partition->size);

    // Mount VFS - format if needed so storage is always usable
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 5,
        .format_if_mount_failed = true,
        .allocation_unit_size = 512,
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(BASE_PATH, PARTITION_LABEL, &mount_config, &s_wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VFS Mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "VFS mounted OK, handle=%d", s_wl_handle);

    ESP_LOGI(TAG, "Filesystem mounted and formatted successfully!");
    
    #if CONFIG_FATFS_USE_LABEL
    f_setlabel("FILEXFER");
    #endif

    return ESP_OK;
}

esp_err_t storage_manager_format(void) {
    ESP_LOGI(TAG, "Formatting storage as FAT16...");

    if (s_wl_handle != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(BASE_PATH, s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
    }

    const esp_vfs_fat_mount_config_t format_config = {
        .max_files = 5,
        .format_if_mount_failed = false,
        .allocation_unit_size = 512,
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(
        BASE_PATH,
        PARTITION_LABEL,
        &format_config,
        &s_wl_handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(ret));
        return ret;
    }

    #if CONFIG_FATFS_USE_LABEL
    f_setlabel("FILEXFER");
    #endif
    
    ESP_LOGI(TAG, "Format complete");
    return storage_manager_init();
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
#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "esp_err.h"
#include "wear_levelling.h"
#include <stdint.h>

/**
 * @brief Initialize the storage manager (mount FAT filesystem)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t storage_manager_init(void);

/**
 * @brief Performs an explicit FAT16 format with 512-byte sectors
 * @return esp_err_t ESP_OK on success
 */
esp_err_t storage_manager_unmount(void);
esp_err_t storage_manager_remount(void);
esp_err_t storage_manager_format(void);

/**
 * @brief Get the wear leveling handle for MSC
 * @return wl_handle_t The active handle
 */
wl_handle_t storage_manager_get_wl_handle(void);

/**
 * @brief Get the base path for storage (/sdcard or /storage)
 */
const char* storage_manager_get_base_path(void);

#endif

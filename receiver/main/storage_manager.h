#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize the storage manager (mount FAT filesystem)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t storage_manager_init(void);

/**
 * @brief Unmount storage FatFS
 */
esp_err_t storage_manager_unmount(void);

/**
 * @brief Remount storage FatFS
 */
esp_err_t storage_manager_remount(void);

/**
 * @brief Performs formatting of active storage
 */
esp_err_t storage_manager_format(void);

/**
 * @brief Get total and free space in bytes and storage type name
 */
esp_err_t storage_manager_get_info(uint64_t *out_total, uint64_t *out_free, const char **out_type);

/**
 * @brief Check whether FatFS is currently mounted on the SD card
 */
bool storage_manager_fatfs_available(void);

/**
 * @brief Check whether an SD card is physically present/initialized
 */
bool storage_manager_is_sd_present(void);

/**
 * @brief Prepare storage for USB MSC operation: checks the SD card has a valid
 * FAT filesystem (never formats) and leaves it in raw mode. No card or an
 * unreadable FAT sets CARD_ERROR.
 */
esp_err_t storage_manager_prepare_for_usb(void);

/**
 * @brief Release storage so USB MSC can access the SD card exclusively
 * If FatFS is mounted on the SD card it is unmounted (flush performed).
 */
esp_err_t storage_manager_release_for_usb(void);

/**
 * @brief Flush pending FatFS writes and release storage for USB MSC
 */
esp_err_t storage_manager_flush_and_release_for_usb(void);

/**
 * @brief Sync and flush FatFS buffers to media
 */
esp_err_t storage_manager_flush_sync(void);

/**
 * @brief Get the base path for storage (/sdcard)
 */
const char* storage_manager_get_base_path(void);

/**
 * @brief FatFS logical drive ("N:") of the mounted storage at the base path,
 * for direct f_* calls that need FAT attributes. Returns false if not mounted.
 */
bool storage_manager_get_fatfs_drive(char *out, size_t len);

#endif


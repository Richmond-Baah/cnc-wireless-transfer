#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>

extern sdmmc_card_t *current_sd_card;

esp_err_t sd_card_init(void);
esp_err_t sd_card_init_raw(void);
bool sd_card_is_mounted(void);

/**
 * @brief Format the SD card filesystem (card must be unmounted)
 */
esp_err_t sd_card_format_fatfs(void);

/**
 * @brief Ensure the SD card has a valid FAT filesystem for USB MSC.
 * Mounts FatFS to verify the card; if mount fails (no valid FAT or
 * corrupt), never formats: sets the card-error flag and STATE_CARD_ERROR.
 * On success clears the flag (CARD_ERROR -> IDLE). Always leaves the card
 * in raw mode ready for USB MSC block access.
 */
esp_err_t sd_card_ensure_fatfs(void);

/**
 * @brief True while the SD card's filesystem is known to be unmountable.
 */
bool sd_card_has_card_error(void);

/**
 * @brief Re-run sd_card_ensure_fatfs() only if a card error is pending and
 * FatFS is not currently mounted. Returns ESP_OK if no error is pending.
 */
esp_err_t sd_card_retry_card_error(void);

/**
 * @brief Unmount the SD card FAT filesystem (flushes all cached data to disk)
 * Call this before exposing raw sectors to USB host.
 */
esp_err_t sd_card_unmount(void);

/**
 * @brief Remount the SD card FAT filesystem after USB host is done
 */
esp_err_t sd_card_remount(void);

/**
 * @brief Acquire exclusive access to the SD card for raw sector I/O
 * Used by USB MSC callbacks to serialize against FatFS mount/unmount.
 * @return true on success
 */
bool sd_card_acquire_io(void);

/**
 * @brief Release exclusive access to the SD card
 */
void sd_card_release_io(void);

#endif // SD_CARD_H

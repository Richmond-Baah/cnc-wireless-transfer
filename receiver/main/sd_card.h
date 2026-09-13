#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>

extern sdmmc_card_t *current_sd_card;

esp_err_t sd_card_init(void);
bool sd_card_is_mounted(void);

/**
 * @brief Unmount the SD card FAT filesystem (flushes all cached data to disk)
 * Call this before exposing raw sectors to USB host.
 */
esp_err_t sd_card_unmount(void);

/**
 * @brief Remount the SD card FAT filesystem after USB host is done
 */
esp_err_t sd_card_remount(void);

#endif // SD_CARD_H

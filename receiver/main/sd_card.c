#include "sd_card.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "state_machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SD_CARD";

// SD Card SPI Pin Definitions (SPI3 is usually safer on S3)
#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 11
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10

#define PIN_NUM_PWR  -1

#define SD_BASE_PATH "/sdcard"
#define SD_MAX_FILES 5

sdmmc_card_t *current_sd_card = NULL;
static bool s_fatfs_mounted = false;
static BYTE s_pdrv = FF_DRV_NOT_USED; // FatFS drive number used by the SD card disk I/O
static SemaphoreHandle_t s_io_lock = NULL; // serializes FatFS ops vs raw USB MSC sector access

static void io_lock_init(void) {
    if (s_io_lock == NULL) {
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();
        if (lock != NULL) {
            s_io_lock = lock;
            ESP_LOGI(TAG, "SD IO mutex created");
        }
    }
}

static bool io_lock_take(TickType_t timeout) {
    io_lock_init();
    return xSemaphoreTake(s_io_lock, timeout) == pdTRUE;
}

static void io_lock_give(void) {
    if (s_io_lock != NULL) {
        xSemaphoreGive(s_io_lock);
    }
}

// Public bus-access guard used by USB MSC callbacks for raw sector I/O.
// Acquring this lock prevents any concurrent FatFS mount/unmount from
// registering/unregistering the raw disk driver while sectors are in flight.
bool sd_card_acquire_io(void) {
    return io_lock_take(pdMS_TO_TICKS(1000));
}

// FatFS mount/unmount wait at most this long for a USB MSC sector access to
// finish (each holds the lock for one READ10/WRITE10), then fail instead of
// hanging the caller (e.g. /prepare) forever.
#define MOUNT_LOCK_TIMEOUT_MS 5000

static bool mount_lock_take(const char *what) {
    if (io_lock_take(pdMS_TO_TICKS(MOUNT_LOCK_TIMEOUT_MS))) {
        return true;
    }
    ESP_LOGE(TAG, "%s: SD IO lock busy for %d ms (USB host access?)", what, MOUNT_LOCK_TIMEOUT_MS);
    return false;
}

void sd_card_release_io(void) {
    io_lock_give();
}

static void power_cycle_sd_card(void) {
    if (PIN_NUM_PWR >= 0) {
        ESP_LOGI(TAG, "Power cycling SD card...");
        gpio_reset_pin(PIN_NUM_PWR);
        gpio_set_direction(PIN_NUM_PWR, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_NUM_PWR, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(PIN_NUM_PWR, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGI(TAG, "No power control pin defined. Waiting for card to stabilize...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Raw hardware probe only: initializes the card in sector mode for USB MSC.
// Does NOT register FatFS. Keeps the SD host/(card) alive permanently so USB
// MSC can read/write raw sectors; FatFS mount/unmount is layered on top later.
static esp_err_t sd_card_init_raw_locked(void) {
    if (current_sd_card != NULL) {
        ESP_LOGI(TAG, "SD card already initialized in raw mode");
        return ESP_OK;
    }

    power_cycle_sd_card();

    gpio_reset_pin(PIN_NUM_MISO);
    gpio_reset_pin(PIN_NUM_MOSI);
    gpio_reset_pin(PIN_NUM_CLK);
    gpio_reset_pin(PIN_NUM_CS);

    gpio_set_pull_mode(PIN_NUM_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CS,  GPIO_PULLUP_ONLY);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPI bus already initialized; continuing");
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Initializing SD card via SPI (Host: SPI3)");

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ret = (*host.init)();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdspi_host_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_dev_handle_t card_handle = -1;
    ret = sdspi_host_init_device(&slot_config, &card_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdspi_host_init_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
    if (card == NULL) {
        ESP_LOGE(TAG, "Failed to allocate sdmmc_card_t");
        return ESP_ERR_NO_MEM;
    }

    // The 'slot' field must be swapped to the actual SPI device handle returned
    // above, mirroring esp_vfs_fat_sdspi_mount().
    sdmmc_host_t host_config = host;
    if (card_handle != host.slot) {
        host_config.slot = card_handle;
    }

    ret = sdmmc_card_init(&host_config, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed: %s", esp_err_to_name(ret));
        sdspi_host_remove_device(card_handle);
        free(card);
        return ret;
    }

    current_sd_card = card;
    ESP_LOGI(TAG, "================ SD CARD INFO ================");
    sdmmc_card_print_info(stdout, current_sd_card);
    ESP_LOGI(TAG, "==============================================");
    return ESP_OK;
}

// Registers the SD disk I/O driver and mounts FatFS on /sdcard.
static esp_err_t sd_card_mount_fatfs_locked(void) {
    if (s_fatfs_mounted) {
        ESP_LOGI(TAG, "SD card FatFS already mounted");
        return ESP_OK;
    }

    if (current_sd_card == NULL) {
        ESP_LOGW(TAG, "Cannot mount FatFS: SD card not initialized (raw probe first)");
        return ESP_ERR_NOT_FOUND;
    }

    if (ff_diskio_get_drive(&s_pdrv) != ESP_OK || s_pdrv == FF_DRV_NOT_USED) {
        ESP_LOGE(TAG, "No free FatFS drive slot available");
        s_pdrv = FF_DRV_NOT_USED;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Using FatFS drive %u for SD card", (unsigned)s_pdrv);

    char drv[3] = {(char)('0' + s_pdrv), ':', 0};

    ff_diskio_register_sdmmc(s_pdrv, current_sd_card);
    // SPI mode has no card-detect pin: keep disk status "present" so FatFS
    // does not treat the card as removed between sector operations.
    ff_sdmmc_set_disk_status_check(s_pdrv, false);

    esp_vfs_fat_conf_t conf = {
        .base_path = SD_BASE_PATH,
        .fat_drive = drv,
        .max_files = SD_MAX_FILES,
    };
    FATFS *fs = NULL;
    esp_err_t err = esp_vfs_fat_register_cfg(&conf, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_register_cfg failed: %s", esp_err_to_name(err));
        goto fail;
    }

    FRESULT fres = f_mount(fs, drv, 1);
    if (fres != FR_OK) {
        ESP_LOGE(TAG, "f_mount failed: %d", (int)fres);
        err = ESP_FAIL;
        goto fail;
    }

    s_fatfs_mounted = true;
    ESP_LOGI(TAG, "SD card FatFS mounted on %s", SD_BASE_PATH);
    return ESP_OK;

fail:
    if (fs) {
        f_mount(NULL, drv, 0);
    }
    esp_vfs_fat_unregister_path(SD_BASE_PATH);
    ff_diskio_unregister(s_pdrv);
    s_pdrv = FF_DRV_NOT_USED;
    return err;
}

// Flushes and unregisters FatFS so the SD card becomes raw again for USB MSC.
// IMPORTANT ORDER (matches IDF unmount): f_mount(NULL) FIRST (flushes open
// files and drops the FatFs[vol] reference), THEN unregister disk I/O, THEN
// unregister the VFS path.
static esp_err_t sd_card_unmount_fatfs_locked(void) {
    if (!s_fatfs_mounted) {
        ESP_LOGI(TAG, "SD card FatFS already unmounted");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Unmounting SD card FatFS (flushing to disk)...");
    char drv[3] = {(char)('0' + s_pdrv), ':', 0};

    FRESULT fres = f_mount(NULL, drv, 0);
    if (fres != FR_OK) {
        ESP_LOGW(TAG, "f_mount(NULL) returned %d (continuing cleanup)", (int)fres);
    }

    esp_err_t err = esp_vfs_fat_unregister_path(SD_BASE_PATH);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_vfs_fat_unregister_path returned: %s", esp_err_to_name(err));
    }

    ff_diskio_unregister(s_pdrv);
    s_pdrv = FF_DRV_NOT_USED;
    s_fatfs_mounted = false;

    ESP_LOGI(TAG, "SD card FatFS unmounted. RAW access active for USB MSC.");
    return (fres == FR_OK) ? err : ESP_FAIL;
}

esp_err_t sd_card_init_raw(void) {
    if (!io_lock_take(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = sd_card_init_raw_locked();
    io_lock_give();
    return ret;
}

esp_err_t sd_card_init(void) {
    if (!mount_lock_take("sd_card_init")) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_OK;
    if (s_fatfs_mounted) {
        ESP_LOGI(TAG, "SD card FatFS already mounted");
    } else {
        if (current_sd_card == NULL) {
            ret = sd_card_init_raw_locked();
        }
        if (ret == ESP_OK) {
            ret = sd_card_mount_fatfs_locked();
        }
    }
    io_lock_give();
    return ret;
}

bool sd_card_is_mounted(void) {
    return s_fatfs_mounted;
}

esp_err_t sd_card_unmount(void) {
    if (!mount_lock_take("sd_card_unmount")) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = sd_card_unmount_fatfs_locked();
    io_lock_give();
    return ret;
}

esp_err_t sd_card_remount(void) {
    if (!mount_lock_take("sd_card_remount")) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = sd_card_mount_fatfs_locked();
    io_lock_give();
    return ret;
}

// Formats the SD card via FatFS f_mkfs. Requires the card to be unmounted.
static esp_err_t sd_card_format_fatfs_locked(void) {
    if (s_fatfs_mounted) {
        ESP_LOGW(TAG, "Cannot format while FatFS is mounted");
        return ESP_ERR_INVALID_STATE;
    }

    if (current_sd_card == NULL) {
        ESP_LOGW(TAG, "Cannot format: SD card not initialized");
        return ESP_ERR_NOT_FOUND;
    }

    BYTE pdrv = FF_DRV_NOT_USED;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        ESP_LOGE(TAG, "No free FatFS drive slot available for formatting");
        return ESP_ERR_NOT_FOUND;
    }

    char drv[3] = {(char)('0' + pdrv), ':', 0};

    ff_diskio_register_sdmmc(pdrv, current_sd_card);
    ff_sdmmc_set_disk_status_check(pdrv, false);

    BYTE work[FF_MAX_SS]; // scratch buffer for f_mkfs
    FRESULT fres = f_mkfs(drv, NULL, work, sizeof(work));
    esp_err_t ret = (fres == FR_OK) ? ESP_OK : ESP_FAIL;
    if (fres != FR_OK) {
        ESP_LOGE(TAG, "f_mkfs failed: %d", (int)fres);
    } else {
        ESP_LOGI(TAG, "SD card formatted (drive %u)", (unsigned)pdrv);
    }

    ff_diskio_unregister(pdrv);
    return ret;
}

esp_err_t sd_card_format_fatfs(void) {
    if (!io_lock_take(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = sd_card_format_fatfs_locked();
    io_lock_give();
    return ret;
}

static bool s_card_error = false; // set when the card's FAT filesystem can't be mounted

bool sd_card_has_card_error(void) {
    return s_card_error;
}

static esp_err_t sd_card_ensure_fatfs_locked(void) {
    if (current_sd_card == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    // If already mounted, unmount first to test from a clean state
    if (s_fatfs_mounted) {
        esp_err_t unmount_err = sd_card_unmount_fatfs_locked();
        if (unmount_err != ESP_OK) {
            return unmount_err;
        }
    }

    // Attempt to mount FatFS to verify existing filesystem
    esp_err_t ret = sd_card_mount_fatfs_locked();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card has valid FAT filesystem; unmounting for raw USB MSC access");
        ret = sd_card_unmount_fatfs_locked();
        if (s_card_error) {
            s_card_error = false;
            ESP_LOGI(TAG, "SD card readable again; leaving CARD_ERROR");
            state_machine_transition(STATE_IDLE);
        }
    } else {
        // Never auto-format: a transient SPI error or a non-FAT card (e.g. exFAT)
        // must not wipe user data. Leave the card raw and flag the error.
        ESP_LOGE(TAG, "SD card FAT mount failed (%s). NOT formatting; card left raw. "
                      "Reformat as FAT32 on a PC or via /format.", esp_err_to_name(ret));
        s_card_error = true;
        state_machine_transition(STATE_CARD_ERROR);
    }
    return ret;
}

esp_err_t sd_card_ensure_fatfs(void) {
    if (!io_lock_take(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = sd_card_ensure_fatfs_locked();
    io_lock_give();
    return ret;
}

esp_err_t sd_card_retry_card_error(void) {
    if (!io_lock_take(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_OK;
    if (s_card_error) {
        // Checked under the lock: never unmount a FatFS session a transfer just opened
        ret = s_fatfs_mounted ? ESP_ERR_INVALID_STATE : sd_card_ensure_fatfs_locked();
    }
    io_lock_give();
    return ret;
}
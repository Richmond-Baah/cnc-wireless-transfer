#include "sd_card.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SD_CARD";

// SD Card SPI Pin Definitions (SPI3 is usually safer on S3)
#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 11
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10

// Optional: Define a power pin if you have a MOSFET controlling SD card VCC
#define PIN_NUM_PWR  -1 // Set to GPIO number if applicable

// Global reference used by usb_msc.c via extern
sdmmc_card_t *current_sd_card = NULL;
static int successful_speed_khz = 400; // Default to safe speed

static void power_cycle_sd_card() {
    if (PIN_NUM_PWR >= 0) {
        ESP_LOGI(TAG, "Power cycling SD card...");
        gpio_reset_pin(PIN_NUM_PWR);
        gpio_set_direction(PIN_NUM_PWR, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_NUM_PWR, 0); // Assuming active high, set low to power off
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(PIN_NUM_PWR, 1); // Power on
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for stabilization
    } else {
        // Soft reset via wait (some cards need time after power-on before SPI commands)
        ESP_LOGI(TAG, "No power control pin defined. Waiting for card to stabilize...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static esp_err_t attempt_mount(int freq_khz, sdmmc_host_t *host, sdspi_device_config_t *slot_config, esp_vfs_fat_sdmmc_mount_config_t *mount_config) {
    host->max_freq_khz = freq_khz;
    ESP_LOGI(TAG, "Attempting to mount at %d kHz...", freq_khz);
    
    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", host, slot_config, mount_config, &current_sd_card);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SUCCESS! Mounted at %d kHz.", freq_khz);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed at %d kHz (Error: %s)", freq_khz, esp_err_to_name(ret));
        return ret;
    }
}

esp_err_t sd_card_init(void) {
    if (current_sd_card != NULL) {
        ESP_LOGI(TAG, "SD card already initialized");
        return ESP_OK;
    }

    esp_err_t ret;

    // Power management: cycle power if possible, or wait
    power_cycle_sd_card();

    // 1. Reset and Configure Pins for Stability
    gpio_reset_pin(PIN_NUM_MISO);
    gpio_reset_pin(PIN_NUM_MOSI);
    gpio_reset_pin(PIN_NUM_CLK);
    gpio_reset_pin(PIN_NUM_CS);

    // Pull-ups are CRITICAL for SPI SD cards, especially MISO and CS.
    gpio_set_pull_mode(PIN_NUM_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CS,  GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "Initializing SD card via SPI (Host: SPI3)");
    ESP_LOGI(TAG, "SPI pins: MOSI=%d, MISO=%d, CLK=%d, CS=%d", PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_CLK, PIN_NUM_CS);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 512
    };

    // 2. Configure SPI Host
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST; // Use SPI3 (HSPI) on ESP32-S3 to avoid internal flash conflicts

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
    };

    // Check if SPI bus is already initialized
    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // Short settle delay

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    // 3. Multi-speed Diagnostics & Initialization
    // We test from highest (20MHz) down to lowest (400kHz) to get the best performance.
    // The driver handles the initial 400kHz negotiation automatically; this frequency 
    // sets the MAX speed used after identification.
    int test_speeds[] = {20000, 10000, 5000, 1000, 400};
    int num_speeds = sizeof(test_speeds) / sizeof(test_speeds[0]);
    bool mounted = false;

    for (int i = 0; i < num_speeds; i++) {
        ret = attempt_mount(test_speeds[i], &host, &slot_config, &mount_config);
        if (ret == ESP_OK) {
            mounted = true;
            successful_speed_khz = test_speeds[i];
            ESP_LOGI(TAG, "SD Card initialized at %d kHz", successful_speed_khz);
            break; // Stop at the highest successful speed
        }
        // Wait before trying the next speed
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!mounted) {
        ESP_LOGE(TAG, "Failed to initialize SD card at ANY speed.");
        ESP_LOGE(TAG, "--- LIKELY CAUSES ---");
        ESP_LOGE(TAG, "1. Wiring: Ensure MOSI, MISO, CLK, CS are correct and wires are SHORT.");
        ESP_LOGE(TAG, "2. Power: The 3.3V rail drops during init. SD cards draw 100mA+. Use a strong 3.3V source or 5V if module has LDO.");
        ESP_LOGE(TAG, "3. SPI Mode Unsupported: Some cheap/fake cards (especially 8GB+) drop SPI mode support.");
        ESP_LOGE(TAG, "4. Format: Card must be formatted as FAT32, not exFAT or NTFS.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "================ SD CARD INFO ================");
    sdmmc_card_print_info(stdout, current_sd_card);
    ESP_LOGI(TAG, "==============================================");

    // 4. Capacity Sanity Check
    uint64_t capacity_mb = ((uint64_t)current_sd_card->csd.capacity) * 512 / (1024 * 1024);
    ESP_LOGI(TAG, "Reported FAT Capacity: %llu MB", capacity_mb);
    
    if (capacity_mb < 2000) {
        ESP_LOGW(TAG, "WARNING: Card capacity is very low (< 2GB).");
        ESP_LOGW(TAG, "If this is your 32GB card, the partition table is corrupted or heavily fragmented.");
        ESP_LOGW(TAG, "Please use 'SD Card Formatter' on PC to restore full capacity.");
    }

    return ESP_OK;
}

bool sd_card_is_mounted(void) {
    return current_sd_card != NULL;
}

esp_err_t sd_card_unmount(void) {
    return ESP_OK;
}

esp_err_t sd_card_remount(void) {
    return ESP_OK;
}

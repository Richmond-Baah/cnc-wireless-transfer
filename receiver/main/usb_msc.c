#include "usb_msc.h"
#include "esp_log.h"
#include "storage_manager.h"
#include "tinyusb.h"
#include "state_machine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "soc/usb_serial_jtag_struct.h"
#include "soc/system_struct.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "USB_MSC";
static bool s_usb_mounted_by_host = false;

#define TOTAL_LUNS 1

static tusb_desc_device_t const dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x0781,   // SanDisk VID
    .idProduct = 0x5581,  // Ultra USB 3.0 PID (widely compatible)
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1
};

#define USB_CONFIG_POWER_MA 500
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static uint8_t const config_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, 0x80, USB_CONFIG_POWER_MA),
    TUD_MSC_DESCRIPTOR(0, 4, 0x01, 0x81, 64)
};

static char const *string_desc[] = {
    (char[]){0x09, 0x04},
    "SanDisk",            // Manufacturer
    "Ultra USB 3.0",      // Product
    "SAN1234567890ABC",   // Serial
    "MSC Interface",
};

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "SanDisk ", 8);
    memcpy(product_id, "Ultra USB 3.0   ", 16);
    memcpy(product_rev, "1.00", 4);
    ESP_LOGD(TAG, "SCSI Inquiry: LUN=%u", lun);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    // FatFS has exclusive ownership of the SD card -> host must retry later
    if (current_sd_card != NULL && sd_card_is_mounted()) {
        return false;
    }
    // FatFS has exclusive ownership of internal flash -> host must retry later
    if (current_sd_card == NULL && storage_manager_is_flash_mounted()) {
        return false;
    }
    // We are ready if we have ANY valid hardware pointer (SD or Internal Flash)
    bool has_sd = (current_sd_card != NULL);
    bool has_flash = (storage_manager_get_wl_handle() != WL_INVALID_HANDLE);

    return (has_sd || has_flash);
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;

    if (current_sd_card != NULL) {
        // SD card: 512-byte sectors (hardware sector size)
        *block_size  = 512;
        *block_count = current_sd_card->csd.capacity;
        ESP_LOGI(TAG, "SD Card: block_size=512, blocks=%lu", *block_count);
    } else {
        wl_handle_t wl = storage_manager_get_wl_handle();
        ESP_LOGI(TAG, "=== CAPACITY CB: WL handle=%d ===", wl);
        if (wl != WL_INVALID_HANDLE) {
            // IMPORTANT: flash erase granularity = wl_sector_size() (typically 4096).
            // Reporting 512-byte blocks causes wl_erase_range to erase 8 USB
            // sectors for every single write, silently corrupting the FAT after
            // every USB session. Report the actual erase-sector size so each USB
            // LBA maps 1:1 to one erase operation, preserving files across reboots.
            uint32_t sec_size  = wl_sector_size(wl);
            uint32_t total     = wl_size(wl);
            *block_size  = (uint16_t)sec_size;
            *block_count = total / sec_size;
            ESP_LOGI(TAG, "Flash: sector_size=%lu, total=%lu, blocks=%lu",
                     sec_size, total, *block_count);
        } else {
            *block_size  = 512;
            *block_count = 0;
            ESP_LOGW(TAG, "NO STORAGE AVAILABLE!");
        }
    }
    ESP_LOGI(TAG, "USB Capacity: block_size=%u, blocks=%lu", *block_size, *block_count);
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return true;
}


int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    (void)lun;
    if (current_sd_card != NULL) {
        if (!sd_card_acquire_io()) {
            ESP_LOGE(TAG, "READ FAILED: could not acquire SD IO lock (FatFS busy?)");
            return -1;
        }
        if (sd_card_is_mounted()) {
            ESP_LOGE(TAG, "READ FAILED: SD owned by FatFS, raw LBA access refused");
            sd_card_release_io();
            return -1;
        }

        uint32_t sectors_total = bufsize / 512;
        uint32_t sectors_done = 0;
        uint8_t *ptr = (uint8_t *)buffer;
        int32_t result = bufsize;

        while (sectors_done < sectors_total) {
            uint32_t count = sectors_total - sectors_done;
            if (count > 8) count = 8; // 8 sectors = 4KB safe chunk

            esp_err_t ret = sdmmc_read_sectors(current_sd_card, ptr, lba + sectors_done, count);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "SD Card READ FAILED at LBA=%lu, count=%lu: %s", lba + sectors_done, count, esp_err_to_name(ret));
                result = -1;
                break;
            }
            ptr += count * 512;
            sectors_done += count;
        }

        sd_card_release_io();
        return result;
    } else {
        if (storage_manager_is_flash_mounted()) {
            ESP_LOGE(TAG, "READ FAILED: Flash owned by FatFS, raw access refused");
            return -1;
        }
        wl_handle_t wl = storage_manager_get_wl_handle();
        if (wl != WL_INVALID_HANDLE) {
            // Address is lba * sector_size (sector_size = wl_sector_size, not 512)
            uint32_t sec_size = wl_sector_size(wl);
            esp_err_t ret = wl_read(wl, (size_t)lba * sec_size + offset, buffer, bufsize);
            if (ret == ESP_OK) {
                return bufsize;
            }
            ESP_LOGE(TAG, "READ FAILED at LBA=%lu offset=%lu: %s", lba, offset, esp_err_to_name(ret));
            return -1;
        }
    }
    ESP_LOGE(TAG, "READ FAILED: No storage handle");
    return -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    (void)lun;
    if (current_sd_card != NULL) {
        if (!sd_card_acquire_io()) {
            ESP_LOGE(TAG, "WRITE FAILED: could not acquire SD IO lock (FatFS busy?)");
            return -1;
        }
        if (sd_card_is_mounted()) {
            ESP_LOGE(TAG, "WRITE FAILED: SD owned by FatFS, raw LBA access refused");
            sd_card_release_io();
            return -1;
        }

        uint32_t sectors_total = bufsize / 512;
        uint32_t sectors_done = 0;
        uint8_t *ptr = buffer;
        int32_t result = bufsize;

        while (sectors_done < sectors_total) {
            uint32_t count = sectors_total - sectors_done;
            if (count > 8) count = 8; // 8 sectors = 4KB safe chunk

            esp_err_t ret = sdmmc_write_sectors(current_sd_card, ptr, lba + sectors_done, count);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "SD Card WRITE FAILED at LBA=%lu, count=%lu: %s", lba + sectors_done, count, esp_err_to_name(ret));
                result = -1;
                break;
            }
            ptr += count * 512;
            sectors_done += count;
        }

        sd_card_release_io();
        return result;
    } else {
        if (storage_manager_is_flash_mounted()) {
            ESP_LOGE(TAG, "WRITE FAILED: Flash owned by FatFS, raw access refused");
            return -1;
        }
        wl_handle_t wl = storage_manager_get_wl_handle();
        if (wl != WL_INVALID_HANDLE) {
            // Each USB LBA maps to exactly one erase sector (wl_sector_size bytes).
            // We must erase before writing — wl_erase_range requires the address
            // and size to be multiples of wl_sector_size(); bufsize == sec_size here.
            uint32_t sec_size = wl_sector_size(wl);
            size_t addr = (size_t)lba * sec_size + offset;
            // Erase the sector(s) covered by this write
            size_t erase_size = ((bufsize + sec_size - 1) / sec_size) * sec_size;
            esp_err_t ret = wl_erase_range(wl, addr, erase_size);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "wl_erase_range failed at addr 0x%zx (size %zu): %s",
                         addr, erase_size, esp_err_to_name(ret));
                return -1;
            }
            ret = wl_write(wl, addr, buffer, bufsize);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "wl_write failed at addr 0x%zx (size %lu): %s",
                         addr, (unsigned long)bufsize, esp_err_to_name(ret));
                return -1;
            }
            return bufsize;
        }
    }
    return -1;
}

void tud_msc_flush_cb(uint8_t lun) {
    (void)lun;
    ESP_LOGD(TAG, "FLUSH: Syncing data (LUN=%u)", lun);
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void) lun; (void) power_condition; (void) load_eject;
    ESP_LOGI(TAG, "MSC %s", start ? "START" : "STOP");
    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)lun;
    (void)bufsize;
    uint8_t const cmd = scsi_cmd[0];
    
    switch (cmd) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        ESP_LOGD(TAG, "SCSI: PREVENT_ALLOW_MEDIUM_REMOVAL");
        return 0;
    case SCSI_CMD_START_STOP_UNIT:
        ESP_LOGD(TAG, "SCSI: START_STOP_UNIT");
        return 0;
    case SCSI_CMD_TEST_UNIT_READY:
        return 0;
    default:
        ESP_LOGD(TAG, "SCSI: Handled by TinyUSB core (cmd 0x%02X)", cmd);
        return 0;
    }
}

void tud_msc_writable_changed_cb(uint8_t lun) {
    (void)lun;
    ESP_LOGI(TAG, "MSC writable changed");
}

void tud_mount_cb(void) {
    ESP_LOGI(TAG, "========== USB MOUNTED BY HOST ==========");
    s_usb_mounted_by_host = true;

    if (state_machine_get() != STATE_RECEIVING) {
        state_machine_transition(STATE_USB_ACTIVE);
    }
}

void tud_umount_cb(void) {
    ESP_LOGI(TAG, "********** USB UNMOUNTED **********");
    s_usb_mounted_by_host = false;

    if (state_machine_get() == STATE_USB_ACTIVE) {
        state_machine_transition(STATE_IDLE);
    }
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    ESP_LOGI(TAG, "USB Suspended");
}

void tud_resume_cb(void) {
    ESP_LOGI(TAG, "USB Resumed");
}

void usb_msc_disconnect(void) {
    ESP_LOGI(TAG, "Disconnecting USB MSC...");
    s_usb_mounted_by_host = false;
    tud_disconnect();
}

void usb_msc_connect(void) {
    ESP_LOGI(TAG, "Connecting USB...");
    // Force a complete physical disconnect state first
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    tud_connect();
}

void usb_msc_notify_change(void) {
    ESP_LOGI(TAG, "Re-enumerating device...");
    usb_msc_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    usb_msc_connect();
}

void usb_msc_init(void) {
    ESP_LOGI(TAG, "[USB_INIT] Starting...");

    /*
    Disable USB Serial/JTAG to prevent pad contention with TinyUSB OTG.
    Do NOT call gpio_reset_pin() on GPIO 19/20 — that switches IO MUX to
    GPIO mode and usb_new_phy() never restores it, preventing enumeration.
    The register writes below stop USJ without touching the IO MUX.
    */
    SYSTEM.perip_clk_en1.usb_device_clk_en = 0;
    SYSTEM.perip_rst_en1.usb_device_rst = 1;
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "[USB_INIT] USJ disabled via registers");

    ESP_LOGI(TAG, "[USB_INIT] TinyUSB driver install...");
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &dev_desc,
        .string_descriptor = string_desc,
        .string_descriptor_count = sizeof(string_desc) / sizeof(string_desc[0]),
        .external_phy = false,
        .configuration_descriptor = config_desc,
    };

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[USB_INIT] FAILED: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[USB_INIT] Driver installed OK");

    ESP_LOGI(TAG, "[USB_INIT] Calling tud_connect...");
    tud_connect();
    ESP_LOGI(TAG, "[USB_INIT] tud_connect() called");
    ESP_LOGI(TAG, "[USB_INIT] COMPLETE - device should enumerate");
}

esp_err_t usb_msc_format_storage(void) {
    return storage_manager_format();
}

bool usb_msc_is_host_mounted(void) {
    // Return true only if USB is actively connected and enumerated by a host
    return s_usb_mounted_by_host;
}
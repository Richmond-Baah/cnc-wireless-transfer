#include "usb_msc.h"
#include "esp_log.h"
#include "storage_manager.h"
#include "tinyusb.h"
#include "state_machine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"
#include "sdmmc_cmd.h"
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
    // We are ready if we have ANY valid hardware pointer (SD or Internal Flash)
    bool has_sd = (current_sd_card != NULL);
    bool has_flash = (storage_manager_get_wl_handle() != WL_INVALID_HANDLE);
    
    return (has_sd || has_flash);
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_size = 512;
    
    if (current_sd_card != NULL) {
        *block_count = current_sd_card->csd.capacity;
        ESP_LOGI(TAG, "SD Card: blocks=%lu", *block_count);
    } else {
        wl_handle_t wl = storage_manager_get_wl_handle();
        ESP_LOGI(TAG, "=== CAPACITY CB: WL handle=%d ===", wl);
        if (wl != WL_INVALID_HANDLE) {
            uint32_t size = wl_size(wl);
            *block_count = size / 512;
            ESP_LOGI(TAG, "Flash: size=%lu bytes, blocks=%lu", size, *block_count);
        } else {
            *block_count = 0;
            ESP_LOGW(TAG, "NO STORAGE AVAILABLE!");
        }
    }
    ESP_LOGI(TAG, "USB Capacity Reported: %lu blocks", *block_count);
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return true;
}

// Permanent buffer for flash sector operations (4KB)
static uint8_t s_internal_flash_buf[4096];

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    (void)lun;
    if (current_sd_card != NULL) {
        uint32_t sectors_total = bufsize / 512;
        uint32_t sectors_done = 0;
        uint8_t *ptr = (uint8_t *)buffer;

        while (sectors_done < sectors_total) {
            uint32_t count = sectors_total - sectors_done;
            if (count > 8) count = 8; // 8 sectors = 4KB safe chunk

            esp_err_t ret = sdmmc_read_sectors(current_sd_card, ptr, lba + sectors_done, count);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "SD Card READ FAILED at LBA=%lu, count=%lu: %s", lba + sectors_done, count, esp_err_to_name(ret));
                return -1;
            }
            ptr += count * 512;
            sectors_done += count;
        }
        return bufsize;
    } else {
        wl_handle_t wl = storage_manager_get_wl_handle();
        if (wl != WL_INVALID_HANDLE) {
            esp_err_t ret = wl_read(wl, lba * 512 + offset, buffer, bufsize);
            if (ret == ESP_OK) {
                return bufsize;
            }
            ESP_LOGE(TAG, "READ FAILED: %s", esp_err_to_name(ret));
            return -1;
        }
    }
    ESP_LOGE(TAG, "READ FAILED: No storage handle");
    return -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    (void)lun;
    if (current_sd_card != NULL) {
        uint32_t sectors_total = bufsize / 512;
        uint32_t sectors_done = 0;
        uint8_t *ptr = buffer;

        while (sectors_done < sectors_total) {
            uint32_t count = sectors_total - sectors_done;
            if (count > 8) count = 8; // 8 sectors = 4KB safe chunk

            esp_err_t ret = sdmmc_write_sectors(current_sd_card, ptr, lba + sectors_done, count);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "SD Card WRITE FAILED at LBA=%lu, count=%lu: %s", lba + sectors_done, count, esp_err_to_name(ret));
                return -1;
            }
            ptr += count * 512;
            sectors_done += count;
        }
        return bufsize;
    } else {
        wl_handle_t wl = storage_manager_get_wl_handle();
        if (wl != WL_INVALID_HANDLE) {
            const size_t sector_sz = 4096;
            uint32_t write_addr = lba * 512;
            uint32_t bytes_written = 0;

            while (bytes_written < bufsize) {
                uint32_t current_addr = write_addr + bytes_written;
                uint32_t sector_start = current_addr & ~(sector_sz - 1);
                uint32_t offset_in_sector = current_addr % sector_sz;
                uint32_t chunk_size = (sector_sz - offset_in_sector < bufsize - bytes_written) ? 
                                       sector_sz - offset_in_sector : bufsize - bytes_written;

                wl_read(wl, sector_start, s_internal_flash_buf, sector_sz);
                memcpy(s_internal_flash_buf + offset_in_sector, buffer + bytes_written, chunk_size);
                wl_erase_range(wl, sector_start, sector_sz);
                wl_write(wl, sector_start, s_internal_flash_buf, sector_sz);

                bytes_written += chunk_size;
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
    default:
        ESP_LOGD(TAG, "SCSI: Unknown command 0x%02X", cmd);
        return -1;
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
    
    ESP_LOGI(TAG, "[USB_INIT] Storage manager init...");
    storage_manager_init();
    ESP_LOGI(TAG, "[USB_INIT] Storage DONE");

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
    return s_usb_mounted_by_host;
}
#ifndef USB_MSC_H
#define USB_MSC_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

void usb_msc_init(void);
void usb_msc_disconnect(void);
void usb_msc_connect(void);
void usb_msc_notify_change(void);
esp_err_t usb_msc_format_storage(void);
bool usb_msc_is_host_mounted(void);
// Milliseconds since the host last read a sector (UINT32_MAX if never)
uint32_t usb_msc_ms_since_host_read(void);
// Host READ10 per 10 s window, for tuning the busy guard. Sectors are split by
// what they hold, using the layout from the last file list refresh.
typedef struct {
    uint32_t reads;       // READ10 transfers
    uint32_t meta;        // sectors: boot, FAT, root directory
    uint32_t data;        // sectors: a listed file
    uint32_t other;       // sectors: anything else (folders, files changed by the host)
    char file[33];        // most-read listed file, "" = none
} usb_msc_read_summary_t;
// False until the first window ends, or when CONFIG_CNC_USB_READ_LOG is off.
bool usb_msc_read_summary(usb_msc_read_summary_t *out);
// Layout, set only while the host cannot read the drive. name NULL = boot/FAT/root.
void usb_msc_read_map_begin(uint32_t data_start_lba);
void usb_msc_read_map_add(uint32_t start_lba, uint32_t sectors, const char *name);
// USB connect -> host mounted the drive, for the last connect (-1 = not yet)
int usb_msc_host_mount_ms(void);
// Reconnects once the drive has been off for min_off_ms in total (no wait if it already has).
void usb_msc_connect_after_off(int min_off_ms);

#endif // USB_MSC_H

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
// USB connect -> host mounted the drive, for the last connect (-1 = not yet)
int usb_msc_host_mount_ms(void);
// Reconnects once the drive has been off for min_off_ms in total (no wait if it already has).
void usb_msc_connect_after_off(int min_off_ms);

#endif // USB_MSC_H

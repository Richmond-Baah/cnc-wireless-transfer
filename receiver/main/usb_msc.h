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

#endif // USB_MSC_H

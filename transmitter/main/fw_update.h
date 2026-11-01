#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_image_format.h"

// Bytes of an image needed to read its app description (image header +
// first segment header + esp_app_desc_t).
#define FW_DESC_END (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

// Compares "1.6.1-3-gabc1234"-style versions (major.minor.patch, then commits
// after the tag). <0, 0, >0 like strcmp.
int fw_version_cmp(const char *a, const char *b);

// Checks the start of an image (at least FW_DESC_END bytes): ESP32-S3 app,
// project name, newer than current_ver. HTTP status; msg gets the reason.
// desc_out (optional) gets the image's app description.
int fw_check_image(const uint8_t *data, size_t n, const char *project, const char *current_ver,
                   esp_app_desc_t *desc_out, char *msg, size_t msg_len);

// Transmitter self-update into the inactive OTA slot. One session at a time;
// a session idle for 30 s is dropped by the next begin. All return an HTTP
// status; msg gets the reason on failure.
int fw_update_begin(size_t size, char *msg, size_t msg_len);
// Sequential writes: offset must equal the bytes written so far (an already
// written chunk is acknowledged again). The first chunk is checked with
// fw_check_image().
int fw_update_write(size_t offset, const uint8_t *data, size_t n, size_t *written, char *msg,
                    size_t msg_len);
// Verifies the image and sets it as the boot slot (caller restarts).
int fw_update_finish(char *version, size_t version_len, char *msg, size_t msg_len);
void fw_update_abort(void);
bool fw_update_active(void);

// After boot: if this image is still pending verification, mark it valid when
// ok (Wi-Fi + web server up), else roll back and reboot.
void fw_update_confirm_boot(bool ok);

#endif // FW_UPDATE_H

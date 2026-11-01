#include "fw_update.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "FW_UPDATE";

#define SESSION_IDLE_US (30LL * 1000 * 1000)
#define SIG_SECTOR 4096 // Secure Boot V2 signature sector appended by espsecure sign_data

static struct {
  bool active;
  esp_ota_handle_t handle;
  const esp_partition_t *part;
  size_t size;
  size_t written;
  int64_t last_us;
  char version[32];
} s_ota;

// "1.6.1-3-gabc" -> {1, 6, 1, 3}; anything unparsable counts as 0
static void parse_version(const char *v, long out[4]) {
  memset(out, 0, sizeof(long) * 4);
  if (*v == 'v') v++;
  char *end;
  for (int i = 0; i < 3; i++) {
    out[i] = strtol(v, &end, 10);
    if (end == v) return;
    v = end;
    if (*v != '.') break;
    v++;
  }
  if (*v == '-') {
    long n = strtol(v + 1, &end, 10);
    if (end != v + 1 && *end == '-') out[3] = n; // "-<n>-g<hash>", not "-dev"
  }
}

int fw_version_cmp(const char *a, const char *b) {
  long va[4], vb[4];
  parse_version(a, va);
  parse_version(b, vb);
  for (int i = 0; i < 4; i++) {
    if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
  }
  return 0;
}

int fw_check_image(const uint8_t *data, size_t n, const char *project, const char *current_ver,
                   esp_app_desc_t *desc_out, char *msg, size_t msg_len) {
  if (n < FW_DESC_END) {
    snprintf(msg, msg_len, "Not a firmware file (too short)");
    return 400;
  }
  const esp_image_header_t *h = (const esp_image_header_t *)data;
  esp_app_desc_t d;
  memcpy(&d, data + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(d));
  if (h->magic != ESP_IMAGE_HEADER_MAGIC || d.magic_word != ESP_APP_DESC_MAGIC_WORD) {
    snprintf(msg, msg_len, "Not a firmware file");
    return 400;
  }
  if (h->chip_id != ESP_CHIP_ID_ESP32S3) {
    snprintf(msg, msg_len, "Firmware is for another chip");
    return 400;
  }
  d.project_name[sizeof(d.project_name) - 1] = '\0';
  d.version[sizeof(d.version) - 1] = '\0';
  if (strcmp(d.project_name, project) != 0) {
    snprintf(msg, msg_len, "Wrong firmware: %.31s, need %.31s", d.project_name, project);
    return 400;
  }
  if (fw_version_cmp(d.version, current_ver) <= 0) {
    snprintf(msg, msg_len, "Version %.31s is not newer than %.31s", d.version, current_ver);
    return 409;
  }
  if (desc_out) *desc_out = d;
  return 200;
}

bool fw_update_active(void) { return s_ota.active; }

void fw_update_abort(void) {
  if (s_ota.active) {
    esp_ota_abort(s_ota.handle);
    ESP_LOGW(TAG, "Update aborted after %u of %u bytes", (unsigned)s_ota.written,
             (unsigned)s_ota.size);
  }
  memset(&s_ota, 0, sizeof(s_ota));
}

int fw_update_begin(size_t size, char *msg, size_t msg_len) {
  if (s_ota.active) {
    if (esp_timer_get_time() - s_ota.last_us < SESSION_IDLE_US) {
      snprintf(msg, msg_len, "A firmware update is already running");
      return 409;
    }
    fw_update_abort(); // stale
  }
  const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
  if (part == NULL) {
    snprintf(msg, msg_len, "No OTA slot (flash the OTA partition table over USB)");
    return 500;
  }
  if (size < FW_DESC_END || size > part->size) {
    snprintf(msg, msg_len, "Firmware size must be up to %u bytes", (unsigned)part->size);
    return 400;
  }
#if CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
  if (size % SIG_SECTOR != 0) { // signed images end on a 4 KB signature sector
    snprintf(msg, msg_len, "Firmware is not signed");
    return 400;
  }
#endif
  esp_ota_handle_t h;
  esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
  if (err != ESP_OK) {
    snprintf(msg, msg_len, "esp_ota_begin: %s", esp_err_to_name(err));
    return 500;
  }
  s_ota.active = true;
  s_ota.handle = h;
  s_ota.part = part;
  s_ota.size = size;
  s_ota.written = 0;
  s_ota.last_us = esp_timer_get_time();
  ESP_LOGI(TAG, "Update started: %u bytes into %s", (unsigned)size, part->label);
  return 200;
}

int fw_update_write(size_t offset, const uint8_t *data, size_t n, size_t *written, char *msg,
                    size_t msg_len) {
  if (!s_ota.active) {
    snprintf(msg, msg_len, "No firmware update running");
    return 410;
  }
  s_ota.last_us = esp_timer_get_time();
  if (offset + n <= s_ota.written) { // retry of a chunk already written
    *written = s_ota.written;
    return 200;
  }
  if (offset != s_ota.written || offset + n > s_ota.size) {
    snprintf(msg, msg_len, "Expected offset %u", (unsigned)s_ota.written);
    return 409;
  }
  if (offset == 0) {
    esp_app_desc_t d;
    int code = fw_check_image(data, n, esp_app_get_description()->project_name,
                              esp_app_get_description()->version, &d, msg, msg_len);
    if (code != 200) {
      fw_update_abort();
      return code;
    }
    strlcpy(s_ota.version, d.version, sizeof(s_ota.version));
  }
#if CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
  // The chunk holding the signature sector must start it with the V2 block
  // magic, checked before that chunk is written. The signature itself is
  // verified by esp_ota_end() before the image can be booted.
  size_t sig = s_ota.size - SIG_SECTOR;
  if (sig >= offset && sig + 1 < offset + n && (data[sig - offset] != 0xE7 || data[sig - offset + 1] != 0x02)) {
    snprintf(msg, msg_len, "Firmware is not signed");
    fw_update_abort();
    return 400;
  }
#endif
  esp_err_t err = esp_ota_write(s_ota.handle, data, n);
  if (err != ESP_OK) {
    snprintf(msg, msg_len, "Flash write failed: %s", esp_err_to_name(err));
    fw_update_abort();
    return 500;
  }
  s_ota.written += n;
  *written = s_ota.written;
  return 200;
}

int fw_update_finish(char *version, size_t version_len, char *msg, size_t msg_len) {
  if (!s_ota.active) {
    snprintf(msg, msg_len, "No firmware update running");
    return 410;
  }
  if (s_ota.written != s_ota.size) {
    snprintf(msg, msg_len, "Incomplete: %u of %u bytes", (unsigned)s_ota.written,
             (unsigned)s_ota.size);
    fw_update_abort();
    return 400;
  }
  esp_err_t err = esp_ota_end(s_ota.handle); // checks the image (hash, signature if enabled)
  s_ota.active = false;
  if (err == ESP_OK) {
    err = esp_ota_set_boot_partition(s_ota.part);
  }
  if (err != ESP_OK) {
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
      snprintf(msg, msg_len, "Firmware image is damaged or not signed");
    } else {
      snprintf(msg, msg_len, "Update failed: %s", esp_err_to_name(err));
    }
    ESP_LOGE(TAG, "Update rejected: %s", esp_err_to_name(err));
    memset(&s_ota, 0, sizeof(s_ota));
    return 400;
  }
  strlcpy(version, s_ota.version, version_len);
  ESP_LOGW(TAG, "Update to %s written to %s; restart to boot it", s_ota.version, s_ota.part->label);
  memset(&s_ota, 0, sizeof(s_ota));
  return 200;
}

static bool boot_pending(void) {
  esp_ota_img_states_t state;
  const esp_partition_t *running = esp_ota_get_running_partition();
  return esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
}

void fw_update_confirm_boot(bool ok) {
  static bool s_done;
  if (s_done || !boot_pending()) {
    s_done = true;
    return;
  }
  s_done = ok;
  if (ok) {
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGW(TAG, "New firmware %s confirmed", esp_app_get_description()->version);
  } else {
    ESP_LOGE(TAG, "New firmware failed its start checks: rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

static void rollback_timeout(void *arg) {
  (void)arg;
  fw_update_confirm_boot(false); // no-op once confirmed
}

void fw_update_rollback_timer(int seconds) {
  if (!boot_pending()) return;
  static esp_timer_handle_t t;
  const esp_timer_create_args_t args = {.callback = rollback_timeout, .name = "fw_rollback"};
  if (esp_timer_create(&args, &t) == ESP_OK) {
    esp_timer_start_once(t, (uint64_t)seconds * 1000000ULL);
    ESP_LOGW(TAG, "New firmware %s: rolls back unless confirmed within %d s",
             esp_app_get_description()->version, seconds);
  }
}

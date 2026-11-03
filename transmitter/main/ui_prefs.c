#include "ui_prefs.h"
#include "esp_log.h"
#include "nvs.h"
#include <ctype.h>
#include <string.h>

static const char *TAG = "UI_PREFS";
#define NVS_NS "sys_cfg"
#define NVS_KEY "ui_accent"

static bool valid_hex(const char *s) {
  if (s == NULL || strlen(s) != 7 || s[0] != '#') return false;
  for (int i = 1; i < 7; i++) {
    if (!isxdigit((unsigned char)s[i])) return false;
  }
  return true;
}

void ui_accent_get(char *out, size_t len) {
  strlcpy(out, UI_ACCENT_DEFAULT, len);
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  char tmp[8];
  size_t n = sizeof(tmp);
  if (nvs_get_str(h, NVS_KEY, tmp, &n) == ESP_OK && valid_hex(tmp)) {
    strlcpy(out, tmp, len);
  }
  nvs_close(h);
}

bool ui_accent_set(const char *hex) {
  if (!valid_hex(hex)) return false;
  char v[8];
  for (int i = 0; i < 8; i++) v[i] = (char)toupper((unsigned char)hex[i]);
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = nvs_set_str(h, NVS_KEY, v) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (ok) ESP_LOGI(TAG, "Accent colour %s", v);
  return ok;
}

void ui_accent_reset(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, NVS_KEY);
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGW(TAG, "Accent colour reset to default");
}

#include "pairing.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "file_receiver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "PAIRING";

#define PAIR_NVS_NS "pairing"
#define HEARTBEAT_INTERVAL_MS 10000
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BOOT_HOLD_MS 10000

static char s_id[16];
static char s_default_name[24];
static char s_name[32];
static char s_key[PAIRING_KEY_HEX_LEN + 1]; // empty = unpaired
static uint32_t s_cfg_ver;
static char s_tx_id[16];
static bool s_provisional; // key not yet confirmed by a heartbeat reply
#define PROVISIONAL_MAX_REJECTS 3
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void nvs_save(void) {
  nvs_handle_t h;
  if (nvs_open(PAIR_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "NVS open failed");
    return;
  }
  if (s_key[0]) {
    nvs_set_str(h, "key", s_key);
    nvs_set_str(h, "name", s_name);
    nvs_set_str(h, "tx_id", s_tx_id);
    nvs_set_u8(h, "prov", s_provisional);
  } else {
    nvs_erase_key(h, "key");
    nvs_erase_key(h, "name");
    nvs_erase_key(h, "tx_id");
    nvs_erase_key(h, "prov");
  }
  nvs_set_u32(h, "cfg_ver", s_cfg_ver);
  nvs_commit(h);
  nvs_close(h);
}

void pairing_init(void) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_id, sizeof(s_id), "rx-%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(s_default_name, sizeof(s_default_name), "Receiver %02X%02X", mac[4], mac[5]);

  nvs_handle_t h;
  if (nvs_open(PAIR_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    size_t len = sizeof(s_key);
    if (nvs_get_str(h, "key", s_key, &len) != ESP_OK || strlen(s_key) != PAIRING_KEY_HEX_LEN) {
      s_key[0] = '\0';
    }
    len = sizeof(s_name);
    if (nvs_get_str(h, "name", s_name, &len) != ESP_OK) {
      s_name[0] = '\0';
    }
    nvs_get_u32(h, "cfg_ver", &s_cfg_ver);
    nvs_close(h);
  }
  ESP_LOGI(TAG, "ID %s (%s), %s, config_version %lu", s_id, s_default_name,
           s_key[0] ? (s_provisional ? "PAIRED (provisional)" : "PAIRED") : "unpaired",
           (unsigned long)s_cfg_ver);
}

const char *pairing_get_id(void) { return s_id; }

void pairing_get_name(char *out, size_t len) {
  portENTER_CRITICAL(&s_mux);
  strlcpy(out, s_name[0] ? s_name : s_default_name, len);
  portEXIT_CRITICAL(&s_mux);
}

bool pairing_is_paired(void) { return s_key[0] != '\0'; }

bool pairing_key_matches(const char *hex) {
  if (!pairing_is_paired() || hex == NULL || strlen(hex) != PAIRING_KEY_HEX_LEN) {
    return false;
  }
  uint8_t diff = 0;
  for (int i = 0; i < PAIRING_KEY_HEX_LEN; i++) {
    diff |= (uint8_t)(hex[i] ^ s_key[i]);
  }
  return diff == 0;
}

static esp_err_t set_key(const char *hex_key, const char *name, const char *tx_id, bool provisional) {
  if (pairing_is_paired()) {
    return ESP_ERR_INVALID_STATE;
  }
  if (hex_key == NULL || strlen(hex_key) != PAIRING_KEY_HEX_LEN ||
      strspn(hex_key, "0123456789abcdef") != PAIRING_KEY_HEX_LEN) {
    return ESP_ERR_INVALID_ARG;
  }
  portENTER_CRITICAL(&s_mux);
  strlcpy(s_key, hex_key, sizeof(s_key));
  strlcpy(s_name, name ? name : "", sizeof(s_name));
  strlcpy(s_tx_id, tx_id ? tx_id : "", sizeof(s_tx_id));
  s_provisional = provisional;
  portEXIT_CRITICAL(&s_mux);
  nvs_save();
  ESP_LOGI(TAG, "Paired with transmitter %s (name \"%s\")%s", s_tx_id[0] ? s_tx_id : "",
           s_name, provisional ? ", provisional until it confirms" : "");
  return ESP_OK;
}

esp_err_t pairing_set(const char *hex_key, const char *name) {
  return set_key(hex_key, name, NULL, false);
}

esp_err_t pairing_set_provisional(const char *hex_key, const char *name, const char *tx_id) {
  return set_key(hex_key, name, tx_id, true);
}

void pairing_clear(void) {
  portENTER_CRITICAL(&s_mux);
  s_key[0] = '\0';
  s_name[0] = '\0';
  portEXIT_CRITICAL(&s_mux);
  nvs_save();
  ESP_LOGW(TAG, "Pairing cleared; waiting to be approved again");
}

void pairing_apply_file(const char *json) {
  cJSON *j = cJSON_Parse(json);
  cJSON *v = cJSON_GetObjectItem(j, "v");
  cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
  cJSON *pass = cJSON_GetObjectItem(j, "pass");
  cJSON *key = cJSON_GetObjectItem(j, "key");
  cJSON *tx_id = cJSON_GetObjectItem(j, "tx_id");
  cJSON *name = cJSON_GetObjectItem(j, "name");
  cJSON *expiry = cJSON_GetObjectItem(j, "expiry");
  // No clock here: expiry is enforced by the transmitter (expired key -> rejected
  // heartbeats -> provisional key dropped)
  if (!cJSON_IsNumber(v) || v->valueint != 1 || !cJSON_IsString(ssid) || !cJSON_IsString(pass) ||
      !cJSON_IsString(key) || !cJSON_IsString(tx_id) || !cJSON_IsString(name) ||
      !cJSON_IsNumber(expiry) || expiry->valuedouble < 1700000000.0 ||
      ssid->valuestring[0] == '\0' || strlen(ssid->valuestring) > 32 ||
      strlen(pass->valuestring) > 63 || (pass->valuestring[0] && strlen(pass->valuestring) < 8) ||
      strlen(tx_id->valuestring) >= sizeof(s_tx_id) || strlen(name->valuestring) >= sizeof(s_name)) {
    ESP_LOGW(TAG, "pair.cfg invalid; deleted");
  } else if (pairing_is_paired()) {
    ESP_LOGW(TAG, "pair.cfg ignored and deleted: already paired (hold BOOT 10 s to reset first)");
  } else if (pairing_set_provisional(key->valuestring, name->valuestring, tx_id->valuestring) != ESP_OK) {
    ESP_LOGW(TAG, "pair.cfg invalid key; deleted");
  } else {
    ESP_LOGI(TAG, "pair.cfg applied (transmitter %s, SSID [%s]); deleted", tx_id->valuestring,
             ssid->valuestring);
    wifi_manager_apply_creds(ssid->valuestring, pass->valuestring);
  }
  if (cJSON_IsString(pass)) memset(pass->valuestring, 0, strlen(pass->valuestring));
  if (cJSON_IsString(key)) memset(key->valuestring, 0, strlen(key->valuestring));
  cJSON_Delete(j);
}

uint32_t pairing_config_version(void) { return s_cfg_ver; }

// ─── Transmitter HTTP helpers ───────────────────────────────────────────────
// Sends method/body to http://<gateway><path>; reads up to resp_len-1 bytes.
// Returns the HTTP status, or -1 if the transmitter could not be reached.
static int tx_request(esp_http_client_method_t method, const char *path, const char *body,
                      char *resp, size_t resp_len) {
  char gw[16];
  if (!wifi_manager_get_gateway(gw, sizeof(gw))) {
    return -1;
  }
  char url[64];
  snprintf(url, sizeof(url), "http://%s%s", gw, path);
  esp_http_client_config_t cfg = {.url = url, .timeout_ms = 3000, .method = method};
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == NULL) {
    return -1;
  }
  esp_http_client_set_header(c, "X-Rx-Id", s_id);
  if (pairing_is_paired()) {
    esp_http_client_set_header(c, "X-Auth-Key", s_key);
  }
  int len = body ? strlen(body) : 0;
  if (body) {
    esp_http_client_set_header(c, "Content-Type", "application/json");
  }
  int status = -1;
  resp[0] = '\0';
  if (esp_http_client_open(c, len) == ESP_OK && (len == 0 || esp_http_client_write(c, body, len) == len) &&
      esp_http_client_fetch_headers(c) >= 0) {
    status = esp_http_client_get_status_code(c);
    int r = esp_http_client_read(c, resp, resp_len - 1);
    resp[r > 0 ? r : 0] = '\0';
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return status;
}

// Pulls the transmitter's AP credentials after its config_version moved on.
static void pull_config(uint32_t tx_ver) {
  char resp[256];
  int status = tx_request(HTTP_METHOD_GET, "/rx/config", NULL, resp, sizeof(resp));
  cJSON *j = status == 200 ? cJSON_Parse(resp) : NULL;
  cJSON *ssid = j ? cJSON_GetObjectItem(j, "ap_ssid") : NULL;
  cJSON *pass = j ? cJSON_GetObjectItem(j, "ap_pass") : NULL;
  if (cJSON_IsString(ssid) && cJSON_IsString(pass) && ssid->valuestring[0] &&
      strlen(ssid->valuestring) < 32 && strlen(pass->valuestring) < 64) {
    s_cfg_ver = tx_ver;
    nvs_save();
    ESP_LOGI(TAG, "Config v%lu from transmitter: AP SSID %s", (unsigned long)tx_ver, ssid->valuestring);
    wifi_manager_apply_creds(ssid->valuestring, pass->valuestring);
  } else {
    ESP_LOGW(TAG, "Config pull failed (HTTP %d)", status);
  }
  cJSON_Delete(j);
}

static void heartbeat_task(void *arg) {
  (void)arg;
  char resp[160];
  int rejects = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    char ip[16];
    if (!wifi_manager_get_ip(ip, sizeof(ip))) {
      continue;
    }
    cJSON *o = cJSON_CreateObject();
    char name[32];
    pairing_get_name(name, sizeof(name));
    cJSON_AddStringToObject(o, "id", s_id);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddNumberToObject(o, "config_version", s_cfg_ver);
    if (pairing_is_paired()) {
      cJSON_AddStringToObject(o, "key", s_key);
    }
    file_receiver_add_status_fields(o); // fw, state, storage, card_error, busy
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (body == NULL) {
      continue;
    }
    int status = tx_request(HTTP_METHOD_POST, "/rx/heartbeat", body, resp, sizeof(resp));
    cJSON_free(body);
    if (status != 200) {
      ESP_LOGD(TAG, "Heartbeat failed (HTTP %d)", status);
      continue;
    }
    cJSON *r = cJSON_Parse(resp);
    cJSON *paired = r ? cJSON_GetObjectItem(r, "paired") : NULL;
    cJSON *ver = r ? cJSON_GetObjectItem(r, "config_version") : NULL;
    if (pairing_is_paired() && s_provisional && r != NULL) {
      if (cJSON_IsTrue(paired)) {
        s_provisional = false;
        rejects = 0;
        nvs_save();
        ESP_LOGI(TAG, "Pairing confirmed by transmitter");
      } else if (++rejects >= PROVISIONAL_MAX_REJECTS) {
        // Expired pair.cfg or an approval that never completed: unpaired again
        // so the transmitter can approve it normally
        rejects = 0;
        ESP_LOGW(TAG, "Transmitter rejected the provisional key %d times", PROVISIONAL_MAX_REJECTS);
        pairing_clear();
      }
    }
    if (pairing_is_paired() && cJSON_IsTrue(paired) && cJSON_IsNumber(ver) &&
        (uint32_t)ver->valuedouble > s_cfg_ver) {
      pull_config((uint32_t)ver->valuedouble);
    }
    cJSON_Delete(r);
  }
}

// Hold BOOT (GPIO0) for 10 s: clear the pairing so the transmitter can approve again.
static void boot_button_task(void *arg) {
  (void)arg;
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&io);
  int held_ms = 0;
  bool fired = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
      held_ms += 100;
      if (!fired && held_ms >= BOOT_HOLD_MS) {
        fired = true;
        ESP_LOGW(TAG, "BOOT held %d s: clearing pairing", BOOT_HOLD_MS / 1000);
        pairing_clear();
      }
    } else {
      held_ms = 0;
      fired = false;
    }
  }
}

void pairing_start_tasks(void) {
  xTaskCreate(heartbeat_task, "rx_heartbeat", 4096, NULL, 3, NULL);
  xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 2, NULL);
}

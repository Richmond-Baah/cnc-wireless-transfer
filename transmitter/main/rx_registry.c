#include "rx_registry.h"
#include "espnow_pair.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "wifi_manager.h"
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "RX_REG";

#define RX_NVS_NS "rx_reg"
#define PENDING_EXPIRE_US (60LL * 1000 * 1000)
#define RX_MAX_PREREG 6
#define ESPNOW_PREREG_US (10LL * 60 * 1000 * 1000) // radio-approved key valid 10 min
#define PAIR_FILE_VALID_S (24 * 3600)
#define EPOCH_VALID 1700000000LL // clock below this = never set
#define EPOCH_SAVE_US (30LL * 60 * 1000 * 1000)

typedef struct {
  // persisted
  char id[RX_ID_LEN];
  char name[RX_NAME_LEN];
  char key[RX_KEY_HEX_LEN + 1];
  // runtime (heartbeat / live status)
  char ip[16];
  char fw[32];
  char state[16];
  char storage_type[16];
  double total_bytes;
  double free_bytes;
  bool card_error;
  bool busy;
  uint32_t cfg_fetched;
  int64_t last_seen_us; // 0 = never since boot
} rx_entry_t;

typedef struct {
  char id[RX_ID_LEN];
  char name[RX_NAME_LEN]; // receiver's own default name, a suggestion
  char ip[16];
  char fw[32];
  int64_t last_seen_us;
  bool espnow; // last seen via ESP-NOW PAIR_REQ (no IP): approve sends PAIR_CFG
  uint8_t mac[6];
  uint8_t pub[32];
} rx_pending_t;

// Key handed out before the receiver has used it; its first heartbeat with the
// key pairs it under this name.
typedef struct {
  char id[RX_ID_LEN];          // "" = any receiver (pairing file)
  char name[RX_NAME_LEN];
  char key[RX_KEY_HEX_LEN + 1];
  int64_t expires_us;          // uptime deadline (ESP-NOW), 0 = none
  int64_t expires_epoch;       // wall-clock deadline (pairing file), 0 = none
} rx_prereg_t;

// What NVS stores per paired receiver
typedef struct {
  char id[RX_ID_LEN];
  char name[RX_NAME_LEN];
  char key[RX_KEY_HEX_LEN + 1];
} rx_saved_t;

static rx_entry_t s_rx[RX_MAX_PAIRED];
static int s_rx_count;
static rx_pending_t s_pending[RX_MAX_PENDING];
static rx_prereg_t s_prereg[RX_MAX_PREREG];
static uint32_t s_cfg_ver;
static SemaphoreHandle_t s_mutex;
static char s_tx_id[16];
static int64_t s_epoch_saved_us; // last NVS save of the wall clock

static void lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }

static bool is_online(int64_t last_seen_us) {
  return last_seen_us != 0 && esp_timer_get_time() - last_seen_us < RX_ONLINE_US;
}

static int find_paired(const char *id) {
  for (int i = 0; i < s_rx_count; i++) {
    if (strcmp(s_rx[i].id, id) == 0) return i;
  }
  return -1;
}

static int find_pending(const char *id) {
  for (int i = 0; i < RX_MAX_PENDING; i++) {
    if (s_pending[i].id[0] && strcmp(s_pending[i].id, id) == 0) return i;
  }
  return -1;
}

static bool key_equal(const char *a, const char *b) {
  if (a == NULL || b == NULL || strlen(a) != RX_KEY_HEX_LEN || strlen(b) != RX_KEY_HEX_LEN) {
    return false;
  }
  uint8_t diff = 0;
  for (int i = 0; i < RX_KEY_HEX_LEN; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

// Called with the lock held
static void save_locked(void) {
  static rx_saved_t saved[RX_MAX_PAIRED];
  for (int i = 0; i < s_rx_count; i++) {
    strlcpy(saved[i].id, s_rx[i].id, sizeof(saved[i].id));
    strlcpy(saved[i].name, s_rx[i].name, sizeof(saved[i].name));
    strlcpy(saved[i].key, s_rx[i].key, sizeof(saved[i].key));
  }
  nvs_handle_t h;
  if (nvs_open(RX_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "NVS open failed; pairing list not saved");
    return;
  }
  if (s_rx_count > 0) {
    nvs_set_blob(h, "paired", saved, sizeof(rx_saved_t) * s_rx_count);
  } else {
    nvs_erase_key(h, "paired");
  }
  nvs_set_u32(h, "cfg_ver", s_cfg_ver);
  // Pairing-file keys (24 h) survive a restart; radio approvals are RAM-only
  static rx_prereg_t files[RX_MAX_PREREG];
  int n = 0;
  for (int k = 0; k < RX_MAX_PREREG; k++) {
    if (s_prereg[k].key[0] && s_prereg[k].expires_epoch) files[n++] = s_prereg[k];
  }
  if (n > 0) {
    nvs_set_blob(h, "prereg", files, sizeof(rx_prereg_t) * n);
  } else {
    nvs_erase_key(h, "prereg");
  }
  // Lower bound for the clock after a power loss (pair-file expiry)
  time_t now = time(NULL);
  if (now > EPOCH_VALID) {
    nvs_set_i64(h, "epoch", (int64_t)now);
    s_epoch_saved_us = esp_timer_get_time();
  }
  memset(files, 0, sizeof(files));
  nvs_commit(h);
  nvs_close(h);
}

const char *rx_registry_tx_id(void) { return s_tx_id; }

static bool prereg_expired(const rx_prereg_t *p) {
  return (p->expires_us && esp_timer_get_time() > p->expires_us) ||
         (p->expires_epoch && (int64_t)time(NULL) > p->expires_epoch);
}

// Called with the lock held. Index of the live pre-registration matching key
// (and id, if it names one), else -1. Drops expired ones on the way.
static int find_prereg_locked(const char *id, const char *key) {
  int found = -1;
  for (int k = 0; k < RX_MAX_PREREG; k++) {
    rx_prereg_t *p = &s_prereg[k];
    if (p->key[0] == '\0') continue;
    if (prereg_expired(p)) {
      ESP_LOGI(TAG, "Pre-registered key for \"%s\" expired", p->name);
      memset(p, 0, sizeof(*p));
      continue;
    }
    if (found < 0 && key_equal(key, p->key) && (p->id[0] == '\0' || strcmp(p->id, id) == 0)) {
      found = k;
    }
  }
  return found;
}

// Called with the lock held; replaces the same id, else a free/expired/first slot.
static void prereg_add_locked(const char *id, const char *name, const char *key,
                              int64_t expires_us, int64_t expires_epoch) {
  int slot = -1;
  for (int k = 0; k < RX_MAX_PREREG && slot < 0; k++) {
    if (id[0] && strcmp(s_prereg[k].id, id) == 0) slot = k;
  }
  for (int k = 0; k < RX_MAX_PREREG && slot < 0; k++) {
    if (s_prereg[k].key[0] == '\0' || prereg_expired(&s_prereg[k])) slot = k;
  }
  if (slot < 0) slot = 0;
  rx_prereg_t *p = &s_prereg[slot];
  memset(p, 0, sizeof(*p));
  strlcpy(p->id, id, sizeof(p->id));
  strlcpy(p->name, name, sizeof(p->name));
  strlcpy(p->key, key, sizeof(p->key));
  p->expires_us = expires_us;
  p->expires_epoch = expires_epoch;
}

void rx_registry_init(void) {
  s_mutex = xSemaphoreCreateMutex();
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(s_tx_id, sizeof(s_tx_id), "tx-%02x%02x%02x", mac[3], mac[4], mac[5]);
  nvs_handle_t h;
  if (nvs_open(RX_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    static rx_saved_t saved[RX_MAX_PAIRED];
    size_t len = sizeof(saved);
    if (nvs_get_blob(h, "paired", saved, &len) == ESP_OK && len % sizeof(rx_saved_t) == 0) {
      s_rx_count = len / sizeof(rx_saved_t);
      for (int i = 0; i < s_rx_count; i++) {
        memset(&s_rx[i], 0, sizeof(s_rx[i]));
        strlcpy(s_rx[i].id, saved[i].id, sizeof(s_rx[i].id));
        strlcpy(s_rx[i].name, saved[i].name, sizeof(s_rx[i].name));
        strlcpy(s_rx[i].key, saved[i].key, sizeof(s_rx[i].key));
      }
    }
    nvs_get_u32(h, "cfg_ver", &s_cfg_ver);
    len = sizeof(s_prereg);
    if (nvs_get_blob(h, "prereg", s_prereg, &len) != ESP_OK || len % sizeof(rx_prereg_t) != 0) {
      memset(s_prereg, 0, sizeof(s_prereg));
    }
    int64_t epoch = 0;
    if (time(NULL) < EPOCH_VALID && nvs_get_i64(h, "epoch", &epoch) == ESP_OK && epoch > EPOCH_VALID) {
      // Power loss cleared the clock: resume from the last saved time (never later than real time)
      struct timeval tv = {.tv_sec = (time_t)epoch};
      settimeofday(&tv, NULL);
    }
    nvs_close(h);
  }
  ESP_LOGI(TAG, "%d paired receiver(s), config_version %lu", s_rx_count, (unsigned long)s_cfg_ver);
}

// ─── HTTP to a receiver ─────────────────────────────────────────────────────
int rx_http(const rx_target_t *t, esp_http_client_method_t method, const char *path,
            const char *body, bool force, int timeout_ms, char *resp, size_t resp_len,
            bool *truncated) {
  char url[64];
  snprintf(url, sizeof(url), "http://%s%s", t->ip, path);
  resp[0] = '\0';
  if (truncated) *truncated = false;

  esp_http_client_config_t cfg = {.url = url, .timeout_ms = timeout_ms, .method = method};
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == NULL) {
    snprintf(resp, resp_len, "HTTP client init failed");
    return -1;
  }
  if (t->key[0]) esp_http_client_set_header(c, "X-Auth-Key", t->key);
  if (force) esp_http_client_set_header(c, "X-Force", "1");
  int len = body ? strlen(body) : 0;
  if (body) esp_http_client_set_header(c, "Content-Type", "application/json");

  int status = -1;
  esp_err_t err = esp_http_client_open(c, len);
  if (err != ESP_OK) {
    snprintf(resp, resp_len, "Receiver unreachable: %s", esp_err_to_name(err));
  } else if (len > 0 && esp_http_client_write(c, body, len) != len) {
    snprintf(resp, resp_len, "Receiver unreachable: write failed");
  } else if (esp_http_client_fetch_headers(c) < 0) {
    snprintf(resp, resp_len, "No response from receiver (timeout)");
  } else {
    status = esp_http_client_get_status_code(c);
    size_t total = 0;
    while (total < resp_len - 1) {
      int r = esp_http_client_read(c, resp + total, resp_len - 1 - total);
      if (r <= 0) break;
      total += r;
    }
    resp[total] = '\0';
    if (truncated && total == resp_len - 1 && !esp_http_client_is_complete_data_received(c)) {
      *truncated = true;
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  if (status < 0) {
    ESP_LOGW(TAG, "%s %s%s: %s", t->id, t->ip, path, resp);
  }
  return status;
}

// ─── Heartbeats ─────────────────────────────────────────────────────────────
static const char *json_str(const cJSON *o, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double json_num(const cJSON *o, const char *k, double dflt) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

// Called with the lock held
static void apply_status_locked(rx_entry_t *e, const cJSON *o) {
  const char *s;
  if ((s = json_str(o, "fw"))) strlcpy(e->fw, s, sizeof(e->fw));
  if ((s = json_str(o, "state"))) strlcpy(e->state, s, sizeof(e->state));
  if ((s = json_str(o, "storage_type"))) strlcpy(e->storage_type, s, sizeof(e->storage_type));
  e->total_bytes = json_num(o, "total_bytes", e->total_bytes);
  e->free_bytes = json_num(o, "free_bytes", e->free_bytes);
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "card_error");
  if (cJSON_IsBool(v)) e->card_error = cJSON_IsTrue(v);
  v = cJSON_GetObjectItemCaseSensitive(o, "busy");
  if (cJSON_IsBool(v)) e->busy = cJSON_IsTrue(v);
}

static bool id_is_valid(const char *id) {
  return id && strlen(id) > 3 && strlen(id) < RX_ID_LEN && strncmp(id, "rx-", 3) == 0 &&
         strspn(id + 3, "0123456789abcdef") == strlen(id + 3);
}

bool rx_registry_heartbeat(const cJSON *hb) {
  const char *id = json_str(hb, "id");
  const char *ip = json_str(hb, "ip");
  if (!id_is_valid(id) || ip == NULL || strlen(ip) >= 16) {
    return false;
  }
  const char *key = json_str(hb, "key");
  int64_t now = esp_timer_get_time();
  bool paired = false;
  int pr;

  lock();
  int i = find_paired(id);
  if (i >= 0 && key_equal(key, s_rx[i].key)) {
    rx_entry_t *e = &s_rx[i];
    if (e->last_seen_us == 0 || !is_online(e->last_seen_us)) {
      ESP_LOGI(TAG, "%s (%s) online at %s", e->id, e->name, ip);
    }
    strlcpy(e->ip, ip, sizeof(e->ip));
    apply_status_locked(e, hb);
    e->last_seen_us = now;
    paired = true;
  } else if ((pr = find_prereg_locked(id, key)) >= 0 && (i >= 0 || s_rx_count < RX_MAX_PAIRED)) {
    // First heartbeat with a key handed out by ESP-NOW approval or pairing file
    if (i < 0) {
      i = s_rx_count++;
      memset(&s_rx[i], 0, sizeof(s_rx[i]));
      strlcpy(s_rx[i].id, id, sizeof(s_rx[i].id));
    }
    rx_entry_t *e = &s_rx[i];
    strlcpy(e->name, s_prereg[pr].name, sizeof(e->name));
    strlcpy(e->key, s_prereg[pr].key, sizeof(e->key));
    strlcpy(e->ip, ip, sizeof(e->ip));
    apply_status_locked(e, hb);
    e->last_seen_us = now;
    e->cfg_fetched = s_cfg_ver; // it just joined with the current AP credentials
    memset(&s_prereg[pr], 0, sizeof(s_prereg[pr]));
    save_locked(); // also drops a used pairing-file key from NVS
    int p = find_pending(id);
    if (p >= 0) memset(&s_pending[p], 0, sizeof(s_pending[p]));
    ESP_LOGI(TAG, "Paired %s as \"%s\" (pre-registered key) at %s", id, e->name, ip);
    paired = true;
  } else {
    // Unknown, or paired here but the receiver lost/has another key (BOOT reset):
    // list it as found so the user can approve it (again).
    int p = find_pending(id);
    if (p < 0) {
      int oldest = 0;
      for (int k = 0; k < RX_MAX_PENDING; k++) {
        if (s_pending[k].id[0] == '\0') { oldest = k; break; }
        if (s_pending[k].last_seen_us < s_pending[oldest].last_seen_us) oldest = k;
      }
      p = oldest;
      memset(&s_pending[p], 0, sizeof(s_pending[p]));
      strlcpy(s_pending[p].id, id, sizeof(s_pending[p].id));
      ESP_LOGI(TAG, "New device found: %s at %s%s", id, ip, i >= 0 ? " (re-pair needed)" : "");
    }
    const char *name = json_str(hb, "name");
    const char *fw = json_str(hb, "fw");
    strlcpy(s_pending[p].name, name ? name : id, sizeof(s_pending[p].name));
    strlcpy(s_pending[p].ip, ip, sizeof(s_pending[p].ip));
    strlcpy(s_pending[p].fw, fw ? fw : "", sizeof(s_pending[p].fw));
    s_pending[p].last_seen_us = now;
    s_pending[p].espnow = false;
  }
  // Keep the saved clock fresh while a pairing file is outstanding
  if (now - s_epoch_saved_us > EPOCH_SAVE_US) {
    for (int k = 0; k < RX_MAX_PREREG; k++) {
      if (s_prereg[k].key[0] && s_prereg[k].expires_epoch) {
        save_locked();
        break;
      }
    }
  }
  unlock();
  return paired;
}

void rx_registry_espnow_found(const char *id, const char *fw, const uint8_t mac[6],
                              const uint8_t pub[32]) {
  if (!id_is_valid(id)) return;
  lock();
  int p = find_pending(id);
  if (p < 0) {
    int oldest = 0;
    for (int k = 0; k < RX_MAX_PENDING; k++) {
      if (s_pending[k].id[0] == '\0') { oldest = k; break; }
      if (s_pending[k].last_seen_us < s_pending[oldest].last_seen_us) oldest = k;
    }
    p = oldest;
    memset(&s_pending[p], 0, sizeof(s_pending[p]));
    strlcpy(s_pending[p].id, id, sizeof(s_pending[p].id));
    snprintf(s_pending[p].name, sizeof(s_pending[p].name), "Receiver %s", id + 3);
    ESP_LOGI(TAG, "New device found by radio: %s (" MACSTR ")%s", id, MAC2STR(mac),
             find_paired(id) >= 0 ? " (re-pair needed)" : "");
  }
  s_pending[p].ip[0] = '\0';
  strlcpy(s_pending[p].fw, fw, sizeof(s_pending[p].fw));
  memcpy(s_pending[p].mac, mac, 6);
  memcpy(s_pending[p].pub, pub, 32);
  s_pending[p].espnow = true;
  s_pending[p].last_seen_us = esp_timer_get_time();
  unlock();
}

bool rx_registry_target(const char *id, rx_target_t *out, bool *known) {
  bool ok = false;
  if (known) *known = false;
  if (id == NULL) return false;
  lock();
  int i = find_paired(id);
  if (i >= 0) {
    if (known) *known = true;
    if (is_online(s_rx[i].last_seen_us)) {
      strlcpy(out->id, s_rx[i].id, sizeof(out->id));
      strlcpy(out->name, s_rx[i].name, sizeof(out->name));
      strlcpy(out->ip, s_rx[i].ip, sizeof(out->ip));
      strlcpy(out->key, s_rx[i].key, sizeof(out->key));
      ok = true;
    }
  }
  unlock();
  return ok;
}

bool rx_registry_check_key(const char *id, const char *key) {
  lock();
  int i = id ? find_paired(id) : -1;
  bool ok = i >= 0 && key_equal(key, s_rx[i].key);
  unlock();
  return ok;
}

void rx_registry_update_status(const char *id, const cJSON *status) {
  lock();
  int i = find_paired(id);
  if (i >= 0) apply_status_locked(&s_rx[i], status);
  unlock();
}

// Called with the lock held
static void add_entry_fields_locked(const rx_entry_t *e, cJSON *o) {
  bool online = is_online(e->last_seen_us);
  cJSON_AddBoolToObject(o, "online", online);
  cJSON_AddStringToObject(o, "state", online ? (e->state[0] ? e->state : "UNKNOWN") : "OFFLINE");
  cJSON_AddStringToObject(o, "fw", e->fw);
  cJSON_AddStringToObject(o, "ip", e->ip);
  cJSON_AddStringToObject(o, "storage_type", e->storage_type);
  cJSON_AddNumberToObject(o, "total_bytes", e->total_bytes);
  cJSON_AddNumberToObject(o, "free_bytes", e->free_bytes);
  cJSON_AddBoolToObject(o, "card_error", e->card_error);
  cJSON_AddBoolToObject(o, "busy", e->busy);
  cJSON_AddNumberToObject(o, "last_seen_s",
                          e->last_seen_us ? (double)((esp_timer_get_time() - e->last_seen_us) / 1000000) : -1);
}

bool rx_registry_add_cached_status(const char *id, cJSON *obj) {
  lock();
  int i = id ? find_paired(id) : -1;
  if (i >= 0) {
    cJSON_AddStringToObject(obj, "rx_id", s_rx[i].id);
    cJSON_AddStringToObject(obj, "rx_name", s_rx[i].name);
    add_entry_fields_locked(&s_rx[i], obj);
  }
  unlock();
  return i >= 0;
}

char *rx_registry_list_json(void) {
  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "receivers");
  cJSON *pend = cJSON_AddArrayToObject(root, "pending");
  int64_t now = esp_timer_get_time();
  lock();
  for (int i = 0; i < s_rx_count; i++) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", s_rx[i].id);
    cJSON_AddStringToObject(o, "name", s_rx[i].name);
    add_entry_fields_locked(&s_rx[i], o);
    cJSON_AddItemToArray(arr, o);
  }
  for (int k = 0; k < RX_MAX_PENDING; k++) {
    if (s_pending[k].id[0] == '\0' || now - s_pending[k].last_seen_us > PENDING_EXPIRE_US) continue;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", s_pending[k].id);
    cJSON_AddStringToObject(o, "name", s_pending[k].name);
    cJSON_AddStringToObject(o, "ip", s_pending[k].ip);
    cJSON_AddStringToObject(o, "fw", s_pending[k].fw);
    cJSON_AddBoolToObject(o, "radio", s_pending[k].espnow);
    cJSON_AddBoolToObject(o, "repair", find_paired(s_pending[k].id) >= 0);
    cJSON_AddItemToArray(pend, o);
  }
  unlock();
  cJSON_AddNumberToObject(root, "add_window_s", espnow_pair_seconds_left());
  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return out;
}

// ─── Pairing management ─────────────────────────────────────────────────────
static bool name_is_valid(const char *name) {
  if (name == NULL || name[0] == '\0' || strlen(name) >= RX_NAME_LEN) return false;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    if (*p < 0x20 || *p == 0x7f) return false;
  }
  return true;
}

int rx_registry_approve(const char *id, const char *name, char *msg, size_t msg_len) {
  msg[0] = '\0';
  if (!name_is_valid(name)) {
    snprintf(msg, msg_len, "Name must be 1-%d printable characters", RX_NAME_LEN - 1);
    return 400;
  }
  rx_target_t t = {0};
  lock();
  int p = id ? find_pending(id) : -1;
  int i = id ? find_paired(id) : -1;
  if (p < 0) {
    unlock();
    snprintf(msg, msg_len, "Device not found (it must be powered on and connected)");
    return 404;
  }
  if (i < 0 && s_rx_count >= RX_MAX_PAIRED) {
    unlock();
    snprintf(msg, msg_len, "Already %d receivers paired; unpair one first", RX_MAX_PAIRED);
    return 409;
  }
  strlcpy(t.id, s_pending[p].id, sizeof(t.id));
  strlcpy(t.ip, s_pending[p].ip, sizeof(t.ip));
  bool via_espnow = s_pending[p].espnow;
  uint8_t mac[6], pub[32];
  memcpy(mac, s_pending[p].mac, sizeof(mac));
  memcpy(pub, s_pending[p].pub, sizeof(pub));
  unlock();

  uint8_t raw[32];
  esp_fill_random(raw, sizeof(raw));
  char key[RX_KEY_HEX_LEN + 1];
  for (int k = 0; k < 32; k++) snprintf(key + 2 * k, 3, "%02x", raw[k]);

  if (via_espnow) {
    // Not on our Wi-Fi yet: pre-register the key and send Wi-Fi settings + key
    // by radio; its first heartbeat with the key completes the pairing.
    lock();
    prereg_add_locked(t.id, name, key, esp_timer_get_time() + ESPNOW_PREREG_US, 0);
    unlock();
    int code = espnow_pair_send_cfg(t.id, mac, pub, key, name, msg, msg_len);
    lock();
    if (code != 200) {
      int pr = find_prereg_locked(t.id, key);
      if (pr >= 0) memset(&s_prereg[pr], 0, sizeof(s_prereg[pr]));
    } else {
      p = find_pending(t.id);
      if (p >= 0) memset(&s_pending[p], 0, sizeof(s_pending[p]));
      snprintf(msg, msg_len, "Wi-Fi settings sent; it shows as paired once it connects");
    }
    unlock();
    memset(key, 0, sizeof(key));
    if (code == 200) ESP_LOGI(TAG, "Approved %s as \"%s\" via ESP-NOW", t.id, name);
    return code;
  }

  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "key", key);
  cJSON_AddStringToObject(body, "name", name);
  char *json = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  char resp[128];
  int status = json ? rx_http(&t, HTTP_METHOD_POST, "/pair", json, false, 5000, resp, sizeof(resp), NULL) : -1;
  cJSON_free(json);
  if (status == 409) {
    snprintf(msg, msg_len, "Receiver is paired to another key: hold its BOOT button 10 s, then approve again");
    return 409;
  }
  if (status != 200) {
    snprintf(msg, msg_len, "Receiver did not accept pairing (%s)", status < 0 ? resp : "HTTP error");
    return 502;
  }

  lock();
  i = find_paired(id);
  if (i < 0 && s_rx_count < RX_MAX_PAIRED) {
    i = s_rx_count++;
    memset(&s_rx[i], 0, sizeof(s_rx[i]));
    strlcpy(s_rx[i].id, id, sizeof(s_rx[i].id));
  }
  if (i >= 0) {
    strlcpy(s_rx[i].name, name, sizeof(s_rx[i].name));
    strlcpy(s_rx[i].key, key, sizeof(s_rx[i].key));
    strlcpy(s_rx[i].ip, t.ip, sizeof(s_rx[i].ip));
    s_rx[i].last_seen_us = esp_timer_get_time();
    // It may already run on the current AP credentials: count them as fetched
    s_rx[i].cfg_fetched = s_cfg_ver;
    save_locked();
  }
  p = find_pending(id);
  if (p >= 0) memset(&s_pending[p], 0, sizeof(s_pending[p]));
  unlock();
  memset(key, 0, sizeof(key));
  ESP_LOGI(TAG, "Paired %s as \"%s\"", id, name);
  return i >= 0 ? 200 : 409;
}

int rx_registry_make_pair_file(const char *name, int64_t client_now, char **out, char *msg,
                               size_t msg_len) {
  msg[0] = '\0';
  *out = NULL;
  if (!name_is_valid(name)) {
    snprintf(msg, msg_len, "Name must be 1-%d printable characters", RX_NAME_LEN - 1);
    return 400;
  }
  int64_t now = (int64_t)time(NULL);
  if (client_now > EPOCH_VALID && (now < EPOCH_VALID || llabs(now - client_now) > 300)) {
    struct timeval tv = {.tv_sec = (time_t)client_now};
    settimeofday(&tv, NULL);
    now = client_now;
    ESP_LOGI(TAG, "Clock set from browser");
  }
  if (now < EPOCH_VALID) {
    snprintf(msg, msg_len, "Transmitter clock unknown; reload the page and try again");
    return 400;
  }

  uint8_t raw[32];
  esp_fill_random(raw, sizeof(raw));
  char key[RX_KEY_HEX_LEN + 1];
  for (int k = 0; k < 32; k++) snprintf(key + 2 * k, 3, "%02x", raw[k]);
  memset(raw, 0, sizeof(raw));
  int64_t expiry = now + PAIR_FILE_VALID_S;

  char ssid[33], pass[65];
  wifi_get_ap_creds(ssid, sizeof(ssid), pass, sizeof(pass));
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(o, "v", 1);
  cJSON_AddStringToObject(o, "ssid", ssid);
  cJSON_AddStringToObject(o, "pass", pass);
  cJSON_AddStringToObject(o, "key", key);
  cJSON_AddStringToObject(o, "tx_id", s_tx_id);
  cJSON_AddStringToObject(o, "name", name);
  cJSON_AddNumberToObject(o, "expiry", (double)expiry);
  *out = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  memset(pass, 0, sizeof(pass));
  if (*out == NULL) {
    memset(key, 0, sizeof(key));
    snprintf(msg, msg_len, "Out of memory");
    return 500;
  }

  lock();
  prereg_add_locked("", name, key, 0, expiry);
  save_locked();
  unlock();
  memset(key, 0, sizeof(key));
  ESP_LOGI(TAG, "Pairing file for \"%s\" issued (valid 24 h)", name);
  return 200;
}

int rx_registry_rename(const char *id, const char *name, char *msg, size_t msg_len) {
  msg[0] = '\0';
  if (!name_is_valid(name)) {
    snprintf(msg, msg_len, "Name must be 1-%d printable characters", RX_NAME_LEN - 1);
    return 400;
  }
  lock();
  int i = id ? find_paired(id) : -1;
  if (i >= 0) {
    strlcpy(s_rx[i].name, name, sizeof(s_rx[i].name));
    save_locked();
  }
  unlock();
  if (i < 0) {
    snprintf(msg, msg_len, "Unknown receiver");
    return 404;
  }
  return 200;
}

int rx_registry_unpair(const char *id, char *msg, size_t msg_len) {
  msg[0] = '\0';
  rx_target_t t;
  bool known = false;
  bool online = rx_registry_target(id, &t, &known);
  if (!known) {
    snprintf(msg, msg_len, "Unknown receiver");
    return 404;
  }
  if (online) {
    // Best effort: tell the receiver to forget its key so it shows up as new again
    char resp[64];
    rx_http(&t, HTTP_METHOD_POST, "/unpair", "{}", false, 3000, resp, sizeof(resp), NULL);
  }
  lock();
  int i = find_paired(id);
  if (i >= 0) {
    memmove(&s_rx[i], &s_rx[i + 1], sizeof(rx_entry_t) * (s_rx_count - i - 1));
    s_rx_count--;
    save_locked();
  }
  unlock();
  ESP_LOGI(TAG, "Unpaired %s%s", id, online ? "" : " (offline: hold its BOOT 10 s to reset it)");
  if (!online) {
    snprintf(msg, msg_len, "Hold its BOOT button 10 s to finish");
  }
  return 200;
}

// ─── AP credential version ──────────────────────────────────────────────────
uint32_t rx_registry_config_version(void) { return s_cfg_ver; }

uint32_t rx_registry_bump_config_version(void) {
  lock();
  uint32_t v = ++s_cfg_ver;
  save_locked();
  unlock();
  return v;
}

void rx_registry_mark_config_fetched(const char *id, uint32_t ver) {
  lock();
  int i = find_paired(id);
  if (i >= 0) s_rx[i].cfg_fetched = ver;
  unlock();
}

bool rx_registry_all_fetched(uint32_t ver) {
  bool all = true;
  lock();
  for (int i = 0; i < s_rx_count; i++) {
    if (is_online(s_rx[i].last_seen_us) && s_rx[i].cfg_fetched < ver) all = false;
  }
  unlock();
  return all;
}

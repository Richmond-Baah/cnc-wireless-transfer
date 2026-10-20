#include "file_transfer.h"
#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led.h"
#include "cJSON.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static const char *TAG = "TRANSFER";

static SemaphoreHandle_t s_up_mutex = NULL; // upload session (s_up), see below

// Resolves a receiver id to a target. Returns 0 if paired and online, else
// the HTTP status for the browser (404 unknown, 502 offline) with msg set.
static int resolve(const char *rx_id, rx_target_t *t, char *msg, size_t msg_len) {
  bool known = false;
  if (rx_registry_target(rx_id, t, &known)) {
    return 0;
  }
  snprintf(msg, msg_len, known ? "Receiver offline" : "Unknown receiver");
  return known ? 502 : 404;
}

// ─── Browser-driven upload session ──────────────────────────────────────────
// The browser slices the file and calls begin, chunk x N, finish. One session
// at a time across all receivers (the transmitter forwards one stream).
#define SESSION_IDLE_TIMEOUT_US (30LL * 1000 * 1000) // idle sessions are expired by the reaper task

// httpd handlers and the reaper both touch s_up
static void up_lock(void) { if (s_up_mutex) xSemaphoreTake(s_up_mutex, portMAX_DELAY); }
static void up_unlock(void) { if (s_up_mutex) xSemaphoreGive(s_up_mutex); }

static struct {
  bool active;
  char id[17];
  rx_target_t rx;          // receiver this session writes to
  char name[128];
  char md5[33];
  size_t size;
  int next_index;          // first chunk index not yet acked by the receiver
  uint64_t acked;          // bytes acked by the receiver
  int64_t t_start_us;
  int64_t prepare_us;
  int64_t last_activity_us;
  esp_http_client_handle_t chunk_client; // reused for all chunks (HTTP keep-alive)
  int connects;            // TCP connects made by chunk_client (1 = full reuse)
  uint32_t chunks;
  int64_t chunk_total_us;
  int64_t chunk_max_us;
} s_up;

static esp_err_t chunk_client_event(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_CONNECTED && evt->user_data) {
    (*(int *)evt->user_data)++;
  }
  return ESP_OK;
}

static int cancel_on_receiver(const rx_target_t *t) {
  char resp[64];
  return rx_http(t, HTTP_METHOD_POST, "/cancel", "{}", false, 2000, resp, sizeof(resp), NULL);
}

static void session_end(void) {
  if (s_up.chunk_client) {
    esp_http_client_cleanup(s_up.chunk_client);
  }
  memset(&s_up, 0, sizeof(s_up));
}

static bool session_matches(const char *session) {
  return s_up.active && session && strcmp(session, s_up.id) == 0;
}

static int begin_locked(const char *rx_id, const char *name, size_t size, const char *md5, bool force,
                        char *session_out, size_t session_len,
                        char *msg, size_t msg_len) {
  int64_t now = esp_timer_get_time();
  msg[0] = '\0';
  rx_target_t rx;
  int code = resolve(rx_id, &rx, msg, msg_len);
  if (code) {
    return code;
  }
  if (s_up.active) {
    if (strcmp(s_up.rx.id, rx.id) != 0) {
      snprintf(msg, msg_len, "Another upload is in progress (to %s)", s_up.rx.name);
      return 409;
    }
    // Same receiver: a new begin closes the old session and its receiver socket
    ESP_LOGW(TAG, "Replacing upload session %s (%s)", s_up.id, s_up.name);
    cancel_on_receiver(&s_up.rx);
    session_end();
  }

  // Build the /prepare body with cJSON so names with quotes stay valid JSON
  cJSON *obj = cJSON_CreateObject();
  cJSON_AddStringToObject(obj, "filename", name);
  cJSON_AddNumberToObject(obj, "filesize", (double)size);
  cJSON_AddStringToObject(obj, "md5", md5);
  char *json = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  if (json == NULL) {
    snprintf(msg, msg_len, "Out of memory");
    return 500;
  }

  ESP_LOGI(TAG, "Upload begin -> %s: %s (%u bytes, md5=%s%s)", rx.id, name, (unsigned)size, md5,
           force ? ", forced" : "");
  int status = rx_http(&rx, HTTP_METHOD_POST, "/prepare", json, force, 10000, msg, msg_len, NULL);
  if (status == 503) {
    // Receiver is stuck RECEIVING from an abandoned transfer we no longer
    // track (e.g. transmitter rebooted mid-upload): reset it once and retry.
    ESP_LOGW(TAG, "Receiver busy with no session here; cancelling stale transfer and retrying");
    cancel_on_receiver(&rx);
    status = rx_http(&rx, HTTP_METHOD_POST, "/prepare", json, force, 10000, msg, msg_len, NULL);
  }
  cJSON_free(json);

  if (status != 200) {
    led_set_state((status == 423 || status == 507) ? LED_STATE_IDLE : LED_STATE_ERROR);
    if (status == 423) {
      snprintf(msg, msg_len, "machine_busy");
    } else if (msg[0] == '\0') {
      snprintf(msg, msg_len, "Receiver /prepare failed (HTTP %d)", status);
    }
    return status < 0 ? 502 : status;
  }

  memset(&s_up, 0, sizeof(s_up));
  snprintf(s_up.id, sizeof(s_up.id), "%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random());
  s_up.rx = rx;
  strlcpy(s_up.name, name, sizeof(s_up.name));
  strlcpy(s_up.md5, md5, sizeof(s_up.md5));
  s_up.size = size;
  s_up.t_start_us = now;
  s_up.prepare_us = esp_timer_get_time() - now;
  s_up.last_activity_us = esp_timer_get_time();

  char chunk_url[64];
  snprintf(chunk_url, sizeof(chunk_url), "http://%s/chunk", rx.ip);
  esp_http_client_config_t chunk_cfg = {
      .url = chunk_url,
      .timeout_ms = 10000,
      .keep_alive_enable = true, // TCP keepalive probes
      .event_handler = chunk_client_event,
      .user_data = &s_up.connects,
  };
  s_up.chunk_client = esp_http_client_init(&chunk_cfg);
  if (s_up.chunk_client == NULL) {
    cancel_on_receiver(&rx);
    session_end();
    snprintf(msg, msg_len, "HTTP client init failed");
    return 500;
  }
  esp_http_client_set_method(s_up.chunk_client, HTTP_METHOD_POST);
  esp_http_client_set_header(s_up.chunk_client, "Content-Type", "application/octet-stream");
  esp_http_client_set_header(s_up.chunk_client, "Connection", "keep-alive");
  esp_http_client_set_header(s_up.chunk_client, "X-Auth-Key", rx.key);
  s_up.active = true;

  led_set_state(LED_STATE_SENDING);
  ESP_LOGI(TAG, "Stage prepare: %lld ms, session %s", (long long)(s_up.prepare_us / 1000), s_up.id);
  strlcpy(session_out, s_up.id, session_len);
  return 200;
}

static int chunk_locked(const char *session, int index, const char *data, int len,
                        uint64_t *acked_out, char *msg, size_t msg_len) {
  msg[0] = '\0';
  if (!session_matches(session)) {
    snprintf(msg, msg_len, "Unknown or expired upload session");
    return 410;
  }
  s_up.last_activity_us = esp_timer_get_time();
  if (index > s_up.next_index) {
    snprintf(msg, msg_len, "Out-of-order chunk %d (expected %d)", index, s_up.next_index);
    return 409;
  }
  if (index == s_up.next_index && s_up.acked + (uint64_t)len > s_up.size) {
    snprintf(msg, msg_len, "Chunk exceeds declared file size");
    return 400;
  }

  // index < next_index is a browser retry of an acked chunk: forward anyway,
  // the receiver acknowledges duplicates without re-appending.
  char idx[16];
  snprintf(idx, sizeof(idx), "%d", index);
  esp_http_client_set_header(s_up.chunk_client, "X-Chunk-Index", idx);
  esp_http_client_set_post_field(s_up.chunk_client, data, len);

  int64_t t0 = esp_timer_get_time();
  esp_err_t err = esp_http_client_perform(s_up.chunk_client);
  int64_t rtt = esp_timer_get_time() - t0;
  s_up.chunk_total_us += rtt;
  if (rtt > s_up.chunk_max_us) s_up.chunk_max_us = rtt;
  s_up.chunks++;

  if (err != ESP_OK) {
    snprintf(msg, msg_len, "Receiver unreachable: %s", esp_err_to_name(err));
    ESP_LOGW(TAG, "Chunk %d: %s", index, msg);
    esp_http_client_close(s_up.chunk_client); // browser retry reconnects
    return 502;
  }
  int status = esp_http_client_get_status_code(s_up.chunk_client);
  if (status != 200) {
    snprintf(msg, msg_len, "Receiver rejected chunk %d (HTTP %d)", index, status);
    ESP_LOGW(TAG, "%s", msg);
    esp_http_client_close(s_up.chunk_client); // browser retry reconnects
    return status == 409 ? 409 : 502;
  }

  if (index == s_up.next_index) {
    s_up.next_index++;
    s_up.acked += len;
  }
  *acked_out = s_up.acked;
  return 200;
}

static int finish_locked(const char *session, char *msg, size_t msg_len) {
  msg[0] = '\0';
  if (!session_matches(session)) {
    snprintf(msg, msg_len, "Unknown or expired upload session");
    return 410;
  }
  if (s_up.acked != s_up.size) {
    snprintf(msg, msg_len, "Incomplete upload: %llu of %u bytes acknowledged",
             (unsigned long long)s_up.acked, (unsigned)s_up.size);
    return 400;
  }
  // Close the chunk connection first so it doesn't hold a receiver socket
  esp_http_client_cleanup(s_up.chunk_client);
  s_up.chunk_client = NULL;

  char json[64];
  snprintf(json, sizeof(json), "{\"md5\":\"%s\"}", s_up.md5); // browser MD5: end-to-end check
  int64_t t0 = esp_timer_get_time();
  // receiver: fsync, MD5 verify, rename, list snapshot, unmount (replies before USB reconnect)
  int status = rx_http(&s_up.rx, HTTP_METHOD_POST, "/finalize", json, false, 30000, msg, msg_len, NULL);
  int64_t fin_us = esp_timer_get_time() - t0;

  int64_t total_us = esp_timer_get_time() - s_up.t_start_us;
  ESP_LOGI(TAG, "XFER summary -> %s: %u bytes in %lld ms = %.1f KB/s | prepare %lld ms, "
                "%lu chunks (rtt avg %lld, max %lld ms, connections %d), finalize %lld ms | %s",
           s_up.rx.id, (unsigned)s_up.size, (long long)(total_us / 1000),
           total_us > 0 ? (double)s_up.size / 1024.0 / ((double)total_us / 1e6) : 0.0,
           (long long)(s_up.prepare_us / 1000), (unsigned long)s_up.chunks,
           (long long)(s_up.chunks ? s_up.chunk_total_us / s_up.chunks / 1000 : 0),
           (long long)(s_up.chunk_max_us / 1000), s_up.connects,
           (long long)(fin_us / 1000), status == 200 ? "OK" : "FAILED");

  session_end();
  if (status != 200) {
    led_set_state(LED_STATE_ERROR);
    if (status >= 0 && msg[0] == '\0') {
      snprintf(msg, msg_len, "Receiver /finalize failed (HTTP %d)", status);
    }
    return status < 0 ? 502 : status;
  }
  led_set_state(LED_STATE_SUCCESS);
  return 200;
}

static int cancel_locked(const char *session) {
  if (!s_up.active) {
    return 200; // nothing to cancel
  }
  if (session && session[0] && !session_matches(session)) {
    return 410; // someone else's upload: leave it alone
  }
  ESP_LOGW(TAG, "Upload %s cancelled after %llu bytes", s_up.id, (unsigned long long)s_up.acked);
  rx_target_t rx = s_up.rx;
  session_end();
  led_set_state(LED_STATE_IDLE);
  int code = cancel_on_receiver(&rx);
  return code == 200 ? 200 : 502;
}

int file_transfer_begin(const char *rx_id, const char *name, size_t size, const char *md5, bool force,
                        char *session_out, size_t session_len, char *msg, size_t msg_len) {
  up_lock();
  int r = begin_locked(rx_id, name, size, md5, force, session_out, session_len, msg, msg_len);
  up_unlock();
  return r;
}

int file_transfer_chunk(const char *session, int index, const char *data, int len,
                        uint64_t *acked_out, char *msg, size_t msg_len) {
  up_lock();
  int r = chunk_locked(session, index, data, len, acked_out, msg, msg_len);
  up_unlock();
  return r;
}

int file_transfer_finish(const char *session, char *msg, size_t msg_len) {
  up_lock();
  int r = finish_locked(session, msg, msg_len);
  up_unlock();
  return r;
}

int file_transfer_cancel(const char *session) {
  up_lock();
  int r = cancel_locked(session);
  up_unlock();
  return r;
}

bool file_transfer_active_rx(char *rx_id_out, size_t len) {
  bool active = false;
  if (s_up_mutex && xSemaphoreTake(s_up_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    active = s_up.active;
    if (active) strlcpy(rx_id_out, s_up.rx.id, len);
    xSemaphoreGive(s_up_mutex);
  }
  return active;
}

// Expire a session the browser abandoned (closes its receiver socket)
static void expire_idle_session(void) {
  if (s_up_mutex == NULL || xSemaphoreTake(s_up_mutex, 0) != pdTRUE) {
    return; // a handler is using the session right now, so it isn't idle
  }
  if (s_up.active && esp_timer_get_time() - s_up.last_activity_us > SESSION_IDLE_TIMEOUT_US) {
    ESP_LOGW(TAG, "Upload session %s idle > %lld s, expiring", s_up.id,
             (long long)(SESSION_IDLE_TIMEOUT_US / 1000000));
    rx_target_t rx = s_up.rx;
    session_end();
    led_set_state(LED_STATE_IDLE);
    cancel_on_receiver(&rx);
  }
  xSemaphoreGive(s_up_mutex);
}

static void reaper_task(void *arg) {
  (void)arg;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    expire_idle_session();
  }
}

void file_transfer_start(void) {
  s_up_mutex = xSemaphoreCreateMutex();
  xTaskCreate(reaper_task, "upload_reaper", 4096, NULL, 3, NULL);
}

// ─── Per-receiver proxies ───────────────────────────────────────────────────
int file_transfer_get_receiver_status(const char *rx_id, char *out, size_t out_len) {
  rx_target_t rx;
  char msg[64];
  cJSON *obj = NULL;
  if (resolve(rx_id, &rx, msg, sizeof(msg)) == 0) {
    // Live, short timeout: the receiver's server is single-threaded (finalize, list)
    if (rx_http(&rx, HTTP_METHOD_GET, "/status", NULL, false, 2000, out, out_len, NULL) == 200) {
      obj = cJSON_Parse(out);
      if (obj) rx_registry_update_status(rx_id, obj);
    }
  }
  cJSON_Delete(obj);
  // Answer from the registry (just updated if the live call worked)
  cJSON *res = cJSON_CreateObject();
  bool known = rx_registry_add_cached_status(rx_id, res);
  char *s = cJSON_PrintUnformatted(res);
  cJSON_Delete(res);
  snprintf(out, out_len, "%s", s ? s : "{}");
  cJSON_free(s);
  return known ? 200 : 404;
}

int file_transfer_delete_receiver(const char *rx_id, const char *json, bool force, char *resp, size_t resp_len) {
  rx_target_t rx;
  int code = resolve(rx_id, &rx, resp, resp_len);
  if (code) return code;
  // Receiver body is {"deleted":[..],"skipped":[..],...} or an error text
  return rx_http(&rx, HTTP_METHOD_POST, "/delete", json, force, 20000, resp, resp_len, NULL);
}

int file_transfer_clear_receiver(const char *rx_id, bool force, char *resp, size_t resp_len) {
  rx_target_t rx;
  int code = resolve(rx_id, &rx, resp, resp_len);
  if (code) return code;
  return rx_http(&rx, HTTP_METHOD_POST, "/clear", "{\"confirm\":\"WIPE\"}", force, 15000,
                 resp, resp_len, NULL);
}

int file_transfer_format_receiver(const char *rx_id, char *resp, size_t resp_len) {
  rx_target_t rx;
  int code = resolve(rx_id, &rx, resp, resp_len);
  if (code) return code;
  // f_mkfs on a large SD card over SPI can take a while
  return rx_http(&rx, HTTP_METHOD_POST, "/format", "{}", false, 120000, resp, resp_len, NULL);
}

int file_transfer_get_receiver_files(const char *rx_id, char *out_json, size_t max_len) {
  rx_target_t rx;
  int code = resolve(rx_id, &rx, out_json, max_len);
  if (code) return code;
  bool truncated = false;
  // receiver may mount + scan the SD card to build the list
  int status = rx_http(&rx, HTTP_METHOD_GET, "/list", NULL, false, 15000, out_json, max_len, &truncated);
  if (status == 200 && (truncated || out_json[0] != '{')) {
    // Truncated JSON would break the UI's res.json(); report it instead
    ESP_LOGW(TAG, "Receiver /list: %s", truncated ? "response too large" : "unexpected body");
    snprintf(out_json, max_len, "Receiver file list unreadable");
    return 502;
  }
  return status < 0 ? 502 : status;
}

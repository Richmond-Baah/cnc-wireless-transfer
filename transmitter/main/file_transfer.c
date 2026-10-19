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

#include "nvs.h"

#ifndef CONFIG_CNC_RECEIVER_IP
#define CONFIG_CNC_RECEIVER_IP "192.168.4.150"
#endif


// ─── Cached receiver status ─────────────────────────────────────────────────
// The background task writes here; the HTTP handler reads from here.
// This prevents the web server from ever blocking on a slow receiver HTTP call.
#define STATUS_CACHE_LEN 384
static char     s_status_cache[STATUS_CACHE_LEN] = "{\"state\":\"OFFLINE\"}";
static SemaphoreHandle_t s_status_mutex = NULL;
static SemaphoreHandle_t s_up_mutex = NULL; // upload session (s_up), see below

// The receiver's HTTP server is single-threaded: a /list refresh (SD mount +
// scan) or /finalize (1.5 s USB reconnect wait) can stall /status for >1 s,
// so allow a generous timeout and require several consecutive failures
// before reporting OFFLINE.
#define STATUS_POLL_TIMEOUT_MS 3000
#define STATUS_OFFLINE_AFTER   3

static void cache_status(const char *json) {
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        strncpy(s_status_cache, json, STATUS_CACHE_LEN - 1);
        s_status_cache[STATUS_CACHE_LEN - 1] = '\0';
        xSemaphoreGive(s_status_mutex);
    }
}

// Fetches receiver /status into buf. On failure returns false and describes why.
static bool fetch_receiver_status(const char *url, char *buf, size_t len,
                                  char *why, size_t why_len) {
    esp_http_client_config_t cfg = {
        .url        = url,
        .timeout_ms = STATUS_POLL_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        snprintf(why, why_len, "client init failed (bad URL?)");
        return false;
    }

    bool ok = false;
    TickType_t t0 = xTaskGetTickCount();
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(why, why_len, "connect failed: %s", esp_err_to_name(err));
    } else {
        int64_t clen = esp_http_client_fetch_headers(client);
        int code = esp_http_client_get_status_code(client);
        if (clen < 0) {
            snprintf(why, why_len, "no response headers (%lld, timeout?)", (long long)clen);
        } else if (code != 200) {
            snprintf(why, why_len, "HTTP %d", code);
        } else if (clen >= (int64_t)len) {
            snprintf(why, why_len, "response too large (%lld >= %u bytes)", (long long)clen, (unsigned)len);
        } else {
            int total = 0;
            while (total < (int)len - 1) {
                int r = esp_http_client_read(client, buf + total, len - 1 - total);
                if (r <= 0) break;
                total += r;
            }
            buf[total] = '\0';
            if (total == 0) {
                snprintf(why, why_len, "empty body");
            } else if (buf[0] != '{' || strstr(buf, "\"state\"") == NULL) {
                snprintf(why, why_len, "unexpected body: %.40s", buf);
            } else {
                ok = true;
            }
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);

    if (!ok) {
        size_t used = strlen(why);
        snprintf(why + used, why_len - used, " (after %lu ms)",
                 (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - t0));
    }
    return ok;
}

// Background task: poll receiver /status every 2 s and update the cache.
static void expire_idle_session(void);

static void receiver_status_task(void *arg) {
    (void)arg;
    char url[128];
    char buf[STATUS_CACHE_LEN];
    char why[96];
    int fails = 0;
    bool online = false;

    for (;;) {
        get_receiver_url("/status", url, sizeof(url));
        why[0] = '\0';

        if (fetch_receiver_status(url, buf, sizeof(buf), why, sizeof(why))) {
            if (!online || fails > 0) {
                ESP_LOGI(TAG, "Receiver status OK at %s", url);
            }
            fails = 0;
            online = true;
            cache_status(buf);
        } else {
            fails++;
            // Log every failure until OFFLINE, then every ~30 s
            if (fails <= STATUS_OFFLINE_AFTER || fails % 15 == 0) {
                ESP_LOGW(TAG, "Receiver status poll failed (%d in a row) %s: %s", fails, url, why);
            }
            if (fails == STATUS_OFFLINE_AFTER) {
                ESP_LOGW(TAG, "Marking receiver OFFLINE");
                online = false;
                cache_status("{\"state\":\"OFFLINE\"}");
            }
        }

        expire_idle_session();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void file_transfer_status_task_start(void) {
    s_status_mutex = xSemaphoreCreateMutex();
    s_up_mutex = xSemaphoreCreateMutex();
    xTaskCreate(receiver_status_task, "rec_status", 6144, NULL, 3, NULL);
    ESP_LOGI(TAG, "Receiver status background poller started.");
}

void get_receiver_url(const char *path, char *out_url, size_t max_len) {
  char ip[32] = CONFIG_CNC_RECEIVER_IP;
  nvs_handle_t nvs_handle;
  if (nvs_open("sys_cfg", NVS_READONLY, &nvs_handle) == ESP_OK) {
    size_t len = sizeof(ip);
    char tmp[32] = "";
    if (nvs_get_str(nvs_handle, "receiver_ip", tmp, &len) == ESP_OK && strlen(tmp) > 0) {
      strncpy(ip, tmp, sizeof(ip) - 1);
      ip[sizeof(ip) - 1] = '\0';
    }
    // else: keep the compiled-in default
    nvs_close(nvs_handle);
  }
  snprintf(out_url, max_len, "http://%s%s", ip, path);
  ESP_LOGD("TRANSFER", "Receiver URL: %s", out_url); // called every 2 s by the poller
}

// ─── Browser-driven upload session ──────────────────────────────────────────
// The browser slices the file and calls begin, chunk x N, finish. The receiver
// handles one transfer at a time, so there is a single session.
#define SESSION_IDLE_TIMEOUT_US (30LL * 1000 * 1000) // idle sessions are expired by the poller task

// httpd handlers and the poller's idle reaper both touch s_up
static void up_lock(void) { if (s_up_mutex) xSemaphoreTake(s_up_mutex, portMAX_DELAY); }
static void up_unlock(void) { if (s_up_mutex) xSemaphoreGive(s_up_mutex); }

static struct {
  bool active;
  char id[17];
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

// POSTs a JSON body to a receiver endpoint. Returns the HTTP status, or -1 if
// the receiver could not be reached; the response body (error text) goes to msg.
static int receiver_post_json(const char *path, const char *json, bool force,
                              int timeout_ms, char *msg, size_t msg_len) {
  char url[128];
  get_receiver_url(path, url, sizeof(url));
  msg[0] = '\0';

  esp_http_client_config_t cfg = {
      .url = url,
      .timeout_ms = timeout_ms,
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == NULL) {
    snprintf(msg, msg_len, "HTTP client init failed (bad receiver URL?)");
    return -1;
  }
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  if (force) {
    esp_http_client_set_header(client, "X-Force", "1");
  }

  int status = -1;
  int len = strlen(json);
  esp_err_t err = esp_http_client_open(client, len);
  if (err != ESP_OK) {
    snprintf(msg, msg_len, "Receiver unreachable: %s", esp_err_to_name(err));
  } else if (esp_http_client_write(client, json, len) != len) {
    snprintf(msg, msg_len, "Receiver unreachable: write failed");
  } else if (esp_http_client_fetch_headers(client) < 0) {
    snprintf(msg, msg_len, "No response from receiver (timeout)");
  } else {
    status = esp_http_client_get_status_code(client);
    int r = esp_http_client_read(client, msg, msg_len - 1);
    msg[r > 0 ? r : 0] = '\0';
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (status < 0) {
    ESP_LOGW(TAG, "Receiver %s failed: %s", path, msg);
  }
  return status;
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

static int begin_locked(const char *name, size_t size, const char *md5, bool force,
                        char *session_out, size_t session_len,
                        char *msg, size_t msg_len) {
  int64_t now = esp_timer_get_time();
  msg[0] = '\0';
  if (s_up.active) {
    // One session at a time: a new begin closes the old one and its receiver socket
    ESP_LOGW(TAG, "Replacing upload session %s (%s)", s_up.id, s_up.name);
    file_transfer_cancel_receiver();
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

  ESP_LOGI(TAG, "Upload begin: %s (%u bytes, md5=%s%s)", name, (unsigned)size, md5,
           force ? ", forced" : "");
  int status = receiver_post_json("/prepare", json, force, 10000, msg, msg_len);
  if (status == 503) {
    // Receiver is stuck RECEIVING from an abandoned transfer we no longer
    // track (e.g. transmitter rebooted mid-upload): reset it once and retry.
    ESP_LOGW(TAG, "Receiver busy with no session here; cancelling stale transfer and retrying");
    file_transfer_cancel_receiver();
    status = receiver_post_json("/prepare", json, force, 10000, msg, msg_len);
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
  strlcpy(s_up.name, name, sizeof(s_up.name));
  strlcpy(s_up.md5, md5, sizeof(s_up.md5));
  s_up.size = size;
  s_up.t_start_us = now;
  s_up.prepare_us = esp_timer_get_time() - now;
  s_up.last_activity_us = esp_timer_get_time();

  char chunk_url[128];
  get_receiver_url("/chunk", chunk_url, sizeof(chunk_url));
  esp_http_client_config_t chunk_cfg = {
      .url = chunk_url,
      .timeout_ms = 10000,
      .keep_alive_enable = true, // TCP keepalive probes
      .event_handler = chunk_client_event,
      .user_data = &s_up.connects,
  };
  s_up.chunk_client = esp_http_client_init(&chunk_cfg);
  if (s_up.chunk_client == NULL) {
    file_transfer_cancel_receiver();
    session_end();
    snprintf(msg, msg_len, "HTTP client init failed");
    return 500;
  }
  esp_http_client_set_method(s_up.chunk_client, HTTP_METHOD_POST);
  esp_http_client_set_header(s_up.chunk_client, "Content-Type", "application/octet-stream");
  esp_http_client_set_header(s_up.chunk_client, "Connection", "keep-alive");
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
  int status = receiver_post_json("/finalize", json, false, 30000, msg, msg_len);
  int64_t fin_us = esp_timer_get_time() - t0;

  int64_t total_us = esp_timer_get_time() - s_up.t_start_us;
  ESP_LOGI(TAG, "XFER summary: %u bytes in %lld ms = %.1f KB/s | prepare %lld ms, "
                "%lu chunks (rtt avg %lld, max %lld ms, connections %d), finalize %lld ms | %s",
           (unsigned)s_up.size, (long long)(total_us / 1000),
           total_us > 0 ? (double)s_up.size / 1024.0 / ((double)total_us / 1e6) : 0.0,
           (long long)(s_up.prepare_us / 1000), (unsigned long)s_up.chunks,
           (long long)(s_up.chunks ? s_up.chunk_total_us / s_up.chunks / 1000 : 0),
           (long long)(s_up.chunk_max_us / 1000), s_up.connects,
           (long long)(fin_us / 1000), status == 200 ? "OK" : "FAILED");

  session_end();
  if (status != 200) {
    led_set_state(LED_STATE_ERROR);
    if (msg[0] == '\0') {
      snprintf(msg, msg_len, "Receiver /finalize failed (HTTP %d)", status);
    }
    return status < 0 ? 502 : status;
  }
  led_set_state(LED_STATE_SUCCESS);
  return 200;
}

static int cancel_locked(const char *session) {
  if (s_up.active && session && session[0] && !session_matches(session)) {
    return 410; // someone else's upload: leave it alone
  }
  if (s_up.active) {
    ESP_LOGW(TAG, "Upload %s cancelled after %llu bytes", s_up.id, (unsigned long long)s_up.acked);
  }
  session_end();
  led_set_state(LED_STATE_IDLE);
  int code = file_transfer_cancel_receiver();
  return code == 200 ? 200 : 502;
}

int file_transfer_begin(const char *name, size_t size, const char *md5, bool force,
                        char *session_out, size_t session_len, char *msg, size_t msg_len) {
  up_lock();
  int r = begin_locked(name, size, md5, force, session_out, session_len, msg, msg_len);
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

// Called by the poller: expire a session the browser abandoned (closes its receiver socket)
static void expire_idle_session(void) {
  if (s_up_mutex == NULL || xSemaphoreTake(s_up_mutex, 0) != pdTRUE) {
    return; // a handler is using the session right now, so it isn't idle
  }
  if (s_up.active && esp_timer_get_time() - s_up.last_activity_us > SESSION_IDLE_TIMEOUT_US) {
    ESP_LOGW(TAG, "Upload session %s idle > %lld s, expiring", s_up.id,
             (long long)(SESSION_IDLE_TIMEOUT_US / 1000000));
    session_end();
    led_set_state(LED_STATE_IDLE);
    file_transfer_cancel_receiver();
  }
  xSemaphoreGive(s_up_mutex);
}

int file_transfer_get_receiver_status(char *out_state_str, size_t max_len) {
  // Return the cached value from the background poller — never blocks.
  if (s_status_mutex && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    strncpy(out_state_str, s_status_cache, max_len - 1);
    out_state_str[max_len - 1] = '\0';
    xSemaphoreGive(s_status_mutex);
  } else {
    snprintf(out_state_str, max_len, "{\"state\":\"OFFLINE\"}");
  }
  return 200;
}

bool file_transfer_is_receiver_online(void) {
  bool online = false;
  if (s_status_mutex && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    online = (strstr(s_status_cache, "OFFLINE") == NULL);
    xSemaphoreGive(s_status_mutex);
  }
  return online;
}

int file_transfer_delete_receiver(const char *json, bool force, char *resp, size_t resp_len) {
  // Receiver body is {"deleted":[..],"skipped":[..],...} or an error text
  return receiver_post_json("/delete", json, force, 20000, resp, resp_len);
}

int file_transfer_clear_receiver(bool force) {
  char clear_url[128];
  get_receiver_url("/clear", clear_url, sizeof(clear_url));

  esp_http_client_config_t config = {
      .url = clear_url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 15000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  static const char confirm_body[] = "{\"confirm\":\"WIPE\"}";
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, confirm_body, sizeof(confirm_body) - 1);
  if (force) {
    esp_http_client_set_header(client, "X-Force", "1");
  }

  esp_err_t err = esp_http_client_perform(client);
  int status_code = 500;
  if (err == ESP_OK) {
    status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Receiver /clear result: %d", status_code);
  } else {
    ESP_LOGE(TAG, "Failed to reach receiver /clear: %s", esp_err_to_name(err));
  }
  esp_http_client_cleanup(client);
  return status_code;
}

int file_transfer_format_receiver(void) {
  char format_url[128];
  get_receiver_url("/format", format_url, sizeof(format_url));

  esp_http_client_config_t config = {
      .url = format_url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 120000, // f_mkfs on a large SD card over SPI can take a while
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  esp_err_t err = esp_http_client_perform(client);
  int status_code = 500;
  if (err == ESP_OK) {
    status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Receiver /format result: %d", status_code);
  } else {
    ESP_LOGE(TAG, "Failed to reach receiver /format: %s", esp_err_to_name(err));
  }
  esp_http_client_cleanup(client);
  return status_code;
}

int file_transfer_cancel_receiver(void) {
  char cancel_url[128];
  get_receiver_url("/cancel", cancel_url, sizeof(cancel_url));

  esp_http_client_config_t config = {
      .url = cancel_url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 2000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);
  int status_code = 500;
  if (err == ESP_OK) {
      status_code = esp_http_client_get_status_code(client);
  }
  esp_http_client_cleanup(client);
  return status_code;
}

int file_transfer_get_receiver_files(char *out_json, size_t max_len) {
  char list_url[128];
  get_receiver_url("/list", list_url, sizeof(list_url));

  esp_http_client_config_t config = {
      .url = list_url,
      .timeout_ms = 15000, // receiver may mount + scan the SD card to build the list
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "Receiver /list: client init failed (bad URL?) %s", list_url);
    return 500;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  int status_code = 500;
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Receiver /list: connect failed: %s", esp_err_to_name(err));
  } else {
    int64_t clen = esp_http_client_fetch_headers(client);
    status_code = esp_http_client_get_status_code(client);
    if (clen < 0) {
      ESP_LOGW(TAG, "Receiver /list: no response headers (%lld, timeout?)", (long long)clen);
      status_code = 500;
    } else if (status_code != 200) {
      ESP_LOGW(TAG, "Receiver /list: HTTP %d", status_code);
    } else {
      size_t total_read = 0;
      while (total_read < max_len - 1) {
        int r = esp_http_client_read(client, out_json + total_read, max_len - 1 - total_read);
        if (r <= 0) break;
        total_read += r;
      }
      out_json[total_read] = '\0';
      if (total_read == max_len - 1 && !esp_http_client_is_complete_data_received(client)) {
        // Truncated JSON would break the UI's res.json(); report it instead
        ESP_LOGW(TAG, "Receiver /list: response exceeds %u-byte buffer", (unsigned)max_len);
        status_code = 500;
      } else if (total_read == 0 || out_json[0] != '{') {
        ESP_LOGW(TAG, "Receiver /list: unexpected body (%u bytes): %.40s", (unsigned)total_read, out_json);
        status_code = 500;
      }
    }
    esp_http_client_close(client);
  }
  esp_http_client_cleanup(client);
  return status_code;
}

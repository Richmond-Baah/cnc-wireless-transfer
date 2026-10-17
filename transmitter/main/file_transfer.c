#include "file_transfer.h"
#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "led.h"
#include "mbedtls/md5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "TRANSFER";

#include "nvs.h"

#ifndef CONFIG_CNC_RECEIVER_IP
#define CONFIG_CNC_RECEIVER_IP "192.168.4.150"
#endif

#define SCRATCH_BUFSIZE (8 * 1024)

static char scratch_buf[SCRATCH_BUFSIZE];
static volatile bool s_abort_transfer = false;
static bool s_last_refused = false;

bool file_transfer_last_refused(void) {
  return s_last_refused;
}

// ─── Cached receiver status ─────────────────────────────────────────────────
// The background task writes here; the HTTP handler reads from here.
// This prevents the web server from ever blocking on a slow receiver HTTP call.
#define STATUS_CACHE_LEN 384
static char     s_status_cache[STATUS_CACHE_LEN] = "{\"state\":\"OFFLINE\"}";
static SemaphoreHandle_t s_status_mutex = NULL;

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

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void file_transfer_status_task_start(void) {
    s_status_mutex = xSemaphoreCreateMutex();
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

bool file_transfer_forward_to_receiver(httpd_req_t *req, const char *filename,
                                       const char *expected_md5,
                                       size_t filesize, bool force) {
  s_abort_transfer = false;
  s_last_refused = false;
  ESP_LOGI(TAG, "Starting transfer for %s (%u bytes, md5=%s)", filename,
           (unsigned)filesize, expected_md5);
  led_set_state(LED_STATE_SENDING);

  char prepare_url[128];
  get_receiver_url("/prepare", prepare_url, sizeof(prepare_url));

  esp_http_client_config_t config = {
      .url = prepare_url,
      .timeout_ms = 5000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  if (force) {
    esp_http_client_set_header(client, "X-Force", "1");
  }

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"filename\":\"%s\",\"filesize\":%u,\"md5\":\"%s\"}", filename,
           (unsigned)filesize, expected_md5);
  // open/write/read instead of perform() so the receiver's error text can be relayed
  int payload_len = strlen(payload);
  int prepare_status = 0;
  char prepare_msg[160] = {0};
  esp_err_t err = esp_http_client_open(client, payload_len);
  if (err == ESP_OK) {
    if (esp_http_client_write(client, payload, payload_len) != payload_len) {
      err = ESP_FAIL;
    } else {
      esp_http_client_fetch_headers(client);
      prepare_status = esp_http_client_get_status_code(client);
      int r = esp_http_client_read(client, prepare_msg, sizeof(prepare_msg) - 1);
      if (r > 0) prepare_msg[r] = '\0';
    }
    esp_http_client_close(client);
  }
  esp_http_client_cleanup(client);

  if (err == ESP_OK && prepare_status == 423) {
    ESP_LOGW(TAG, "Receiver refused /prepare: machine is reading the drive");
    s_last_refused = true;
    led_set_state(LED_STATE_IDLE);
    httpd_resp_set_status(req, "423 Locked");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"error\":\"machine_busy\"}");
    return false;
  }

  if (err == ESP_OK && prepare_status == 400) {
    // e.g. invalid filename: pass the receiver's reason through to the UI
    ESP_LOGW(TAG, "Receiver rejected /prepare: %s", prepare_msg);
    s_last_refused = true;
    led_set_state(LED_STATE_ERROR);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        prepare_msg[0] ? prepare_msg : "Receiver rejected the file");
    return false;
  }

  if (err != ESP_OK || prepare_status != 200) {
    ESP_LOGE(TAG, "Receiver /prepare failed. Err: %s. Code: %d",
             esp_err_to_name(err), prepare_status);
    led_set_state(LED_STATE_ERROR);
    if (prepare_status == 503) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receiver is busy (503).");
    } else {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receiver preparation failed.");
    }
    return false;
  }

  // 2. Stream chunks directly from http_req body to the receiver
  int remaining = (int)filesize;
  int chunk_index = 0;

  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts(&ctx);

  char chunk_url[128];
  get_receiver_url("/chunk", chunk_url, sizeof(chunk_url));

  esp_http_client_config_t chunk_cfg = {
      .url = chunk_url,
      .timeout_ms = 10000,
      .keep_alive_enable = true,
  };
  esp_http_client_handle_t chunk_client = esp_http_client_init(&chunk_cfg);

  while (remaining > 0) {
    if (s_abort_transfer) {
        ESP_LOGW(TAG, "Transfer aborted by user");
        esp_http_client_cleanup(chunk_client);
        file_transfer_cancel_receiver();
        led_set_state(LED_STATE_IDLE);
        return false;
    }
    int to_read = remaining < SCRATCH_BUFSIZE ? remaining : SCRATCH_BUFSIZE;
    int read_len = httpd_req_recv(req, scratch_buf, to_read);
    if (read_len <= 0) {
      if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
        continue; // Retry
      }
      ESP_LOGE(TAG, "Failed reading client upload");
      led_set_state(LED_STATE_ERROR);
      mbedtls_md5_free(&ctx);
      esp_http_client_cleanup(chunk_client);
      file_transfer_cancel_receiver(); // release receiver storage + reconnect USB
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Client read error");
      return false;
    }

    // Update hash
    mbedtls_md5_update(&ctx, (unsigned char *)scratch_buf, read_len);

    esp_http_client_set_method(chunk_client, HTTP_METHOD_POST);
    esp_http_client_set_header(chunk_client, "Content-Type", "application/octet-stream");
    char chunk_hdr[32];
    snprintf(chunk_hdr, sizeof(chunk_hdr), "%d", chunk_index);
    esp_http_client_set_header(chunk_client, "X-Chunk-Index", chunk_hdr);
    esp_http_client_set_post_field(chunk_client, scratch_buf, read_len);

    esp_err_t err = ESP_FAIL;
    int chunk_status = 500;
    int max_retries = 3;
    bool success = false;

    for (int retry = 0; retry < max_retries; retry++) {
        err = esp_http_client_perform(chunk_client);
        chunk_status = esp_http_client_get_status_code(chunk_client);
        if (err == ESP_OK && chunk_status == 200) {
            success = true;
            break;
        }
        ESP_LOGW(TAG, "Chunk %d upload attempt %d failed (err %s, status %d). Retrying...", 
                 chunk_index, retry + 1, esp_err_to_name(err), chunk_status);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (s_abort_transfer) {
        ESP_LOGW(TAG, "Transfer aborted after chunk %d", chunk_index);
        esp_http_client_cleanup(chunk_client);
        file_transfer_cancel_receiver();
        led_set_state(LED_STATE_IDLE);
        return false;
    }

    if (!success) {
      ESP_LOGE(TAG, "Receiver /chunk failed at index %d after %d retries (err %s, status %d)",
               chunk_index, max_retries, esp_err_to_name(err), chunk_status);
      mbedtls_md5_free(&ctx);
      led_set_state(LED_STATE_ERROR);
      esp_http_client_cleanup(chunk_client);
      file_transfer_cancel_receiver(); // release receiver storage + reconnect USB
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receiver chunk failure");
      return false;
    }

    remaining -= read_len;
    chunk_index++;
  }

  esp_http_client_cleanup(chunk_client);

  // Compute final MD5
  unsigned char hash[16];
  mbedtls_md5_finish(&ctx, hash);
  mbedtls_md5_free(&ctx);

  char computed_md5[33] = {0};
  for (int i = 0; i < 16; i++) {
    snprintf(&computed_md5[i * 2], 3, "%02x", hash[i]);
  }
  ESP_LOGI(TAG, "Upload computed MD5: %s (expected: %s)", computed_md5,
           expected_md5);

  // 3. Finalize — send the computed checksum for the receiver to verify
  if (s_abort_transfer) {
    ESP_LOGW(TAG, "Transfer aborted just before finalize");
    file_transfer_cancel_receiver();
    led_set_state(LED_STATE_IDLE);
    return false;
  }

  char fin_url[128];
  get_receiver_url("/finalize", fin_url, sizeof(fin_url));

  esp_http_client_config_t fin_cfg = {
      .url = fin_url,
      .timeout_ms = 5000,
  };
  client = esp_http_client_init(&fin_cfg);
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");

  snprintf(payload, sizeof(payload), "{\"md5\":\"%s\"}", computed_md5);
  esp_http_client_set_post_field(client, payload, strlen(payload));

  err = esp_http_client_perform(client);
  int fin_status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK || fin_status != 200) {
    ESP_LOGE(TAG, "Receiver /finalize failed. Code: %d", fin_status);
    led_set_state(LED_STATE_ERROR);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Checksum mismatch on receiver side.");
    return false;
  }

  ESP_LOGI(TAG, "File successfully transferred and verified.");
  led_set_state(LED_STATE_SUCCESS);
  s_abort_transfer = false;
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, "{\"status\":\"success\"}");
  return true;
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

void file_transfer_abort(void) {
    s_abort_transfer = true;
}

bool file_transfer_is_aborted(void) {
    return s_abort_transfer;
}

void file_transfer_clear_abort(void) {
    s_abort_transfer = false;
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

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
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdlib.h>

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
  int64_t chunk_total_us;  // TX -> RX (forward + RX write + ack)
  int64_t chunk_max_us;
  int64_t recv_total_us;   // browser -> TX (reading the chunk body)
  uint64_t next_log;       // acked bytes at which the next progress line is logged
  bool stream;             // X-Relay: stream (pieces forwarded as they arrive)
  bool no_reuse;           // X-Relay: noreuse (control: new TCP connection per chunk)
  int relay_fd;            // stream mode: keep-alive socket to the receiver, -1 = none
} s_up = {.relay_fd = -1};

// Default relay for uploads that don't send X-Relay. Pick from the speed
// test's "Full transfer" rows (buffered vs stream).
#define RELAY_STREAM_DEFAULT false

#define KBPS(bytes, us) ((us) > 0 ? (double)(bytes) / 1024.0 / ((double)(us) / 1e6) : 0.0)
#define PROGRESS_LOG_BYTES (1024 * 1024)

// Reply of the request on chunk_client that wants one (/prepare, /finalize)
static char *s_ka_resp;
static size_t s_ka_cap, s_ka_len;

// Timing of the chunk in flight
#define CHUNK_LOG_ALL 64      // log every chunk of the first 2 MB, then only slow ones
#define CHUNK_LOG_SLOW_MS 500
static struct {
  int64_t headers_sent_us; // headers written; the body follows
  int64_t first_resp_us;   // first response header from the receiver
  int got, recv_ms, write_ms, md5_ms, timeouts; // X-Rx-Timing; got = -1 if absent
} s_ct;

static void parse_rx_timing(const char *v) {
  if (sscanf(v, "%d,%d,%d,%d,%d", &s_ct.got, &s_ct.recv_ms, &s_ct.write_ms, &s_ct.md5_ms, &s_ct.timeouts) != 5) {
    s_ct.got = -1;
  }
}

static void chunk_timing_reset(void) {
  memset(&s_ct, 0, sizeof(s_ct));
  s_ct.got = -1;
}

static void chunk_timing_log(const char *how, int index, int len, int64_t recv_us, int64_t fwd_us) {
  int fwd_ms = (int)(fwd_us / 1000), recv_ms = (int)(recv_us / 1000);
  if (index >= CHUNK_LOG_ALL && fwd_ms + recv_ms <= CHUNK_LOG_SLOW_MS) return;
  ESP_LOGI(TAG, "CHUNK %d %s: sent %d B | browser->TX %d ms | TX->RX %d ms | RX got %d, recv %d, write %d, md5 %d ms, "
                "%d timeouts | connects %d",
           index, how, len, recv_ms, fwd_ms, s_ct.got, s_ct.recv_ms, s_ct.write_ms, s_ct.md5_ms, s_ct.timeouts,
           s_up.connects);
  if (s_ct.got >= 0 && s_ct.got != len) {
    ESP_LOGW(TAG, "CHUNK %d: sent %d B but receiver read %d B", index, len, s_ct.got);
  }
}

static esp_err_t chunk_client_event(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_CONNECTED && evt->user_data) {
    (*(int *)evt->user_data)++;
  } else if (evt->event_id == HTTP_EVENT_HEADERS_SENT) {
    s_ct.headers_sent_us = esp_timer_get_time();
  } else if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (!s_ct.first_resp_us) s_ct.first_resp_us = esp_timer_get_time();
    if (strcasecmp(evt->header_key, "X-Rx-Timing") == 0) parse_rx_timing(evt->header_value);
  } else if (evt->event_id == HTTP_EVENT_ON_DATA && s_ka_resp && s_ka_len + 1 < s_ka_cap) {
    size_t n = evt->data_len < (int)(s_ka_cap - 1 - s_ka_len) ? evt->data_len : s_ka_cap - 1 - s_ka_len;
    memcpy(s_ka_resp + s_ka_len, evt->data, n);
    s_ka_len += n;
    s_ka_resp[s_ka_len] = '\0';
  }
  return ESP_OK;
}

// JSON POST on the session's keep-alive connection. HTTP status, -1 on failure.
static int ka_post(const char *path, const char *json, bool force, int timeout_ms, char *resp, size_t resp_len) {
  esp_http_client_handle_t c = s_up.chunk_client;
  char url[64];
  snprintf(url, sizeof(url), "http://%s%s", s_up.rx.ip, path);
  esp_http_client_set_url(c, url);
  esp_http_client_set_method(c, HTTP_METHOD_POST);
  esp_http_client_set_timeout_ms(c, timeout_ms);
  esp_http_client_set_header(c, "Content-Type", "application/json");
  if (force) {
    esp_http_client_set_header(c, "X-Force", "1");
  } else {
    esp_http_client_delete_header(c, "X-Force");
  }
  esp_http_client_delete_header(c, "X-Chunk-Index");
  esp_http_client_set_post_field(c, json, strlen(json));
  resp[0] = '\0';
  s_ka_resp = resp;
  s_ka_cap = resp_len;
  s_ka_len = 0;
  esp_err_t err = esp_http_client_perform(c);
  s_ka_resp = NULL;
  if (err != ESP_OK) {
    snprintf(resp, resp_len, "Receiver unreachable: %s", esp_err_to_name(err));
    esp_http_client_close(c);
    ESP_LOGW(TAG, "%s %s: %s", s_up.rx.id, path, resp);
    return -1;
  }
  return esp_http_client_get_status_code(c);
}

#define PREPARE_TIMEOUT_MS 10000

static int cancel_on_receiver(const rx_target_t *t) {
  char resp[64];
  return rx_http(t, HTTP_METHOD_POST, "/cancel", "{}", false, 2000, resp, sizeof(resp), NULL);
}

static void relay_close(void) {
  if (s_up.relay_fd >= 0) {
    close(s_up.relay_fd);
    s_up.relay_fd = -1;
  }
}

static void session_end(void) {
  if (s_up.chunk_client) {
    esp_http_client_cleanup(s_up.chunk_client);
  }
  relay_close();
  memset(&s_up, 0, sizeof(s_up));
  s_up.relay_fd = -1;
}

static bool session_matches(const char *session) {
  return s_up.active && session && strcmp(session, s_up.id) == 0;
}

static int begin_locked(const char *rx_id, const char *name, size_t size, const char *md5, bool force,
                        const char *relay, char *session_out, size_t session_len,
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
      snprintf(msg, msg_len, "Busy: uploading to %s", s_up.rx.name);
      return 409;
    }
    // Same receiver: a new begin closes the old session and its receiver socket
    ESP_LOGW(TAG, "Replacing upload session %s (%s)", s_up.id, s_up.name);
    cancel_on_receiver(&s_up.rx);
    session_end();
  }

  // One keep-alive connection for /prepare, the chunks and /finalize
  memset(&s_up, 0, sizeof(s_up));
  s_up.relay_fd = -1;
  s_up.rx = rx;
  char prep_url[64];
  snprintf(prep_url, sizeof(prep_url), "http://%s/prepare", rx.ip);
  esp_http_client_config_t chunk_cfg = {
      .url = prep_url,
      .timeout_ms = PREPARE_TIMEOUT_MS,
      .keep_alive_enable = true, // TCP keepalive probes
      .event_handler = chunk_client_event,
      .user_data = &s_up.connects,
  };
  s_up.chunk_client = esp_http_client_init(&chunk_cfg);
  if (s_up.chunk_client == NULL) {
    session_end();
    snprintf(msg, msg_len, "HTTP client init failed");
    return 500;
  }
  esp_http_client_set_header(s_up.chunk_client, "Connection", "keep-alive");
  esp_http_client_set_header(s_up.chunk_client, "X-Auth-Key", rx.key);

  // Build the /prepare body with cJSON so names with quotes stay valid JSON
  cJSON *obj = cJSON_CreateObject();
  cJSON_AddStringToObject(obj, "filename", name);
  cJSON_AddNumberToObject(obj, "filesize", (double)size);
  cJSON_AddStringToObject(obj, "md5", md5);
  char *json = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  if (json == NULL) {
    session_end();
    snprintf(msg, msg_len, "Out of memory");
    return 500;
  }

  ESP_LOGI(TAG, "Upload begin -> %s: %s (%u bytes, md5=%s%s)", rx.id, name, (unsigned)size, md5,
           force ? ", forced" : "");
  // One 10 s budget for the whole prepare (incl. the stale-transfer retry)
  int64_t deadline = now + PREPARE_TIMEOUT_MS * 1000LL;
  int status = ka_post("/prepare", json, force, PREPARE_TIMEOUT_MS, msg, msg_len);
  if (status == 503) {
    // Receiver is stuck RECEIVING from an abandoned transfer we no longer
    // track (e.g. transmitter rebooted mid-upload): reset it once and retry.
    ESP_LOGW(TAG, "Receiver busy with no session here; cancelling stale transfer and retrying");
    esp_http_client_close(s_up.chunk_client); // the receiver closes its side after a 503
    cancel_on_receiver(&rx);
    int left_ms = (int)((deadline - esp_timer_get_time()) / 1000);
    if (left_ms >= 1000) {
      status = ka_post("/prepare", json, force, left_ms, msg, msg_len);
    } else {
      status = -1;
    }
  }
  cJSON_free(json);
  if (status < 0 && esp_timer_get_time() >= deadline - 500000) {
    session_end();
    led_set_state(LED_STATE_ERROR);
    ESP_LOGW(TAG, "Receiver %s did not answer /prepare within %d s", rx.id, PREPARE_TIMEOUT_MS / 1000);
    snprintf(msg, msg_len, "Receiver did not respond within %d s while preparing. "
             "If the machine is running a job from the drive, wait for it to finish; otherwise try again.",
             PREPARE_TIMEOUT_MS / 1000);
    return 504;
  }

  if (status != 200) {
    session_end();
    led_set_state((status == 423 || status == 507) ? LED_STATE_IDLE : LED_STATE_ERROR);
    if (status == 423) {
      snprintf(msg, msg_len, "machine_busy");
    } else if (msg[0] == '\0') {
      snprintf(msg, msg_len, "Receiver /prepare failed (HTTP %d)", status);
    }
    return status < 0 ? 502 : status;
  }

  s_up.stream = relay ? strcmp(relay, "stream") == 0 : RELAY_STREAM_DEFAULT;
  s_up.no_reuse = relay && strcmp(relay, "noreuse") == 0;
  snprintf(s_up.id, sizeof(s_up.id), "%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random());
  strlcpy(s_up.name, name, sizeof(s_up.name));
  strlcpy(s_up.md5, md5, sizeof(s_up.md5));
  s_up.size = size;
  s_up.t_start_us = now;
  s_up.prepare_us = esp_timer_get_time() - now;
  s_up.last_activity_us = esp_timer_get_time();

  char chunk_url[64];
  snprintf(chunk_url, sizeof(chunk_url), "http://%s/chunk", rx.ip);
  esp_http_client_set_url(s_up.chunk_client, chunk_url);
  esp_http_client_set_timeout_ms(s_up.chunk_client, 10000);
  esp_http_client_set_header(s_up.chunk_client, "Content-Type", "application/octet-stream");
  esp_http_client_delete_header(s_up.chunk_client, "X-Force");
  if (s_up.stream) {
    esp_http_client_close(s_up.chunk_client); // chunks use the relay socket; /finalize reconnects
  }
  s_up.active = true;

  led_set_state(LED_STATE_SENDING);
  ESP_LOGI(TAG, "Stage prepare: %lld ms, session %s, relay %s", (long long)(s_up.prepare_us / 1000), s_up.id,
           s_up.stream ? "stream" : "buffered");
  strlcpy(session_out, s_up.id, session_len);
  return 200;
}

static int chunk_locked(const char *session, int index, const char *data, int len, int64_t recv_us,
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

  chunk_timing_reset();
  int connects_before = s_up.connects;
  int64_t t0 = esp_timer_get_time();
  esp_err_t err = esp_http_client_perform(s_up.chunk_client);
  int64_t rtt = esp_timer_get_time() - t0;
  if (s_up.connects != connects_before && index > 0) {
    ESP_LOGW(TAG, "Chunk %d: new TCP connection to the receiver (%d so far)", index, s_up.connects);
  }
  if (index < CHUNK_LOG_ALL || rtt > CHUNK_LOG_SLOW_MS * 1000) {
    ESP_LOGI(TAG, "CHUNK %d TX->RX: send start, headers sent +%lld ms, response +%lld ms, done +%lld ms", index,
             s_ct.headers_sent_us ? (long long)((s_ct.headers_sent_us - t0) / 1000) : -1LL,
             s_ct.first_resp_us ? (long long)((s_ct.first_resp_us - t0) / 1000) : -1LL, (long long)(rtt / 1000));
  }
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

  chunk_timing_log("buffered", index, len, recv_us, rtt);
  if (s_up.no_reuse) esp_http_client_close(s_up.chunk_client);

  if (index == s_up.next_index) {
    s_up.next_index++;
    s_up.acked += len;
    s_up.recv_total_us += recv_us;
    if (s_up.acked >= s_up.next_log) {
      s_up.next_log = s_up.acked + PROGRESS_LOG_BYTES;
      ESP_LOGI(TAG, "XFER %llu KB: browser->TX %.0f KB/s, TX->RX %.0f KB/s, overall %.0f KB/s",
               (unsigned long long)(s_up.acked / 1024), KBPS(s_up.acked, s_up.recv_total_us),
               KBPS(s_up.acked, s_up.chunk_total_us),
               KBPS(s_up.acked, esp_timer_get_time() - s_up.t_start_us - s_up.prepare_us));
    }
  }
  *acked_out = s_up.acked;
  return 200;
}

// ─── Streaming relay ────────────────────────────────────────────────────────
// esp_http_client can't reuse a keep-alive connection with open()/write(), so
// stream mode speaks HTTP/1.1 on a plain socket kept for the whole session.
static int relay_connect(const rx_target_t *rx) {
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(80)};
  if (inet_pton(AF_INET, rx->ip, &a.sin_addr) != 1) return -1;
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  struct timeval tv = {.tv_sec = 10};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
    close(fd);
    return -1;
  }
  s_up.connects++;
  return fd;
}

static bool send_all(int fd, const char *p, int n) {
  while (n > 0) {
    int w = send(fd, p, n, 0);
    if (w <= 0) return false;
    p += w;
    n -= w;
  }
  return true;
}

// Reads the receiver's reply; returns the HTTP status, -1 on error. Leaves the
// socket at the next request unless the receiver closes it.
static int relay_response(int fd, bool *keep) {
  char h[512];
  int n = 0;
  char *end = NULL;
  while (!end) {
    if (n >= (int)sizeof(h) - 1) return -1;
    int r = recv(fd, h + n, sizeof(h) - 1 - n, 0);
    if (r <= 0) return -1;
    n += r;
    h[n] = '\0';
    end = strstr(h, "\r\n\r\n");
  }
  int status = (strncmp(h, "HTTP/1.", 7) == 0) ? atoi(h + 9) : -1;
  int body = 0;
  for (char *p = h; p < end; p = strstr(p, "\r\n") + 2) {
    if (strncasecmp(p, "Content-Length:", 15) == 0) body = atoi(p + 15);
    if (strncasecmp(p, "Connection: close", 17) == 0) *keep = false;
    if (strncasecmp(p, "X-Rx-Timing:", 12) == 0) parse_rx_timing(p + 12);
  }
  body -= n - (int)(end + 4 - h); // body bytes already read with the header
  char sink[64];
  while (body > 0) {
    int r = recv(fd, sink, body < (int)sizeof(sink) ? body : (int)sizeof(sink), 0);
    if (r <= 0) return -1;
    body -= r;
  }
  return status;
}

static int chunk_stream_locked(const char *session, int index, int len, ft_read_fn read, void *ctx,
                               char *buf, int buf_len, uint64_t *acked_out, char *msg, size_t msg_len) {
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
  int64_t t0 = esp_timer_get_time();
  int64_t recv_us = 0;
  if (s_up.relay_fd < 0 && (s_up.relay_fd = relay_connect(&s_up.rx)) < 0) {
    snprintf(msg, msg_len, "Receiver unreachable");
    return 502;
  }
  char head[256];
  int hl = snprintf(head, sizeof(head),
                    "POST /chunk HTTP/1.1\r\nHost: %s\r\nX-Auth-Key: %s\r\nX-Chunk-Index: %d\r\n"
                    "Content-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n",
                    s_up.rx.ip, s_up.rx.key, index, len);
  bool ok = send_all(s_up.relay_fd, head, hl);
  int done = 0;
  while (ok && done < len) {
    int64_t r0 = esp_timer_get_time();
    int n = read(ctx, buf, len - done < buf_len ? len - done : buf_len);
    recv_us += esp_timer_get_time() - r0;
    if (n <= 0) {
      // Browser gone mid-chunk: the receiver sees a short body and writes nothing
      relay_close();
      snprintf(msg, msg_len, "Lost the browser connection during chunk %d", index);
      return 400;
    }
    ok = send_all(s_up.relay_fd, buf, n);
    done += n;
  }
  bool keep = true;
  chunk_timing_reset();
  int status = ok ? relay_response(s_up.relay_fd, &keep) : -1;
  if (status != 200 || !keep) relay_close();
  int64_t fwd_us = esp_timer_get_time() - t0 - recv_us;
  s_up.chunk_total_us += fwd_us;
  if (fwd_us > s_up.chunk_max_us) s_up.chunk_max_us = fwd_us;
  s_up.chunks++;
  if (status < 0) {
    snprintf(msg, msg_len, "Receiver unreachable (chunk %d)", index);
    ESP_LOGW(TAG, "%s", msg);
    return 502;
  }
  if (status != 200) {
    snprintf(msg, msg_len, "Receiver rejected chunk %d (HTTP %d)", index, status);
    ESP_LOGW(TAG, "%s", msg);
    return status == 409 ? 409 : 502;
  }
  chunk_timing_log("stream", index, len, recv_us, fwd_us);
  if (index == s_up.next_index) {
    s_up.next_index++;
    s_up.acked += len;
    s_up.recv_total_us += recv_us;
    if (s_up.acked >= s_up.next_log) {
      s_up.next_log = s_up.acked + PROGRESS_LOG_BYTES;
      ESP_LOGI(TAG, "XFER %llu KB (stream): browser->TX %.0f KB/s, TX->RX %.0f KB/s, overall %.0f KB/s",
               (unsigned long long)(s_up.acked / 1024), KBPS(s_up.acked, s_up.recv_total_us),
               KBPS(s_up.acked, s_up.chunk_total_us),
               KBPS(s_up.acked, esp_timer_get_time() - s_up.t_start_us - s_up.prepare_us));
    }
  }
  *acked_out = s_up.acked;
  return 200;
}

static int finish_locked(const char *session, const char *md5, char *msg, size_t msg_len) {
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
  // Hashed while sending: the browser's MD5 comes now (older callers gave it at begin)
  const char *check = (md5 && md5[0]) ? md5 : s_up.md5;
  if (strlen(check) != 32 || strspn(check, "0123456789abcdefABCDEF") != 32) {
    snprintf(msg, msg_len, "md5 must be 32 hex characters");
    return 400;
  }
  relay_close();

  char json[64];
  snprintf(json, sizeof(json), "{\"md5\":\"%s\"}", check); // browser MD5: end-to-end check
  int64_t t0 = esp_timer_get_time();
  // receiver: fsync, MD5 verify, rename, list snapshot, unmount (replies before USB reconnect)
  int status = ka_post("/finalize", json, false, 30000, msg, msg_len);
  if (status < 0) {
    // Kept connection went stale: once more on a new one
    status = rx_http(&s_up.rx, HTTP_METHOD_POST, "/finalize", json, false, 30000, msg, msg_len, NULL);
  }
  int64_t fin_us = esp_timer_get_time() - t0;

  int64_t total_us = esp_timer_get_time() - s_up.t_start_us;
  ESP_LOGI(TAG, "XFER summary -> %s (%s relay): %u bytes in %lld ms = %.1f KB/s | browser->TX %.0f KB/s, "
                "TX->RX %.0f KB/s | prepare %lld ms, "
                "%lu chunks (rtt avg %lld, max %lld ms, connections %d), finalize %lld ms | %s",
           s_up.rx.id, s_up.stream ? "stream" : "buffered", (unsigned)s_up.size, (long long)(total_us / 1000),
           KBPS(s_up.size, total_us),
           KBPS(s_up.acked, s_up.recv_total_us), KBPS(s_up.acked, s_up.chunk_total_us),
           (long long)(s_up.prepare_us / 1000), (unsigned long)s_up.chunks,
           (long long)(s_up.chunks ? s_up.chunk_total_us / s_up.chunks / 1000 : 0),
           (long long)(s_up.chunk_max_us / 1000), s_up.connects,
           (long long)(fin_us / 1000), status == 200 ? "OK" : "FAILED");

  if (status == 200) {
    // The cached state and timings are from before /finalize until the next
    // heartbeat (the receiver sends one as soon as the drive is back)
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", "FILE_READY");
    cJSON_AddNumberToObject(o, "reattach_ms", -1);
    cJSON_AddNumberToObject(o, "host_ms", -1);
    rx_registry_update_status(s_up.rx.id, o);
    cJSON_Delete(o);
  }
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
                        const char *relay, char *session_out, size_t session_len, char *msg, size_t msg_len) {
  up_lock();
  int r = begin_locked(rx_id, name, size, md5, force, relay, session_out, session_len, msg, msg_len);
  up_unlock();
  return r;
}

int file_transfer_chunk(const char *session, int index, const char *data, int len, int64_t recv_us,
                        uint64_t *acked_out, char *msg, size_t msg_len) {
  up_lock();
  int r = chunk_locked(session, index, data, len, recv_us, acked_out, msg, msg_len);
  up_unlock();
  return r;
}

bool file_transfer_streams(const char *session) {
  up_lock();
  bool s = session_matches(session) && s_up.stream;
  up_unlock();
  return s;
}

int file_transfer_chunk_stream(const char *session, int index, int len, ft_read_fn read, void *ctx,
                               char *buf, int buf_len, uint64_t *acked_out, char *msg, size_t msg_len) {
  up_lock();
  int r = chunk_stream_locked(session, index, len, read, ctx, buf, buf_len, acked_out, msg, msg_len);
  up_unlock();
  return r;
}

int file_transfer_finish(const char *session, const char *md5, char *msg, size_t msg_len) {
  up_lock();
  int r = finish_locked(session, md5, msg, msg_len);
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
  // Heartbeat cache only: never block a request handler on the receiver
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
  // Receiver body is {"deleted":[..],"skipped":[..],free_bytes,total_bytes} or an error text
  int status = rx_http(&rx, HTTP_METHOD_POST, "/delete", json, force, 20000, resp, resp_len, NULL);
  if (status == 200) {
    cJSON *r = cJSON_Parse(resp);
    if (r) rx_registry_update_status(rx_id, r); // free space now, not at the next heartbeat
    cJSON_Delete(r);
  }
  return status;
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

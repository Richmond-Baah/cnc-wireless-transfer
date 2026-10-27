#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"

#include "auth.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "file_transfer.h"
#include "espnow_pair.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "web_server.h"
#include "wifi_manager.h"
#include <stddef.h>

#ifndef CONFIG_CNC_DEFAULT_WIFI_SSID
#define CONFIG_CNC_DEFAULT_WIFI_SSID "CNC_WIRELESS"
#endif

#ifndef CONFIG_CNC_DEFAULT_WIFI_PASSWORD
#define CONFIG_CNC_DEFAULT_WIFI_PASSWORD "cnc12345"
#endif

#ifndef CONFIG_CNC_DEFAULT_WEB_USER
#define CONFIG_CNC_DEFAULT_WEB_USER "admin"
#endif

#ifndef CONFIG_CNC_DEFAULT_WEB_PASS
#define CONFIG_CNC_DEFAULT_WEB_PASS "admin123"
#endif

static const char *TAG = "WEB";
#define SCRATCH_BUFSIZE 8192
static char scratch_buf[SCRATCH_BUFSIZE];

static void restart_delayed_task(void *pvParameters);
static uint32_t s_restart_wait_ver; // non-zero: wait for receivers to fetch this config version

// ─── Auth Helper
// ────────────────────────────────────────────────────────────────
static bool is_authorized(httpd_req_t *req) {
  char auth_header[128];
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header,
                                  sizeof(auth_header)) != ESP_OK) {
    return false;
  }
  if (strncmp(auth_header, "Bearer ", 7) != 0) {
    return false;
  }
  return auth_validate_token(auth_header + 7);
}

// ─── Web UI ─────────────────────────────────────────────────────────────────
// One gzipped HTML (web/ bundled by tools/bundle_ui.py) embedded in the app.
// "/" redirects to /ui/<crc of the bundle>; that URL is cached for a year and
// a new build gets a new URL.
extern const uint8_t ui_gz_start[] asm("_binary_ui_html_gz_start");
extern const uint8_t ui_gz_end[] asm("_binary_ui_html_gz_end");
static char s_ui_path[16]; // "/ui/xxxxxxxx"

static esp_err_t ui_redirect_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", s_ui_path);
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t ui_get_handler(httpd_req_t *req) {
  if (strcmp(req->uri, s_ui_path) != 0) { // another build's URL
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return ui_redirect_handler(req);
  }
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
  return httpd_resp_send(req, (const char *)ui_gz_start, ui_gz_end - ui_gz_start);
}

// ─── Login
// ──────────────────────────────────────────────────────────────────────
static esp_err_t login_post_handler(httpd_req_t *req) {
  int total_len = req->content_len;
  int cur_len = 0;
  if (total_len >= SCRATCH_BUFSIZE) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
    return ESP_FAIL;
  }
  int timeouts = 0;
  while (cur_len < total_len) {
    int received =
        httpd_req_recv(req, scratch_buf + cur_len, total_len - cur_len);
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) // stalled client must not hold httpd
        continue;
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Failed to receive post data");
      return ESP_FAIL;
    }
    cur_len += received;
  }
  scratch_buf[cur_len] = '\0';

  cJSON *json = cJSON_Parse(scratch_buf);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *user = cJSON_GetObjectItem(json, "username");
  cJSON *pass = cJSON_GetObjectItem(json, "password");

  if (user && pass &&
      auth_check_credentials(user->valuestring, pass->valuestring)) {
    const char *token = auth_create_session();
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"token\":\"%s\"}", token);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid credentials");
  }
  cJSON_Delete(json);
  return ESP_OK;
}

// ─── Browser-driven chunked upload ──────────────────────────────────────────
// POST /upload_begin   {name, size, md5}           -> {session, chunk_size}
// POST /upload_chunk   X-Session, X-Chunk-Index, raw body (<= 32 KB) -> {acked}
// POST /upload_finish  X-Session                   -> {status:"verified"}
// POST /upload_cancel  X-Session (optional)        -> {status:"cancelled"}
// Each request returns only after the receiver has answered, so the browser
// drives pacing, retries and progress (acked bytes / total).

static char s_chunk_buf[UPLOAD_CHUNK_MAX];

// Sends an error with an arbitrary status code (httpd_resp_send_err only knows a few)
static void send_status_text(httpd_req_t *req, int code, const char *msg) {
  const char *status;
  switch (code) {
  case 400: status = "400 Bad Request"; break;
  case 401: status = "401 Unauthorized"; break;
  case 409: status = "409 Conflict"; break;
  case 410: status = "410 Gone"; break;
  case 413: status = "413 Payload Too Large"; break;
  case 423: status = "423 Locked"; break;
  case 502: status = "502 Bad Gateway"; break;
  case 503: status = "503 Service Unavailable"; break;
  case 504: status = "504 Gateway Timeout"; break;
  case 404: status = "404 Not Found"; break;
  case 507: status = "507 Insufficient Storage"; break;
  default:  status = "500 Internal Server Error"; break;
  }
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_sendstr(req, (msg && msg[0]) ? msg : status);
}

// Reads exactly content_len bytes into buf. Returns false on error/too large.
// Receiver selected by the UI: X-Rx-Id header, else ?rx= query. False if absent.
static bool get_rx_id(httpd_req_t *req, char *out, size_t len) {
  out[0] = '\0';
  if (httpd_req_get_hdr_value_str(req, "X-Rx-Id", out, len) == ESP_OK && out[0]) {
    return true;
  }
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "rx", out, len) == ESP_OK && out[0]) {
    return true;
  }
  out[0] = '\0';
  return false;
}

static bool force_requested(httpd_req_t *req) {
  char force_hdr[4] = {0};
  return httpd_req_get_hdr_value_str(req, "X-Force", force_hdr, sizeof(force_hdr)) == ESP_OK &&
         strcmp(force_hdr, "1") == 0;
}

#define BODY_RECV_TIMEOUTS 3 // x recv_wait_timeout, then give up on the client

static bool read_full_body(httpd_req_t *req, char *buf, size_t cap) {
  if (req->content_len > cap) {
    return false;
  }
  size_t got = 0;
  int timeouts = 0;
  while (got < req->content_len) {
    int r = httpd_req_recv(req, buf + got, req->content_len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts >= BODY_RECV_TIMEOUTS) {
        return false;
      }
      continue;
    }
    if (r <= 0) {
      return false;
    }
    got += r;
  }
  return true;
}

static bool get_session_header(httpd_req_t *req, char *out, size_t len) {
  return httpd_req_get_hdr_value_str(req, "X-Session", out, len) == ESP_OK && out[0];
}

static char s_begin_body[512]; // upload worker only

static esp_err_t upload_begin_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  if (req->content_len >= sizeof(s_begin_body) ||
      !read_full_body(req, s_begin_body, sizeof(s_begin_body) - 1)) {
    send_status_text(req, 400, "Invalid request body");
    return ESP_OK;
  }
  s_begin_body[req->content_len] = '\0';

  cJSON *json = cJSON_Parse(s_begin_body);
  cJSON *j_name = json ? cJSON_GetObjectItem(json, "name") : NULL;
  cJSON *j_size = json ? cJSON_GetObjectItem(json, "size") : NULL;
  cJSON *j_md5 = json ? cJSON_GetObjectItem(json, "md5") : NULL;
  const char *name = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : NULL;
  const char *md5 = (j_md5 && cJSON_IsString(j_md5)) ? j_md5->valuestring : NULL;
  double size = (j_size && cJSON_IsNumber(j_size)) ? j_size->valuedouble : -1;

  // Transmitter-side checks; the receiver also validates the filename
  const char *problem = NULL;
  if (!name || !md5 || size < 0) {
    problem = "Missing name, size or md5";
  } else if (strlen(name) == 0 || strlen(name) > 127) {
    problem = "Filename must be 1-127 characters";
  } else if (strlen(md5) != 32 || strspn(md5, "0123456789abcdefABCDEF") != 32) {
    problem = "md5 must be 32 hex characters";
  } else if (size > 0xFFFFFFFFu) {
    problem = "File too large";
  }
  if (problem) {
    cJSON_Delete(json);
    send_status_text(req, 400, problem);
    return ESP_OK;
  }

  bool force = force_requested(req);
  char rx_id[RX_ID_LEN];
  if (!get_rx_id(req, rx_id, sizeof(rx_id))) {
    cJSON_Delete(json);
    send_status_text(req, 400, "Select a receiver (X-Rx-Id)");
    return ESP_OK;
  }

  char session[24];
  char msg[160];
  int code = file_transfer_begin(rx_id, name, (size_t)size, md5, force, session, sizeof(session),
                                 msg, sizeof(msg));
  cJSON_Delete(json);

  if (code == 423) {
    httpd_resp_set_status(req, "423 Locked");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"error\":\"machine_busy\"}");
    return ESP_OK;
  }
  if (code == 507) {
    // Receiver's JSON {file_size, free_bytes, needed_bytes} passes through as-is
    httpd_resp_set_status(req, "507 Insufficient Storage");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
  }
  if (code != 200) {
    send_status_text(req, code, msg);
    return ESP_OK;
  }
  char resp[96];
  snprintf(resp, sizeof(resp), "{\"session\":\"%s\",\"chunk_size\":%d}", session, UPLOAD_CHUNK_MAX);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, resp);
  return ESP_OK;
}

static esp_err_t upload_chunk_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  char session[24] = {0};
  char idx_hdr[16] = {0};
  if (!get_session_header(req, session, sizeof(session)) ||
      httpd_req_get_hdr_value_str(req, "X-Chunk-Index", idx_hdr, sizeof(idx_hdr)) != ESP_OK) {
    send_status_text(req, 400, "Missing X-Session or X-Chunk-Index");
    return ESP_OK;
  }
  char *end = NULL;
  long index = strtol(idx_hdr, &end, 10);
  if (end == idx_hdr || *end != '\0' || index < 0) {
    send_status_text(req, 400, "Invalid X-Chunk-Index");
    return ESP_OK;
  }
  if (req->content_len == 0 || req->content_len > UPLOAD_CHUNK_MAX) {
    send_status_text(req, 413, "Chunk must be 1-32768 bytes");
    return ESP_OK;
  }
  if (!read_full_body(req, s_chunk_buf, sizeof(s_chunk_buf))) {
    send_status_text(req, 400, "Failed to read chunk from browser");
    return ESP_OK;
  }

  uint64_t acked = 0;
  char msg[160];
  int code = file_transfer_chunk(session, (int)index, s_chunk_buf, req->content_len,
                                 &acked, msg, sizeof(msg));
  if (code != 200) {
    send_status_text(req, code, msg);
    return ESP_OK;
  }
  char resp[48];
  snprintf(resp, sizeof(resp), "{\"acked\":%llu}", (unsigned long long)acked);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, resp);
  return ESP_OK;
}

static esp_err_t upload_finish_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  char session[24] = {0};
  if (!get_session_header(req, session, sizeof(session))) {
    send_status_text(req, 400, "Missing X-Session");
    return ESP_OK;
  }
  char msg[160];
  int code = file_transfer_finish(session, msg, sizeof(msg));
  if (code != 200) {
    send_status_text(req, code, msg);
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, "{\"status\":\"verified\"}");
  return ESP_OK;
}

static esp_err_t upload_cancel_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  char session[24] = {0};
  get_session_header(req, session, sizeof(session)); // optional
  int code = file_transfer_cancel(session);
  if (code != 200) {
    send_status_text(req, code, code == 410 ? "Session does not match the active upload"
                                            : "Receiver did not confirm cancel");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, "{\"status\":\"cancelled\"}");
  return ESP_OK;
}

// ─── Status
// ─────────────────────────────────────────────────────────────────────
static char s_status_buf[768];
static esp_err_t status_get_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  // Selected receiver's status (live when reachable, else last heartbeat)
  char rx_id[RX_ID_LEN];
  cJSON *obj = NULL;
  if (get_rx_id(req, rx_id, sizeof(rx_id)) &&
      file_transfer_get_receiver_status(rx_id, s_status_buf, sizeof(s_status_buf)) == 200) {
    obj = cJSON_Parse(s_status_buf);
  }
  if (obj == NULL) {
    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "state", "NO_RECEIVER");
  }
  cJSON_AddStringToObject(obj, "tx_fw", esp_app_get_description()->version);
  cJSON_AddStringToObject(obj, "tx_id", rx_registry_tx_id());

  // Router (STA) link
  char sta_state[16], sta_reason[48], sta_ip[16];
  wifi_get_sta_status(sta_state, sizeof(sta_state), sta_reason, sizeof(sta_reason),
                      sta_ip, sizeof(sta_ip));
  cJSON *sta = cJSON_AddObjectToObject(obj, "sta");
  cJSON_AddStringToObject(sta, "state", sta_state);
  cJSON_AddStringToObject(sta, "reason", sta_reason);
  cJSON_AddStringToObject(sta, "ip", sta_ip);

  char active[RX_ID_LEN];
  if (file_transfer_active_rx(active, sizeof(active))) {
    cJSON_AddStringToObject(obj, "upload_rx", active);
  }
  cJSON_AddNumberToObject(obj, "recovery_s", wifi_recovery_seconds_left());

  // Devices on this AP (receivers + phones/laptops) vs the SoftAP limit
  wifi_sta_list_t ap_sta;
  wifi_mode_t mode = WIFI_MODE_AP;
  esp_wifi_get_mode(&mode);
  cJSON_AddStringToObject(obj, "wifi_mode", mode == WIFI_MODE_APSTA ? "STA" : "AP");
  cJSON_AddNumberToObject(obj, "ap_clients", esp_wifi_ap_get_sta_list(&ap_sta) == ESP_OK ? ap_sta.num : 0);
  cJSON_AddNumberToObject(obj, "ap_max_clients", ESP_WIFI_MAX_CONN_NUM);

  char *out = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
  cJSON_free(out);
  return ESP_OK;
}

// ─── Config (shared by GET and POST)
// ─────────────────────────────────────────────────────────────────
// Buffer sizes match the readers in wifi_manager.c, auth.c, file_transfer.c.
typedef struct {
  char wifi_mode[8];
  char ap_ssid[32];
  char ap_pass[64];
  char sta_ssid[32];
  char sta_pass[64];
  char auth_user[32];
  char auth_pass[64];
} sys_config_t;

static const struct {
  const char *key;
  const char *ns;
  size_t offset;
  size_t size;
} s_cfg_fields[] = {
    {"wifi_mode", "wifi_cfg", offsetof(sys_config_t, wifi_mode), sizeof(((sys_config_t *)0)->wifi_mode)},
    {"ap_ssid", "wifi_cfg", offsetof(sys_config_t, ap_ssid), sizeof(((sys_config_t *)0)->ap_ssid)},
    {"ap_pass", "wifi_cfg", offsetof(sys_config_t, ap_pass), sizeof(((sys_config_t *)0)->ap_pass)},
    {"sta_ssid", "wifi_cfg", offsetof(sys_config_t, sta_ssid), sizeof(((sys_config_t *)0)->sta_ssid)},
    {"sta_pass", "wifi_cfg", offsetof(sys_config_t, sta_pass), sizeof(((sys_config_t *)0)->sta_pass)},
    {"auth_user", "sys_cfg", offsetof(sys_config_t, auth_user), sizeof(((sys_config_t *)0)->auth_user)},
    {"auth_pass", "sys_cfg", offsetof(sys_config_t, auth_pass), sizeof(((sys_config_t *)0)->auth_pass)},
};
#define CFG_FIELD(cfg, i) ((char *)(cfg) + s_cfg_fields[i].offset)
#define CFG_NUM_FIELDS (sizeof(s_cfg_fields) / sizeof(s_cfg_fields[0]))

static void config_load(sys_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  strlcpy(cfg->wifi_mode, "AP", sizeof(cfg->wifi_mode));
  strlcpy(cfg->ap_ssid, CONFIG_CNC_DEFAULT_WIFI_SSID, sizeof(cfg->ap_ssid));
  strlcpy(cfg->ap_pass, CONFIG_CNC_DEFAULT_WIFI_PASSWORD, sizeof(cfg->ap_pass));
  strlcpy(cfg->auth_user, CONFIG_CNC_DEFAULT_WEB_USER, sizeof(cfg->auth_user));
  strlcpy(cfg->auth_pass, CONFIG_CNC_DEFAULT_WEB_PASS, sizeof(cfg->auth_pass));

  for (size_t i = 0; i < CFG_NUM_FIELDS; i++) {
    nvs_handle_t h;
    if (nvs_open(s_cfg_fields[i].ns, NVS_READONLY, &h) != ESP_OK) {
      continue;
    }
    char tmp[64];
    size_t len = s_cfg_fields[i].size;
    // Only overwrite the default with a non-empty stored value
    if (nvs_get_str(h, s_cfg_fields[i].key, tmp, &len) == ESP_OK && tmp[0]) {
      strlcpy(CFG_FIELD(cfg, i), tmp, s_cfg_fields[i].size);
    }
    nvs_close(h);
  }
}

// ─── Config GET
// ─────────────────────────────────────────────────────────────────
// Passwords are never sent back; the form leaves them blank (= keep current).
static esp_err_t config_get_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  sys_config_t cfg;
  config_load(&cfg);

  // cJSON escapes quotes/backslashes in SSIDs; the old snprintf did not
  cJSON *obj = cJSON_CreateObject();
  cJSON_AddStringToObject(obj, "wifi_mode", cfg.wifi_mode);
  cJSON_AddStringToObject(obj, "ap_ssid", cfg.ap_ssid);
  cJSON_AddStringToObject(obj, "sta_ssid", cfg.sta_ssid);
  cJSON_AddStringToObject(obj, "auth_user", cfg.auth_user);
  char *json = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  memset(&cfg, 0, sizeof(cfg));
  if (json == NULL) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  cJSON_free(json);
  return ESP_OK;
}

// ─── Config POST (save Wi-Fi + credentials to NVS, then restart)
// ────────────────
// Every field: missing or blank = keep current value. After merging, AP SSID
// and admin user must be non-empty. An AP credential change bumps the config
// version; the restart then waits (max 30 s) until online receivers fetched it.
static esp_err_t config_post_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  int total_len = req->content_len;
  if (total_len >= SCRATCH_BUFSIZE || !read_full_body(req, scratch_buf, SCRATCH_BUFSIZE - 1)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
    return ESP_FAIL;
  }
  scratch_buf[total_len] = '\0';

  cJSON *json = cJSON_Parse(scratch_buf);
  if (!json) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  sys_config_t cfg;
  config_load(&cfg);

  bool changed[CFG_NUM_FIELDS] = {false};
  for (size_t i = 0; i < CFG_NUM_FIELDS; i++) {
    cJSON *item = cJSON_GetObjectItem(json, s_cfg_fields[i].key);
    const char *v = cJSON_IsString(item) ? item->valuestring : NULL;
    if (v == NULL || v[0] == '\0') {
      continue; // blank = keep current
    }
    // Reject values that would not fit the fixed buffers they are read back into
    if (strlen(v) >= s_cfg_fields[i].size) {
      char msg[64];
      snprintf(msg, sizeof(msg), "%s too long (max %u)", s_cfg_fields[i].key,
               (unsigned)(s_cfg_fields[i].size - 1));
      cJSON_Delete(json);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
      return ESP_FAIL;
    }
    strlcpy(CFG_FIELD(&cfg, i), v, s_cfg_fields[i].size);
    changed[i] = true;
  }
  cJSON_Delete(json);

  const char *problem = NULL;
  if (strcmp(cfg.wifi_mode, "AP") != 0 && strcmp(cfg.wifi_mode, "STA") != 0) {
    problem = "Wi-Fi mode must be AP or STA";
  } else if (cfg.ap_ssid[0] == '\0') {
    problem = "AP SSID is required";
  } else if (cfg.auth_user[0] == '\0') {
    problem = "Admin username is required";
  }
  if (problem) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, problem);
    return ESP_FAIL;
  }


  // Save only the fields that were provided
  for (size_t i = 0; i < CFG_NUM_FIELDS; i++) {
    if (!changed[i]) {
      continue;
    }
    nvs_handle_t h;
    if (nvs_open(s_cfg_fields[i].ns, NVS_READWRITE, &h) != ESP_OK) {
      memset(&cfg, 0, sizeof(cfg));
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed");
      return ESP_FAIL;
    }
    nvs_set_str(h, s_cfg_fields[i].key, CFG_FIELD(&cfg, i));
    nvs_commit(h);
    nvs_close(h);
  }
  memset(&cfg, 0, sizeof(cfg));

  // Paired receivers pull the new AP credentials (newer config_version in
  // their heartbeat reply) before this transmitter restarts on them.
  bool ap_changed = changed[1] || changed[2];
  if (ap_changed) {
    s_restart_wait_ver = rx_registry_bump_config_version();
  }

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_send(req, ap_changed
                      ? "{\"status\":\"ok\",\"message\":\"Restarting after receivers update (max 30 s)...\"}"
                      : "{\"status\":\"ok\",\"message\":\"Restarting...\"}",
                  HTTPD_RESP_USE_STRLEN);

  // Schedule restart task after response is sent
  static bool restart_scheduled = false;
  if (!restart_scheduled) {
    restart_scheduled = true;
    ESP_LOGI(TAG, "Config updated. Restarting%s...", ap_changed ? " after receivers fetch it" : " in 1 second");
    xTaskCreate(restart_delayed_task, "restart_task", 2048, NULL, 5, NULL);
  }
  return ESP_OK;
}

// Reads a small JSON body and checks it is {"confirm":"<word>"}.
// Sends 400 and returns false otherwise.
static bool body_confirms(httpd_req_t *req, const char *word) {
  char body[64] = {0};
  int cur_len = 0;
  if (req->content_len > 0 && req->content_len < (int)sizeof(body) &&
      read_full_body(req, body, sizeof(body) - 1)) {
    cur_len = req->content_len;
  }
  bool confirmed = false;
  if (cur_len == req->content_len && cur_len > 0) {
    cJSON *json = cJSON_Parse(body);
    cJSON *confirm = json ? cJSON_GetObjectItem(json, "confirm") : NULL;
    confirmed = confirm && confirm->valuestring && strcmp(confirm->valuestring, word) == 0;
    cJSON_Delete(json);
  }
  if (!confirmed) {
    char msg[48];
    snprintf(msg, sizeof(msg), "Confirmation required: type %s", word);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
  }
  return confirmed;
}

// Sends a receiver's reply on: JSON for 200/423, else text with the status.
static void send_rx_reply(httpd_req_t *req, int code, const char *body) {
  if (code == 200 || code == 423) {
    httpd_resp_set_status(req, code == 200 ? "200 OK" : "423 Locked");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, (body && body[0]) ? body : (code == 200 ? "{\"status\":\"ok\"}"
                                                                    : "{\"error\":\"machine_busy\"}"));
  } else {
    send_status_text(req, code < 0 ? 502 : code, body);
  }
}

// Receiver proxies run on the rx worker only (see dispatch())
static char s_rx_resp[4096];
static char s_rx_body[SCRATCH_BUFSIZE];

// Proxies a wipe to the selected receiver. Body must be {"confirm":"WIPE"};
// X-Force: 1 overrides the receiver's machine-busy guard.
static esp_err_t clear_post_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  if (!body_confirms(req, "WIPE")) {
    return ESP_FAIL;
  }
  char rx_id[RX_ID_LEN];
  if (!get_rx_id(req, rx_id, sizeof(rx_id))) {
    send_status_text(req, 400, "Select a receiver (X-Rx-Id)");
    return ESP_OK;
  }
  int code = file_transfer_clear_receiver(rx_id, force_requested(req), s_rx_resp, sizeof(s_rx_resp));
  send_rx_reply(req, code, s_rx_resp);
  return ESP_OK;
}

// Proxies {"files":[...]} to the selected receiver's /delete (validation
// happens there); X-Force: 1 overrides the machine-busy guard.
static esp_err_t delete_post_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  if (req->content_len >= SCRATCH_BUFSIZE || !read_full_body(req, s_rx_body, SCRATCH_BUFSIZE - 1)) {
    send_status_text(req, 400, "Invalid request body");
    return ESP_OK;
  }
  s_rx_body[req->content_len] = '\0';
  char rx_id[RX_ID_LEN];
  if (!get_rx_id(req, rx_id, sizeof(rx_id))) {
    send_status_text(req, 400, "Select a receiver (X-Rx-Id)");
    return ESP_OK;
  }
  int code = file_transfer_delete_receiver(rx_id, s_rx_body, force_requested(req), s_rx_resp,
                                           sizeof(s_rx_resp));
  send_rx_reply(req, code, s_rx_resp);
  return ESP_OK;
}

// Proxies a full storage format to the selected receiver. Body must be
// {"confirm":"FORMAT"} so a stray request can't wipe the card.
static esp_err_t format_post_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  if (!body_confirms(req, "FORMAT")) {
    return ESP_FAIL;
  }
  char rx_id[RX_ID_LEN];
  if (!get_rx_id(req, rx_id, sizeof(rx_id))) {
    send_status_text(req, 400, "Select a receiver (X-Rx-Id)");
    return ESP_OK;
  }
  int code = file_transfer_format_receiver(rx_id, s_rx_resp, sizeof(s_rx_resp));
  send_rx_reply(req, code, s_rx_resp);
  return ESP_OK;
}

static esp_err_t receiver_list_get_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  char rx_id[RX_ID_LEN];
  if (!get_rx_id(req, rx_id, sizeof(rx_id))) {
    send_status_text(req, 400, "Select a receiver (X-Rx-Id)");
    return ESP_OK;
  }
  char *list_buf = malloc(8192); // receiver caches up to 48 names
  if (!list_buf) {
    send_status_text(req, 500, "Out of memory");
    return ESP_OK;
  }
  int code = file_transfer_get_receiver_files(rx_id, list_buf, 8192);
  if (code == 200) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, list_buf);
  } else if (code == 502) {
    // Offline: an empty list the UI can render
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"files\":[],\"storage_available\":false,\"reason\":\"receiver_offline\"}");
  } else {
    send_status_text(req, code, list_buf);
  }
  free(list_buf);
  return ESP_OK;
}

// ─── Receivers: heartbeat, config pull, pairing management ──────────────────
// POST /rx/heartbeat (from receivers, no user auth): updates the registry.
// Reply tells the receiver whether it is paired and the AP config version.
static esp_err_t rx_heartbeat_handler(httpd_req_t *req) {
  char body[768];
  if (req->content_len <= 0 || req->content_len >= (int)sizeof(body) ||
      !read_full_body(req, body, sizeof(body) - 1)) {
    send_status_text(req, 400, "Invalid heartbeat");
    return ESP_OK;
  }
  body[req->content_len] = '\0';
  cJSON *hb = cJSON_Parse(body);
  bool paired = hb && rx_registry_heartbeat(hb);
  cJSON_Delete(hb);
  char resp[64];
  snprintf(resp, sizeof(resp), "{\"paired\":%s,\"config_version\":%lu}", paired ? "true" : "false",
           paired ? (unsigned long)rx_registry_config_version() : 0UL);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  // Free the socket at once: up to 16 receivers must not hold browser slots
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_sendstr(req, resp);
  httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
  return ESP_OK;
}

// GET /rx/config (receiver auth: X-Rx-Id + X-Auth-Key): AP credentials.
static esp_err_t rx_config_handler(httpd_req_t *req) {
  char id[RX_ID_LEN] = {0};
  char key[RX_KEY_HEX_LEN + 2] = {0};
  httpd_req_get_hdr_value_str(req, "X-Rx-Id", id, sizeof(id));
  httpd_req_get_hdr_value_str(req, "X-Auth-Key", key, sizeof(key));
  if (!rx_registry_check_key(id, key)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  sys_config_t cfg;
  config_load(&cfg);
  uint32_t ver = rx_registry_config_version();
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "ap_ssid", cfg.ap_ssid);
  cJSON_AddStringToObject(o, "ap_pass", cfg.ap_pass);
  cJSON_AddNumberToObject(o, "config_version", ver);
  memset(&cfg, 0, sizeof(cfg));
  char *out = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
  cJSON_free(out);
  rx_registry_mark_config_fetched(id, ver);
  ESP_LOGI(TAG, "%s fetched AP config v%lu", id, (unsigned long)ver);
  return ESP_OK;
}

// GET /receivers: paired receivers (with cached status) + newly found devices.
static esp_err_t receivers_get_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  char *out = rx_registry_list_json();
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_send(req, out ? out : "{\"receivers\":[],\"pending\":[]}", HTTPD_RESP_USE_STRLEN);
  cJSON_free(out);
  return ESP_OK;
}

// POST /receivers/approve|rename|unpair {"id":"rx-...","name":"..."}
static esp_err_t receivers_manage_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  char body[256];
  if (req->content_len <= 0 || req->content_len >= (int)sizeof(body) ||
      !read_full_body(req, body, sizeof(body) - 1)) {
    send_status_text(req, 400, "Invalid request body");
    return ESP_OK;
  }
  body[req->content_len] = '\0';
  cJSON *json = cJSON_Parse(body);
  cJSON *j_id = json ? cJSON_GetObjectItem(json, "id") : NULL;
  cJSON *j_name = json ? cJSON_GetObjectItem(json, "name") : NULL;
  const char *id = cJSON_IsString(j_id) ? j_id->valuestring : NULL;
  const char *name = cJSON_IsString(j_name) ? j_name->valuestring : NULL;

  char msg[128] = "";
  int code;
  if (id == NULL) {
    code = 400;
    snprintf(msg, sizeof(msg), "Missing id");
  } else if (strcmp(req->uri, "/receivers/approve") == 0) {
    code = rx_registry_approve(id, name, msg, sizeof(msg));
  } else if (strcmp(req->uri, "/receivers/rename") == 0) {
    code = rx_registry_rename(id, name, msg, sizeof(msg));
  } else {
    char active[RX_ID_LEN];
    if (file_transfer_active_rx(active, sizeof(active)) && strcmp(active, id) == 0) {
      code = 409;
      snprintf(msg, sizeof(msg), "Upload in progress to this receiver");
    } else {
      code = rx_registry_unpair(id, msg, sizeof(msg));
    }
  }
  cJSON_Delete(json);
  if (code == 200) {
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    if (msg[0]) cJSON_AddStringToObject(ok, "message", msg);
    char *out = cJSON_PrintUnformatted(ok);
    cJSON_Delete(ok);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, out ? out : "{\"status\":\"ok\"}");
    cJSON_free(out);
  } else {
    send_status_text(req, code, msg);
  }
  return ESP_OK;
}

// POST /receivers/add: opens the 120 s "Add machine" window (ESP-NOW pairing).
static esp_err_t receivers_add_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  esp_err_t err = espnow_pair_open_window(ESPNOW_PAIR_WINDOW_S);
  if (err != ESP_OK) {
    send_status_text(req, 500, "Could not start radio pairing");
    return ESP_OK;
  }
  char resp[48];
  snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"seconds\":%d}", ESPNOW_PAIR_WINDOW_S);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, resp);
  return ESP_OK;
}

// POST /pair_window/stop: closes the "Add machine" window immediately.
static esp_err_t pair_window_stop_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  espnow_pair_close_window();
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
  return ESP_OK;
}

// POST /receivers/pair_file {"name":"...","now":<browser epoch s>} -> pair.cfg
// (contains the AP password and a new key, pre-registered for 24 h).
static esp_err_t receivers_pair_file_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  char body[128];
  if (req->content_len <= 0 || req->content_len >= (int)sizeof(body) ||
      !read_full_body(req, body, sizeof(body) - 1)) {
    send_status_text(req, 400, "Invalid request body");
    return ESP_OK;
  }
  body[req->content_len] = '\0';
  cJSON *json = cJSON_Parse(body);
  cJSON *j_name = json ? cJSON_GetObjectItem(json, "name") : NULL;
  cJSON *j_now = json ? cJSON_GetObjectItem(json, "now") : NULL;
  char msg[96];
  char *out = NULL;
  int code = rx_registry_make_pair_file(cJSON_IsString(j_name) ? j_name->valuestring : NULL,
                                        cJSON_IsNumber(j_now) ? (int64_t)j_now->valuedouble : 0,
                                        &out, msg, sizeof(msg));
  cJSON_Delete(json);
  if (code != 200) {
    send_status_text(req, code, msg);
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"pair.cfg\"");
  httpd_resp_sendstr(req, out);
  memset(out, 0, strlen(out));
  cJSON_free(out);
  return ESP_OK;
}

// POST /recovery {"confirm":"RECOVERY"}: AP on factory credentials for 5 min
// so receivers that missed a credential change can rejoin and pull them.
#define RECOVERY_SECONDS 300
static esp_err_t recovery_post_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    send_status_text(req, 401, "Unauthorized");
    return ESP_OK;
  }
  if (!body_confirms(req, "RECOVERY")) {
    return ESP_FAIL;
  }
  // Reply first: switching the AP drops this browser's connection
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, "{\"status\":\"ok\",\"ssid\":\"" CONFIG_CNC_DEFAULT_WIFI_SSID "\",\"seconds\":300}");
  vTaskDelay(pdMS_TO_TICKS(300));
  wifi_recovery_start(RECOVERY_SECONDS);
  return ESP_OK;
}

// ─── Debug endpoint — shows connected stations and receiver connectivity
static esp_err_t debug_get_handler(httpd_req_t *req) {
  char resp[1024];
  int pos = 0;

  pos += snprintf(resp + pos, sizeof(resp) - pos, "<html><body style='background:#1a1a2e;color:#fff;font-family:monospace;padding:20px'>");
  pos += snprintf(resp + pos, sizeof(resp) - pos, "<h2>CNC Debug Info</h2>");

  // Show current Wi-Fi configuration
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_AP, &conf);
  pos += snprintf(resp + pos, sizeof(resp) - pos, "<h3>Current Broadcaster Config:</h3>");
  pos += snprintf(resp + pos, sizeof(resp) - pos, "<p style='color:yellow;font-size:1.2em'>ACTUAL SSID: [%s]</p>", (char*)conf.ap.ssid);
  pos += snprintf(resp + pos, sizeof(resp) - pos, "<p>Password: [%s]</p>", (char*)conf.ap.password);
  pos += snprintf(resp + pos, sizeof(resp) - pos, "<p>Channel: %d</p>", conf.ap.channel);
  wifi_sta_list_t sta_list;
  esp_wifi_ap_get_sta_list(&sta_list);
  pos += snprintf(resp + pos, sizeof(resp) - pos, "<h3>Connected Stations: %d</h3><ul>", sta_list.num);
  for (int i = 0; i < sta_list.num; i++) {
    pos += snprintf(resp + pos, sizeof(resp) - pos,
      "<li>" MACSTR " (RSSI: %d)</li>",
      MAC2STR(sta_list.sta[i].mac), sta_list.sta[i].rssi);
  }
  pos += snprintf(resp + pos, sizeof(resp) - pos, "</ul>");

  // Receivers known to this transmitter (no keys)
  char *list = rx_registry_list_json();
  pos += snprintf(resp + pos, sizeof(resp) - pos, "<h3>Receivers:</h3><pre>%.400s</pre>",
                  list ? list : "-");
  cJSON_free(list);

  pos += snprintf(resp + pos, sizeof(resp) - pos, "</body></html>");

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, resp, pos);
  return ESP_OK;
}

static void restart_delayed_task(void *pvParameters) {
  vTaskDelay(pdMS_TO_TICKS(1000));
  // Receivers heartbeat every 10 s and then pull /rx/config
  for (int i = 0; s_restart_wait_ver && i < 30 && !rx_registry_all_fetched(s_restart_wait_ver); i++) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  esp_restart();
}

// ─── Web Server Init
// ────────────────────────────────────────────────────────────
// ─── Routes ─────────────────────────────────────────────────────────────────
// Every URI goes through dispatch(), which times the handler (Kconfig
// CNC_HTTP_REQ_LOG) and reports slow ones. Anything that talks to a receiver
// runs on a worker task through an httpd async request, so the httpd task
// itself only does quick work: static files, /status, cached lists.
#define SLOW_HANDLER_MS 500

typedef enum {
  RT_SYNC,   // runs on the httpd task: must never wait on a receiver
  RT_UPLOAD, // upload worker: begin/chunk/finish/cancel (one session at a time)
  RT_RX,     // rx worker: list/delete/wipe/format, pairing, recovery
} route_kind_t;

typedef struct {
  const char *uri;
  httpd_method_t method;
  esp_err_t (*fn)(httpd_req_t *req);
  route_kind_t kind;
} route_t;

static const route_t s_routes[] = {
    // Web UI (old page URLs redirect too)
    {"/", HTTP_GET, ui_redirect_handler, RT_SYNC},
    {"/index.html", HTTP_GET, ui_redirect_handler, RT_SYNC},
    {"/upload.html", HTTP_GET, ui_redirect_handler, RT_SYNC},
    {"/ui/*", HTTP_GET, ui_get_handler, RT_SYNC},
    // UI API
    {"/login", HTTP_POST, login_post_handler, RT_SYNC},
    {"/status", HTTP_GET, status_get_handler, RT_SYNC},
    {"/ping", HTTP_GET, status_get_handler, RT_SYNC},
    {"/config", HTTP_GET, config_get_handler, RT_SYNC},
    {"/config", HTTP_POST, config_post_handler, RT_SYNC},
    {"/upload_begin", HTTP_POST, upload_begin_handler, RT_UPLOAD},
    {"/upload_chunk", HTTP_POST, upload_chunk_handler, RT_UPLOAD},
    {"/upload_finish", HTTP_POST, upload_finish_handler, RT_UPLOAD},
    {"/upload_cancel", HTTP_POST, upload_cancel_handler, RT_UPLOAD},
    {"/receiver_list", HTTP_GET, receiver_list_get_handler, RT_RX},
    {"/receiver_delete", HTTP_POST, delete_post_handler, RT_RX},
    {"/receiver_clear", HTTP_POST, clear_post_handler, RT_RX},
    {"/receiver_format", HTTP_POST, format_post_handler, RT_RX},
    {"/receivers", HTTP_GET, receivers_get_handler, RT_SYNC},
    {"/receivers/approve", HTTP_POST, receivers_manage_handler, RT_RX},
    {"/receivers/rename", HTTP_POST, receivers_manage_handler, RT_RX},
    {"/receivers/unpair", HTTP_POST, receivers_manage_handler, RT_RX},
    {"/receivers/add", HTTP_POST, receivers_add_handler, RT_SYNC},
    {"/receivers/pair_file", HTTP_POST, receivers_pair_file_handler, RT_SYNC},
    {"/pair_window/stop", HTTP_POST, pair_window_stop_handler, RT_SYNC},
    {"/recovery", HTTP_POST, recovery_post_handler, RT_RX},
    {"/debug", HTTP_GET, debug_get_handler, RT_SYNC},
    // Receivers
    {"/rx/heartbeat", HTTP_POST, rx_heartbeat_handler, RT_SYNC},
    {"/rx/config", HTTP_GET, rx_config_handler, RT_SYNC},
};
#define NUM_ROUTES (sizeof(s_routes) / sizeof(s_routes[0]))

static httpd_handle_t s_server;

#if CONFIG_CNC_HTTP_REQ_LOG
static int open_sockets(void) {
  int fds[CONFIG_LWIP_MAX_SOCKETS];
  size_t n = sizeof(fds) / sizeof(fds[0]);
  return httpd_get_client_list(s_server, &n, fds) == ESP_OK ? (int)n : -1;
}

static void log_request(httpd_req_t *req, int64_t t0_us, const char *how) {
  int64_t ms = (esp_timer_get_time() - t0_us) / 1000;
  const char *m = http_method_str((enum http_method)req->method);
  if (ms > SLOW_HANDLER_MS) {
    ESP_LOGW(TAG, "SLOW %s %s %lld ms%s, %d socket(s) open", m, req->uri, (long long)ms, how,
             open_sockets());
  } else {
    ESP_LOGI(TAG, "%s %s %lld ms%s, %d socket(s) open", m, req->uri, (long long)ms, how, open_sockets());
  }
}
#endif

// Async requests waiting for / running on a worker
#define WORKER_QUEUE_LEN 4
#define WORKER_STACK 6144

typedef struct {
  httpd_req_t *req; // async copy, completed by the worker
  const route_t *rt;
  int64_t t0_us;
} job_t;

static QueueHandle_t s_queue[3]; // indexed by route_kind_t (RT_SYNC unused)

// Everything but the versioned UI page is live data: never cached
static void set_cache_policy(httpd_req_t *req, const route_t *rt) {
  if (rt->fn != ui_get_handler) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  }
}

static void worker_task(void *arg) {
  QueueHandle_t q = (QueueHandle_t)arg;
  job_t j;
  for (;;) {
    if (xQueueReceive(q, &j, portMAX_DELAY) != pdTRUE) continue;
    set_cache_policy(j.req, j.rt);
    j.rt->fn(j.req);
#if CONFIG_CNC_HTTP_REQ_LOG
    log_request(j.req, j.t0_us, " (worker)");
#endif
    httpd_req_async_handler_complete(j.req);
  }
}

static esp_err_t dispatch(httpd_req_t *req) {
  const route_t *rt = (const route_t *)req->user_ctx;
  int64_t t0 = esp_timer_get_time();
  if (rt->kind != RT_SYNC) {
    // Hand over to the worker; the httpd task moves on to other sockets
    QueueHandle_t q = s_queue[rt->kind];
    httpd_req_t *copy = NULL;
    if (uxQueueSpacesAvailable(q) == 0 || httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
      ESP_LOGW(TAG, "%s: worker busy, 503", req->uri);
      send_status_text(req, 503, "Transmitter busy, try again");
      return ESP_OK;
    }
    job_t j = {.req = copy, .rt = rt, .t0_us = t0};
    if (xQueueSend(q, &j, 0) != pdTRUE) { // cannot happen: only this task enqueues
      send_status_text(copy, 503, "Transmitter busy, try again");
      httpd_req_async_handler_complete(copy);
    }
    return ESP_OK;
  }
  set_cache_policy(req, rt);
#if CONFIG_CNC_HTTP_REQ_LOG
  esp_err_t ret = rt->fn(req);
  log_request(req, t0, "");
  return ret;
#else
  return rt->fn(req);
#endif
}

static void start_workers(void) {
  s_queue[RT_UPLOAD] = xQueueCreate(WORKER_QUEUE_LEN, sizeof(job_t));
  s_queue[RT_RX] = xQueueCreate(WORKER_QUEUE_LEN, sizeof(job_t));
  xTaskCreate(worker_task, "http_upload", WORKER_STACK, s_queue[RT_UPLOAD], 5, NULL);
  xTaskCreate(worker_task, "http_rx", WORKER_STACK, s_queue[RT_RX], 5, NULL);
}

void web_server_start(void) {
  auth_init();
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = NUM_ROUTES;
  config.uri_match_fn = httpd_uri_match_wildcard; // "/ui/*"
  config.max_open_sockets = 7;
  config.lru_purge_enable = true; // reclaim stale keep-alive sockets so asset GETs are never refused
  config.recv_wait_timeout = 30; // large uploads
  config.send_wait_timeout = 30;
  config.stack_size = 8192;

  snprintf(s_ui_path, sizeof(s_ui_path), "/ui/%08lx",
           (unsigned long)esp_rom_crc32_le(0, ui_gz_start, ui_gz_end - ui_gz_start));
  start_workers();
  if (httpd_start(&s_server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Error starting Web server.");
    return;
  }
  for (size_t i = 0; i < NUM_ROUTES; i++) {
    httpd_uri_t u = {.uri = s_routes[i].uri, .method = s_routes[i].method, .handler = dispatch,
                     .user_ctx = (void *)&s_routes[i]};
    if (httpd_register_uri_handler(s_server, &u) != ESP_OK) {
      ESP_LOGE(TAG, "Could not register %s", s_routes[i].uri);
    }
  }
  ESP_LOGI(TAG, "Web server started on port %d (%u routes), UI %s (%u bytes gz)", config.server_port,
           (unsigned)NUM_ROUTES, s_ui_path, (unsigned)(ui_gz_end - ui_gz_start));
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_client.h"

#include "auth.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "file_transfer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "web_server.h"

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

#ifndef CONFIG_CNC_RECEIVER_IP
#define CONFIG_CNC_RECEIVER_IP "192.168.4.150"
#endif

static const char *TAG = "WEB";
#define SCRATCH_BUFSIZE 8192
static char scratch_buf[SCRATCH_BUFSIZE];

static void restart_delayed_task(void *pvParameters);

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

// ─── SPIFFS File Serve
// ──────────────────────────────────────────────────────────
static esp_err_t serve_spiffs_file(httpd_req_t *req, const char *filepath) {
  FILE *f = fopen(filepath, "rb");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file : %s", filepath);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }

  // Get file size
  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (file_size <= 0) {
    fclose(f);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Empty file");
    return ESP_FAIL;
  }

  // Set MIME type
  const char *ext = strrchr(filepath, '.');
  if (ext && strcmp(ext, ".html") == 0)
    httpd_resp_set_type(req, "text/html");
  else if (ext && strcmp(ext, ".css") == 0)
    httpd_resp_set_type(req, "text/css");
  else if (ext && (strcmp(ext, ".js") == 0))
    httpd_resp_set_type(req, "application/javascript");

  // Read entire file into memory and send in one shot.
  // All SPIFFS web assets are small (< 20KB), so this is safe.
  char *buf = (char *)malloc(file_size);
  if (buf == NULL) {
    ESP_LOGE(TAG, "malloc failed for %s (%ld bytes)", filepath, file_size);
    fclose(f);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }

  size_t read_len = fread(buf, 1, file_size, f);
  fclose(f);

  if ((long)read_len != file_size) {
    ESP_LOGE(TAG, "Short read on %s: got %u, expected %ld", filepath,
             (unsigned)read_len, file_size);
    free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read error");
    return ESP_FAIL;
  }

  // Single-shot send — sets Content-Length automatically, no chunked encoding
  esp_err_t ret = httpd_resp_send(req, buf, read_len);
  free(buf);
  return ret;
}

// ─── Static Asset Handlers
// ──────────────────────────────────────────────────────
static esp_err_t index_get_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/index.html");
}
static esp_err_t upload_get_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/upload.html");
}
static esp_err_t css_get_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/style.css");
}
static esp_err_t js_get_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/app.js");
}
static esp_err_t md5_get_handler(httpd_req_t *req) {
  return serve_spiffs_file(req, "/spiffs/spark-md5.min.js");
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
  while (cur_len < total_len) {
    int received =
        httpd_req_recv(req, scratch_buf + cur_len, total_len - cur_len);
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT)
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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid credentials");
  }
  cJSON_Delete(json);
  return ESP_OK;
}

// ─── Upload Handler (multipart/form-data)
// ───────────────────────────────────────
//
// The browser sends:  FormData { file: <blob>, filename: "...", md5: "..." }
// Content-Type: multipart/form-data; boundary=----...
//
// Approach: We read the full body into a temp file on SPIFFS, then extract the
// metadata from the multipart header fields, and finally forward the raw file
// data to the receiver via file_transfer_forward_to_receiver.
//
// For ESP32 with limited RAM, we use a simpler approach: the JS client sends
// the metadata as custom HTTP headers, and the body is the raw file binary.
// This avoids multipart parsing entirely.
//
// Expected from app.js:
//   X-Filename: job.nc
//   X-Filesize: 102400
//   X-MD5: abc123...
//   Body: raw file bytes
//
static esp_err_t upload_post_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  char filename[128] = {0};
  char md5[64] = {0};
  char filesize_str[32] = {0};

  if (httpd_req_get_hdr_value_str(req, "X-Filename", filename,
                                  sizeof(filename)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing X-Filename header");
    return ESP_FAIL;
  }
  if (httpd_req_get_hdr_value_str(req, "X-MD5", md5, sizeof(md5)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing X-MD5 header");
    return ESP_FAIL;
  }
  if (httpd_req_get_hdr_value_str(req, "X-Filesize", filesize_str,
                                  sizeof(filesize_str)) != ESP_OK) {
    snprintf(filesize_str, sizeof(filesize_str), "%d", req->content_len);
  }

  size_t filesize = (size_t)atoi(filesize_str);
  if (filesize == 0)
    filesize = req->content_len;

  ESP_LOGI(TAG, "Upload request: file=%s size=%u md5=%s", filename,
           (unsigned)filesize, md5);

  bool success =
      file_transfer_forward_to_receiver(req, filename, md5, filesize);
  if (!success && file_transfer_is_aborted()) {
    file_transfer_clear_abort();
    return ESP_OK;
  }
  return success ? ESP_OK : ESP_FAIL;
}

// ─── Status
// ─────────────────────────────────────────────────────────────────────
static esp_err_t status_get_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  char state_str[256] = {0}; // matches STATUS_CACHE_LEN; receiver JSON can exceed 128
  file_transfer_get_receiver_status(state_str, sizeof(state_str));

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, state_str, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// ─── Config GET
// ─────────────────────────────────────────────────────────────────
static esp_err_t config_get_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  nvs_handle_t nvs_handle;
  char wifi_mode[8] = "AP";
  char ap_ssid[32] = CONFIG_CNC_DEFAULT_WIFI_SSID;
  char ap_pass[64] = CONFIG_CNC_DEFAULT_WIFI_PASSWORD;
  char sta_ssid[32] = "";
  char sta_pass[64] = "";

  if (nvs_open("wifi_cfg", NVS_READONLY, &nvs_handle) == ESP_OK) {
    size_t len = sizeof(wifi_mode);
    nvs_get_str(nvs_handle, "wifi_mode", wifi_mode, &len);
    len = sizeof(ap_ssid);
    nvs_get_str(nvs_handle, "ap_ssid", ap_ssid, &len);
    len = sizeof(ap_pass);
    nvs_get_str(nvs_handle, "ap_pass", ap_pass, &len);
    len = sizeof(sta_ssid);
    nvs_get_str(nvs_handle, "sta_ssid", sta_ssid, &len);
    len = sizeof(sta_pass);
    nvs_get_str(nvs_handle, "sta_pass", sta_pass, &len);
    nvs_close(nvs_handle);
  }

  char auth_user[32] = CONFIG_CNC_DEFAULT_WEB_USER;
  char auth_pass[64] = CONFIG_CNC_DEFAULT_WEB_PASS;
  char receiver_ip[32] = CONFIG_CNC_RECEIVER_IP;

  if (nvs_open("sys_cfg", NVS_READONLY, &nvs_handle) == ESP_OK) {
    size_t len = sizeof(auth_user);
    nvs_get_str(nvs_handle, "auth_user", auth_user, &len);
    len = sizeof(auth_pass);
    nvs_get_str(nvs_handle, "auth_pass", auth_pass, &len);
    len = sizeof(receiver_ip);
    nvs_get_str(nvs_handle, "receiver_ip", receiver_ip, &len);
    nvs_close(nvs_handle);
  }

  char resp[768];
  snprintf(resp, sizeof(resp),
           "{\"wifi_mode\":\"%s\",\"ap_ssid\":\"%s\",\"ap_pass\":\"%s\","
           "\"sta_ssid\":\"%s\",\"sta_pass\":\"%s\","
           "\"auth_user\":\"%s\",\"auth_pass\":\"%s\",\"receiver_ip\":\"%s\"}",
           wifi_mode, ap_ssid, ap_pass, sta_ssid, sta_pass, auth_user,
           auth_pass, receiver_ip);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// ─── Config POST (save Wi-Fi + credentials to NVS, then restart)
// ────────────────
static esp_err_t config_post_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  int total_len = req->content_len;
  int cur_len = 0;
  if (total_len >= SCRATCH_BUFSIZE) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
    return ESP_FAIL;
  }
  while (cur_len < total_len) {
    int received =
        httpd_req_recv(req, scratch_buf + cur_len, total_len - cur_len);
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT)
        continue;
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Receive error");
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

  // Forward AP settings to receiver if receiver is online
  cJSON *j_ap_ssid = cJSON_GetObjectItem(json, "ap_ssid");
  cJSON *j_ap_pass = cJSON_GetObjectItem(json, "ap_pass");

  if (file_transfer_is_receiver_online() && (j_ap_ssid || j_ap_pass)) {
      char rec_url[128];
      extern void get_receiver_url(const char *path, char *out_url, size_t max_len);
      get_receiver_url("/settings", rec_url, sizeof(rec_url));

      ESP_LOGI(TAG, "Syncing settings with receiver at %s", rec_url);
      esp_http_client_config_t client_cfg = {
          .url = rec_url,
          .method = HTTP_METHOD_POST,
          .timeout_ms = 2000,
      };
      esp_http_client_handle_t client = esp_http_client_init(&client_cfg);
      if (client) {
          esp_http_client_set_post_field(client, scratch_buf, strlen(scratch_buf));
          esp_http_client_set_header(client, "Content-Type", "application/json");
          esp_http_client_perform(client);
          esp_http_client_cleanup(client);
      }
  }

  // Update local NVS
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS wifi_cfg fail");
    return ESP_FAIL;
  }

  cJSON *j_wifi_mode = cJSON_GetObjectItem(json, "wifi_mode");
  cJSON *j_sta_ssid = cJSON_GetObjectItem(json, "sta_ssid");
  cJSON *j_sta_pass = cJSON_GetObjectItem(json, "sta_pass");

  if (j_wifi_mode && j_wifi_mode->valuestring)
    nvs_set_str(nvs_handle, "wifi_mode", j_wifi_mode->valuestring);
  if (j_ap_ssid && j_ap_ssid->valuestring)
    nvs_set_str(nvs_handle, "ap_ssid", j_ap_ssid->valuestring);
  if (j_ap_pass && j_ap_pass->valuestring && strlen(j_ap_pass->valuestring) > 0)
    nvs_set_str(nvs_handle, "ap_pass", j_ap_pass->valuestring);
  if (j_sta_ssid && j_sta_ssid->valuestring)
    nvs_set_str(nvs_handle, "sta_ssid", j_sta_ssid->valuestring);
  if (j_sta_pass && j_sta_pass->valuestring && strlen(j_sta_pass->valuestring) > 0)
    nvs_set_str(nvs_handle, "sta_pass", j_sta_pass->valuestring);

  nvs_commit(nvs_handle);
  nvs_close(nvs_handle);

  err = nvs_open("sys_cfg", NVS_READWRITE, &nvs_handle);
  if (err == ESP_OK) {
    cJSON *auth_user = cJSON_GetObjectItem(json, "auth_user");
    cJSON *auth_pass = cJSON_GetObjectItem(json, "auth_pass");
    cJSON *receiver_ip = cJSON_GetObjectItem(json, "receiver_ip");

    if (auth_user && auth_user->valuestring)
      nvs_set_str(nvs_handle, "auth_user", auth_user->valuestring);
    if (auth_pass && auth_pass->valuestring &&
        strlen(auth_pass->valuestring) > 0)
      nvs_set_str(nvs_handle, "auth_pass", auth_pass->valuestring);
    if (receiver_ip && receiver_ip->valuestring)
      nvs_set_str(nvs_handle, "receiver_ip", receiver_ip->valuestring);

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
  }

  cJSON_Delete(json);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Restarting...\"}",
                  HTTPD_RESP_USE_STRLEN);

  // Schedule restart task after response is sent
  static bool restart_scheduled = false;
  if (!restart_scheduled) {
    restart_scheduled = true;
    ESP_LOGI(TAG, "Config updated. Restarting in 1 second...");
    xTaskCreate(restart_delayed_task, "restart_task", 2048, NULL, 5, NULL);
  }
  return ESP_OK;
}

static esp_err_t clear_post_handler(httpd_req_t *req) {
  if (!is_authorized(req)) {
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    return ESP_FAIL;
  }

  if (!file_transfer_is_receiver_online()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receiver offline");
    return ESP_FAIL;
  }

  // We call the clear endpoint to delete all files from the SD card.
  int code = file_transfer_clear_receiver();
  if (code == 200) {
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
  } else {
    char err[64];
    snprintf(err, sizeof(err), "{\"status\":\"error\",\"code\":%d}", code);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err);
  }
  return ESP_OK;
}

static esp_err_t upload_cancel_handler(httpd_req_t *req) {
    if (!is_authorized(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }
    file_transfer_abort();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"aborting\"}");
    return ESP_OK;
}

static esp_err_t receiver_list_get_handler(httpd_req_t *req) {
    if (!is_authorized(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }

    if (!file_transfer_is_receiver_online()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"files\":[],\"storage_available\":false,\"reason\":\"receiver_offline\"}");
        return ESP_OK;
    }

    char *list_buf = malloc(4096);
    if (!list_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int code = file_transfer_get_receiver_files(list_buf, 4096);
    if (code == 200) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, list_buf);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receiver list failed");
    }

    free(list_buf);
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

  // Show target receiver IP
  char ip[32] = CONFIG_CNC_RECEIVER_IP;
  nvs_handle_t nvs_handle;
  if (nvs_open("sys_cfg", NVS_READONLY, &nvs_handle) == ESP_OK) {
    size_t len = sizeof(ip);
    nvs_get_str(nvs_handle, "receiver_ip", ip, &len);
    nvs_close(nvs_handle);
  }
  pos += snprintf(resp + pos, sizeof(resp) - pos, "<h3>Target Receiver IP: %s (Reserved)</h3>", ip);

  // Test receiver connectivity
  char test_url[128];
  snprintf(test_url, sizeof(test_url), "http://%s/status", ip);
  pos += snprintf(resp + pos, sizeof(resp) - pos, "<h3>Receiver Test:</h3>");

  esp_http_client_config_t cfg = { .url = test_url, .timeout_ms = 3000 };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    int code = esp_http_client_get_status_code(client);
    pos += snprintf(resp + pos, sizeof(resp) - pos, "<p style='color:lime'>REACHABLE (HTTP %d)</p>", code);
  } else {
    pos += snprintf(resp + pos, sizeof(resp) - pos, "<p style='color:red'>UNREACHABLE: %s</p>", esp_err_to_name(err));
  }
  esp_http_client_cleanup(client);

  pos += snprintf(resp + pos, sizeof(resp) - pos, "</body></html>");

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, resp, pos);
  return ESP_OK;
}

static void restart_delayed_task(void *pvParameters) {
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
}

// ─── Web Server Init
// ────────────────────────────────────────────────────────────
void web_server_start(void) {
  auth_init();
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 16;
  config.max_open_sockets = 7;
  config.recv_wait_timeout = 30; // large uploads
  config.send_wait_timeout = 30;
  config.stack_size = 8192;

  if (httpd_start(&server, &config) == ESP_OK) {
    // Static assets
    httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = index_get_handler};
    httpd_uri_t uri_index_html = {
        .uri = "/index.html", .method = HTTP_GET, .handler = index_get_handler};
    httpd_uri_t uri_upload_html = {.uri = "/upload.html",
                                   .method = HTTP_GET,
                                   .handler = upload_get_handler};
    httpd_uri_t uri_css = {
        .uri = "/style.css", .method = HTTP_GET, .handler = css_get_handler};
    httpd_uri_t uri_js = {
        .uri = "/app.js", .method = HTTP_GET, .handler = js_get_handler};
    httpd_uri_t uri_md5 = {.uri = "/spark-md5.min.js",
                           .method = HTTP_GET,
                           .handler = md5_get_handler};

    // APIs
    httpd_uri_t uri_login = {
        .uri = "/login", .method = HTTP_POST, .handler = login_post_handler};
    httpd_uri_t uri_upload = {
        .uri = "/upload", .method = HTTP_POST, .handler = upload_post_handler};
    httpd_uri_t uri_status = {
        .uri = "/status", .method = HTTP_GET, .handler = status_get_handler};
    httpd_uri_t uri_config_get = {
        .uri = "/config", .method = HTTP_GET, .handler = config_get_handler};
    httpd_uri_t uri_config_post = {
        .uri = "/config", .method = HTTP_POST, .handler = config_post_handler};
    httpd_uri_t uri_clear = {
        .uri = "/receiver_clear", .method = HTTP_POST, .handler = clear_post_handler};
    httpd_uri_t uri_upload_cancel = {
        .uri = "/upload_cancel", .method = HTTP_POST, .handler = upload_cancel_handler};
    httpd_uri_t uri_receiver_list = {
        .uri = "/receiver_list", .method = HTTP_GET, .handler = receiver_list_get_handler};
    httpd_uri_t uri_ping = {
        .uri = "/ping", .method = HTTP_GET, .handler = status_get_handler}; // Reuse status handler (returns ok)

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_index_html);
    httpd_register_uri_handler(server, &uri_upload_html);
    httpd_register_uri_handler(server, &uri_css);
    httpd_register_uri_handler(server, &uri_js);
    httpd_register_uri_handler(server, &uri_md5);
    httpd_register_uri_handler(server, &uri_login);
    httpd_register_uri_handler(server, &uri_upload);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_config_get);
    httpd_register_uri_handler(server, &uri_config_post);
    httpd_register_uri_handler(server, &uri_clear);
    httpd_register_uri_handler(server, &uri_upload_cancel);
    httpd_register_uri_handler(server, &uri_receiver_list);
    httpd_register_uri_handler(server, &uri_ping);

    httpd_uri_t uri_debug = {
        .uri = "/debug", .method = HTTP_GET, .handler = debug_get_handler};
    httpd_register_uri_handler(server, &uri_debug);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
  } else {
    ESP_LOGE(TAG, "Error starting Web server.");
  }
}

#include "file_receiver.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "mbedtls/md5.h"
#include "state_machine.h"
#include "usb_msc.h"
#include "storage_manager.h"
#include "sd_card.h"
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <dirent.h>
#include <unistd.h>
#include "nvs.h"
#include "esp_system.h"
#include "esp_random.h"
static const char *TAG = "FILE_RCV";
#define STORAGE_PATH storage_manager_get_base_path()

static FILE *active_file = NULL;
static char active_filename[256] = {0}; // temporary filename during transfer (.tmp)
static char final_filename[256] = {0};  // target filename after successful verification
static char expected_md5[33] = {0};
static mbedtls_md5_context active_md5_ctx;

#define RECEIVER_CHUNK_BUFSIZE 16384
static char chunk_buf[RECEIVER_CHUNK_BUFSIZE];
static int chunk_count = 0;
static int expected_chunk_index = 0;

// ERROR exit for an in-flight transfer: drop the partial staging file, release
// FatFS and hand the storage back to the USB host so the machine keeps its drive.
static void fail_transfer_and_release(void) {
    if (active_file) {
        fclose(active_file);
        active_file = NULL;
        remove(active_filename);
    }
    mbedtls_md5_free(&active_md5_ctx);
    state_machine_transition(STATE_ERROR);
    storage_manager_flush_and_release_for_usb();
    usb_msc_connect();
}

// Discard any unread request body so the connection stays usable.
static void drain_request_body(httpd_req_t *req) {
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(chunk_buf) ? remaining : (int)sizeof(chunk_buf);
        int r = httpd_req_recv(req, chunk_buf, to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        remaining -= r;
    }
}

// Helper to sanitize filename and prevent path traversal
static void sanitize_filename(const char *input, char *output, size_t max_len) {
    const char *basename = strrchr(input, '/');
    if (!basename) {
        basename = strrchr(input, '\\');
    }
    if (basename) {
        basename++;
    } else {
        basename = input;
    }
    
    // Remove leading dots or hidden prefixes
    while (*basename == '.' || *basename == ' ') {
        basename++;
    }

    if (strlen(basename) == 0) {
        snprintf(output, max_len, "job_%ul.nc", (unsigned)esp_random());
    } else {
        strncpy(output, basename, max_len - 1);
        output[max_len - 1] = '\0';
    }
}

// Cleanup any orphan temporary files on startup
static void cleanup_orphan_temp_files(void) {
    DIR *dir = opendir(STORAGE_PATH);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            if (strstr(entry->d_name, ".tmp") != NULL) {
                char filepath[320];
                snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_PATH, entry->d_name);
                ESP_LOGI(TAG, "Removing orphan temp file: %s", filepath);
                remove(filepath);
            }
        }
    }
    closedir(dir);
}

static esp_err_t prepare_post_handler(httpd_req_t *req) {
    app_state_t cur = state_machine_get();
    if (cur == STATE_RECEIVING) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Receiver Busy", -1);
        return ESP_FAIL;
    }
    if (cur == STATE_USB_ACTIVE) {
        ESP_LOGI(TAG, "USB is active, will disconnect host for wireless transfer...");
    }

    int total_len = req->content_len;
    if (total_len >= (int)sizeof(chunk_buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, chunk_buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive Error");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    chunk_buf[cur_len] = '\0';

    cJSON *json = cJSON_Parse(chunk_buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *fname = cJSON_GetObjectItem(json, "filename");
    cJSON *md5 = cJSON_GetObjectItem(json, "md5");

    if (!fname || !fname->valuestring || !md5 || !md5->valuestring) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename or md5");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    if (!state_machine_transition(STATE_RECEIVING)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "State transition failed");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    char sanitized[128];
    sanitize_filename(fname->valuestring, sanitized, sizeof(sanitized));

    if (active_file) {
        fclose(active_file);
        mbedtls_md5_free(&active_md5_ctx);
        active_file = NULL;
    }

    // Step 1: Disconnect USB so the host stops reading raw SD sectors
    usb_msc_disconnect();

    // Step 2: Ensure the target storage filesystem is mounted before opening.
    // The SD card is the transfer target when present; internal flash is the
    // fallback. FatFS and USB MSC are strictly exclusive: USB was just
    // disconnected, so we are free to mount FatFS now. Never mount the SD
    // card while the USB host could still be reading it.
    if (!storage_manager_fatfs_available() || (storage_manager_is_sd_present() && !sd_card_is_mounted())) {
        storage_manager_remount();
    }
    if (!storage_manager_fatfs_available()) {
        ESP_LOGE(TAG, "No storage filesystem available for transfer");
        fail_transfer_and_release();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "File system error");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    // Build paths only after the active storage path has been resolved
    snprintf(final_filename, sizeof(final_filename), "%s/%s", STORAGE_PATH, sanitized);
    snprintf(active_filename, sizeof(active_filename), "%s/%s.tmp", STORAGE_PATH, sanitized);
    strncpy(expected_md5, md5->valuestring, sizeof(expected_md5) - 1);
    expected_md5[sizeof(expected_md5) - 1] = '\0';

    // Step 3: Open the temporary staging file for writing with a retry
    active_file = fopen(active_filename, "wb");
    if (!active_file) {
        ESP_LOGW(TAG, "First fopen failed, remounting storage and retrying...");
        storage_manager_remount();
        // Rebuild paths in case the storage path changed after the remount
        snprintf(final_filename, sizeof(final_filename), "%s/%s", STORAGE_PATH, sanitized);
        snprintf(active_filename, sizeof(active_filename), "%s/%s.tmp", STORAGE_PATH, sanitized);
        active_file = fopen(active_filename, "wb");
    }

    if (!active_file) {
        ESP_LOGE(TAG, "Failed to open temporary file for writing: %s", active_filename);
        fail_transfer_and_release();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "File system error");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    mbedtls_md5_init(&active_md5_ctx);
    mbedtls_md5_starts(&active_md5_ctx);
    chunk_count = 0;
    expected_chunk_index = 0;

    ESP_LOGI(TAG, "Ready to receive staging file: %s (final: %s, expected MD5: %s)", active_filename, final_filename, expected_md5);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);

    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t chunk_post_handler(httpd_req_t *req) {
    if (state_machine_get() != STATE_RECEIVING || !active_file) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not in RECEIVING state");
        return ESP_FAIL;
    }

    char idx_hdr[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Chunk-Index", idx_hdr, sizeof(idx_hdr)) != ESP_OK) {
        drain_request_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing X-Chunk-Index");
        return ESP_FAIL;
    }
    char *idx_end = NULL;
    long chunk_index = strtol(idx_hdr, &idx_end, 10);
    if (idx_end == idx_hdr || *idx_end != '\0' || chunk_index < 0) {
        drain_request_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid X-Chunk-Index");
        return ESP_FAIL;
    }

    if (chunk_index < expected_chunk_index) {
        // Retry of a chunk already written (its response was lost): acknowledge, don't re-append.
        ESP_LOGW(TAG, "Duplicate chunk %ld (expected %d) ignored", chunk_index, expected_chunk_index);
        drain_request_body(req);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"duplicate\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (chunk_index > expected_chunk_index) {
        ESP_LOGE(TAG, "Out-of-order chunk %ld (expected %d) rejected", chunk_index, expected_chunk_index);
        drain_request_body(req);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Out-of-order chunk", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(chunk_buf) ? remaining : (int)sizeof(chunk_buf);
        int read_len = httpd_req_recv(req, chunk_buf, to_read);
        if (read_len <= 0) {
            if (read_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Error reading chunk data");
            fail_transfer_and_release();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Chunk read error");
            return ESP_FAIL;
        }

        size_t written = fwrite(chunk_buf, 1, read_len, active_file);
        if ((int)written != read_len) {
            ESP_LOGE(TAG, "Write error (wrote %d of %d)", (int)written, read_len);
            fail_transfer_and_release();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
            return ESP_FAIL;
        }
        
        // Flush every 16 chunks (approx 256KB) to improve speed while maintaining some safety
        if (++chunk_count % 16 == 0) {
            fflush(active_file);
            fsync(fileno(active_file));
        }

        mbedtls_md5_update(&active_md5_ctx, (const unsigned char *)chunk_buf, read_len);
        remaining -= read_len;
    }
    expected_chunk_index++;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t finalize_post_handler(httpd_req_t *req) {
    if (state_machine_get() != STATE_RECEIVING || !active_file) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not in RECEIVING state");
        return ESP_FAIL;
    }

    int total_len = req->content_len;
    char finalize_md5[33] = {0};

    if (total_len > 0 && total_len < (int)sizeof(chunk_buf)) {
        int cur_len = 0;
        while (cur_len < total_len) {
            int received = httpd_req_recv(req, chunk_buf + cur_len, total_len - cur_len);
            if (received <= 0) {
                if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Error reading finalize body");
            fail_transfer_and_release();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Chunk read error");
            return ESP_FAIL;
            }
            cur_len += received;
        }
        chunk_buf[cur_len] = '\0';

        cJSON *json = cJSON_Parse(chunk_buf);
        if (json) {
            cJSON *md5_item = cJSON_GetObjectItem(json, "md5");
            if (md5_item && md5_item->valuestring) {
                strncpy(finalize_md5, md5_item->valuestring, sizeof(finalize_md5) - 1);
            }
            cJSON_Delete(json);
        }
    }

    fflush(active_file);
    fsync(fileno(active_file));
    long final_size = ftell(active_file);
    fclose(active_file);
    active_file = NULL;

    unsigned char hash[16];
    mbedtls_md5_finish(&active_md5_ctx, hash);
    mbedtls_md5_free(&active_md5_ctx);

    char computed_md5[33] = {0};
    for (int i = 0; i < 16; i++) {
        snprintf(&computed_md5[i * 2], 3, "%02x", hash[i]);
    }

    ESP_LOGI(TAG, "Finalize: local MD5=%s | size=%ld | path=%s",
             computed_md5, final_size, active_filename);

    const char *check_against = strlen(finalize_md5) > 0 ? finalize_md5 : expected_md5;

    if (strcasecmp(computed_md5, check_against) != 0) {
        ESP_LOGE(TAG, "MD5 MISMATCH! Deleting corrupted temp file: %s", active_filename);
        remove(active_filename);
        state_machine_transition(STATE_ERROR);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Checksum mismatch - file deleted");
        // Release FatFS back to raw USB mode before reconnecting the host
        storage_manager_flush_and_release_for_usb();
        usb_msc_connect();
        return ESP_FAIL;
    }

    // ATOMIC COMMIT: Remove existing final file if present, then rename .tmp -> final
    remove(final_filename);
    if (rename(active_filename, final_filename) != 0) {
        ESP_LOGE(TAG, "Failed to atomically rename %s to %s", active_filename, final_filename);
        remove(active_filename);
        state_machine_transition(STATE_ERROR);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "File commit error");
        // Release FatFS back to raw USB mode before reconnecting the host
        storage_manager_flush_and_release_for_usb();
        usb_msc_connect();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "File verified and atomically committed: %s", final_filename);
    state_machine_transition(STATE_FILE_READY);

    // CRITICAL: Flush metadata to physical sectors, then unmount FatFS so the
    // storage is back in raw USB MSC mode before the host is reconnected.
    storage_manager_flush_and_release_for_usb();
    
    state_machine_transition(STATE_IDLE);

    // Drop the USB connection to signal disconnect to the PC
    usb_msc_disconnect();
    
    // Give the host time to notice the disconnect, then reconnect USB
    ESP_LOGI(TAG, "Waiting 1.5s before reconnecting USB host...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    usb_msc_connect();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cancel_post_handler(httpd_req_t *req) {
    if (active_file) {
        fclose(active_file);
        active_file = NULL;
        ESP_LOGI(TAG, "Transfer cancelled. Deleting partial file: %s", active_filename);
        remove(active_filename);
    }
    mbedtls_md5_free(&active_md5_ctx);
    state_machine_transition(STATE_IDLE);

    // Release FatFS (flush + unmount) so the SD card is raw for USB again,
    // then re-connect the USB host even if transfer was cancelled.
    storage_manager_flush_and_release_for_usb();
    usb_msc_connect();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    uint64_t total = 0, free_bytes = 0;
    const char *stype = "Unknown";
    storage_manager_get_info(&total, &free_bytes, &stype);

    char resp[256];
    snprintf(resp, sizeof(resp), 
             "{\"state\":\"%s\",\"storage_type\":\"%s\",\"total_bytes\":%llu,\"free_bytes\":%llu}", 
             state_to_string(state_machine_get()), stype, total, free_bytes);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t clear_files_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Clearing all files in %s", STORAGE_PATH);
    
    // Disconnect USB so host doesn't interfere
    usb_msc_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Ensure the target storage is mounted before clearing
    if (!storage_manager_fatfs_available() || (storage_manager_is_sd_present() && !sd_card_is_mounted())) {
        storage_manager_remount();
    }

    DIR *dir = opendir(STORAGE_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory for clearing");
        storage_manager_remount();
        storage_manager_flush_and_release_for_usb();
        usb_msc_connect();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open storage");
        return ESP_FAIL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char filepath[300];
        snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_PATH, entry->d_name);
        ESP_LOGI(TAG, "Deleting: %s", filepath);
        remove(filepath);
    }
    closedir(dir);

    // Flush directory entries to physical media, then release SD for USB mode
    ESP_LOGI(TAG, "Flushing clear operation to storage...");
    storage_manager_flush_and_release_for_usb();

    // Give the host time to notice the disconnect, then reconnect USB
    vTaskDelay(pdMS_TO_TICKS(1000));
    usb_msc_connect();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Storage cleared\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t format_storage_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Full format requested");

    // Disconnect USB so no raw sector access races with the format
    usb_msc_disconnect();

    esp_err_t ret = usb_msc_format_storage();

    if (ret != ESP_OK) {
        usb_msc_notify_change();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Format failed");
        return ESP_FAIL;
    }

    usb_msc_notify_change();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Storage formatted\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t list_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    // Only block listing if the USB host has actually enumerated and mounted us.
    // If USB cable is plugged into a power-only port or PC is asleep, 
    // s_usb_mounted_by_host is false, allowing normal Wi-Fi file listing/transfer.
    if (usb_msc_is_host_mounted()) {
        httpd_resp_sendstr_chunk(req, "{\"files\":[],\"storage_available\":false,\"reason\":\"usb_active\"}");
        httpd_resp_sendstr_chunk(req, NULL); // End of chunks
        return ESP_OK;
    }

    // USB not mounted: it is safe to mount storage for listing. If we mount it
    // here, release it afterwards so USB MSC regains ownership.
    bool mounted_here = false;
    if (!storage_manager_fatfs_available()) {
        mounted_here = (storage_manager_remount() == ESP_OK);
    }

    httpd_resp_sendstr_chunk(req, "{\"files\":[");

    bool first = true;
    DIR *dir = opendir(STORAGE_PATH);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            
            // Get file size using stat() instead of opening the file
            char filepath[300];
            snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_PATH, entry->d_name);
            
            struct stat st;
            long fsize = 0;
            if (stat(filepath, &st) == 0) {
                fsize = st.st_size;
            }
            
            char entry_buf[512];
            snprintf(entry_buf, sizeof(entry_buf), "%s{\"name\":\"%s\",\"size\":%ld}", 
                     first ? "" : ",", entry->d_name, fsize);
            httpd_resp_sendstr_chunk(req, entry_buf);
            first = false;
        }
        closedir(dir);
    }

    if (mounted_here) {
        storage_manager_flush_and_release_for_usb();
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL); // End of chunks
    return ESP_OK;
}

static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><title>CNC Files</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
"body{font-family:Arial;margin:40px;background:#1a1a2e;color:#fff}"
"h1{color:#e94560}"
"ul{list-style:none;padding:0}"
"li{padding:12px;border-bottom:1px solid #333;display:flex;justify-content:space-between}"
"a{color:#4cc9f0;text-decoration:none}"
"a:hover{text-decoration:underline}"
".btn{background:#e94560;color:#fff;padding:8px 16px;border:none;border-radius:4px;cursor:pointer}"
".btn:hover{background:#ff6b6b}"
"</style></head><body>"
"<h1>CNC File Receiver</h1>"
"<p>Connected to <b>CNC_WIRELESS</b></p>"
"<div style='display:flex;justify-content:space-between;align-items:center'>"
"<h2>Uploaded Files</h2>"
"<button class='btn' onclick='clearAll()'>Clear All</button>"
"</div>"
"<ul id='filelist'><li>Loading...</li></ul>"
"<script>"
"function clearAll(){if(confirm('Are you sure you want to delete all files?')){fetch('/clear',{method:'POST'}).then(()=>location.reload());}}"
"fetch('/list').then(r=>r.json()).then(d=>{"
"  var l=document.getElementById('filelist');"
"  if(!d.files || d.files.length==0){l.innerHTML='<li>No files uploaded</li>';return;}"
"  l.innerHTML='';"
"  d.files.forEach(f=>{"
"    var fname = f.name || f;"
"    l.innerHTML+=\"<li><span>\"+fname+\"</span><a href='/download/\"+fname+\"' download style='margin-left:auto;margin-right:15px'>Download</a></li>\";"
"  });"
"});"
"</script></body></html>";

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t download_get_handler(httpd_req_t *req) {
    char filename[256];
    char filepath[300];

    const char *uri = req->uri;
    const char *filename_start = strchr(uri, '/');
    if (filename_start) {
        filename_start++;
        strncpy(filename, filename_start, sizeof(filename) - 1);
        filename[sizeof(filename) - 1] = '\0';
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename");
        return ESP_FAIL;
    }

    snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_PATH, filename);

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    char buffer[1024];
    size_t read;
    httpd_resp_set_type(req, "application/octet-stream");

    while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        httpd_resp_send_chunk(req, buffer, read);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

static void restart_delayed_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t settings_post_handler(httpd_req_t *req) {
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        cJSON *j_wifi_mode = cJSON_GetObjectItem(json, "wifi_mode");
        cJSON *j_ap_ssid = cJSON_GetObjectItem(json, "ap_ssid");
        cJSON *j_ap_pass = cJSON_GetObjectItem(json, "ap_pass");
        cJSON *j_sta_ssid = cJSON_GetObjectItem(json, "sta_ssid");
        cJSON *j_sta_pass = cJSON_GetObjectItem(json, "sta_pass");

        // The receiver must ALWAYS connect to the transmitter's private AP SSID.
        // It should never attempt to connect to the shop router SSID.
        if (j_ap_ssid && j_ap_ssid->valuestring && strlen(j_ap_ssid->valuestring) > 0) {
            nvs_set_str(nvs_handle, "sta_ssid", j_ap_ssid->valuestring);
            ESP_LOGI(TAG, "Saved new STA SSID (Transmitter AP): %s", j_ap_ssid->valuestring);
        }
        if (j_ap_pass && j_ap_pass->valuestring && strlen(j_ap_pass->valuestring) > 0) {
            nvs_set_str(nvs_handle, "sta_pass", j_ap_pass->valuestring);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);

    xTaskCreate(restart_delayed_task, "restart_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void file_receiver_init(void) {
    ESP_LOGI(TAG, "Cleaning up any orphan temp files on storage...");
    cleanup_orphan_temp_files();

    ESP_LOGI(TAG, "File receiver HTTP server starting...");

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.stack_size = 10240;
    config.max_uri_handlers = 16;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_prepare = {.uri = "/prepare", .method = HTTP_POST, .handler = prepare_post_handler};
        httpd_uri_t uri_chunk = {.uri = "/chunk", .method = HTTP_POST, .handler = chunk_post_handler};
        httpd_uri_t uri_finalize = {.uri = "/finalize", .method = HTTP_POST, .handler = finalize_post_handler};
        httpd_uri_t uri_status = {.uri = "/status", .method = HTTP_GET, .handler = status_get_handler};
        httpd_uri_t uri_list = {.uri = "/list", .method = HTTP_GET, .handler = list_get_handler};
        httpd_uri_t uri_download = {.uri = "/download/*", .method = HTTP_GET, .handler = download_get_handler};
        httpd_uri_t uri_clear = {.uri = "/clear", .method = HTTP_POST, .handler = clear_files_handler};
        httpd_uri_t uri_format = {.uri = "/format", .method = HTTP_POST, .handler = format_storage_handler};
        httpd_uri_t uri_cancel = {.uri = "/cancel", .method = HTTP_POST, .handler = cancel_post_handler};
        httpd_uri_t uri_index = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler};
        httpd_uri_t uri_settings = {.uri = "/settings", .method = HTTP_POST, .handler = settings_post_handler};

        httpd_register_uri_handler(server, &uri_prepare);
        httpd_register_uri_handler(server, &uri_chunk);
        httpd_register_uri_handler(server, &uri_finalize);
        httpd_register_uri_handler(server, &uri_status);
        httpd_register_uri_handler(server, &uri_list);
        httpd_register_uri_handler(server, &uri_download);
        httpd_register_uri_handler(server, &uri_clear);
        httpd_register_uri_handler(server, &uri_format);
        httpd_register_uri_handler(server, &uri_cancel);
        httpd_register_uri_handler(server, &uri_index);
        httpd_register_uri_handler(server, &uri_settings);

        ESP_LOGI(TAG, "File receiver HTTP server started on port %d", config.server_port);
    } else {
        ESP_LOGE(TAG, "Failed to start file receiver HTTP server");
    }
}
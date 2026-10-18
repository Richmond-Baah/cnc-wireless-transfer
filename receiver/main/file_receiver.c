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
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <sys/stat.h>
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

// Per-transfer stage timings (µs), logged as one summary line at /finalize
static struct {
    int64_t t_start;      // /prepare accepted
    int64_t usb_release;  // USB disconnect
    int64_t mount;        // FatFS mount
    int64_t open;         // staging file open
    int64_t recv;         // network reads inside /chunk
    int64_t write;        // fwrite + periodic/final fsync
    int64_t md5;          // incremental MD5 + finish/verify
    int64_t commit;       // remove + rename
    int64_t snapshot;     // file-list/free-space cache refresh
    int64_t unmount;      // flush + FatFS unmount
    int64_t usb_reconnect;// disconnect + settle wait + connect
    uint64_t bytes;
    uint32_t chunks;
} s_tm;
#define TM_START() int64_t _tm0 = esp_timer_get_time()
#define TM_ADD(field) (s_tm.field += esp_timer_get_time() - _tm0, _tm0 = esp_timer_get_time())
#define TM_MS(us) ((long long)((us) / 1000))

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

// Host busy guard: the machine read the drive recently (likely running a job
// from it). Sends 423 {"error":"machine_busy"} and returns true unless the
// request carries X-Force: 1.
static bool reject_if_machine_busy(httpd_req_t *req) {
    char force_hdr[4] = {0};
    bool force = httpd_req_get_hdr_value_str(req, "X-Force", force_hdr, sizeof(force_hdr)) == ESP_OK &&
                 strcmp(force_hdr, "1") == 0;
    if (force || usb_msc_ms_since_host_read() >= 10000) {
        return false;
    }
    ESP_LOGW(TAG, "Host read %lu ms ago; refusing %s (machine_busy)",
             (unsigned long)usb_msc_ms_since_host_read(), req->uri);
    httpd_resp_set_status(req, "423 Locked");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"error\":\"machine_busy\"}", HTTPD_RESP_USE_STRLEN);
    return true;
}

// ─── File list / storage-info cache ───────────────────────────────────────
// USB MSC owns the storage most of the time, so the file list and total/free
// space are snapshotted whenever FatFS is mounted (boot, finalize, wipe,
// format) and served from RAM while the host has the drive.
#define FILE_CACHE_MAX 48
typedef struct {
    char name[128];
    uint32_t size;
    bool is_dir;
} cached_file_t;
static cached_file_t s_file_cache[FILE_CACHE_MAX];
static int s_file_cache_count = 0;
static bool s_file_cache_truncated = false;
static bool s_file_cache_valid = false;
static uint64_t s_cache_total = 0;
static uint64_t s_cache_free = 0;

// True if the storage that transfers target (SD if present, else flash) has FatFS mounted.
static bool active_storage_mounted(void) {
    return storage_manager_is_sd_present() ? sd_card_is_mounted() : storage_manager_is_flash_mounted();
}

static void clear_file_cache(void) {
    s_file_cache_count = 0;
    s_file_cache_truncated = false;
    s_cache_total = 0;
    s_cache_free = 0;
    s_file_cache_valid = true;
}

// Requires active_storage_mounted().
static void snapshot_storage(void) {
    clear_file_cache();
    // Read the root via FatFS directly: VFS readdir/stat don't expose the
    // HID/SYS attributes (e.g. "System Volume Information" written by Windows).
    char drv[8];
    FF_DIR dir;
    FILINFO fno;
    if (storage_manager_get_fatfs_drive(drv, sizeof(drv)) && f_opendir(&dir, drv) == FR_OK) {
        while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
            size_t len = strlen(fno.fname);
            if (fno.fname[0] == '.' || (fno.fattrib & (AM_HID | AM_SYS)) ||
                (len > 4 && strcmp(fno.fname + len - 4, ".tmp") == 0)) {
                continue; // hidden/system entries and in-flight staging files
            }
            if (s_file_cache_count >= FILE_CACHE_MAX) {
                s_file_cache_truncated = true;
                break;
            }
            cached_file_t *c = &s_file_cache[s_file_cache_count++];
            strlcpy(c->name, fno.fname, sizeof(c->name));
            c->is_dir = (fno.fattrib & AM_DIR) != 0;
            c->size = c->is_dir ? 0 : (uint32_t)fno.fsize;
        }
        f_closedir(&dir);
    }
    uint64_t total = 0, free_bytes = 0;
    const char *stype = NULL;
    if (storage_manager_get_info(&total, &free_bytes, &stype) == ESP_OK) {
        s_cache_total = total;
        s_cache_free = free_bytes;
    }
}

// Mounts the active storage if needed, snapshots it, and releases it again.
// Only call while the USB host cannot be accessing the storage.
void file_receiver_refresh_cache(void) {
    if (storage_manager_is_sd_present() && sd_card_has_card_error()) {
        clear_file_cache(); // unreadable card: nothing to list
        return;
    }
    bool mounted_here = false;
    if (!active_storage_mounted()) {
        mounted_here = (storage_manager_remount() == ESP_OK) && active_storage_mounted();
    }
    if (active_storage_mounted()) {
        snapshot_storage();
    }
    if (mounted_here) {
        storage_manager_flush_and_release_for_usb();
    }
}

// Rejects names with path separators, drive colons, "..", control characters,
// or that are empty / consist only of dots.
static bool filename_is_valid(const char *name) {
    if (name[0] == '\0' || strstr(name, "..") != NULL) {
        return false;
    }
    bool dots_only = true;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\' || *p == ':') {
            return false;
        }
        if (*p != '.') {
            dots_only = false;
        }
    }
    return !dots_only;
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
    // Reject instead of silently truncating into sanitized[128] / expected_md5[33]
    if (strlen(fname->valuestring) > 127 || strlen(md5->valuestring) > 32) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename or md5 too long");
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    if (!filename_is_valid(fname->valuestring)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Invalid filename: no / \\ : .. or control characters, not empty or dots-only");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    if (reject_if_machine_busy(req)) {
        cJSON_Delete(json);
        return ESP_OK;
    }

    // Card was unreadable: retry the mount (USB off so the host can't race it)
    if (sd_card_has_card_error()) {
        usb_msc_disconnect();
        if (sd_card_retry_card_error() != ESP_OK) {
            usb_msc_connect();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Card unreadable - format FAT32 on PC or use Format");
            cJSON_Delete(json);
            return ESP_FAIL;
        }
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

    memset(&s_tm, 0, sizeof(s_tm));
    s_tm.t_start = esp_timer_get_time();
    TM_START();

    // Step 1: Disconnect USB so the host stops reading raw SD sectors
    usb_msc_disconnect();
    TM_ADD(usb_release);

    // Step 2: Ensure the target storage filesystem is mounted before opening.
    // The SD card is the transfer target when present; internal flash is the
    // fallback. FatFS and USB MSC are strictly exclusive: USB was just
    // disconnected, so we are free to mount FatFS now. Never mount the SD
    // card while the USB host could still be reading it.
    if (!storage_manager_fatfs_available() || (storage_manager_is_sd_present() && !sd_card_is_mounted())) {
        storage_manager_remount();
    }
    TM_ADD(mount);
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

    TM_ADD(open);
    mbedtls_md5_init(&active_md5_ctx);
    mbedtls_md5_starts(&active_md5_ctx);
    chunk_count = 0;
    expected_chunk_index = 0;

    ESP_LOGI(TAG, "Ready to receive staging file: %s (final: %s, expected MD5: %s)", active_filename, final_filename, expected_md5);
    ESP_LOGI(TAG, "Stage prepare: usb_release %lld ms, mount %lld ms, open %lld ms",
             TM_MS(s_tm.usb_release), TM_MS(s_tm.mount), TM_MS(s_tm.open));
    
    httpd_resp_set_type(req, "application/json; charset=utf-8");
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
        httpd_resp_set_type(req, "application/json; charset=utf-8");
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
    TM_START();
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
        TM_ADD(recv);

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
        TM_ADD(write);

        mbedtls_md5_update(&active_md5_ctx, (const unsigned char *)chunk_buf, read_len);
        TM_ADD(md5);
        s_tm.bytes += read_len;
        remaining -= read_len;
    }
    expected_chunk_index++;
    s_tm.chunks++;

    httpd_resp_set_type(req, "application/json; charset=utf-8");
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

    TM_START();
    fflush(active_file);
    fsync(fileno(active_file));
    long final_size = ftell(active_file);
    fclose(active_file);
    active_file = NULL;
    TM_ADD(write);

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
    bool md5_ok = strcasecmp(computed_md5, check_against) == 0;
    TM_ADD(md5);

    if (!md5_ok) {
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

    TM_ADD(commit);
    ESP_LOGI(TAG, "File verified and atomically committed: %s", final_filename);
    state_machine_transition(STATE_FILE_READY);

    snapshot_storage(); // still mounted: refresh cached file list + free space
    TM_ADD(snapshot);

    // CRITICAL: Flush metadata to physical sectors, then unmount FatFS so the
    // storage is back in raw USB MSC mode before the host is reconnected.
    storage_manager_flush_and_release_for_usb();
    TM_ADD(unmount);

    // The file is safely on disk: answer now so the sender isn't held up by
    // the USB reconnect wait. State stays FILE_READY until the drive is back
    // (the transmitter UI shows "Reconnecting drive" meanwhile).
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);

    // Drop the USB connection to signal disconnect to the PC
    usb_msc_disconnect();

    // Give the host time to notice the disconnect, then reconnect USB
    ESP_LOGI(TAG, "Waiting 1.5s before reconnecting USB host...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    usb_msc_connect();
    TM_ADD(usb_reconnect);
    if (state_machine_get() == STATE_FILE_READY) {
        state_machine_transition(STATE_IDLE); // host mount then moves it to USB_ACTIVE
    }

    int64_t total_us = esp_timer_get_time() - s_tm.t_start;
    ESP_LOGI(TAG, "XFER summary: %llu bytes, %lu chunks in %lld ms = %.1f KB/s | "
                  "usb_release %lld, mount %lld, open %lld, recv %lld, write %lld, md5 %lld, "
                  "commit %lld, snapshot %lld, unmount %lld, usb_reconnect %lld ms",
             (unsigned long long)s_tm.bytes, (unsigned long)s_tm.chunks, TM_MS(total_us),
             total_us > 0 ? (double)s_tm.bytes / 1024.0 / ((double)total_us / 1e6) : 0.0,
             TM_MS(s_tm.usb_release), TM_MS(s_tm.mount), TM_MS(s_tm.open), TM_MS(s_tm.recv),
             TM_MS(s_tm.write), TM_MS(s_tm.md5), TM_MS(s_tm.commit), TM_MS(s_tm.snapshot),
             TM_MS(s_tm.unmount), TM_MS(s_tm.usb_reconnect));
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

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    uint64_t total = 0, free_bytes = 0;
    const char *stype = "Unknown";
    storage_manager_get_info(&total, &free_bytes, &stype);
    if (!active_storage_mounted() && s_file_cache_valid) {
        // USB owns the storage: report the last snapshot, not a stale/other volume
        total = s_cache_total;
        free_bytes = s_cache_free;
    }

    char resp[320];
    snprintf(resp, sizeof(resp),
             "{\"state\":\"%s\",\"storage_type\":\"%s\",\"total_bytes\":%llu,\"free_bytes\":%llu,\"card_error\":%s,\"fw\":\"%s\"}",
             state_to_string(state_machine_get()), stype, total, free_bytes,
             sd_card_has_card_error() ? "true" : "false",
             esp_app_get_description()->version);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t clear_files_handler(httpd_req_t *req) {
    // Require {"confirm":"WIPE"} so a stray POST can't delete everything
    char body[64] = {0};
    int len = 0;
    if (req->content_len > 0 && req->content_len < (int)sizeof(body)) {
        while (len < req->content_len) {
            int r = httpd_req_recv(req, body + len, req->content_len - len);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (r <= 0) break;
            len += r;
        }
    }
    cJSON *json = (len == req->content_len && len > 0) ? cJSON_Parse(body) : NULL;
    cJSON *confirm = json ? cJSON_GetObjectItem(json, "confirm") : NULL;
    bool confirmed = confirm && confirm->valuestring && strcmp(confirm->valuestring, "WIPE") == 0;
    cJSON_Delete(json);
    if (!confirmed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Confirmation required: type WIPE");
        return ESP_FAIL;
    }
    if (reject_if_machine_busy(req)) {
        return ESP_OK;
    }

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
    snapshot_storage(); // still mounted: cache the (now empty) list + free space

    // Flush directory entries to physical media, then release SD for USB mode
    ESP_LOGI(TAG, "Flushing clear operation to storage...");
    storage_manager_flush_and_release_for_usb();

    // Give the host time to notice the disconnect, then reconnect USB
    vTaskDelay(pdMS_TO_TICKS(1000));
    usb_msc_connect();

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Storage cleared\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t format_storage_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Full format requested");

    // Disconnect USB so no raw sector access races with the format
    usb_msc_disconnect();

    esp_err_t ret = usb_msc_format_storage();
    if (ret == ESP_OK) {
        // Freshly formatted card: re-verify it so CARD_ERROR clears (-> IDLE)
        sd_card_retry_card_error();
    }

    // Flash format leaves FatFS mounted (USB would report not-ready): release
    // storage to raw USB MSC mode before reconnecting the host on every path.
    storage_manager_flush_and_release_for_usb();
    // USB is still disconnected: snapshot the fresh volume (empty list, new sizes)
    file_receiver_refresh_cache();

    if (ret != ESP_OK) {
        usb_msc_notify_change();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Format failed");
        return ESP_FAIL;
    }

    usb_msc_notify_change();

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Storage formatted\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t list_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");

    // While the USB host has the drive, never mount it: serve the snapshot
    // taken at the last transfer/wipe/format/boot. Otherwise list live
    // (mounting and releasing the storage if needed).
    bool cached = usb_msc_is_host_mounted();
    if (!cached) {
        file_receiver_refresh_cache();
    }

    httpd_resp_sendstr_chunk(req, "{\"files\":[");
    for (int i = 0; i < s_file_cache_count; i++) {
        // FAT names cannot contain " or \, so no JSON escaping is needed
        char entry_buf[192];
        if (s_file_cache[i].is_dir) {
            snprintf(entry_buf, sizeof(entry_buf), "%s{\"name\":\"%s\",\"type\":\"dir\"}",
                     i ? "," : "", s_file_cache[i].name);
        } else {
            snprintf(entry_buf, sizeof(entry_buf), "%s{\"name\":\"%s\",\"type\":\"file\",\"size\":%lu}",
                     i ? "," : "", s_file_cache[i].name, (unsigned long)s_file_cache[i].size);
        }
        httpd_resp_sendstr_chunk(req, entry_buf);
    }
    char tail[96];
    snprintf(tail, sizeof(tail), "],\"cached\":%s,\"truncated\":%s,\"storage_available\":%s}",
             cached ? "true" : "false", s_file_cache_truncated ? "true" : "false",
             s_file_cache_valid ? "true" : "false");
    httpd_resp_sendstr_chunk(req, tail);
    httpd_resp_sendstr_chunk(req, NULL); // End of chunks
    return ESP_OK;
}

static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'><title>CNC Files</title>"
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
"function clearAll(){if(prompt('Type WIPE to delete all files')!=='WIPE')return;"
"fetch('/clear',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({confirm:'WIPE'})})"
".then(r=>{if(r.status==423)alert('Machine is reading the drive - stop the job first.');location.reload();});}"
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
    httpd_resp_set_type(req, "text/html; charset=utf-8");
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
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Must fit sta_ssid[32] / sta_pass[64] in wifi_manager.c
    cJSON *chk_ssid = cJSON_GetObjectItem(json, "ap_ssid");
    cJSON *chk_pass = cJSON_GetObjectItem(json, "ap_pass");
    if ((chk_ssid && chk_ssid->valuestring && strlen(chk_ssid->valuestring) > 31) ||
        (chk_pass && chk_pass->valuestring && strlen(chk_pass->valuestring) > 63)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID or password too long");
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

    httpd_resp_set_type(req, "application/json; charset=utf-8");
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
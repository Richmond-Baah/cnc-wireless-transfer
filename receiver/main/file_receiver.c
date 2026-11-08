#include "coredump_http.h"
#include "fw_update.h"
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
#include "pairing.h"
#include "radio_power.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <dirent.h>
#include <unistd.h>
#include "nvs.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <sys/stat.h>
#include "lwip/sockets.h"
static const char *TAG = "FILE_RCV";
#define STORAGE_PATH storage_manager_get_base_path()

static FILE *active_file = NULL;
static char active_filename[256] = {0}; // temporary filename during transfer (.tmp)
static char final_filename[256] = {0};  // target filename after successful verification
static char expected_md5[33] = {0};
static mbedtls_md5_context active_md5_ctx;

#define RECEIVER_CHUNK_BUFSIZE 32768 // one whole upload chunk (UPLOAD_CHUNK_MAX on the transmitter)
static char chunk_buf[RECEIVER_CHUNK_BUFSIZE];
static int expected_chunk_index = 0;

// Per-transfer stage timings (µs), logged as one summary line at /finalize
static struct {
    int64_t t_start;      // /prepare accepted
    int64_t usb_release;  // USB disconnect
    int64_t mount;        // FatFS mount
    int64_t space;        // free-space check (f_getfree)
    int64_t open;         // staging file open
    int64_t recv;         // network reads inside /chunk
    int64_t write;        // fwrite of the chunks
    int64_t flush;        // final fflush + fsync + fclose
    int64_t md5;          // incremental MD5 + finish/verify
    int64_t commit;       // remove + rename
    int64_t snapshot;     // file-list/free-space cache refresh
    int64_t unmount;      // flush + FatFS unmount
    int64_t usb_reconnect;// disconnect + settle wait + connect
    uint64_t bytes;
    uint32_t chunks;
    uint64_t next_log;    // bytes at which the next progress line is logged
} s_tm;
static int s_reattach_ms = -1; // last transfer: /finalize reply -> drive back on USB (-1 = not yet)
#define CHUNK_LOG_ALL 64       // log every chunk of the first 2 MB, then only slow ones
#define CHUNK_LOG_SLOW_MS 500
#define RX_KBPS(bytes, us) ((us) > 0 ? (double)(bytes) / 1024.0 / ((double)(us) / 1e6) : 0.0)
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

// /prepare gave up after USB was disconnected: undo exactly that (no ERROR
// state, no other storage) and hand the drive straight back to the host.
static void prepare_abort_reconnect(void) {
    if (active_file) {
        fclose(active_file);
        active_file = NULL;
        remove(active_filename);
    }
    mbedtls_md5_free(&active_md5_ctx);
    state_machine_transition(STATE_IDLE);
    storage_manager_flush_and_release_for_usb();
    usb_msc_connect();
}

// No /prepare, /chunk or /finalize for this long while RECEIVING: the
// transmitter is gone (it expires an idle session itself after 30 s and
// cancels it here), so give up and hand the drive back to the machine.
#define XFER_IDLE_TIMEOUT_US (60LL * 1000 * 1000)
static httpd_handle_t s_server;
static volatile int64_t s_xfer_alive_us; // last transfer request
static const char *s_xfer_abort = "";    // why the last transfer was dropped here ("" = it wasn't)
static void xfer_touch(void) { s_xfer_alive_us = esp_timer_get_time(); }

// On the httpd task (queued), so it never runs inside a transfer handler.
static void xfer_timeout_work(void *arg) {
    (void)arg;
    if (state_machine_get() != STATE_RECEIVING || !active_file ||
        esp_timer_get_time() - s_xfer_alive_us < XFER_IDLE_TIMEOUT_US) {
        return;
    }
    ESP_LOGW(TAG, "Transfer cancelled: no data for %lld s. Deleting %s, drive back to USB",
             (long long)(XFER_IDLE_TIMEOUT_US / 1000000), active_filename);
    s_xfer_abort = "no_data";
    prepare_abort_reconnect(); // .tmp deleted, IDLE, FS released, USB on
    pairing_heartbeat_now();   // the transmitter shows it at once
}

static void xfer_watch(void *arg) {
    (void)arg;
    if (s_server && state_machine_get() == STATE_RECEIVING &&
        esp_timer_get_time() - s_xfer_alive_us >= XFER_IDLE_TIMEOUT_US) {
        httpd_queue_work(s_server, xfer_timeout_work, NULL);
    }
}

// Receiver-side budget for /prepare, below the transmitter's 10 s timeout so a
// prepare the transmitter already gave up on never leaves USB disconnected.
#define PREPARE_BUDGET_US (8LL * 1000 * 1000)
#define USB_MIN_OFF_MS 1500

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

// Once paired, every API request must carry the pairing key (X-Auth-Key).
// Sends 401 and returns true when it doesn't.
static bool reject_unauthorized(httpd_req_t *req) {
    if (!pairing_is_paired()) {
        return false;
    }
    char key[PAIRING_KEY_HEX_LEN + 2] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Key", key, sizeof(key)) == ESP_OK &&
        pairing_key_matches(key)) {
        return false;
    }
    drain_request_body(req);
    ESP_LOGW(TAG, "Rejected %s: missing/wrong X-Auth-Key", req->uri);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "Unauthorized");
    return true;
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

// True if the SD card has FatFS mounted (the only storage).
static bool active_storage_mounted(void) {
    return sd_card_is_mounted();
}

static void clear_file_cache(void) {
    s_file_cache_count = 0;
    s_file_cache_truncated = false;
    s_cache_total = 0;
    s_cache_free = 0;
    s_file_cache_valid = true;
}

#define READ_MAP_ROOT_MAX 8

// Where each listed file sits, for the host read summary. Assumes files are
// contiguous, so a fragmented file's later clusters can be named wrongly.
static void read_map_add_files(FATFS *fs, const char *drv) {
    FIL *fp = malloc(sizeof(FIL));
    if (!fp) return;
    const uint32_t cluster_bytes = fs->csize * 512u;
    for (int i = 0; i < s_file_cache_count; i++) {
        const cached_file_t *c = &s_file_cache[i];
        char path[sizeof(c->name) + 8];
        snprintf(path, sizeof(path), "%s/%s", drv, c->name);
        if (c->is_dir || c->size == 0 || f_open(fp, path, FA_READ) != FR_OK) continue;
        if (fp->obj.sclust >= 2) {
            uint32_t clusters = c->size / cluster_bytes + (c->size % cluster_bytes != 0);
            usb_msc_read_map_add(fs->database + (fp->obj.sclust - 2) * fs->csize, clusters * fs->csize, c->name);
        }
        f_close(fp);
    }
    free(fp);
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
        FATFS *fs = dir.obj.fs;
        usb_msc_read_map_begin(fs->database);
        DWORD root_clust = 0;
        int root_areas = 0;
        while (1) {
            // FAT32's root directory lives in the data area
            if (fs->fs_type == FS_FAT32 && dir.clust >= 2 && dir.clust != root_clust &&
                root_areas < READ_MAP_ROOT_MAX) {
                root_clust = dir.clust;
                root_areas++;
                usb_msc_read_map_add(fs->database + (root_clust - 2) * fs->csize, fs->csize, NULL);
            }
            if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') break;
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
        read_map_add_files(fs, drv);
    }
    uint64_t total = 0, free_bytes = 0;
    const char *stype = NULL;
    if (storage_manager_get_info(&total, &free_bytes, &stype) == ESP_OK) {
        s_cache_total = total;
        s_cache_free = free_bytes;
    }
}

static void cleanup_orphan_temp_files(void);

// Boot, before USB attaches: first mount of the card. Removes orphan staging
// files, then caches the file list + free space and releases the card.
void file_receiver_boot_scan(void) {
    if (storage_manager_is_sd_present() && sd_card_has_card_error()) {
        clear_file_cache();
        return;
    }
    bool mounted_here = false;
    if (!active_storage_mounted()) {
        mounted_here = (storage_manager_remount() == ESP_OK) && active_storage_mounted();
    }
    if (active_storage_mounted()) {
        cleanup_orphan_temp_files();
        snapshot_storage();
    }
    if (mounted_here) {
        storage_manager_flush_and_release_for_usb();
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

bool file_receiver_take_root_file(const char *name, char *buf, size_t len) {
    buf[0] = '\0';
    if (storage_manager_is_sd_present() && sd_card_has_card_error()) {
        return false;
    }
    bool mounted_here = false;
    if (!active_storage_mounted()) {
        mounted_here = (storage_manager_remount() == ESP_OK) && active_storage_mounted();
    }
    bool found = false;
    if (active_storage_mounted()) {
        char path[64];
        snprintf(path, sizeof(path), "%s/%s", storage_manager_get_base_path(), name);
        FILE *f = fopen(path, "rb");
        if (f != NULL) {
            found = true;
            size_t n = fread(buf, 1, len - 1, f);
            bool too_large = n == len - 1 && fgetc(f) != EOF;
            fclose(f);
            buf[too_large ? 0 : n] = '\0';
            if (unlink(path) != 0) {
                ESP_LOGE(TAG, "Could not delete %s", path);
            }
        }
    }
    if (mounted_here) {
        storage_manager_flush_and_release_for_usb();
    }
    return found;
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

// Headroom required beyond the file size (cluster rounding, FAT updates):
// max(64 KB, 1% of the volume). The web UI applies the same rule.
static uint64_t space_margin(uint64_t total) {
    uint64_t pct = total / 100;
    return pct > 64 * 1024 ? pct : 64 * 1024;
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
// Staging files a reset or power loss left behind: "<name>.tmp" (what /prepare
// writes) and older ".<name>.tmp". Storage must be mounted; boot only, never
// while a transfer could be writing one. Names are collected first, then removed.
static void cleanup_orphan_temp_files(void) {
    DIR *dir = opendir(STORAGE_PATH);
    if (!dir) return;
    static char names[8][256];
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && n < 8) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcasecmp(entry->d_name + len - 4, ".tmp") == 0) {
            strlcpy(names[n++], entry->d_name, sizeof(names[0]));
        }
    }
    closedir(dir);
    for (int i = 0; i < n; i++) {
        char filepath[320];
        snprintf(filepath, sizeof(filepath), "%s/%s", STORAGE_PATH, names[i]);
        ESP_LOGW(TAG, "Removing orphan temp file: %s (%s)", filepath, remove(filepath) == 0 ? "ok" : "failed");
    }
}

static esp_err_t prepare_post_handler(httpd_req_t *req) {
    const int64_t t_req = esp_timer_get_time();
    if (reject_unauthorized(req)) return ESP_OK; // header check only
    // Busy guard first: no body read, state change, lock, unmount or USB
    // disconnect while the machine is reading the drive (e.g. printing from it)
    if (reject_if_machine_busy(req)) {
        drain_request_body(req);
        return ESP_OK;
    }
    app_state_t cur = state_machine_get();
    if (cur == STATE_RECEIVING || fw_update_active()) {
        drain_request_body(req);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, fw_update_active() ? "Firmware update running" : "Receiver Busy", -1);
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
    xfer_touch();
    s_xfer_abort = "";

    char sanitized[128];
    sanitize_filename(fname->valuestring, sanitized, sizeof(sanitized));

    if (active_file) {
        fclose(active_file);
        mbedtls_md5_free(&active_md5_ctx);
        active_file = NULL;
    }

    memset(&s_tm, 0, sizeof(s_tm));
    s_tm.t_start = esp_timer_get_time();
    s_reattach_ms = -1;
    TM_START();

    // Step 1: Disconnect USB so the host stops reading raw SD sectors
    usb_msc_disconnect();
    TM_ADD(usb_release);

    // Step 2: Ensure the target storage filesystem is mounted before opening.
    // The SD card is the transfer target when present; internal flash is the
    // fallback. FatFS and USB MSC are strictly exclusive: USB was just
    // disconnected, so we are free to mount FatFS now. Never mount the SD
    // card while the USB host could still be reading it.
    // SD IO lock still held by a USB sector access after 5 s: reconnect now
    // instead of falling back to internal flash with USB off.
    if (storage_manager_is_sd_present() && !sd_card_is_mounted() &&
        sd_card_remount() == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "SD busy with USB access; aborting prepare, USB reconnected");
        prepare_abort_reconnect();
        httpd_resp_set_status(req, "423 Locked");
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_send(req, "{\"error\":\"machine_busy\"}", HTTPD_RESP_USE_STRLEN);
        cJSON_Delete(json);
        return ESP_OK;
    }
    if (!storage_manager_fatfs_available() || (storage_manager_is_sd_present() && !sd_card_is_mounted())) {
        storage_manager_remount();
    }
    TM_ADD(mount);
    if (!storage_manager_fatfs_available()) {
        // Active storage could not be mounted: USB straight back, nothing else used
        ESP_LOGE(TAG, "SD card could not be mounted; aborting prepare, USB reconnected");
        prepare_abort_reconnect();
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "Drive could not be mounted - try again or check the card");
        cJSON_Delete(json);
        return ESP_OK;
    }

    // Space check on the real volume before writing anything. A same-name file
    // stays on the card until /finalize renames over it, so it is not counted free.
    cJSON *fsize = cJSON_GetObjectItem(json, "filesize");
    uint64_t vol_total = 0, vol_free = 0;
    const char *vol_type = NULL;
    if (cJSON_IsNumber(fsize) && fsize->valuedouble >= 0 &&
        storage_manager_get_info(&vol_total, &vol_free, &vol_type) == ESP_OK) {
        uint64_t need = (uint64_t)fsize->valuedouble + space_margin(vol_total);
        if (vol_free < need) {
            ESP_LOGW(TAG, "Not enough space for %s: size %.0f, free %llu, need %llu",
                     sanitized, fsize->valuedouble, vol_free, need);
            snapshot_storage(); // still mounted: /status and /list now show live free space
            state_machine_transition(STATE_IDLE);
            storage_manager_flush_and_release_for_usb();
            usb_msc_connect();
            char body[160];
            snprintf(body, sizeof(body),
                     "{\"error\":\"insufficient_space\",\"file_size\":%.0f,\"free_bytes\":%llu,"
                     "\"needed_bytes\":%llu}", fsize->valuedouble, vol_free, need);
            httpd_resp_set_status(req, "507 Insufficient Storage");
            httpd_resp_set_type(req, "application/json; charset=utf-8");
            httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
            cJSON_Delete(json);
            return ESP_OK;
        }
    }

    // Build paths only after the active storage path has been resolved
    TM_ADD(space);
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
    expected_chunk_index = 0;

    if (esp_timer_get_time() - t_req > PREPARE_BUDGET_US) {
        ESP_LOGW(TAG, "Prepare took %lld ms (transmitter gave up); aborting, USB reconnected",
                 (esp_timer_get_time() - t_req) / 1000);
        prepare_abort_reconnect();
        httpd_resp_set_status(req, "504 Gateway Timeout");
        httpd_resp_sendstr(req, "Prepare took too long");
        cJSON_Delete(json);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Ready to receive staging file: %s (final: %s, expected MD5: %s)", active_filename, final_filename, expected_md5);
    ESP_LOGI(TAG, "Stage prepare: usb_release %lld ms, mount %lld ms, space %lld ms, open %lld ms",
             TM_MS(s_tm.usb_release), TM_MS(s_tm.mount), TM_MS(s_tm.space), TM_MS(s_tm.open));
    
    char ready[160];
    snprintf(ready, sizeof(ready), "{\"status\":\"ok\",\"ms\":{\"detach\":%lld,\"mount\":%lld,\"space\":%lld,\"open\":%lld}}",
             TM_MS(s_tm.usb_release), TM_MS(s_tm.mount), TM_MS(s_tm.space), TM_MS(s_tm.open));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, ready);

    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t chunk_post_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    if (state_machine_get() != STATE_RECEIVING || !active_file) {
        if (s_xfer_abort[0]) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "Transfer cancelled: no data for 60 s");
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not in RECEIVING state");
        }
        return ESP_FAIL;
    }
    xfer_touch();

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

    // The whole chunk is read into RAM first and then written with one fwrite
    // (32 KB, sector aligned). A short body (sender gone mid-chunk) writes
    // nothing, so the sender can retry the same index. No fsync per chunk: the
    // staging file is fsynced once in /finalize and deleted on any failure.
    int len = req->content_len;
    if (len <= 0 || len > (int)sizeof(chunk_buf)) {
        drain_request_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Chunk must be 1-32768 bytes");
        return ESP_OK;
    }
    TM_START();
    int64_t t_recv = _tm0;
    int got = 0, recv_timeouts = 0;
    while (got < len) {
        int read_len = httpd_req_recv(req, chunk_buf + got, len - got);
        if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
            recv_timeouts++;
            ESP_LOGW(TAG, "Chunk %ld: recv timeout #%d at %d of %d bytes", chunk_index, recv_timeouts, got, len);
            continue;
        }
        if (read_len <= 0) {
            ESP_LOGW(TAG, "Chunk %ld: body ended after %d of %d bytes; not written, retry expected",
                     chunk_index, got, len);
            return ESP_FAIL; // socket is gone: httpd closes it
        }
        got += read_len;
    }
    TM_ADD(recv);
    int64_t t_write = _tm0;

    size_t written = fwrite(chunk_buf, 1, len, active_file);
    if ((int)written != len) {
        ESP_LOGE(TAG, "Write error (wrote %d of %d)", (int)written, len);
        fail_transfer_and_release();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write error");
        return ESP_FAIL;
    }
    TM_ADD(write);
    int64_t t_md5 = _tm0;

    mbedtls_md5_update(&active_md5_ctx, (const unsigned char *)chunk_buf, len);
    TM_ADD(md5);
    s_tm.bytes += len;
    expected_chunk_index++;
    s_tm.chunks++;
    if (s_tm.bytes >= s_tm.next_log) {
        s_tm.next_log = s_tm.bytes + 1024 * 1024;
        ESP_LOGI(TAG, "RX %llu KB: recv %.0f KB/s, SD write %.0f KB/s, md5 %.0f KB/s",
                 (unsigned long long)(s_tm.bytes / 1024), RX_KBPS(s_tm.bytes, s_tm.recv),
                 RX_KBPS(s_tm.bytes, s_tm.write), RX_KBPS(s_tm.bytes, s_tm.md5));
    }

    // Per-chunk timing, also returned to the TX because the RX console is UART only.
    // "got,recv_ms,write_ms,md5_ms,recv_timeouts"
    int recv_ms = (int)((t_write - t_recv) / 1000), write_ms = (int)((t_md5 - t_write) / 1000);
    int md5_ms = (int)((_tm0 - t_md5) / 1000);
    char timing[48];
    snprintf(timing, sizeof(timing), "%d,%d,%d,%d,%d", got, recv_ms, write_ms, md5_ms, recv_timeouts);
    httpd_resp_set_hdr(req, "X-Rx-Timing", timing);
    if (chunk_index < CHUNK_LOG_ALL || recv_ms + write_ms + md5_ms > CHUNK_LOG_SLOW_MS) {
        ESP_LOGI(TAG, "CHUNK %ld: recv complete %d of %d B in %d ms (%d timeouts), write %d ms, md5 %d ms",
                 chunk_index, got, len, recv_ms, recv_timeouts, write_ms, md5_ms);
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t finalize_post_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    if (state_machine_get() != STATE_RECEIVING || !active_file) {
        if (s_xfer_abort[0]) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "Transfer cancelled: no data for 60 s");
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not in RECEIVING state");
        }
        return ESP_FAIL;
    }
    xfer_touch();

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
    TM_ADD(flush);

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
    char done[192];
    snprintf(done, sizeof(done), "{\"status\":\"ok\",\"ms\":{\"flush\":%lld,\"md5\":%lld,\"commit\":%lld,\"list\":%lld,\"unmount\":%lld}}",
             TM_MS(s_tm.flush), TM_MS(s_tm.md5), TM_MS(s_tm.commit), TM_MS(s_tm.snapshot), TM_MS(s_tm.unmount));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, done);

    // USB has been off since /prepare. The host must see it gone for at least
    // USB_MIN_OFF_MS in total; a transfer that took longer needs no extra wait.
    usb_msc_connect_after_off(USB_MIN_OFF_MS);
    TM_ADD(usb_reconnect);
    s_reattach_ms = (int)TM_MS(s_tm.usb_reconnect);
    if (state_machine_get() == STATE_FILE_READY) {
        state_machine_transition(STATE_IDLE); // host mount then moves it to USB_ACTIVE
    }
    pairing_heartbeat_now(); // the UI is waiting for this state

    int64_t total_us = esp_timer_get_time() - s_tm.t_start;
    ESP_LOGI(TAG, "XFER summary: %llu bytes, %lu chunks in %lld ms = %.1f KB/s | "
                  "recv %.0f KB/s, SD write %.0f KB/s | "
                  "usb_release %lld, mount %lld, space %lld, open %lld, recv %lld, write %lld, flush %lld, md5 %lld, "
                  "commit %lld, snapshot %lld, unmount %lld, usb_reconnect %lld ms (host mount logged separately)",
             (unsigned long long)s_tm.bytes, (unsigned long)s_tm.chunks, TM_MS(total_us),
             RX_KBPS(s_tm.bytes, total_us), RX_KBPS(s_tm.bytes, s_tm.recv), RX_KBPS(s_tm.bytes, s_tm.write),
             TM_MS(s_tm.usb_release), TM_MS(s_tm.mount), TM_MS(s_tm.space), TM_MS(s_tm.open), TM_MS(s_tm.recv),
             TM_MS(s_tm.write), TM_MS(s_tm.flush), TM_MS(s_tm.md5), TM_MS(s_tm.commit), TM_MS(s_tm.snapshot),
             TM_MS(s_tm.unmount), TM_MS(s_tm.usb_reconnect));
    return ESP_OK;
}

static esp_err_t cancel_post_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
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

// Adds fw, state, storage type/total/free, card_error and busy to obj.
void file_receiver_add_status_fields(cJSON *obj) {
    uint64_t total = 0, free_bytes = 0;
    const char *stype = "Unknown";
    storage_manager_get_info(&total, &free_bytes, &stype);
    if (!active_storage_mounted() && s_file_cache_valid) {
        // USB owns the storage: report the last snapshot, not a stale/other volume
        total = s_cache_total;
        free_bytes = s_cache_free;
    }
    cJSON_AddStringToObject(obj, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(obj, "state", state_to_string(state_machine_get()));
    cJSON_AddStringToObject(obj, "storage_type", stype);
    cJSON_AddNumberToObject(obj, "total_bytes", (double)total);
    cJSON_AddNumberToObject(obj, "free_bytes", (double)free_bytes);
    cJSON_AddBoolToObject(obj, "card_error", sd_card_has_card_error());
    // Same rule as the busy guard: the machine read the drive in the last 10 s
    cJSON_AddBoolToObject(obj, "busy", usb_msc_ms_since_host_read() < 10000);
    usb_msc_read_summary_t rd;
    if (usb_msc_read_summary(&rd)) {
        cJSON_AddNumberToObject(obj, "rd_n", rd.reads);
        cJSON_AddNumberToObject(obj, "rd_meta", rd.meta);
        cJSON_AddNumberToObject(obj, "rd_data", rd.data);
        cJSON_AddNumberToObject(obj, "rd_other", rd.other);
        cJSON_AddStringToObject(obj, "rd_file", rd.file);
    }
    cJSON_AddNumberToObject(obj, "reattach_ms", s_reattach_ms);
    cJSON_AddNumberToObject(obj, "host_ms", usb_msc_host_mount_ms());
    cJSON_AddNumberToObject(obj, "rssi", radio_sta_rssi());
    cJSON_AddNumberToObject(obj, "qdbm", radio_power_actual());
    cJSON_AddNumberToObject(obj, "brownouts", radio_brownouts());
    cJSON_AddStringToObject(obj, "reset_reason", radio_reset_reason());
    cJSON_AddBoolToObject(obj, "coredump", coredump_http_present());
    cJSON_AddStringToObject(obj, "xfer_abort", s_xfer_abort);
    cJSON_AddNumberToObject(obj, "uptime_s", (double)(esp_timer_get_time() / 1000000));
}

// ─── Speed test (Settings > System on the transmitter) ──────────────────────
// SD write pattern of chunk_post_handler, so /perf/sd measures what a real
// transfer does: one fwrite per piece, fflush+fsync every PERF_SD_FSYNC_EVERY.
#define PERF_SD_PIECE RECEIVER_CHUNK_BUFSIZE // one fwrite per whole chunk
#define PERF_SD_FSYNC_EVERY 0                // fsync only at the end (/finalize)
#define PERF_SD_MAX (100u * 1024 * 1024)

static void perf_reply(httpd_req_t *req, uint64_t bytes, int64_t us) {
    char out[96];
    snprintf(out, sizeof(out), "{\"bytes\":%llu,\"ms\":%lld,\"kbps\":%.1f}", (unsigned long long)bytes,
             (long long)(us / 1000), RX_KBPS(bytes, us));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, out);
}

// POST /perf/sink: read and discard the body (TX -> RX link test)
static esp_err_t perf_sink_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    int64_t t0 = esp_timer_get_time();
    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, chunk_buf, remaining < (int)sizeof(chunk_buf) ? remaining : (int)sizeof(chunk_buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        remaining -= r;
    }
    perf_reply(req, req->content_len, esp_timer_get_time() - t0);
    return ESP_OK;
}

// POST /perf/sd {"size":n}: USB off -> mount -> write n bytes from RAM to
// FXPERF.TMP -> fsync -> delete -> unmount -> USB on. Busy guard applies.
static esp_err_t perf_sd_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    if (reject_if_machine_busy(req)) {
        drain_request_body(req);
        return ESP_OK;
    }
    char body[48] = {0};
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : 0;
    if (len <= 0 || httpd_req_recv(req, body, len) != len) {
        drain_request_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_OK;
    }
    cJSON *json = cJSON_Parse(body);
    cJSON *j_size = json ? cJSON_GetObjectItem(json, "size") : NULL;
    double size = cJSON_IsNumber(j_size) ? j_size->valuedouble : 0;
    cJSON_Delete(json);
    if (size <= 0 || size > PERF_SD_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size must be 1..100 MB");
        return ESP_OK;
    }
    app_state_t cur = state_machine_get();
    if (cur == STATE_RECEIVING || cur == STATE_CARD_ERROR || sd_card_has_card_error()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, cur == STATE_RECEIVING ? "Receiver Busy" : "Card error");
        return ESP_OK;
    }
    char *buf = malloc(PERF_SD_PIECE);
    if (!buf || !state_machine_transition(STATE_RECEIVING)) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory or busy");
        return ESP_OK;
    }
    for (int i = 0; i < PERF_SD_PIECE; i++) buf[i] = (char)(i * 31 + 7);

    usb_msc_disconnect();
    if (storage_manager_is_sd_present() && !sd_card_is_mounted() && sd_card_remount() == ESP_ERR_TIMEOUT) {
        free(buf);
        prepare_abort_reconnect();
        httpd_resp_set_status(req, "423 Locked");
        httpd_resp_sendstr(req, "{\"error\":\"machine_busy\"}");
        return ESP_OK;
    }
    if (!storage_manager_fatfs_available()) storage_manager_remount();
    uint64_t vol_total = 0, vol_free = 0;
    const char *vol_type = NULL;
    if (!storage_manager_fatfs_available() ||
        storage_manager_get_info(&vol_total, &vol_free, &vol_type) != ESP_OK ||
        vol_free < (uint64_t)size + space_margin(vol_total)) {
        free(buf);
        prepare_abort_reconnect();
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "Not enough free space (or drive not mounted) for the SD test");
        return ESP_OK;
    }
    char path[48];
    snprintf(path, sizeof(path), "%s/FXPERF.TMP", STORAGE_PATH);
    FILE *f = fopen(path, "wb");
    uint64_t done = 0;
    int pieces = 0;
    int64_t t0 = esp_timer_get_time();
    while (f && done < (uint64_t)size) {
        size_t n = (uint64_t)size - done < PERF_SD_PIECE ? (size_t)((uint64_t)size - done) : PERF_SD_PIECE;
        if (fwrite(buf, 1, n, f) != n) break;
        done += n;
        if (PERF_SD_FSYNC_EVERY > 0 && ++pieces % (PERF_SD_FSYNC_EVERY > 0 ? PERF_SD_FSYNC_EVERY : 1) == 0) {
            fflush(f);
            fsync(fileno(f));
        }
    }
    if (f) {
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    int64_t us = esp_timer_get_time() - t0;
    remove(path);
    free(buf);
    storage_manager_flush_and_release_for_usb();
    state_machine_transition(STATE_IDLE);
    usb_msc_connect();
    if (done != (uint64_t)size) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD write failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "PERF SD write: %llu bytes in %lld ms = %.1f KB/s (%d-byte writes, fsync every %d)",
             (unsigned long long)done, (long long)(us / 1000), RX_KBPS(done, us), PERF_SD_PIECE,
             PERF_SD_FSYNC_EVERY);
    perf_reply(req, done, us);
    return ESP_OK;
}

// POST /radio {"qdbm":n,"save":true} sets the Wi-Fi TX power (save = kept over
// restarts); {"restore":true} goes back to the saved level; {} just reports.
// Reply: rssi, qdbm, brownouts, reset_reason, uptime_s.
static esp_err_t radio_post_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    char body[64] = {0};
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : 0;
    if (len > 0 && httpd_req_recv(req, body, len) != len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_OK;
    }
    cJSON *json = cJSON_Parse(body);
    cJSON *q = json ? cJSON_GetObjectItem(json, "qdbm") : NULL;
    bool bad = false;
    if (cJSON_IsNumber(q)) {
        if (radio_power_valid(q->valueint)) {
            radio_power_apply(q->valueint);
            if (cJSON_IsTrue(cJSON_GetObjectItem(json, "save"))) radio_power_save(q->valueint);
        } else {
            bad = true;
        }
    } else if (json && cJSON_IsTrue(cJSON_GetObjectItem(json, "restore"))) {
        radio_power_apply(radio_power_saved());
    }
    cJSON_Delete(json);
    if (bad) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported TX power level");
        return ESP_OK;
    }
    char out[128];
    snprintf(out, sizeof(out), "{\"rssi\":%d,\"qdbm\":%d,\"brownouts\":%lu,\"reset_reason\":\"%s\",\"uptime_s\":%lld}",
             radio_sta_rssi(), radio_power_actual(), (unsigned long)radio_brownouts(), radio_reset_reason(),
             (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

// GET /perf/source: sends test data until the caller closes (radio test, RX
// transmitting; capped at 100 MB).
static esp_err_t perf_source_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    memset(chunk_buf, 0x5A, sizeof(chunk_buf));
    httpd_resp_set_type(req, "application/octet-stream");
    for (uint32_t sent = 0; sent < 100u * 1024 * 1024; sent += sizeof(chunk_buf)) {
        if (httpd_resp_send_chunk(req, chunk_buf, sizeof(chunk_buf)) != ESP_OK) return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ─── Firmware update (streamed from the transmitter, never stored) ─────────
// POST /fw/begin {"size":n} -> {chunk_size}; /fw/chunk X-Offset + raw body
// (<= RECEIVER_CHUNK_BUFSIZE) -> {written}; /fw/finish -> {version}, then
// restart; /fw/cancel. Paired only; refused while the machine reads the
// drive or a file transfer runs.
static void fw_reply(httpd_req_t *req, int code, const char *json, const char *msg) {
    if (code == 200) {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_sendstr(req, json);
        return;
    }
    char status[40];
    snprintf(status, sizeof(status), "%d %s", code,
             code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 409 ? "Conflict" :
             code == 410 ? "Gone" : "Internal Server Error");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, msg);
}

static bool fw_request_allowed(httpd_req_t *req) {
    if (reject_unauthorized(req)) return false;
    if (!pairing_is_paired()) {
        drain_request_body(req);
        fw_reply(req, 403, NULL, "Not paired");
        return false;
    }
    return true;
}

static esp_err_t fw_begin_handler(httpd_req_t *req) {
    if (!fw_request_allowed(req)) return ESP_OK;
    if (reject_if_machine_busy(req)) { // X-Force is never sent for firmware
        drain_request_body(req);
        return ESP_OK;
    }
    char body[64] = {0};
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : 0;
    if (len <= 0 || httpd_req_recv(req, body, len) != len) {
        drain_request_body(req);
        fw_reply(req, 400, NULL, "Invalid request body");
        return ESP_OK;
    }
    cJSON *json = cJSON_Parse(body);
    cJSON *j_size = json ? cJSON_GetObjectItem(json, "size") : NULL;
    double size = cJSON_IsNumber(j_size) ? j_size->valuedouble : -1;
    cJSON_Delete(json);
    char msg[96] = "";
    int code;
    if (state_machine_get() == STATE_RECEIVING) {
        code = 409;
        snprintf(msg, sizeof(msg), "A file transfer is running");
    } else if (size <= 0) {
        code = 400;
        snprintf(msg, sizeof(msg), "Missing size");
    } else {
        code = fw_update_begin((size_t)size, msg, sizeof(msg));
    }
    char resp[40];
    snprintf(resp, sizeof(resp), "{\"chunk_size\":%d}", RECEIVER_CHUNK_BUFSIZE);
    fw_reply(req, code, resp, msg);
    return ESP_OK;
}

static esp_err_t fw_chunk_handler(httpd_req_t *req) {
    if (!fw_request_allowed(req)) return ESP_OK;
    char off_hdr[16] = {0};
    char *end = NULL;
    long offset = -1;
    if (httpd_req_get_hdr_value_str(req, "X-Offset", off_hdr, sizeof(off_hdr)) == ESP_OK) {
        offset = strtol(off_hdr, &end, 10);
    }
    int len = req->content_len;
    int got = 0;
    if (offset >= 0 && end != off_hdr && *end == '\0' && len > 0 && len <= RECEIVER_CHUNK_BUFSIZE) {
        while (got < len) {
            int r = httpd_req_recv(req, chunk_buf + got, len - got);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (r <= 0) break;
            got += r;
        }
    }
    if (got != len || len <= 0) {
        drain_request_body(req);
        fw_reply(req, 400, NULL, "Missing X-Offset or bad chunk");
        return ESP_OK;
    }
    char msg[96] = "";
    size_t written = 0;
    int code = fw_update_write((size_t)offset, (const uint8_t *)chunk_buf, len, &written, msg, sizeof(msg));
    char resp[40];
    snprintf(resp, sizeof(resp), "{\"written\":%u}", (unsigned)written);
    fw_reply(req, code, resp, msg);
    return ESP_OK;
}

static void fw_restart(void *arg) {
    (void)arg;
    esp_restart();
}

static esp_err_t fw_finish_handler(httpd_req_t *req) {
    if (!fw_request_allowed(req)) return ESP_OK;
    char msg[96] = "";
    char ver[32] = "";
    int code = fw_update_finish(ver, sizeof(ver), msg, sizeof(msg));
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"version\":\"%s\"}", ver);
    fw_reply(req, code, resp, msg);
    if (code == 200) {
        ESP_LOGW(TAG, "Firmware %s installed; restarting in 1 s", ver);
        static esp_timer_handle_t t;
        const esp_timer_create_args_t args = {.callback = fw_restart, .name = "fw_restart"};
        if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_once(t, 1000000);
    }
    return ESP_OK;
}

static esp_err_t fw_cancel_handler(httpd_req_t *req) {
    if (!fw_request_allowed(req)) return ESP_OK;
    fw_update_abort();
    fw_reply(req, 200, "{\"status\":\"cancelled\"}", NULL);
    return ESP_OK;
}

// GET /coredump: last crash report (paired only; it can hold RAM contents)
static esp_err_t coredump_get_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    if (!pairing_is_paired()) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "Not paired");
        return ESP_OK;
    }
    return coredump_http_send(req, "receiver-coredump.bin");
}

// POST /coredump/clear: erases the saved crash report.
static esp_err_t coredump_clear_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    drain_request_body(req);
    if (!pairing_is_paired()) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "Not paired");
        return ESP_OK;
    }
    if (state_machine_get() == STATE_RECEIVING) { // the flash erase would stall it
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "A file transfer is running");
        return ESP_OK;
    }
    if (coredump_http_clear() != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Couldn't erase the crash report");
        return ESP_OK;
    }
    pairing_heartbeat_now(); // the transmitter's flag follows at once
    httpd_resp_sendstr(req, "{}");
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", pairing_get_id());
    file_receiver_add_status_fields(obj);
    char *out = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return ESP_OK;
}

static esp_err_t clear_files_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
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

// POST /delete {"files":["a.nc","b.nc"]}: deletes files in the storage root in
// one mount cycle. Names use the upload validation (no path parts), folders are
// refused, missing files are skipped and reported. Busy guard as for /clear.
#define DELETE_MAX_FILES FILE_CACHE_MAX
static esp_err_t delete_post_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= (int)sizeof(chunk_buf)) {
        drain_request_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing or too large");
        return ESP_FAIL;
    }
    int cur_len = 0;
    while (cur_len < total_len) {
        int r = httpd_req_recv(req, chunk_buf + cur_len, total_len - cur_len);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive Error");
            return ESP_FAIL;
        }
        cur_len += r;
    }
    chunk_buf[cur_len] = '\0';

    cJSON *json = cJSON_Parse(chunk_buf);
    cJSON *files = json ? cJSON_GetObjectItem(json, "files") : NULL;
    int count = cJSON_IsArray(files) ? cJSON_GetArraySize(files) : 0;
    const char *problem = NULL;
    if (count == 0) {
        problem = "Expected {\"files\":[names]}";
    } else if (count > DELETE_MAX_FILES) {
        problem = "Too many files in one request";
    }
    for (int i = 0; !problem && i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(files, i);
        if (!cJSON_IsString(item) || strlen(item->valuestring) > 127 ||
            !filename_is_valid(item->valuestring)) {
            problem = "Invalid filename: no / \\ : .. or control characters, not empty or dots-only";
        }
    }
    if (problem) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, problem);
        return ESP_FAIL;
    }
    if (state_machine_get() == STATE_RECEIVING) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Receiver Busy", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (sd_card_has_card_error()) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Card unreadable - format FAT32 on PC or use Format");
        return ESP_FAIL;
    }
    if (reject_if_machine_busy(req)) {
        cJSON_Delete(json);
        return ESP_OK;
    }

    // One cycle: USB off -> mount -> delete all -> snapshot -> unmount -> USB on
    usb_msc_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!storage_manager_fatfs_available() || (storage_manager_is_sd_present() && !sd_card_is_mounted())) {
        storage_manager_remount();
    }
    if (!active_storage_mounted()) {
        cJSON_Delete(json);
        storage_manager_flush_and_release_for_usb();
        usb_msc_connect();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot mount storage");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *deleted = cJSON_AddArrayToObject(resp, "deleted");
    cJSON *skipped = cJSON_AddArrayToObject(resp, "skipped");
    for (int i = 0; i < count; i++) {
        const char *name = cJSON_GetArrayItem(files, i)->valuestring;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", STORAGE_PATH, name);
        struct stat st;
        const char *why = NULL;
        if (stat(path, &st) != 0) {
            why = "not found";
        } else if (S_ISDIR(st.st_mode)) {
            why = "is a folder";
        } else if (remove(path) != 0) {
            why = "delete failed";
        }
        if (why) {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddStringToObject(s, "name", name);
            cJSON_AddStringToObject(s, "reason", why);
            cJSON_AddItemToArray(skipped, s);
            ESP_LOGW(TAG, "Delete skipped %s: %s", name, why);
        } else {
            cJSON_AddItemToArray(deleted, cJSON_CreateString(name));
            ESP_LOGI(TAG, "Deleted %s", path);
        }
    }
    cJSON_Delete(json);

    snapshot_storage(); // still mounted: cache the new list + free space
    cJSON_AddNumberToObject(resp, "free_bytes", (double)s_cache_free);
    cJSON_AddNumberToObject(resp, "total_bytes", (double)s_cache_total);
    storage_manager_flush_and_release_for_usb();
    vTaskDelay(pdMS_TO_TICKS(1000)); // let the host notice the disconnect
    usb_msc_connect();

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, out ? out : "{}", HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return ESP_OK;
}

static esp_err_t format_storage_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
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

    if (ret == ESP_ERR_INVALID_SIZE) {
        usb_msc_notify_change();
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "Card too small for FAT32 with 4 KB clusters (about 270 MB minimum). Nothing was erased.");
        return ESP_OK;
    }
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
    if (reject_unauthorized(req)) return ESP_OK;
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

// Receiver's own page: information only, no actions (those go through the transmitter).
static esp_err_t index_get_handler(httpd_req_t *req) {
    cJSON *st = cJSON_CreateObject();
    file_receiver_add_status_fields(st);
    char name[32];
    pairing_get_name(name, sizeof(name));
    cJSON *j_state = cJSON_GetObjectItem(st, "state");
    cJSON *j_type = cJSON_GetObjectItem(st, "storage_type");
    cJSON *j_fw = cJSON_GetObjectItem(st, "fw");
    cJSON *j_total = cJSON_GetObjectItem(st, "total_bytes");
    cJSON *j_free = cJSON_GetObjectItem(st, "free_bytes");

    char page[1024];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html><html><head><meta charset='utf-8'><title>CNC Receiver</title>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<style>body{font-family:Arial;margin:24px;background:#1a1a2e;color:#fff}"
             "h1{color:#e94560;font-size:1.4em}td{padding:6px 12px 6px 0}</style></head><body>"
             "<h1>FileXfer Receiver</h1><table>"
             "<tr><td>ID</td><td>%s</td></tr><tr><td>Name</td><td>%s</td></tr>"
             "<tr><td>Paired</td><td>%s</td></tr><tr><td>State</td><td>%s</td></tr>"
             "<tr><td>Storage</td><td>%s, %.1f MB free of %.1f MB</td></tr>"
             "<tr><td>Firmware</td><td>%s</td></tr></table>"
             "<p>Upload and manage files from the transmitter page.%s</p></body></html>",
             pairing_get_id(), name, pairing_is_paired() ? "yes" : "no - approve it on the transmitter",
             j_state->valuestring, j_type->valuestring, j_free->valuedouble / 1048576.0,
             j_total->valuedouble / 1048576.0, j_fw->valuestring,
             pairing_is_paired() ? " To re-pair, hold BOOT for 10 s." : "");
    cJSON_Delete(st);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t download_get_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
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

// POST /pair {"key":"<64 hex>","name":"..."}: accepted only while unpaired.
static esp_err_t pair_post_handler(httpd_req_t *req) {
    char buf[192];
    int len = 0;
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(buf)) {
        drain_request_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing or too large");
        return ESP_FAIL;
    }
    while (len < req->content_len) {
        int r = httpd_req_recv(req, buf + len, req->content_len - len);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        len += r;
    }
    buf[len] = '\0';
    if (pairing_is_paired()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "Already paired - hold BOOT 10 s on the receiver to reset");
        return ESP_OK;
    }
    cJSON *json = cJSON_Parse(buf);
    cJSON *key = json ? cJSON_GetObjectItem(json, "key") : NULL;
    cJSON *name = json ? cJSON_GetObjectItem(json, "name") : NULL;
    esp_err_t err = pairing_set(cJSON_IsString(key) ? key->valuestring : NULL,
                                cJSON_IsString(name) ? name->valuestring : NULL);
    cJSON_Delete(json);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key must be 64 lowercase hex characters");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"status\":\"paired\"}");
    return ESP_OK;
}

// POST /unpair (with X-Auth-Key): the transmitter removed this receiver.
static esp_err_t unpair_post_handler(httpd_req_t *req) {
    if (reject_unauthorized(req)) return ESP_OK;
    drain_request_body(req);
    pairing_clear();
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"status\":\"unpaired\"}");
    return ESP_OK;
}

// Response headers and body leave at once instead of waiting for the peer's
// delayed ACK (up to 250 ms per chunk).
static esp_err_t httpd_sock_open(httpd_handle_t hd, int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return ESP_OK;
}

void file_receiver_init(void) {
    ESP_LOGI(TAG, "File receiver HTTP server starting...");

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.open_fn = httpd_sock_open;
    config.max_open_sockets = 4;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.stack_size = 10240;
    config.max_uri_handlers = 24;

    if (httpd_start(&server, &config) == ESP_OK) {
        s_server = server;
        const esp_timer_create_args_t watch_args = {.callback = xfer_watch, .name = "xfer_watch"};
        esp_timer_handle_t watch;
        if (esp_timer_create(&watch_args, &watch) == ESP_OK) {
            esp_timer_start_periodic(watch, 1000LL * 1000); // drive back within ~61 s
        }
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
        httpd_uri_t uri_pair = {.uri = "/pair", .method = HTTP_POST, .handler = pair_post_handler};
        httpd_uri_t uri_unpair = {.uri = "/unpair", .method = HTTP_POST, .handler = unpair_post_handler};
        httpd_uri_t uri_delete = {.uri = "/delete", .method = HTTP_POST, .handler = delete_post_handler};
        httpd_uri_t uri_perf_sink = {.uri = "/perf/sink", .method = HTTP_POST, .handler = perf_sink_handler};
        httpd_uri_t uri_perf_sd = {.uri = "/perf/sd", .method = HTTP_POST, .handler = perf_sd_handler};
        httpd_uri_t uri_perf_source = {.uri = "/perf/source", .method = HTTP_GET, .handler = perf_source_handler};
        httpd_uri_t uri_radio = {.uri = "/radio", .method = HTTP_POST, .handler = radio_post_handler};
        httpd_uri_t uri_coredump = {.uri = "/coredump", .method = HTTP_GET, .handler = coredump_get_handler};
        httpd_uri_t uri_coredump_clear = {.uri = "/coredump/clear", .method = HTTP_POST, .handler = coredump_clear_handler};
        httpd_uri_t uri_fw_begin = {.uri = "/fw/begin", .method = HTTP_POST, .handler = fw_begin_handler};
        httpd_uri_t uri_fw_chunk = {.uri = "/fw/chunk", .method = HTTP_POST, .handler = fw_chunk_handler};
        httpd_uri_t uri_fw_finish = {.uri = "/fw/finish", .method = HTTP_POST, .handler = fw_finish_handler};
        httpd_uri_t uri_fw_cancel = {.uri = "/fw/cancel", .method = HTTP_POST, .handler = fw_cancel_handler};

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
        httpd_register_uri_handler(server, &uri_pair);
        httpd_register_uri_handler(server, &uri_unpair);
        httpd_register_uri_handler(server, &uri_delete);
        httpd_register_uri_handler(server, &uri_perf_sink);
        httpd_register_uri_handler(server, &uri_perf_sd);
        httpd_register_uri_handler(server, &uri_perf_source);
        httpd_register_uri_handler(server, &uri_radio);
        httpd_register_uri_handler(server, &uri_coredump);
        httpd_register_uri_handler(server, &uri_coredump_clear);
        httpd_register_uri_handler(server, &uri_fw_begin);
        httpd_register_uri_handler(server, &uri_fw_chunk);
        httpd_register_uri_handler(server, &uri_fw_finish);
        httpd_register_uri_handler(server, &uri_fw_cancel);

        ESP_LOGI(TAG, "File receiver HTTP server started on port %d", config.server_port);
    } else {
        ESP_LOGE(TAG, "Failed to start file receiver HTTP server");
    }
}
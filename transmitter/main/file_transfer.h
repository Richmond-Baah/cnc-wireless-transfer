#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ─── Browser-driven upload (one session at a time) ──────────────────────────
// All return an HTTP status for the browser (200 on success) and put a
// human-readable reason in msg on failure.
#define UPLOAD_CHUNK_MAX (32 * 1024)

// Calls receiver /prepare (force = bypass machine-busy guard); on success
// writes the session id (16 hex chars) to session_out.
int file_transfer_begin(const char *name, size_t size, const char *md5, bool force,
                        char *session_out, size_t session_len,
                        char *msg, size_t msg_len);
// Forwards one chunk to receiver /chunk and returns after the receiver acks;
// acked_out = total bytes acknowledged so far.
int file_transfer_chunk(const char *session, int index, const char *data, int len,
                        uint64_t *acked_out, char *msg, size_t msg_len);
// Calls receiver /finalize with the browser's MD5 and ends the session.
int file_transfer_finish(const char *session, char *msg, size_t msg_len);
// Ends the session (if it matches, or none is given) and calls receiver /cancel.
int file_transfer_cancel(const char *session);

// Get the current operational status of the receiver (polls receiver endpoint)
int file_transfer_get_receiver_status(char *out_state_str, size_t max_len);

// Request the receiver to clear its storage
// POSTs {"files":[...]} to the receiver /delete; resp gets its body. Returns HTTP status or -1.
int file_transfer_delete_receiver(const char *json, bool force, char *resp, size_t resp_len);
int file_transfer_clear_receiver(bool force); // sends {"confirm":"WIPE"}; force bypasses busy guard

// Request the receiver to perform a full format of its storage
int file_transfer_format_receiver(void);

// Start the background task that polls the receiver status every 2 s.
// Must be called once after Wi-Fi and the web server are both up.
void file_transfer_status_task_start(void);
int file_transfer_cancel_receiver(void);

// Fetch the list of files from the receiver
int file_transfer_get_receiver_files(char *out_json, size_t max_len);

// Helper to check if receiver is currently online (cached, non-blocking)
bool file_transfer_is_receiver_online(void);

// Helper to get the full URL for a receiver endpoint
void get_receiver_url(const char *path, char *out_url, size_t max_len);

#endif // FILE_TRANSFER_H

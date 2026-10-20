#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include "esp_http_server.h"
#include "rx_registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ─── Browser-driven upload (one session at a time, across all receivers) ────
// All return an HTTP status for the browser (200 on success) and put a
// human-readable reason in msg on failure.
#define UPLOAD_CHUNK_MAX (32 * 1024)

// Calls /prepare on receiver rx_id (force = bypass machine-busy guard); on
// success writes the session id (16 hex chars) to session_out. 409 if another
// receiver has an active session.
int file_transfer_begin(const char *rx_id, const char *name, size_t size, const char *md5, bool force,
                        char *session_out, size_t session_len,
                        char *msg, size_t msg_len);
// Forwards one chunk to the session's receiver and returns after it acks;
// acked_out = total bytes acknowledged so far.
int file_transfer_chunk(const char *session, int index, const char *data, int len,
                        uint64_t *acked_out, char *msg, size_t msg_len);
// Calls receiver /finalize with the browser's MD5 and ends the session.
int file_transfer_finish(const char *session, char *msg, size_t msg_len);
// Ends the session (if it matches, or none is given) and calls receiver /cancel.
int file_transfer_cancel(const char *session);
// True while a session is active; rx_id_out gets its receiver.
bool file_transfer_active_rx(char *rx_id_out, size_t len);

// Creates the session lock and the idle-session reaper task.
void file_transfer_start(void);

// ─── Per-receiver proxies (rx_id = paired receiver; 404 unknown, 502 offline) ─
// Receiver status JSON from the heartbeat cache (never contacts the receiver).
int file_transfer_get_receiver_status(const char *rx_id, char *out, size_t out_len);
// POSTs {"files":[...]} to /delete; resp gets the receiver's body.
int file_transfer_delete_receiver(const char *rx_id, const char *json, bool force, char *resp, size_t resp_len);
// Sends {"confirm":"WIPE"} to /clear; force bypasses the busy guard.
int file_transfer_clear_receiver(const char *rx_id, bool force, char *resp, size_t resp_len);
int file_transfer_format_receiver(const char *rx_id, char *resp, size_t resp_len);
int file_transfer_get_receiver_files(const char *rx_id, char *out_json, size_t max_len);

#endif // FILE_TRANSFER_H

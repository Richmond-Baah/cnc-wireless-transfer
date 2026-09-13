#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>


// Forward incoming multipart body to the receiver in chunks
bool file_transfer_forward_to_receiver(httpd_req_t *req, const char *filename,
                                       const char *expected_md5,
                                       size_t filesize);

// Get the current operational status of the receiver (polls receiver endpoint)
int file_transfer_get_receiver_status(char *out_state_str, size_t max_len);

// Request the receiver to clear its storage
int file_transfer_clear_receiver(void);

// Request the receiver to perform a full format of its storage
int file_transfer_format_receiver(void);
void file_transfer_abort(void);

// Start the background task that polls the receiver status every 2 s.
// Must be called once after Wi-Fi and the web server are both up.
void file_transfer_status_task_start(void);
int file_transfer_cancel_receiver(void);
bool file_transfer_is_aborted(void);
void file_transfer_clear_abort(void);

// Fetch the list of files from the receiver
int file_transfer_get_receiver_files(char *out_json, size_t max_len);

// Helper to get the full URL for a receiver endpoint
void get_receiver_url(const char *path, char *out_url, size_t max_len);

#endif // FILE_TRANSFER_H

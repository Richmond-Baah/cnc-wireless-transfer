#ifndef FILE_RECEIVER_H
#define FILE_RECEIVER_H

#include <stdbool.h>
#include <stddef.h>

void file_receiver_init(void);

// Snapshot the file list + total/free space into RAM (mounting and releasing
// storage if needed). Call only while the USB host cannot access the storage.
void file_receiver_refresh_cache(void);

// Reads (up to len-1 bytes, NUL-terminated) and deletes a file in the storage
// root, mounting and releasing storage if needed. Boot only, before USB.
// True if the file existed (buf is empty when it was too large).
bool file_receiver_take_root_file(const char *name, char *buf, size_t len);

// Adds fw, state, storage_type, total_bytes, free_bytes, card_error, busy (for /status and the heartbeat).
typedef struct cJSON cJSON;
void file_receiver_add_status_fields(cJSON *obj);

#endif // FILE_RECEIVER_H
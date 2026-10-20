#ifndef FILE_RECEIVER_H
#define FILE_RECEIVER_H

void file_receiver_init(void);

// Snapshot the file list + total/free space into RAM (mounting and releasing
// storage if needed). Call only while the USB host cannot access the storage.
void file_receiver_refresh_cache(void);

// Adds fw, state, storage_type, total_bytes, free_bytes, card_error, busy (for /status and the heartbeat).
typedef struct cJSON cJSON;
void file_receiver_add_status_fields(cJSON *obj);

#endif // FILE_RECEIVER_H
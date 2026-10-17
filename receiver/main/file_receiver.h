#ifndef FILE_RECEIVER_H
#define FILE_RECEIVER_H

void file_receiver_init(void);

// Snapshot the file list + total/free space into RAM (mounting and releasing
// storage if needed). Call only while the USB host cannot access the storage.
void file_receiver_refresh_cache(void);

#endif // FILE_RECEIVER_H
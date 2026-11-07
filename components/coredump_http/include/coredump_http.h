#pragma once

#include "esp_http_server.h"
#include <stdbool.h>

// Sends the core dump saved in the coredump partition as
// application/octet-stream (coredump.bin, for `idf.py coredump-info -c`),
// or 404 "No crash report saved". The caller checks authorization.
esp_err_t coredump_http_send(httpd_req_t *req, const char *filename);

// A valid crash report is saved. Checked once (reads the whole dump), so call it
// first from app_main: a crash only writes a new one just before a restart.
bool coredump_http_present(void);
// Erases the saved crash report.
esp_err_t coredump_http_clear(void);

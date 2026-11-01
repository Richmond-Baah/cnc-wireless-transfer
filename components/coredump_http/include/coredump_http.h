#pragma once

#include "esp_http_server.h"

// Sends the core dump saved in the coredump partition as
// application/octet-stream (coredump.bin, for `idf.py coredump-info -c`),
// or 404 "No crash report saved". The caller checks authorization.
esp_err_t coredump_http_send(httpd_req_t *req, const char *filename);

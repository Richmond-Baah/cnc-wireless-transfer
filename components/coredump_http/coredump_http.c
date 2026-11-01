#include "coredump_http.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "COREDUMP";

esp_err_t coredump_http_send(httpd_req_t *req, const char *filename) {
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK ||
      size == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "No crash report saved");
    return ESP_OK;
  }
  char disp[80];
  snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", disp);
  static char buf[1024]; // httpd task (or one rx worker) only
  for (size_t off = 0; off < size;) {
    size_t n = size - off < sizeof(buf) ? size - off : sizeof(buf);
    if (esp_flash_read(NULL, buf, addr + off, n) != ESP_OK ||
        httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
      ESP_LOGW(TAG, "Core dump send failed at %u/%u", (unsigned)off, (unsigned)size);
      httpd_resp_send_chunk(req, NULL, 0);
      return ESP_FAIL;
    }
    off += n;
  }
  ESP_LOGI(TAG, "Core dump sent (%u bytes)", (unsigned)size);
  return httpd_resp_send_chunk(req, NULL, 0);
}

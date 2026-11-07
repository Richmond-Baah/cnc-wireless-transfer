#include "stack_audit.h"

#ifdef FILEXFER_STACK_AUDIT
#include "esp_log.h"
#include "esp_private/freertos_debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIT_PERIOD_MS 60000
#define AUDIT_LOW_BYTES 512
#define AUDIT_MAX_TASKS 32

static const char *TAG = "STACK";
static const char *s_who;
static void (*s_publish)(const char *text);

typedef struct {
  char name[configMAX_TASK_NAME_LEN];
  unsigned size, free;
} audit_row_t;

static void log_row(const char *who, const char *name, unsigned size, unsigned free_b) {
  ESP_LOGI(TAG, "STACK %s %s size=%u free=%u%s", who, name, size, free_b, free_b < AUDIT_LOW_BYTES ? " LOW" : "");
}

void stack_audit_log_report(const char *who, char *text) {
  char *save = NULL;
  for (char *row = strtok_r(text, ";", &save); row; row = strtok_r(NULL, ";", &save)) {
    // Task names can contain spaces: the two numbers are counted from the end
    char *b = strrchr(row, ' ');
    if (b == NULL) continue;
    unsigned free_b = strtoul(b + 1, NULL, 10);
    *b = '\0';
    char *a = strrchr(row, ' ');
    if (a == NULL) continue;
    unsigned size = strtoul(a + 1, NULL, 10);
    *a = '\0';
    log_row(who, row, size, free_b);
  }
}

static void audit_task(void *arg) {
  (void)arg;
  static audit_row_t rows[AUDIT_MAX_TASKS];
  static char text[AUDIT_MAX_TASKS * 32];
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(AUDIT_PERIOD_MS));
    int n = 0;
    TaskIterator_t it = {0};
    vTaskSuspendAll();
    while (xTaskGetNext(&it) != -1 && n < AUDIT_MAX_TASKS) {
      TaskSnapshot_t snap;
      if (it.pxTaskHandle == NULL || vTaskGetSnapshot(it.pxTaskHandle, &snap) != pdTRUE) continue;
      strlcpy(rows[n].name, pcTaskGetName(it.pxTaskHandle), sizeof(rows[n].name));
      rows[n].size = (char *)snap.pxEndOfStack - (char *)pxTaskGetStackStart(it.pxTaskHandle);
      rows[n].free = uxTaskGetStackHighWaterMark(it.pxTaskHandle); // bytes on ESP-IDF
      n++;
    }
    xTaskResumeAll();
    size_t pos = 0;
    for (int i = 0; i < n; i++) {
      log_row(s_who, rows[i].name, rows[i].size, rows[i].free);
      pos += snprintf(text + pos, sizeof(text) - pos, "%s %u %u;", rows[i].name, rows[i].size, rows[i].free);
    }
    if (s_publish) s_publish(text);
  }
}

void stack_audit_start(const char *who, void (*publish)(const char *text)) {
  s_who = who;
  s_publish = publish;
  xTaskCreate(audit_task, "stack_audit", 8192, NULL, 1, NULL);
}
#endif

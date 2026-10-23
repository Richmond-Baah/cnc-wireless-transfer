#include "espnow_pair.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "pair_proto.h"
#include "rx_registry.h"
#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ESPNOW_PAIR";

#define MAX_DELIVERIES 4
#define APPROVE_MIN_LEFT_S 60 // keep listening this long after an approval

typedef struct {
  uint8_t mac[6];
  pair_req_t req;
} rx_req_t;

// Approved receiver waiting for its PAIR_CFG (only the sealed message is kept)
typedef struct {
  char id[RX_ID_LEN];
  uint8_t mac[6];
  uint8_t rx_pub[32];
  pair_cfg_msg_t msg;
} delivery_t;

static SemaphoreHandle_t s_lock; // ESP-NOW on/off, window, deliveries
static QueueHandle_t s_req_queue;
static bool s_on;
static int64_t s_window_end_us;
static delivery_t s_deliv[MAX_DELIVERIES];

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

// Wi-Fi task context: copy and hand over
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(pair_req_t)) return;
  rx_req_t r;
  memcpy(r.mac, info->src_addr, 6);
  memcpy(&r.req, data, sizeof(r.req));
  if (r.req.magic == PAIR_MAGIC && r.req.type == PAIR_MSG_REQ && r.req.ver == PAIR_PROTO_VER) {
    xQueueSend(s_req_queue, &r, 0);
  }
}

// Called with the lock held, ESP-NOW on
static void send_locked(const delivery_t *d) {
  if (!esp_now_is_peer_exist(d->mac)) {
    esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_AP, .encrypt = false};
    memcpy(peer.peer_addr, d->mac, 6);
    esp_now_add_peer(&peer);
  }
  esp_err_t err = esp_now_send(d->mac, (const uint8_t *)&d->msg, sizeof(d->msg));
  if (err != ESP_OK) ESP_LOGW(TAG, "PAIR_CFG to %s: %s", d->id, esp_err_to_name(err));
}

// Called with the lock held
static void close_locked(void) {
  if (s_on) {
    esp_now_deinit();
    s_on = false;
    ESP_LOGI(TAG, "Add-machine window closed");
  }
  s_window_end_us = 0;
  mbedtls_platform_zeroize(s_deliv, sizeof(s_deliv));
}

static void handle_req(const rx_req_t *r) {
  char id[RX_ID_LEN], fw[sizeof(r->req.fw) + 1];
  strlcpy(id, r->req.id, sizeof(id));
  memcpy(fw, r->req.fw, sizeof(r->req.fw));
  fw[sizeof(r->req.fw)] = '\0';
  lock();
  if (!s_on) {
    unlock();
    return;
  }
  for (int i = 0; i < MAX_DELIVERIES; i++) {
    delivery_t *d = &s_deliv[i];
    if (d->id[0] == '\0' || strcmp(d->id, id) != 0) continue;
    if (memcmp(d->rx_pub, r->req.pub, 32) == 0) {
      // Approved already: resend now that it listens on our channel
      memcpy(d->mac, r->mac, 6);
      send_locked(d);
      unlock();
      return;
    }
    // Rebooted (new key pair): the sealed config is useless, approve again
    mbedtls_platform_zeroize(d, sizeof(*d));
  }
  unlock();
  rx_registry_espnow_found(id, fw, r->mac, r->req.pub);
}

static void espnow_task(void *arg) {
  (void)arg;
  rx_req_t r;
  for (;;) {
    if (xQueueReceive(s_req_queue, &r, pdMS_TO_TICKS(500)) == pdTRUE) {
      handle_req(&r);
    }
    lock();
    if (s_on && esp_timer_get_time() >= s_window_end_us) close_locked();
    unlock();
  }
}

esp_err_t espnow_pair_open_window(int seconds) {
  if (s_lock == NULL) {
    s_lock = xSemaphoreCreateMutex();
    s_req_queue = xQueueCreate(8, sizeof(rx_req_t));
    xTaskCreate(espnow_task, "espnow_pair", 4096, NULL, 4, NULL);
  }
  lock();
  if (!s_on) {
    esp_err_t err = esp_now_init();
    if (err == ESP_OK) err = esp_now_register_recv_cb(on_recv);
    if (err != ESP_OK) {
      esp_now_deinit();
      unlock();
      ESP_LOGE(TAG, "ESP-NOW start failed: %s", esp_err_to_name(err));
      return err;
    }
    s_on = true;
  }
  s_window_end_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
  uint8_t ch = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&ch, &second);
  unlock();
  ESP_LOGI(TAG, "Add-machine window open for %d s (channel %u)", seconds, ch);
  return ESP_OK;
}

int espnow_pair_seconds_left(void) {
  int64_t end = s_window_end_us;
  int64_t left = end ? (end - esp_timer_get_time()) / 1000000 : 0;
  return left > 0 ? (int)left : 0;
}

int espnow_pair_send_cfg(const char *rx_id, const uint8_t mac[6], const uint8_t rx_pub[32],
                         const char *key_hex, const char *name, char *msg, size_t msg_len) {
  static pair_cfg_plain_t plain; // httpd task: keep the stack small
  memset(&plain, 0, sizeof(plain));
  wifi_get_ap_creds(plain.ssid, sizeof(plain.ssid), plain.pass, sizeof(plain.pass));
  for (int i = 0; i < 32; i++) sscanf(key_hex + 2 * i, "%2hhx", &plain.key[i]);
  strlcpy(plain.tx_id, rx_registry_tx_id(), sizeof(plain.tx_id));
  strlcpy(plain.name, name, sizeof(plain.name));

  static delivery_t d;
  memset(&d, 0, sizeof(d));
  strlcpy(d.id, rx_id, sizeof(d.id));
  memcpy(d.mac, mac, 6);
  memcpy(d.rx_pub, rx_pub, 32);
  int ret = pair_seal_cfg(&plain, rx_id, rx_pub, &d.msg);
  mbedtls_platform_zeroize(&plain, sizeof(plain));
  if (ret != 0) {
    snprintf(msg, msg_len, "Encryption failed (%d)", ret);
    return 500;
  }

  if (s_lock == NULL) {
    mbedtls_platform_zeroize(&d, sizeof(d));
    snprintf(msg, msg_len, "Add-machine window closed; click Add machine again");
    return 409;
  }
  int code = 200;
  lock();
  if (!s_on) {
    snprintf(msg, msg_len, "Add-machine window closed; click Add machine again");
    code = 409;
  } else {
    int slot = 0;
    for (int i = 0; i < MAX_DELIVERIES; i++) {
      if (strcmp(s_deliv[i].id, rx_id) == 0 || s_deliv[i].id[0] == '\0') { slot = i; break; }
    }
    s_deliv[slot] = d;
    if (s_window_end_us - esp_timer_get_time() < APPROVE_MIN_LEFT_S * 1000000LL) {
      s_window_end_us = esp_timer_get_time() + APPROVE_MIN_LEFT_S * 1000000LL;
    }
    send_locked(&s_deliv[slot]);
  }
  unlock();
  mbedtls_platform_zeroize(&d, sizeof(d));
  return code;
}

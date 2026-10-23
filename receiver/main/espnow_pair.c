#include "espnow_pair.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "pair_proto.h"
#include "pairing.h"
#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ESPNOW_PAIR";

#define CHANNEL_FIRST 1
#define CHANNEL_LAST 13
#define DWELL_MS 100      // listen for PAIR_CFG on each channel
#define WIFI_SLOT_MS 2000 // normal connect attempts between bursts

static QueueHandle_t s_cfg_queue; // pair_cfg_msg_t from the transmitter
static bool s_espnow_on;
static uint8_t s_priv[32], s_pub[32];
static bool s_have_keys;
static const uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// Wi-Fi task context: copy and hand over
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  if (len == sizeof(pair_cfg_msg_t) && ((const pair_cfg_msg_t *)data)->type == PAIR_MSG_CFG) {
    xQueueSend(s_cfg_queue, data, 0);
  }
}

static bool espnow_on(void) {
  if (s_espnow_on) return true;
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(on_recv);
  esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
  memcpy(peer.peer_addr, BROADCAST, 6);
  esp_now_add_peer(&peer);
  s_espnow_on = true;
  ESP_LOGI(TAG, "Unpaired: looking for a transmitter in \"Add machine\" mode");
  return true;
}

static void espnow_off(void) {
  if (!s_espnow_on) return;
  esp_now_deinit();
  s_espnow_on = false;
}

// Validates and applies a decrypted PAIR_CFG. True when now (provisionally) paired.
static bool apply_cfg(pair_cfg_plain_t *c) {
  c->ssid[sizeof(c->ssid) - 1] = '\0';
  c->pass[sizeof(c->pass) - 1] = '\0';
  c->tx_id[sizeof(c->tx_id) - 1] = '\0';
  c->name[sizeof(c->name) - 1] = '\0';
  size_t pass_len = strlen(c->pass);
  if (c->ssid[0] == '\0' || (pass_len > 0 && pass_len < 8)) {
    ESP_LOGW(TAG, "PAIR_CFG rejected: invalid Wi-Fi settings");
    return false;
  }
  char hex[PAIRING_KEY_HEX_LEN + 1];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", c->key[i]);
  esp_err_t err = pairing_set_provisional(hex, c->name, c->tx_id);
  mbedtls_platform_zeroize(hex, sizeof(hex));
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "PAIR_CFG rejected: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "PAIR_CFG from %s: joining SSID [%s]", c->tx_id, c->ssid);
  wifi_manager_pause_connect(false);
  wifi_manager_apply_creds(c->ssid, c->pass);
  return true;
}

// Waits up to ms for a PAIR_CFG addressed to this receiver.
static bool wait_cfg(int ms) {
  static pair_cfg_msg_t msg;
  static pair_cfg_plain_t plain;
  bool done = false;
  while (!done && xQueueReceive(s_cfg_queue, &msg, pdMS_TO_TICKS(ms)) == pdTRUE) {
    if (pair_open_cfg(&msg, pairing_get_id(), s_priv, s_pub, &plain) == 0) {
      done = apply_cfg(&plain);
      mbedtls_platform_zeroize(&plain, sizeof(plain));
    } else {
      ESP_LOGW(TAG, "PAIR_CFG ignored (not for this receiver or corrupt)");
    }
    ms = 1; // drain anything else queued
  }
  return done;
}

// One PAIR_REQ on every channel, listening DWELL_MS after each.
static bool burst(const pair_req_t *req) {
  wifi_manager_pause_connect(true);
  vTaskDelay(pdMS_TO_TICKS(50)); // let the abort settle before switching channel
  bool done = false;
  for (int ch = CHANNEL_FIRST; ch <= CHANNEL_LAST && !done; ch++) {
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) continue;
    esp_now_send(BROADCAST, (const uint8_t *)req, sizeof(*req));
    done = wait_cfg(DWELL_MS);
  }
  if (!done) wifi_manager_pause_connect(false);
  return done;
}

static void espnow_pair_task(void *arg) {
  (void)arg;
  pair_req_t req = {.magic = PAIR_MAGIC, .type = PAIR_MSG_REQ, .ver = PAIR_PROTO_VER};
  strlcpy(req.id, pairing_get_id(), sizeof(req.id));
  strlcpy(req.fw, esp_app_get_description()->version, sizeof(req.fw));
  for (;;) {
    char ip[16];
    // Paired, or already on the transmitter's Wi-Fi (HTTP heartbeat pairing)
    if (pairing_is_paired() || wifi_manager_get_ip(ip, sizeof(ip))) {
      espnow_off();
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    if (!s_have_keys) {
      s_have_keys = pair_x25519_keypair(s_priv, s_pub) == 0;
      if (!s_have_keys) {
        ESP_LOGE(TAG, "Key generation failed");
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }
      memcpy(req.pub, s_pub, sizeof(req.pub));
    }
    if (!espnow_on()) {
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    if (burst(&req)) {
      espnow_off();
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(WIFI_SLOT_MS));
  }
}

void espnow_pair_start(void) {
  s_cfg_queue = xQueueCreate(2, sizeof(pair_cfg_msg_t));
  xTaskCreate(espnow_pair_task, "espnow_pair", 6144, NULL, 3, NULL);
}

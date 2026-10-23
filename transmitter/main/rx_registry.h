#ifndef RX_REGISTRY_H
#define RX_REGISTRY_H

// Receivers known to this transmitter. Paired ones (id, name, key) persist in
// NVS; everything else (IP, state, storage, last heartbeat) is RAM-only and
// refreshed by each receiver's heartbeat every 10 s.

#include "cJSON.h"
#include "esp_http_client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RX_MAX_PAIRED 16
#define RX_MAX_PENDING 8
#define RX_ID_LEN 16
#define RX_NAME_LEN 32
#define RX_KEY_HEX_LEN 64
#define RX_ONLINE_US (30LL * 1000 * 1000) // online = heartbeat within 30 s

// What a proxy needs to talk to one paired, online receiver.
typedef struct {
  char id[RX_ID_LEN];
  char name[RX_NAME_LEN];
  char ip[16];
  char key[RX_KEY_HEX_LEN + 1];
} rx_target_t;

// One HTTP request to a receiver, with its X-Auth-Key (if set) and optional
// X-Force: 1. body may be NULL. Reads the reply into resp (NUL-terminated);
// *truncated (optional) is set if it didn't fit. Returns the HTTP status, or -1
// if unreachable (resp then holds the reason).
int rx_http(const rx_target_t *t, esp_http_client_method_t method, const char *path,
            const char *body, bool force, int timeout_ms, char *resp, size_t resp_len,
            bool *truncated);

void rx_registry_init(void);

// Handles a heartbeat body. Returns true if it came from a paired receiver
// with the right key; anything else lands in the pending list.
// A pre-registered key (ESP-NOW approval, pairing file) pairs the receiver
// that first heartbeats with it.
bool rx_registry_heartbeat(const cJSON *hb);

// PAIR_REQ heard during the add-machine window: listed as a new device.
void rx_registry_espnow_found(const char *id, const char *fw, const uint8_t mac[6],
                              const uint8_t pub[32]);

// "tx-" + last 3 AP MAC bytes
const char *rx_registry_tx_id(void);

// Paired + online receiver -> target. False: unknown id (404) or offline (502).
bool rx_registry_target(const char *id, rx_target_t *out, bool *known);

// Receiver auth for /rx/config (constant-time key compare).
bool rx_registry_check_key(const char *id, const char *key);

// Merges status fields (heartbeat, /delete reply) into the cached runtime info.
void rx_registry_update_status(const char *id, const cJSON *status);
// Adds the cached runtime fields (online, state, storage, busy...) of id to obj.
bool rx_registry_add_cached_status(const char *id, cJSON *obj);

// Pairing management. Return an HTTP status; msg gets a reason on failure.
int rx_registry_approve(const char *id, const char *name, char *msg, size_t msg_len);
int rx_registry_rename(const char *id, const char *name, char *msg, size_t msg_len);
// unpair: 200 with msg set when the receiver was offline (BOOT reset still needed)
int rx_registry_unpair(const char *id, char *msg, size_t msg_len);

// Pairing file: pre-registers a new key (any receiver, 24 h) under name and
// returns pair.cfg JSON {v, ssid, pass, key, tx_id, name, expiry} (caller frees
// with cJSON_free). client_now (browser clock, epoch s) sets our clock if it is
// unset or off by > 5 min. HTTP status; msg gets the reason on failure.
int rx_registry_make_pair_file(const char *name, int64_t client_now, char **out, char *msg,
                               size_t msg_len);

// {"receivers":[...],"pending":[...]} for the UI; caller frees with cJSON_free.
char *rx_registry_list_json(void);

// AP credential version: receivers pull /rx/config when theirs is older.
uint32_t rx_registry_config_version(void);
uint32_t rx_registry_bump_config_version(void);
void rx_registry_mark_config_fetched(const char *id, uint32_t ver);
// True when every online paired receiver has fetched version ver.
bool rx_registry_all_fetched(uint32_t ver);

#endif // RX_REGISTRY_H

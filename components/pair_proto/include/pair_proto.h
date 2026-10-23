#ifndef PAIR_PROTO_H
#define PAIR_PROTO_H

// ESP-NOW "Add machine" pairing, shared by receiver and transmitter.
// Receiver broadcasts PAIR_REQ (its ID + X25519 public key). After the user
// approves it, the transmitter answers PAIR_CFG by unicast: its own ephemeral
// X25519 public key + AES-256-GCM(key from ECDH) of pair_cfg_plain_t, with the
// receiver ID as associated data.

#include <stddef.h>
#include <stdint.h>

#define PAIR_MAGIC 0x52504E43u // "CNPR"
#define PAIR_PROTO_VER 1
#define PAIR_MSG_REQ 1
#define PAIR_MSG_CFG 2

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t type; // PAIR_MSG_REQ
  uint8_t ver;
  char id[16];  // "rx-xxxxxx", NUL-terminated
  char fw[32];
  uint8_t pub[32];
} pair_req_t;

typedef struct __attribute__((packed)) {
  char ssid[33];
  char pass[64];
  uint8_t key[32]; // receiver key (sent as 64 hex in X-Auth-Key)
  char tx_id[16];
  char name[32];
} pair_cfg_plain_t;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t type; // PAIR_MSG_CFG
  uint8_t ver;
  uint8_t tx_pub[32];
  uint8_t nonce[12];
  uint8_t ct[sizeof(pair_cfg_plain_t)];
  uint8_t tag[16];
} pair_cfg_msg_t;

_Static_assert(sizeof(pair_cfg_msg_t) <= 250, "PAIR_CFG must fit one ESP-NOW v1 frame");

// New X25519 key pair (private key little-endian). 0 on success.
int pair_x25519_keypair(uint8_t priv[32], uint8_t pub[32]);

// Session key = HKDF-SHA256(ECDH(my_priv, peer_pub), info = rx_pub || tx_pub).
int pair_derive_key(const uint8_t my_priv[32], const uint8_t peer_pub[32],
                    const uint8_t rx_pub[32], const uint8_t tx_pub[32], uint8_t out[32]);

// Transmitter: fills msg (header, tx_pub, nonce, ct, tag) for receiver rx_id.
int pair_seal_cfg(const pair_cfg_plain_t *plain, const char *rx_id, const uint8_t rx_pub[32],
                  pair_cfg_msg_t *msg);

// Receiver: decrypts msg with its key pair. 0 only if the tag verifies.
int pair_open_cfg(const pair_cfg_msg_t *msg, const char *rx_id, const uint8_t rx_priv[32],
                  const uint8_t rx_pub[32], pair_cfg_plain_t *plain);

#endif // PAIR_PROTO_H

#include "pair_proto.h"
#include "esp_random.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include <string.h>

static int rng(void *ctx, unsigned char *buf, size_t len) {
  (void)ctx;
  esp_fill_random(buf, len);
  return 0;
}

int pair_x25519_keypair(uint8_t priv[32], uint8_t pub[32]) {
  mbedtls_ecp_group grp;
  mbedtls_mpi d;
  mbedtls_ecp_point q;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&q);
  size_t olen = 0;
  int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
  if (ret == 0) ret = mbedtls_ecdh_gen_public(&grp, &d, &q, rng, NULL);
  if (ret == 0) ret = mbedtls_mpi_write_binary_le(&d, priv, 32);
  if (ret == 0) ret = mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub, 32);
  if (ret == 0 && olen != 32) ret = -1;
  mbedtls_ecp_point_free(&q);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ret;
}

static int hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *a, size_t a_len,
                       const uint8_t *b, size_t b_len, uint8_t out[32]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  int ret = mbedtls_md_setup(&ctx, md, 1);
  if (ret == 0) ret = mbedtls_md_hmac_starts(&ctx, key, key_len);
  if (ret == 0 && a_len) ret = mbedtls_md_hmac_update(&ctx, a, a_len);
  if (ret == 0 && b_len) ret = mbedtls_md_hmac_update(&ctx, b, b_len);
  if (ret == 0) ret = mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return ret;
}

int pair_derive_key(const uint8_t my_priv[32], const uint8_t peer_pub[32],
                    const uint8_t rx_pub[32], const uint8_t tx_pub[32], uint8_t out[32]) {
  mbedtls_ecp_group grp;
  mbedtls_mpi d, z;
  mbedtls_ecp_point p;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  mbedtls_ecp_point_init(&p);
  uint8_t shared[32];
  int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
  if (ret == 0) ret = mbedtls_mpi_read_binary_le(&d, my_priv, 32);
  if (ret == 0) ret = mbedtls_ecp_point_read_binary(&grp, &p, peer_pub, 32);
  if (ret == 0) ret = mbedtls_ecdh_compute_shared(&grp, &z, &p, &d, rng, NULL);
  if (ret == 0) ret = mbedtls_mpi_write_binary_le(&z, shared, 32);
  if (ret == 0) {
    // HKDF-SHA256 (RFC 5869), one output block
    static const uint8_t salt[] = "cnc-pair-v1";
    uint8_t prk[32], info[65];
    memcpy(info, rx_pub, 32);
    memcpy(info + 32, tx_pub, 32);
    info[64] = 0x01;
    ret = hmac_sha256(salt, sizeof(salt) - 1, shared, 32, NULL, 0, prk);
    if (ret == 0) ret = hmac_sha256(prk, 32, info, sizeof(info), NULL, 0, out);
    mbedtls_platform_zeroize(prk, sizeof(prk));
  }
  mbedtls_platform_zeroize(shared, sizeof(shared));
  mbedtls_ecp_point_free(&p);
  mbedtls_mpi_free(&z);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ret;
}

static void rx_id_aad(const char *rx_id, uint8_t aad[16]) {
  memset(aad, 0, 16);
  strncpy((char *)aad, rx_id, 15);
}

int pair_seal_cfg(const pair_cfg_plain_t *plain, const char *rx_id, const uint8_t rx_pub[32],
                  pair_cfg_msg_t *msg) {
  uint8_t tx_priv[32], key[32], aad[16];
  memset(msg, 0, sizeof(*msg));
  msg->magic = PAIR_MAGIC;
  msg->type = PAIR_MSG_CFG;
  msg->ver = PAIR_PROTO_VER;
  int ret = pair_x25519_keypair(tx_priv, msg->tx_pub);
  if (ret == 0) ret = pair_derive_key(tx_priv, rx_pub, rx_pub, msg->tx_pub, key);
  if (ret == 0) {
    esp_fill_random(msg->nonce, sizeof(msg->nonce));
    rx_id_aad(rx_id, aad);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret == 0) {
      ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(*plain), msg->nonce,
                                      sizeof(msg->nonce), aad, sizeof(aad), (const uint8_t *)plain,
                                      msg->ct, sizeof(msg->tag), msg->tag);
    }
    mbedtls_gcm_free(&gcm);
  }
  mbedtls_platform_zeroize(tx_priv, sizeof(tx_priv));
  mbedtls_platform_zeroize(key, sizeof(key));
  return ret;
}

int pair_open_cfg(const pair_cfg_msg_t *msg, const char *rx_id, const uint8_t rx_priv[32],
                  const uint8_t rx_pub[32], pair_cfg_plain_t *plain) {
  if (msg->magic != PAIR_MAGIC || msg->type != PAIR_MSG_CFG || msg->ver != PAIR_PROTO_VER) {
    return -1;
  }
  uint8_t key[32], aad[16];
  int ret = pair_derive_key(rx_priv, msg->tx_pub, rx_pub, msg->tx_pub, key);
  if (ret == 0) {
    rx_id_aad(rx_id, aad);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret == 0) {
      ret = mbedtls_gcm_auth_decrypt(&gcm, sizeof(*plain), msg->nonce, sizeof(msg->nonce), aad,
                                     sizeof(aad), msg->tag, sizeof(msg->tag), msg->ct,
                                     (uint8_t *)plain);
    }
    mbedtls_gcm_free(&gcm);
  }
  mbedtls_platform_zeroize(key, sizeof(key));
  if (ret != 0) mbedtls_platform_zeroize(plain, sizeof(*plain));
  return ret;
}

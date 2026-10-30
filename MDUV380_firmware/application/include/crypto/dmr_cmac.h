/*
 * dmr_cmac.h — AES-CMAC (RFC 4493 / NIST SP 800-38B), the SP 800-108 counter-mode KDF with
 * CMAC as the PRF, AES-256-CTR and a constant-time compare, for the remote-provisioning
 * protocol (see PROVISIONING.md). Built on the existing AES-256 block cipher in dmr_aes.c.
 *
 * Only compiled with ENABLE_REMOTE_PROVISION (the host tests define it too); a build without
 * the flag is unchanged. No libc beyond memcpy/memset, no allocation, no state.
 */
#ifndef DMR_CMAC_H
#define DMR_CMAC_H

#include <stdint.h>
#include <stddef.h>

#if defined(ENABLE_REMOTE_PROVISION)

/* A 16-byte block cipher: encrypt block in place under key. The engine is generic so the host
 * test can run the RFC 4493 AES-128 vectors through the same code the firmware uses with
 * AES-256. */
typedef void (*dmr_block_fn_t)(const uint8_t *key, uint8_t block[16]);

/* CMAC over msg[0..len-1] with the given block cipher and key. tag = 16 bytes. */
void dmr_cmac_generic(dmr_block_fn_t blk, const uint8_t *key, const uint8_t *msg, size_t len, uint8_t tag[16]);

/* CMAC-AES-256 over one or two message parts (the second may be NULL/0): the frame MAC runs
 * over associated data followed by the ciphertext without concatenating them first. */
void aes256_cmac2(const uint8_t key[32], const uint8_t *a, size_t alen,
                  const uint8_t *b, size_t blen, uint8_t tag[16]);
static inline void aes256_cmac(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16])
{
    aes256_cmac2(key, msg, len, NULL, 0, tag);
}

/* SP 800-108 KDF in counter mode, PRF = CMAC-AES-256, 8-bit counter, Label = ASCII bytes
 * (no terminator), empty Context, L = 16-bit big-endian bit length (256). Output 32 bytes:
 *   out = CMAC(key, 01 || label || 00 || 01 00) || CMAC(key, 02 || label || 00 || 01 00) */
void aes256_kdf108(const uint8_t key[32], const char *label, uint8_t out[32]);

/* AES-256-CTR: XOR data[0..len-1] in place with the keystream E(key, counter), where
 * counter = nonce13 || 24-bit big-endian block index starting at 0. Same call encrypts
 * and decrypts. */
void aes256_ctr_crypt(const uint8_t key[32], const uint8_t nonce13[13], uint8_t *data, size_t len);

/* 0 when a[0..len-1] == b[0..len-1], non-zero otherwise, in time independent of where they
 * differ. */
int dmr_consttime_neq(const uint8_t *a, const uint8_t *b, size_t len);

#endif /* ENABLE_REMOTE_PROVISION */
#endif /* DMR_CMAC_H */

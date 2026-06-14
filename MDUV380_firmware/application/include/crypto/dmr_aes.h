/*
 * dmr_aes.h — DMRA (Motorola/Hytera-compatible) AES-256 encrypted voice for OpenGD77.
 *
 * Standardized DMR Association privacy: AES in OFB mode, 32-bit Message Indicator (MI)
 * carried in the PI header, expanded to a 128-bit IV by an LFSR (taps 32,22,2,1), keystream
 * XORed onto the AMBE voice octets, MI advanced every superframe.
 *
 * This module is self-contained (no libc beyond memcpy/memset) and AES-256 only.
 * Build behind ENABLE_AES. See ../DMRA_AES256_SPEC.md.
 *
 * LEGAL: encrypted voice is illegal on amateur bands in most jurisdictions. Commercial/PMR only.
 */
#ifndef DMR_AES_H
#define DMR_AES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMR_AES_KEY_BYTES   32      /* AES-256 */
#define DMR_AES_IV_BYTES    16
#define DMR_AES_MAX_KEYS    16      /* key slots, indexed by PI KEY ID */

/* Place AES state in CCM RAM, not main RAM. The AMBE codec blob hardcodes the
 * addresses of its main-RAM buffers (ambebuffer_*), so any .bss we add to main RAM
 * shifts them and crashes DMR voice. CCM keeps the main-RAM layout identical to stock. */
#define DMR_AES_CCM __attribute__((section(".aes_ccmram")))

/* DMRA PI-header algorithm IDs (confirm TYT's exact value via OTA, spec §6). */
#define DMR_ALG_AES128      0x24
#define DMR_ALG_AES256      0x25
#define DMR_MFID_DMRA       0x10

/* Parsed Privacy-Indicator header. */
typedef struct {
    uint8_t  alg_id;   /* PI[0] */
    uint8_t  mfid;     /* PI[1] — 0x10 for DMRA */
    uint8_t  key_id;   /* PI[2] */
    uint32_t mi;       /* PI[3..6], big-endian 32-bit Message Indicator */
    uint8_t  valid;    /* 1 if header parsed & MFID/alg recognized */
} dmr_pi_t;

/* Per-call/per-slot crypto state. One instance per logical voice stream. */
typedef struct {
    uint8_t  key[DMR_AES_KEY_BYTES];   /* active 32-byte AES-256 key */
    uint8_t  have_key;                 /* 0 = no key loaded → passthrough */
    uint32_t mi;                       /* current 32-bit MI (advances per superframe) */
    uint8_t  iv[DMR_AES_IV_BYTES];     /* current 128-bit OFB IV (derived from mi) */
    uint8_t  alg_id;
    uint8_t  key_id;
} dmr_aes_ctx_t;

/* ---- key store ---------------------------------------------------------- */
/* Load a 32-byte key into a slot (0..DMR_AES_MAX_KEYS-1). Returns 0 on success. */
int  dmr_aes_set_key(uint8_t slot, const uint8_t key[DMR_AES_KEY_BYTES]);
void dmr_aes_clear_keys(void);

/* ---- PI header ---------------------------------------------------------- */
/* Parse a 12-byte (or longer) post-FEC PI byte buffer. Returns 1 if usable. */
int  dmr_pi_parse(const uint8_t *pi_bytes, size_t len, dmr_pi_t *out);
/* Build a DMRA PI header (bytes [0..6]; caller adds FEC/CRC framing). */
void dmr_pi_build(uint8_t alg_id, uint8_t key_id, uint32_t mi, uint8_t *pi_out7);

/* ---- stream lifecycle --------------------------------------------------- */
/* RX: start from a received PI header (selects key by key_id, seeds MI/IV). */
int  dmr_aes_rx_init(dmr_aes_ctx_t *c, const dmr_pi_t *pi);
/* TX: start a new encrypted call. mi_seed should be random (e.g. STM32 RNG). */
int  dmr_aes_tx_init(dmr_aes_ctx_t *c, uint8_t alg_id, uint8_t key_id, uint32_t mi_seed);

/* Call once per voice SUPERFRAME (before processing its frames): regenerates the
 * 128-bit IV from the current MI, then advances MI for the next superframe.
 * Returns the 32-bit MI to emit in the next PI/late-entry burst (TX side). */
uint32_t dmr_aes_superframe(dmr_aes_ctx_t *c);

/* En/decrypt one AMBE voice frame in place (OFB ⇒ same op both directions).
 * `voice` points to the packed voice octets; `n` = octet count for this frame;
 * `octet_off` = running offset into the superframe keystream (see spec §1d / dmr_block.c).
 * Returns the updated octet_off. No-op (passthrough) if no key loaded. */
size_t dmr_aes_crypt_frame(dmr_aes_ctx_t *c, uint8_t *voice, size_t n, size_t octet_off);

/* ---- bit-domain VOICE decrypt (the correct layer) ----------------------- *
 * DMRA AES-256 VOICE applies the OFB keystream to the 49 DECODED AMBE voice
 * bits (one bit per array entry; only bit 0 used), NOT the 27 raw FEC octets.
 * This is validated byte-exact vs DSD-FME (ground truth) on 690 captured frames:
 *   - keystream = AES-256-OFB(iv, key), discard the first 16-byte block,
 *     bits taken MSB-first;
 *   - each voice frame consumes 56 keystream bits (49 applied + 7 skipped),
 *     continuously across the superframe (bit offset is absolute);
 *   - silence / comfort-noise (CCR) frames are sent in the clear: skip the XOR
 *     but the absolute bit offset still accounts for them.
 * `iv` must already be set (dmr_lfsr128d of this superframe's MI). `bitpos` is the
 * ABSOLUTE keystream bit offset for this frame = (vc * 56), vc = frame index in
 * the 18-frame superframe. Operates on b49[0..48] in place. Returns bitpos + 56. */
size_t dmr_aes_voice_frame(dmr_aes_ctx_t *c, uint16_t *b49, size_t bitpos);
/* AMBE silence / comfort-noise detectors on the 49-bit decoded vector. */
int dmr_ambe_is_silence(const uint16_t *b49);
int dmr_ambe_is_ccr(const uint16_t *b49);

/* ---- low-level (exposed for unit tests) --------------------------------- */
void dmr_lfsr128d(uint32_t mi, uint8_t iv_out[16], uint32_t *next_mi_out);
void aes256_ofb_keystream(const uint8_t iv[16], const uint8_t key[32],
                          uint8_t *out, int nblocks);
void aes256_ecb_encrypt(const uint8_t key[32], uint8_t block[16]); /* test vectors */
uint32_t dmr_aes_have_mask(void);                  /* debug: loaded-key slot bitmask */
void     dmr_aes_peek_key(uint8_t slot, uint8_t out4[4]); /* debug: first 4 key bytes */

#ifdef __cplusplus
}
#endif
#endif /* DMR_AES_H */

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
/* Lowest loaded key slot, or -1 if none (late-entry bootstrap key fallback). */
int  dmr_aes_first_keyid(void);

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

/* ---- DMRA "Late Entry MI" conveyance (TX) ------------------------------- *
 * Build the Golay(24,12)+CRC4 fragment nibbles that carry the 32-bit MI in the
 * AMBE voice codewords (how the stock TYT conveys it; validated vs dsd-fme).
 * frag[vc][cw] (vc 1..6, cw 0..2) = 4-bit value for ambe_fr[3][0..3] of codeword
 * cw in voice frame vc. Those 4 bits map to post-ECC bitbuffer_encode[71,67,63,59]. */
void dmr_le_mi_build(uint32_t mi, uint8_t frag[7][3]);

/* RX: recover the 32-bit MI from the assembled Late-Entry fragment nibbles (inverse of
 * dmr_le_mi_build). Returns 1 + *mi_out when the Golay(24,12) words + CRC4 all check. */
int  dmr_le_mi_decode(const uint8_t frag[7][3], uint32_t *mi_out);

/* ---- DMRA "Late Entry Single Block" (alg/key announcement, TX) ----------- *
 * BPTC(16x2) single-burst codeword for the burst-F EMB. 11-bit payload =
 * key_id<<3 | alg (alg 5 = AES256, 4 = AES128). out4 = page-0x02 regs 0x29..0x2C. */
void dmr_emb_sb_build(uint8_t key_id, uint8_t alg, uint8_t out4[4]);

/* ---- low-level (exposed for unit tests) --------------------------------- */
void dmr_lfsr128d(uint32_t mi, uint8_t iv_out[16], uint32_t *next_mi_out);
void aes256_ofb_keystream(const uint8_t iv[16], const uint8_t key[32],
                          uint8_t *out, int nblocks);
void aes256_ecb_encrypt(const uint8_t key[32], uint8_t block[16]); /* test vectors */

#ifdef __cplusplus
}
#endif

void aes256_ecb_decrypt(const uint8_t key[32], uint8_t block[16]);
void dmr_sms_ecb_decrypt(uint8_t *payload, int len, const uint8_t key[32]);
int  dmr_sms_rx_decrypt(uint8_t *pdu, int pdu_len, const uint8_t key[32], char *out, int out_max);

int  dmr_aes_sms_decrypt(uint8_t key_id, uint8_t *pdu, int pdu_len, char *out, int out_max);
const uint8_t *dmr_aes_key_ptr(uint8_t key_id);   /* 32-byte key for a loaded slot, or NULL */

#endif /* DMR_AES_H */

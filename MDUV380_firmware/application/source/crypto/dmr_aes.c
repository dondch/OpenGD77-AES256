/*
 * dmr_aes.c — DMRA AES-256 encrypted voice for OpenGD77.  See dmr_aes.h / ../DMRA_AES256_SPEC.md.
 *
 * AES-256 core is a trimmed tiny-AES (kokke/tiny-AES-c, public domain) — encrypt path only,
 * which is all OFB needs (decrypt == encrypt the keystream then XOR). LFSR128d + OFB + frame
 * application mirror DSD-FME (reference/dmr_pi.c, dmr_block.c). Self-contained; embedded-friendly.
 */
#include "crypto/dmr_aes.h"
#include <string.h>

/* ===== AES-256 (encrypt only) =========================================== */
#define Nb 4
#define Nk 8
#define Nr 14

static const uint8_t sbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static const uint8_t Rcon[11] = {
  0x8d,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

static void key_expansion(uint8_t *rk, const uint8_t *key) {
    unsigned i, j, k; uint8_t t[4];
    for (i = 0; i < Nk; ++i) {
        rk[i*4+0]=key[i*4+0]; rk[i*4+1]=key[i*4+1];
        rk[i*4+2]=key[i*4+2]; rk[i*4+3]=key[i*4+3];
    }
    for (i = Nk; i < Nb*(Nr+1); ++i) {
        k = (i-1)*4; t[0]=rk[k+0]; t[1]=rk[k+1]; t[2]=rk[k+2]; t[3]=rk[k+3];
        if (i % Nk == 0) {
            uint8_t tmp=t[0]; t[0]=t[1]; t[1]=t[2]; t[2]=t[3]; t[3]=tmp;   /* RotWord */
            t[0]=sbox[t[0]]; t[1]=sbox[t[1]]; t[2]=sbox[t[2]]; t[3]=sbox[t[3]];
            t[0]^=Rcon[i/Nk];
        } else if (i % Nk == 4) {                                         /* AES-256 only */
            t[0]=sbox[t[0]]; t[1]=sbox[t[1]]; t[2]=sbox[t[2]]; t[3]=sbox[t[3]];
        }
        j=i*4; k=(i-Nk)*4;
        rk[j+0]=rk[k+0]^t[0]; rk[j+1]=rk[k+1]^t[1];
        rk[j+2]=rk[k+2]^t[2]; rk[j+3]=rk[k+3]^t[3];
    }
}

static uint8_t xtime(uint8_t x) { return (uint8_t)((x<<1) ^ (((x>>7)&1)*0x1b)); }

static void cipher(uint8_t *s, const uint8_t *rk) {
    int r, c;
    for (c=0;c<16;c++) s[c]^=rk[c];                       /* AddRoundKey 0 */
    for (r=1;r<=Nr;r++) {
        for (c=0;c<16;c++) s[c]=sbox[s[c]];               /* SubBytes */
        { uint8_t t;                                       /* ShiftRows */
          t=s[1];  s[1]=s[5];  s[5]=s[9];  s[9]=s[13]; s[13]=t;
          t=s[2];  s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
          t=s[15]; s[15]=s[11];s[11]=s[7];s[7]=s[3];  s[3]=t; }
        if (r != Nr) {                                     /* MixColumns */
            for (c=0;c<16;c+=4) {
                uint8_t a0=s[c],a1=s[c+1],a2=s[c+2],a3=s[c+3];
                uint8_t h=a0^a1^a2^a3;
                s[c+0]^= h ^ xtime((uint8_t)(a0^a1));
                s[c+1]^= h ^ xtime((uint8_t)(a1^a2));
                s[c+2]^= h ^ xtime((uint8_t)(a2^a3));
                s[c+3]^= h ^ xtime((uint8_t)(a3^a0));
            }
        }
        for (c=0;c<16;c++) s[c]^=rk[r*16+c];               /* AddRoundKey r */
    }
}

void aes256_ecb_encrypt(const uint8_t key[32], uint8_t block[16]) {
    uint8_t rk[240]; key_expansion(rk, key); cipher(block, rk);
}

/* OFB keystream: IR=IV; repeatedly IR=E(IR); emit. (mirror aes_ofb_keystream_output type=2) */
void aes256_ofb_keystream(const uint8_t iv[16], const uint8_t key[32],
                          uint8_t *out, int nblocks) {
    uint8_t rk[240], ir[16];
    int b;
    key_expansion(rk, key);
    memcpy(ir, iv, 16);
    for (b=0; b<nblocks; ++b) { cipher(ir, rk); memcpy(out+16*b, ir, 16); }
}

/* ===== DMRA MI → 128-bit IV (LFSR taps 32,22,2,1) ====================== */
/* IV[0..3] = MI big-endian; then 96 LFSR bits packed into IV[4..15].
 * next_MI (following superframe) = IV[4..7].  (mirror dmr_pi.c LFSR128d) */
void dmr_lfsr128d(uint32_t mi, uint8_t iv_out[16], uint32_t *next_mi_out) {
    uint64_t lfsr = mi;
    int cnt, x;
    iv_out[0]=(uint8_t)(mi>>24); iv_out[1]=(uint8_t)(mi>>16);
    iv_out[2]=(uint8_t)(mi>>8);  iv_out[3]=(uint8_t)(mi>>0);
    for (x=4; x<16; ++x) iv_out[x]=0;
    x = 32;
    for (cnt=0; cnt<96; ++cnt) {
        uint64_t bit = ((lfsr>>31) ^ (lfsr>>21) ^ (lfsr>>1) ^ (lfsr>>0)) & 1u;
        lfsr = (lfsr<<1) | bit;
        iv_out[x/8] = (uint8_t)((iv_out[x/8]<<1) + (uint8_t)bit);
        ++x;
    }
    if (next_mi_out)
        *next_mi_out = ((uint32_t)iv_out[4]<<24)|((uint32_t)iv_out[5]<<16)|
                       ((uint32_t)iv_out[6]<<8) |((uint32_t)iv_out[7]<<0);
}

/* ===== key store ======================================================== */
static uint8_t  s_keys[DMR_AES_MAX_KEYS][DMR_AES_KEY_BYTES] DMR_AES_CCM;
static uint8_t  s_have[DMR_AES_MAX_KEYS] DMR_AES_CCM;

int dmr_aes_set_key(uint8_t slot, const uint8_t key[DMR_AES_KEY_BYTES]) {
    if (slot >= DMR_AES_MAX_KEYS) return -1;
    memcpy(s_keys[slot], key, DMR_AES_KEY_BYTES); s_have[slot]=1; return 0;
}
void dmr_aes_clear_keys(void) { memset(s_keys,0,sizeof s_keys); memset(s_have,0,sizeof s_have); }

/* ===== PI header ======================================================== */
int dmr_pi_parse(const uint8_t *p, size_t len, dmr_pi_t *o) {
    if (len < 7) { o->valid=0; return 0; }
    o->alg_id=p[0]; o->mfid=p[1]; o->key_id=p[2];
    o->mi = ((uint32_t)p[3]<<24)|((uint32_t)p[4]<<16)|((uint32_t)p[5]<<8)|((uint32_t)p[6]);
    /* Require an actual AES alg id. dmrAesRxPI is fed EVERY CRC-valid LC, and ordinary
     * group-voice LCs (FLCO byte[0]=0x00) also carry MFID 0x10 — accepting alg<0x26 made
     * those parse as bogus PIs. Only 0x24 (AES-128) / 0x25 (AES-256) are real here. */
    o->valid = (o->mfid==DMR_MFID_DMRA &&
                (o->alg_id==DMR_ALG_AES128 || o->alg_id==DMR_ALG_AES256)) ? 1 : 0;
    return o->valid;
}
void dmr_pi_build(uint8_t alg_id, uint8_t key_id, uint32_t mi, uint8_t *o) {
    o[0]=alg_id; o[1]=DMR_MFID_DMRA; o[2]=key_id;
    o[3]=(uint8_t)(mi>>24); o[4]=(uint8_t)(mi>>16);
    o[5]=(uint8_t)(mi>>8);  o[6]=(uint8_t)(mi>>0);
}

/* ===== stream lifecycle ================================================= */
static int ctx_load_key(dmr_aes_ctx_t *c, uint8_t key_id) {
    if (key_id < DMR_AES_MAX_KEYS && s_have[key_id]) {
        memcpy(c->key, s_keys[key_id], DMR_AES_KEY_BYTES); c->have_key=1; c->key_id=key_id; return 0;
    }
    c->have_key=0; return -1;
}
int dmr_aes_rx_init(dmr_aes_ctx_t *c, const dmr_pi_t *pi) {
    memset(c,0,sizeof *c);
    c->alg_id=pi->alg_id; c->mi=pi->mi;
    return ctx_load_key(c, pi->key_id);
}
int dmr_aes_tx_init(dmr_aes_ctx_t *c, uint8_t alg_id, uint8_t key_id, uint32_t mi_seed) {
    memset(c,0,sizeof *c);
    c->alg_id=alg_id; c->mi=mi_seed;
    return ctx_load_key(c, key_id);
}

/* Per-superframe: regenerate IV from current MI, advance MI. Returns MI to advertise next. */
uint32_t dmr_aes_superframe(dmr_aes_ctx_t *c) {
    uint32_t next=c->mi;
    if (c->have_key) dmr_lfsr128d(c->mi, c->iv, &next);
    c->mi = next;                 /* next superframe / next PI late-entry uses this */
    return next;
}

/* OFB: en/decrypt identical. Caller passes the running superframe octet offset.
 * NOTE: DMR voice octet mapping (n, octet_off start) must match TYT — confirm via OTA (spec §6). */
size_t dmr_aes_crypt_frame(dmr_aes_ctx_t *c, uint8_t *voice, size_t n, size_t octet_off) {
    if (!c->have_key) return octet_off;            /* passthrough = clear voice */
    /* Generate enough keystream to cover octet_off+n; OFB is deterministic from IV. */
    /* AES-OFB discards the first keystream block; voice application starts after it.
     * DMR_AES_KS_DISCARD is the octet base (16 = one AES block). Confirm exact DMR base at bench. */
    #define DMR_AES_KS_DISCARD 16
    uint8_t ks[16*24]; size_t need = DMR_AES_KS_DISCARD + octet_off + n; int nblk = (int)((need+15)/16);
    if (nblk > 24) nblk = 24;
    aes256_ofb_keystream(c->iv, c->key, ks, nblk);
    /* Hard bound: never read past ks[] even if octet_off is unexpectedly large
     * (e.g. a missed superframe reset) — a wrong assumption must not corrupt the stack. */
    for (size_t i=0; i<n; ++i) {
        size_t idx = DMR_AES_KS_DISCARD + octet_off + i;
        if (idx < sizeof(ks)) { voice[i] ^= ks[idx]; }
    }
    return octet_off + n;
}

/* ===== bit-domain VOICE decrypt (49-bit AMBE params) ==================== */
/* AMBE+2 default silence vector as a 56-bit value; bit i of the 49-bit decoded
 * vector = (SILENCE56 >> (55-i)) & 1.  (mirror DSD-FME dsd_mbe.c ambe_silence) */
#define DMR_AMBE_SILENCE56  0xF801A99F8CE080ULL

int dmr_ambe_is_silence(const uint16_t *b) {
    for (int i = 0; i < 49; ++i) {
        if ((uint8_t)(b[i] & 1u) != (uint8_t)((DMR_AMBE_SILENCE56 >> (55 - i)) & 1u)) { return 0; }
    }
    return 1;
}
/* Comfort-noise / zeroed frame: bits 24..43 (20 bits) all zero. (DSD-FME zeroes+24) */
int dmr_ambe_is_ccr(const uint16_t *b) {
    for (int i = 24; i < 44; ++i) { if (b[i] & 1u) { return 0; } }
    return 1;
}

size_t dmr_aes_voice_frame(dmr_aes_ctx_t *c, uint16_t *b49, size_t bitpos) {
    if (!c->have_key) { return bitpos + 56; }                 /* passthrough = clear */
    /* silence and comfort-noise frames are transmitted UNENCRYPTED: skip the XOR
     * but still account for the 56 bits so the absolute offset stays aligned. */
    if (dmr_ambe_is_silence(b49) || dmr_ambe_is_ccr(b49)) { return bitpos + 56; }

    /* keystream = OFB(iv) with the first 16-byte block discarded, bits MSB-first.
     * Need bits [bitpos .. bitpos+48] → octets up to 16 + (bitpos+48)/8. */
    size_t lastoct = 16 + (bitpos + 48) / 8;
    int nblk = (int)((lastoct + 16) / 16);                    /* ceil((lastoct+1)/16) */
    uint8_t ks[16 * 18];
    if (nblk > 18) { nblk = 18; }
    aes256_ofb_keystream(c->iv, c->key, ks, nblk);

    for (int i = 0; i < 49; ++i) {
        size_t j   = bitpos + (size_t)i;
        size_t oct = 16 + j / 8;                              /* skip 16-byte discard */
        if (oct < sizeof(ks)) {
            uint8_t bit = (uint8_t)((ks[oct] >> (7 - (j & 7))) & 1u);
            b49[i] ^= bit;
        }
    }
    return bitpos + 56;
}

/* ---- DMRA "Late Entry MI" conveyance ------------------------------------ *
 * The stock TYT carries the 32-bit MI by stuffing a Golay(24,12)+CRC4 encoding
 * of it into ambe_fr[3][0..3] (4 bits) of EACH of the 3 AMBE codewords, across
 * voice frames vc=1..6. The receiver reads it as "Late Entry MI" and bootstraps
 * the keystream, then self-advances the MI by LFSR each superframe. This mirrors
 * dsd-fme dmr_le.c (decode) byte-exact — validated offline on 200k+ vectors. */

/* Golay(24,12) generator (systematic; first 12 cols = identity) — from dsd-fme fec.c. */
static const uint8_t LE_G[24*12] = {
  1,0,0,0,0,0,0,0,0,0,0,0, 1,1,0,0,0,1,1,1,0,1,0,1,
  0,1,0,0,0,0,0,0,0,0,0,0, 0,1,1,0,0,0,1,1,1,0,1,1,
  0,0,1,0,0,0,0,0,0,0,0,0, 1,1,1,1,0,1,1,0,1,0,0,0,
  0,0,0,1,0,0,0,0,0,0,0,0, 0,1,1,1,1,0,1,1,0,1,0,0,
  0,0,0,0,1,0,0,0,0,0,0,0, 0,0,1,1,1,1,0,1,1,0,1,0,
  0,0,0,0,0,1,0,0,0,0,0,0, 1,1,0,1,1,0,0,1,1,0,0,1,
  0,0,0,0,0,0,1,0,0,0,0,0, 0,1,1,0,1,1,0,0,1,1,0,1,
  0,0,0,0,0,0,0,1,0,0,0,0, 0,0,1,1,0,1,1,0,0,1,1,1,
  0,0,0,0,0,0,0,0,1,0,0,0, 1,1,0,1,1,1,0,0,0,1,1,0,
  0,0,0,0,0,0,0,0,0,1,0,0, 1,0,1,0,1,0,0,1,0,1,1,1,
  0,0,0,0,0,0,0,0,0,0,1,0, 1,0,0,1,0,0,1,1,1,1,1,0,
  0,0,0,0,0,0,0,0,0,0,0,1, 1,0,0,0,1,1,1,0,1,0,1,1,
};

static void le_golay24_encode(const uint8_t in12[12], uint8_t out24[24]) {
    for (int j = 0; j < 24; ++j) {
        uint8_t s = 0;
        for (int i = 0; i < 12; ++i) { s = (uint8_t)(s + in12[i] * LE_G[24*i + j]); }
        out24[j] = (uint8_t)(s & 1u);
    }
}

/* CRC4 (x^4+x+1, inverted) — bit-exact with dsd-fme dmr_le.c crc4(). */
static uint8_t le_crc4(const uint8_t *bits, int len) {
    uint8_t buf[40]; const uint8_t poly[5] = {1,0,0,1,1}; uint8_t crc = 0;
    if (len + 4 > (int)sizeof(buf)) { return 0; }
    memset(buf, 0, sizeof buf);
    for (int i = 0; i < len; ++i) { buf[i] = bits[i]; }
    for (int i = 0; i < len; ++i) {
        if (buf[i]) { for (int j = 0; j < 5; ++j) { buf[i+j] ^= poly[j]; } }
    }
    for (int i = 0; i < 4; ++i) { crc = (uint8_t)((crc << 1) + buf[len + i]); }
    return (uint8_t)(crc ^ 0xF);
}

void dmr_le_mi_build(uint32_t mi, uint8_t frag[7][3]) {
    uint8_t mi_bits[36];
    for (int i = 0; i < 32; ++i) { mi_bits[i] = (uint8_t)((mi >> (31 - i)) & 1u); }
    uint8_t crc = le_crc4(mi_bits, 32);
    for (int b = 0; b < 4; ++b) { mi_bits[32 + b] = (uint8_t)((crc >> (3 - b)) & 1u); }

    /* Three Golay(24,12) codewords: info bits → mi_test, parity bits → go_test,
     * placed at bit (35-(i+j*12)) of each 36-bit word (inverse of the decode). */
    uint64_t mi_test = 0, go_test = 0;
    for (int j = 0; j < 3; ++j) {
        uint8_t enc[24];
        le_golay24_encode(&mi_bits[j*12], enc);
        for (int i = 0; i < 12; ++i) {
            int pos = 35 - (i + j*12);
            mi_test |= (uint64_t)(enc[i]    & 1u) << pos;
            go_test |= (uint64_t)(enc[i+12] & 1u) << pos;
        }
    }
    /* Distribute the two 36-bit words into 4-bit fragment nibbles per (vc, codeword),
     * the exact inverse of dmr_le.c's mi_test/go_test assembly. frag[vc][cw], vc 1..6. */
    frag[1][0]=(uint8_t)((mi_test>>32)&0xF); frag[2][0]=(uint8_t)((mi_test>>28)&0xF); frag[3][0]=(uint8_t)((mi_test>>24)&0xF);
    frag[1][1]=(uint8_t)((mi_test>>20)&0xF); frag[2][1]=(uint8_t)((mi_test>>16)&0xF); frag[3][1]=(uint8_t)((mi_test>>12)&0xF);
    frag[1][2]=(uint8_t)((mi_test>>8 )&0xF); frag[2][2]=(uint8_t)((mi_test>>4 )&0xF); frag[3][2]=(uint8_t)((mi_test>>0 )&0xF);
    frag[4][0]=(uint8_t)((go_test>>32)&0xF); frag[5][0]=(uint8_t)((go_test>>28)&0xF); frag[6][0]=(uint8_t)((go_test>>24)&0xF);
    frag[4][1]=(uint8_t)((go_test>>20)&0xF); frag[5][1]=(uint8_t)((go_test>>16)&0xF); frag[6][1]=(uint8_t)((go_test>>12)&0xF);
    frag[4][2]=(uint8_t)((go_test>>8 )&0xF); frag[5][2]=(uint8_t)((go_test>>4 )&0xF); frag[6][2]=(uint8_t)((go_test>>0 )&0xF);
}

/* ---- DMRA "Late Entry Single Block" (alg/key announcement) --------------- *
 * Builds the 4-octet BPTC(16x2) single-burst codeword for the burst-F EMB. The
 * HR-C6000 emits its page-0x02 enc registers 0x29..0x2C RAW in this mode (it does
 * not BPTC-encode them), so we supply the already-encoded octets to write there.
 * 11-bit payload = key_id(8)<<3 | alg(3); alg 5 = AES256, 4 = AES128. Encoding =
 * Hamming(16,11,4) systematic + even-parity duplicate (single burst) + RC interleave.
 * The receiver (dsd-fme dmr_sbrc / a stock TYT) BPTC-decodes it to ALG ID + KEY ID.
 * Verified vs dsd-fme BPTC_16x2_Extract_Data (key_id 1 / AES256 -> 44 42 88 81). */
void dmr_emb_sb_build(uint8_t key_id, uint8_t alg, uint8_t out4[4]) {
    /* Hamming(16,11,4) generator, systematic (cols 0..10 = identity) — dsd-fme fec.c. */
    static const uint8_t G[16*11] = {
      1,0,0,0,0,0,0,0,0,0,0, 1,0,0,1,1,  0,1,0,0,0,0,0,0,0,0,0, 1,1,0,1,0,
      0,0,1,0,0,0,0,0,0,0,0, 1,1,1,1,1,  0,0,0,1,0,0,0,0,0,0,0, 1,1,1,0,0,
      0,0,0,0,1,0,0,0,0,0,0, 0,1,1,1,0,  0,0,0,0,0,1,0,0,0,0,0, 1,0,1,0,1,
      0,0,0,0,0,0,1,0,0,0,0, 0,1,0,1,1,  0,0,0,0,0,0,0,1,0,0,0, 1,0,1,1,0,
      0,0,0,0,0,0,0,0,1,0,0, 1,1,0,0,1,  0,0,0,0,0,0,0,0,0,1,0, 0,1,1,0,1,
      0,0,0,0,0,0,0,0,0,0,1, 0,0,1,1,1,
    };
    /* Reverse-channel BPTC deinterleave tables — dsd-fme bptc.c. */
    static const uint8_t BPTC[32]  = { 0,17, 2,19, 4,21, 6,23, 8,25,10,27,12,29,14,31,
                                      16, 1,18, 3,20, 5,22, 7,24, 9,26,11,28,13,30,15};
    static const uint8_t PLACE[32] = { 0,16, 1,17, 2,18, 3,19, 4,20, 5,21, 6,22, 7,23,
                                       8,24, 9,25,10,26,11,27,12,28,13,29,14,30,15,31};
    uint16_t payload = (uint16_t)(((uint16_t)key_id << 3) | (alg & 0x07u)); /* 11-bit */
    uint8_t d[11], line[16], dm[32], in[32];
    int i, j;
    for (i = 0; i < 11; i++) { d[i] = (uint8_t)((payload >> (10 - i)) & 1u); }
    for (j = 0; j < 16; j++) { uint8_t s = 0; for (i = 0; i < 11; i++) { s = (uint8_t)(s + d[i] * G[16*i + j]); } line[j] = (uint8_t)(s & 1u); }
    for (i = 0; i < 16; i++) { dm[i] = line[i]; dm[i + 16] = line[i]; }       /* even-parity duplicate */
    for (i = 0; i < 32; i++) { in[i] = (uint8_t)(dm[PLACE[BPTC[i]]] & 1u); }  /* RC interleave */
    for (i = 0; i < 4; i++) { uint8_t b = 0; for (j = 0; j < 8; j++) { b = (uint8_t)((b << 1) | in[i*8 + j]); } out4[i] = b; }
}

/* ---- DMRA "Late Entry MI" DECODE (RX) ----------------------------------- *
 * Inverse of dmr_le_mi_build: recover the 32-bit MI from the Golay(24,12)+CRC4
 * fragment nibbles the transmitter stuffed into ambe_fr[3][0..3] across the 6
 * voice frames of a superframe. Lets the RX read the MI straight from the AMBE
 * bits (mirror dsd-fme dmr_le.c) instead of the HR-C6000 reformatted PI-LC, which
 * lags by a frame at a new call. Returns 1 + *mi_out only when all 3 Golay words
 * are syndrome-clean AND the CRC4 checks (conservative no-correction decode: a bad
 * read is rejected and the caller falls back to the chip PI-LC). frag[vc][cw]. */
static const uint8_t LE_H[24*12] = {
  1,0,1,0,0,1,0,0,1,1,1,1, 1,0,0,0,0,0,0,0,0,0,0,0,
  1,1,1,1,0,1,1,0,1,0,0,0, 0,1,0,0,0,0,0,0,0,0,0,0,
  0,1,1,1,1,0,1,1,0,1,0,0, 0,0,1,0,0,0,0,0,0,0,0,0,
  0,0,1,1,1,1,0,1,1,0,1,0, 0,0,0,1,0,0,0,0,0,0,0,0,
  0,0,0,1,1,1,1,0,1,1,0,1, 0,0,0,0,1,0,0,0,0,0,0,0,
  1,0,1,0,1,0,1,1,1,0,0,1, 0,0,0,0,0,1,0,0,0,0,0,0,
  1,1,1,1,0,0,0,1,0,0,1,1, 0,0,0,0,0,0,1,0,0,0,0,0,
  1,1,0,1,1,1,0,0,0,1,1,0, 0,0,0,0,0,0,0,1,0,0,0,0,
  0,1,1,0,1,1,1,0,0,0,1,1, 0,0,0,0,0,0,0,0,1,0,0,0,
  1,0,0,1,0,0,1,1,1,1,1,0, 0,0,0,0,0,0,0,0,0,1,0,0,
  0,1,0,0,1,0,0,1,1,1,1,1, 0,0,0,0,0,0,0,0,0,0,1,0,
  1,1,0,0,0,1,1,1,0,1,0,1, 0,0,0,0,0,0,0,0,0,0,0,1,
};

/* Table-free Golay(24,12,8) decode-with-correction of one 24-bit codeword in place.
 * Returns the number of corrected errors (0..3), or -1 if uncorrectable (>3 errors).
 * The early no-correction version REJECTED any nonzero syndrome, so on real RF the
 * unprotected late-entry nibbles (which carry a few channel bit errors) NEVER decoded
 * — the whole direct-late-entry path was inert. d=8 ⇒ every weight-<=3 error pattern has
 * a UNIQUE coset leader, so a min-weight syndrome search is deterministic and matches
 * dsd-fme's table-based Golay_24_12_decode exactly (verified offline over all <=3-bit
 * patterns), without its 12 KB syndrome table. */
static int golay24_correct(uint8_t cw[24]) {
    unsigned col[24], s = 0;
    int j, a, b, c, r;
    for (j = 0; j < 24; j++) {                    /* column j = single-error-at-j syndrome */
        unsigned cj = 0;
        for (r = 0; r < 12; r++) { cj |= (unsigned)(LE_H[24*r + j] & 1) << (11 - r); }
        col[j] = cj;
    }
    for (r = 0; r < 12; r++) {                    /* syndrome of the received word */
        int acc = 0;
        for (j = 0; j < 24; j++) { acc += cw[j] * LE_H[24*r + j]; }
        s |= (unsigned)(acc & 1) << (11 - r);
    }
    if (s == 0) { return 0; }
    for (a = 0; a < 24; a++) { if (col[a] == s) { cw[a] ^= 1; return 1; } }
    for (a = 0; a < 24; a++) { for (b = a+1; b < 24; b++) { if ((col[a]^col[b]) == s) { cw[a]^=1; cw[b]^=1; return 2; } } }
    for (a = 0; a < 24; a++) { for (b = a+1; b < 24; b++) { for (c = b+1; c < 24; c++) {
        if ((col[a]^col[b]^col[c]) == s) { cw[a]^=1; cw[b]^=1; cw[c]^=1; return 3; } } } }
    return -1;
}

int dmr_le_mi_decode(const uint8_t frag[7][3], uint32_t *mi_out) {
    uint64_t mi_test = ((uint64_t)frag[1][0]<<32)|((uint64_t)frag[2][0]<<28)|((uint64_t)frag[3][0]<<24)|
                       ((uint64_t)frag[1][1]<<20)|((uint64_t)frag[2][1]<<16)|((uint64_t)frag[3][1]<<12)|
                       ((uint64_t)frag[1][2]<<8 )|((uint64_t)frag[2][2]<<4 )|((uint64_t)frag[3][2]<<0);
    uint64_t go_test = ((uint64_t)frag[4][0]<<32)|((uint64_t)frag[5][0]<<28)|((uint64_t)frag[6][0]<<24)|
                       ((uint64_t)frag[4][1]<<20)|((uint64_t)frag[5][1]<<16)|((uint64_t)frag[6][1]<<12)|
                       ((uint64_t)frag[4][2]<<8 )|((uint64_t)frag[5][2]<<4 )|((uint64_t)frag[6][2]<<0);
    uint8_t mi_bits[36];
    int good = 1, i, j;
    for (j = 0; j < 3; j++) {
        uint8_t cw[24];
        for (i = 0; i < 12; i++) {
            cw[i]    = (uint8_t)(((mi_test << (i + j*12)) & 0x800000000ULL) >> 35);
            cw[i+12] = (uint8_t)(((go_test << (i + j*12)) & 0x800000000ULL) >> 35);
        }
        if (golay24_correct(cw) < 0) { good = 0; }     /* correct up to 3 errors, else reject */
        for (i = 0; i < 12; i++) { mi_bits[i + j*12] = cw[i]; }
    }
    uint32_t mi_final = 0;
    for (i = 0; i < 32; i++) { mi_final = (mi_final << 1) | mi_bits[i]; }
    uint8_t crc_ext = 0;
    for (i = 0; i < 4; i++) { crc_ext = (uint8_t)((crc_ext << 1) | mi_bits[32 + i]); }
    *mi_out = mi_final;
    return (good && (crc_ext == le_crc4(mi_bits, 32))) ? 1 : 0;
}

/* Lowest key slot that holds a loaded key, or -1 if none. Used as a fallback keyId
 * when bootstrapping a rapid call from the late-entry MI (which carries no key id). */
int dmr_aes_first_keyid(void) {
    int i;
    for (i = 0; i < DMR_AES_MAX_KEYS; i++) { if (s_have[i]) { return i; } }
    return -1;
}

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
static uint8_t  s_keys[DMR_AES_MAX_KEYS][DMR_AES_KEY_BYTES];
static uint8_t  s_have[DMR_AES_MAX_KEYS];

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
    o->valid = (o->mfid==DMR_MFID_DMRA && o->alg_id < 0x26) ? 1 : 0;
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
    for (size_t i=0; i<n; ++i) voice[i] ^= ks[DMR_AES_KS_DISCARD + octet_off + i];
    return octet_off + n;
}

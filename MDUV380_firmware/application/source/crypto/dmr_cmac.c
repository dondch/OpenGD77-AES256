/*
 * dmr_cmac.c — AES-CMAC, SP 800-108 KDF, AES-256-CTR, constant-time compare. See dmr_cmac.h.
 * Compiles to nothing unless built -DENABLE_REMOTE_PROVISION.
 */
#include "crypto/dmr_cmac.h"

#if defined(ENABLE_REMOTE_PROVISION)

#include "crypto/dmr_aes.h"
#include <string.h>

/* ---- CMAC (RFC 4493 / SP 800-38B) --------------------------------------- */

/* Subkey generation: L = E(0); K1 = L<<1 (^ Rb if msb(L)); K2 = K1<<1 (^ Rb if msb(K1)). */
static void shift_left_rb(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--)
    {
        uint8_t c = (uint8_t)(in[i] >> 7);
        out[i] = (uint8_t)((in[i] << 1) | carry);
        carry = c;
    }
    out[15] ^= (uint8_t)(0x87 & (uint8_t)(0 - (in[0] >> 7)));   /* branch-free Rb */
}

/* The running engine: feeds whole 16-byte blocks of (a || b) through CBC-MAC and keeps the
 * final (possibly partial) block for the subkey step. Streaming two parts avoids a copy of
 * the associated data + ciphertext on the firmware's small stack. */
typedef struct
{
    dmr_block_fn_t blk;
    const uint8_t *key;
    uint8_t x[16];        /* CBC state */
    uint8_t last[16];     /* bytes held back (0..16 of them) */
    size_t  nlast;
    size_t  total;
} cmac_st_t;

static void cmac_update(cmac_st_t *s, const uint8_t *m, size_t len)
{
    while (len > 0)
    {
        if (s->nlast == 16)
        {
            /* a full block is held and more follows: it is not the last one, absorb it */
            for (int i = 0; i < 16; i++) { s->x[i] ^= s->last[i]; }
            s->blk(s->key, s->x);
            s->nlast = 0;
        }
        size_t take = 16 - s->nlast;
        if (take > len) { take = len; }
        memcpy(s->last + s->nlast, m, take);
        s->nlast += take; m += take; len -= take; s->total += take;
    }
}

static void cmac_final(cmac_st_t *s, uint8_t tag[16])
{
    uint8_t L[16] = { 0 }, k1[16], k2[16];
    s->blk(s->key, L);
    shift_left_rb(L, k1);
    shift_left_rb(k1, k2);
    if ((s->total > 0) && (s->nlast == 16))
    {
        for (int i = 0; i < 16; i++) { s->x[i] ^= s->last[i] ^ k1[i]; }
    }
    else
    {
        /* incomplete (or empty) last block: pad 10..0 and use K2 */
        s->last[s->nlast] = 0x80;
        for (size_t i = s->nlast + 1; i < 16; i++) { s->last[i] = 0; }
        for (int i = 0; i < 16; i++) { s->x[i] ^= s->last[i] ^ k2[i]; }
    }
    s->blk(s->key, s->x);
    memcpy(tag, s->x, 16);
    memset(s, 0, sizeof *s);
}

static void cmac_init(cmac_st_t *s, dmr_block_fn_t blk, const uint8_t *key)
{
    memset(s, 0, sizeof *s);
    s->blk = blk; s->key = key;
}

void dmr_cmac_generic(dmr_block_fn_t blk, const uint8_t *key, const uint8_t *msg, size_t len, uint8_t tag[16])
{
    cmac_st_t s;
    cmac_init(&s, blk, key);
    cmac_update(&s, msg, len);
    cmac_final(&s, tag);
}

static void aes256_blk(const uint8_t *key, uint8_t block[16]) { aes256_ecb_encrypt(key, block); }

void aes256_cmac2(const uint8_t key[32], const uint8_t *a, size_t alen,
                  const uint8_t *b, size_t blen, uint8_t tag[16])
{
    cmac_st_t s;
    cmac_init(&s, aes256_blk, key);
    if (a && alen) { cmac_update(&s, a, alen); }
    if (b && blen) { cmac_update(&s, b, blen); }
    cmac_final(&s, tag);
}

/* ---- SP 800-108 counter-mode KDF ---------------------------------------- */
void aes256_kdf108(const uint8_t key[32], const char *label, uint8_t out[32])
{
    uint8_t in[1 + 16 + 1 + 2];
    size_t ll = strlen(label);
    if (ll > 16) { ll = 16; }
    for (uint8_t i = 1; i <= 2; i++)
    {
        size_t n = 0;
        in[n++] = i;                              /* [i]_8 */
        memcpy(in + n, label, ll); n += ll;       /* Label */
        in[n++] = 0x00;                           /* separator, Context empty */
        in[n++] = 0x01; in[n++] = 0x00;           /* [L]_16 = 256 bits */
        aes256_cmac(key, in, n, out + (size_t)(i - 1) * 16);
    }
    memset(in, 0, sizeof in);
}

/* ---- AES-256-CTR ---------------------------------------------------------- */
void aes256_ctr_crypt(const uint8_t key[32], const uint8_t nonce13[13], uint8_t *data, size_t len)
{
    uint8_t ctr[16], ks[16];
    uint32_t idx = 0;
    memcpy(ctr, nonce13, 13);
    while (len > 0)
    {
        ctr[13] = (uint8_t)(idx >> 16); ctr[14] = (uint8_t)(idx >> 8); ctr[15] = (uint8_t)idx;
        memcpy(ks, ctr, 16);
        aes256_ecb_encrypt(key, ks);
        size_t n = (len < 16) ? len : 16;
        for (size_t i = 0; i < n; i++) { data[i] ^= ks[i]; }
        data += n; len -= n; idx++;
    }
    memset(ks, 0, sizeof ks);
}

/* ---- constant-time compare ------------------------------------------------ */
int dmr_consttime_neq(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t d = 0;
    for (size_t i = 0; i < len; i++) { d |= (uint8_t)(a[i] ^ b[i]); }
    return (int)d;
}

#endif /* ENABLE_REMOTE_PROVISION */

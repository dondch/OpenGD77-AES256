/*
 * test_dmr_aes.c — host unit tests for the DMRA AES-256 module.
 * Build & run on a PC (no radio needed):
 *     gcc -O2 -Wall -o test_dmr_aes test_dmr_aes.c dmr_aes.c && ./test_dmr_aes
 * Verifies the crypto is bit-exact before integrating into OpenGD77.
 */
#include "dmr_aes.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static int eq(const char *name, const uint8_t *a, const uint8_t *b, size_t n) {
    if (memcmp(a,b,n)==0) { printf("  PASS %s\n", name); return 1; }
    printf("  FAIL %s\n   got: ", name);
    for (size_t i=0;i<n;i++) printf("%02x", a[i]);
    printf("\n   exp: ");
    for (size_t i=0;i<n;i++) printf("%02x", b[i]);
    printf("\n"); fails++; return 0;
}
static void hx(const char *h, uint8_t *o) {
    for (size_t i=0;i<strlen(h)/2;i++){ unsigned v; sscanf(h+2*i,"%2x",&v); o[i]=(uint8_t)v; }
}

int main(void) {
    printf("DMRA AES-256 self-test\n");

    /* 1) FIPS-197 AES-256 ECB known-answer */
    uint8_t key[32], pt[16], ct[16];
    hx("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key);
    hx("00112233445566778899aabbccddeeff", pt);
    hx("8ea2b7ca516745bfeafc49904b496089", ct);
    aes256_ecb_encrypt(key, pt);
    eq("AES-256 ECB (FIPS-197)", pt, ct, 16);

    /* 2) LFSR128d MI->IV expansion (reference vector from dmr_pi.c logic) */
    uint8_t iv[16], iv_exp[16]; uint32_t next;
    dmr_lfsr128d(0x12345678u, iv, &next);
    hx("12345678b451463a41d78991a49a6402", iv_exp);
    eq("LFSR128d IV (MI=12345678)", iv, iv_exp, 16);
    printf("  %s next_MI=%08X (exp B451463A)\n", next==0xB451463Au?"PASS":(fails++,"FAIL"), next);

    /* 3) OFB en/decrypt round-trip (same op both directions) */
    dmr_aes_clear_keys();
    dmr_aes_set_key(3, key);
    dmr_aes_ctx_t tx, rx;
    dmr_aes_tx_init(&tx, DMR_ALG_AES256, 3, 0x12345678u);
    dmr_pi_t pi; uint8_t pib[7];
    dmr_pi_build(DMR_ALG_AES256, 3, 0x12345678u, pib);
    dmr_pi_parse(pib, 7, &pi);
    dmr_aes_rx_init(&rx, &pi);

    dmr_aes_superframe(&tx); dmr_aes_superframe(&rx);   /* both derive same IV from MI */
    uint8_t voice[9]  = {0xDE,0xAD,0xBE,0xEF,0x01,0x23,0x45,0x67,0x89};
    uint8_t orig[9];   memcpy(orig, voice, 9);
    size_t off = 0;
    off = dmr_aes_crypt_frame(&tx, voice, 9, off);       /* encrypt */
    if (memcmp(voice, orig, 9)==0) { printf("  FAIL OFB actually encrypted\n"); fails++; }
    else printf("  PASS OFB encrypts (ciphertext != plaintext)\n");
    dmr_aes_crypt_frame(&rx, voice, 9, 0);               /* decrypt */
    eq("OFB round-trip (decrypt==original)", voice, orig, 9);

    /* 4) PI build/parse round-trip */
    printf("  %s PI parse (alg=%02X key=%u mi=%08X)\n",
           (pi.valid && pi.alg_id==DMR_ALG_AES256 && pi.key_id==3 && pi.mi==0x12345678u)
             ? "PASS" : (fails++,"FAIL"), pi.alg_id, pi.key_id, pi.mi);

    printf(fails? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}

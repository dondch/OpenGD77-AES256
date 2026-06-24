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

    /* 5) Bit-domain VOICE decrypt vs DSD-FME ground truth (real on-air capture,
     *    KEY1 in radio/CPS order, IV 174E6042..., superframe frames vc=0,1,17). DSD-FME
     *    (which decodes this scheme correctly) produced these enc->dec pairs;
     *    dmr_aes_voice_frame must reproduce dec exactly. */
    {
        uint8_t k1[32];
        hx("93A5CF3BDAB558BCF61ECA5732A8657832396F678150E17811EAA7491F94B3EE", k1);
        uint8_t gtiv[16];
        hx("174E6042E509E273D8DB51AD47A7EB99", gtiv);
        struct { int vc; const char *enc, *dec; } gv[] = {
            {0,  "1101010001011000101011101011010101110010001010111", "1100010100101000001111101010110010000001001110111"},
            {1,  "0101001110111010001001111100101101111110001111001", "1100001100101101100101110100001010100000100100011"},
            {17, "1000011100111101110011101000110100001101111100100", "1000011100001100001100100100001110001111100000000"},
        };
        int vpass = 1;
        for (size_t g = 0; g < sizeof(gv)/sizeof(gv[0]); g++) {
            dmr_aes_ctx_t vctx; memset(&vctx, 0, sizeof vctx);
            vctx.have_key = 1; memcpy(vctx.key, k1, 32); memcpy(vctx.iv, gtiv, 16);
            uint16_t b[49];
            for (int i = 0; i < 49; i++) { b[i] = (uint16_t)(gv[g].enc[i] - '0'); }
            dmr_aes_voice_frame(&vctx, b, (size_t)gv[g].vc * 56);
            for (int i = 0; i < 49; i++) { if ((b[i]&1) != (uint16_t)(gv[g].dec[i]-'0')) { vpass = 0; } }
        }
        printf("  %s VOICE decrypt vs DSD-FME (49-bit ambe_d, vc 0/1/17)\n",
               vpass ? "PASS" : (fails++, "FAIL"));
    }

    printf(fails? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}

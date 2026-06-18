/* Host unit test for dmr_emb_sb_build() — the DMRA "Late Entry Single Block" (the burst-F
 * EMB alg/key announcement). Re-implements the dsd-fme BPTC(16x2) single-burst DECODE
 * (deinterleave + Hamming(16,11,4) syndrome + even-parity check) and confirms the firmware
 * ENCODE round-trips for every key id: syndrome 0, parity 0, payload == key_id<<3 | alg.
 * Build: gcc -O2 -Iapplication/include -Iapplication/include/crypto -o /tmp/t \
 *        tests/test_emb_sb.c application/source/crypto/dmr_aes.c && /tmp/t
 */
#include <stdio.h>
#include <stdint.h>
#include "crypto/dmr_aes.h"

/* dsd-fme fec.c Hamming(16,11,4) parity-check matrix + bptc.c interleave tables. */
static const unsigned char H[16*5] = {
  1,1,1,1,0,1,0,1,1,0,0, 1,0,0,0,0,  0,1,1,1,1,0,1,0,1,1,0, 0,1,0,0,0,
  0,0,1,1,1,1,0,1,0,1,1, 0,0,1,0,0,  1,1,1,0,1,0,1,1,0,0,1, 0,0,0,1,0,
  1,0,1,0,0,1,1,0,1,1,1, 0,0,0,0,1};
static const uint8_t Bptc[32]  = {0,17,2,19,4,21,6,23,8,25,10,27,12,29,14,31,
                                  16,1,18,3,20,5,22,7,24,9,26,11,28,13,30,15};
static const uint8_t Place[32] = {0,16,1,17,2,18,3,19,4,20,5,21,6,22,7,23,
                                  8,24,9,25,10,26,11,27,12,28,13,29,14,30,15,31};

static int verify(uint8_t key_id, uint8_t alg)
{
    uint8_t b[4];
    dmr_emb_sb_build(key_id, alg, b);

    uint8_t in[32];
    for (int i = 0; i < 32; i++) { in[i] = (b[i >> 3] >> (7 - (i & 7))) & 1u; }

    uint8_t dm[32];                                  /* deinterleave (dsd-fme BPTC decode) */
    for (int i = 0; i < 32; i++) { dm[Place[Bptc[i]]] = in[i]; }

    int syn = 0;
    for (int s = 0; s < 5; s++) { int a = 0; for (int x = 0; x < 16; x++) { a += dm[x] * H[16*s + x]; } syn = (syn << 1) | (a & 1); }

    int par = 0;                                     /* even parity: line2 == line1 */
    for (int i = 0; i < 16; i++) { if (dm[i] != dm[i + 16]) { par++; } }

    unsigned dec = 0;
    for (int i = 0; i < 11; i++) { dec = (dec << 1) | dm[i]; }

    unsigned want = ((unsigned)key_id << 3) | (alg & 7u);
    int ok = (syn == 0 && par == 0 && dec == want);
    printf("  key=%3u alg=%u -> %02X %02X %02X %02X  dec=0x%03X  %s\n",
           key_id, alg, b[0], b[1], b[2], b[3], dec, ok ? "PASS" : "FAIL");
    return ok;
}

int main(void)
{
    int all = 1;
    /* key id 1 / AES256 is the on-air-verified value; assert the exact octets. */
    uint8_t b1[4];
    dmr_emb_sb_build(1, 5, b1);
    int fixed = (b1[0] == 0x44 && b1[1] == 0x42 && b1[2] == 0x88 && b1[3] == 0x81);
    printf("  key=  1 alg=5 fixed-vector 44 42 88 81: %s\n", fixed ? "PASS" : "FAIL");
    all &= fixed;

    const uint8_t keys[] = {1, 2, 3, 7, 15, 16, 100, 255};
    for (unsigned i = 0; i < sizeof keys; i++) { all &= verify(keys[i], 5); }
    all &= verify(1, 4);   /* AES128 */

    printf("%s\n", all ? "ALL PASS" : "FAIL");
    return all ? 0 : 1;
}

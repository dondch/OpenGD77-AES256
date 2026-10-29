/* Host unit test for dmr_emb_sb_build() / dmr_emb_sb_decode() — the DMRA "Late Entry Single
 * Block" (the burst-F EMB alg/key announcement). Re-implements the dsd-fme BPTC(16x2)
 * single-burst DECODE (deinterleave + Hamming(16,11,4) syndrome + even-parity check) and
 * confirms the firmware ENCODE round-trips for every key id: syndrome 0, parity 0, payload
 * == key_id<<3 | alg. Then the firmware DECODE (the receiver's late-entry key id, issue #11):
 * it round-trips every key id 0..31 with every alg 0..7, agrees with the reference decoder,
 * rejects every single-bit flip of each codeword (it corrects nothing: a damaged announcement
 * costs one superframe, a mis-corrected one the whole call), and accepts a random 32-bit word
 * at the 2^-21 rate of 11 data bits in 32.
 * Build: gcc -O2 -Iapplication/include -Iapplication/include/crypto -o /tmp/t \
 *        tests/test_emb_sb.c application/source/crypto/dmr_aes.c && /tmp/t
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
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

/* The reference decoder alone: 1 = syndrome 0 and parity 0, *dec = the 11-bit payload. */
static int refDecode(const uint8_t b[4], unsigned *dec)
{
    uint8_t in[32], dm[32];
    for (int i = 0; i < 32; i++) { in[i] = (b[i >> 3] >> (7 - (i & 7))) & 1u; }
    for (int i = 0; i < 32; i++) { dm[Place[Bptc[i]]] = in[i]; }
    int syn = 0;
    for (int s = 0; s < 5; s++) { int a = 0; for (int x = 0; x < 16; x++) { a += dm[x] * H[16*s + x]; } syn = (syn << 1) | (a & 1); }
    int par = 0;
    for (int i = 0; i < 16; i++) { if (dm[i] != dm[i + 16]) { par++; } }
    *dec = 0;
    for (int i = 0; i < 11; i++) { *dec = (*dec << 1) | dm[i]; }
    return (syn == 0 && par == 0);
}

/* Firmware decoder: round trip of every key id 0..31 x alg 0..7, and every single-bit flip
 * of each of those 256 codewords rejected (8192 flips). */
static int verifyDecoder(void)
{
    int ok = 1, flipsRejected = 0, flips = 0;
    for (unsigned key = 0; key < 32; key++)
    {
        for (unsigned alg = 0; alg < 8; alg++)
        {
            uint8_t b[4], k = 0xFF, a = 0xFF;
            dmr_emb_sb_build((uint8_t)key, (uint8_t)alg, b);
            if (!(dmr_emb_sb_decode(b, &k, &a) == 1 && k == key && a == alg))
            {
                printf("  FAIL decode round trip key=%u alg=%u -> ok? k=%u a=%u\n", key, alg, k, a);
                ok = 0;
            }
            for (int bit = 0; bit < 32; bit++)
            {
                uint8_t f[4];
                unsigned dec;
                memcpy(f, b, 4);
                f[bit >> 3] ^= (uint8_t)(0x80u >> (bit & 7));
                flips++;
                int fw = dmr_emb_sb_decode(f, &k, &a);
                int rf = refDecode(f, &dec);
                if (fw == 0 && rf == 0) { flipsRejected++; }
                else { printf("  FAIL flip bit %d of key=%u alg=%u: firmware %d reference %d\n", bit, key, alg, fw, rf); ok = 0; }
            }
        }
    }
    printf("  %s decoder: 256 codewords round-trip, %d of %d single-bit flips rejected\n", ok ? "PASS" : "FAIL", flipsRejected, flips);
    return ok;
}

/* Random words: the firmware decoder and the reference decoder must agree on every one, and
 * the acceptance rate must be the 2^-21 of the code (11 data bits in 32). Deterministic PRNG,
 * so the count is reproducible: 2^24 words, expected 8 accepted. */
static int verifyRandom(void)
{
    const unsigned n = 1u << 24;
    unsigned accepted = 0, disagree = 0;
    uint32_t x = 0x9E3779B9u;
    for (unsigned i = 0; i < n; i++)
    {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        uint8_t b[4] = { (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16), (uint8_t)(x >> 24) };
        uint8_t k, a;
        unsigned dec;
        int fw = dmr_emb_sb_decode(b, &k, &a);
        int rf = refDecode(b, &dec);
        if (fw != rf || (fw && dec != (((unsigned)k << 3) | a))) { disagree++; }
        if (fw) { accepted++; }
    }
    int ok = (disagree == 0) && (accepted >= 2) && (accepted <= 32);
    printf("  %s random words: %u of %u accepted (expected %u, rate 2^-21), %u disagreements with the reference\n",
           ok ? "PASS" : "FAIL", accepted, n, n >> 21, disagree);
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
    all &= verifyDecoder();
    /* The word HR-C6000.c parks in the chip's registers between calls must never decode. */
    {
        const uint8_t parked[4] = { 0xFF, 0x00, 0xFF, 0x00 };
        uint8_t k, a;
        int rej = (dmr_emb_sb_decode(parked, &k, &a) == 0);
        printf("  %s parked word FF 00 FF 00 rejected\n", rej ? "PASS" : "FAIL");
        all &= rej;
    }
    all &= verifyRandom();

    printf("%s\n", all ? "ALL PASS" : "FAIL");
    return all ? 0 : 1;
}

/* Host test of the AES RX late-entry bootstrap in dmr_aes_hook.c (issue #2).
 *
 * The RX path reads 4 "late-entry MI" bits out of every received AMBE codeword,
 * clear calls included. On a CLEAR call those bits are ordinary voice data, and
 * they pass the Golay(24,12)+CRC4 check by chance on ~1% of superframes. Decryption
 * must never engage on that. A real encrypted call with no PI header (rapid re-PTT)
 * must still bootstrap from the in-band late entry and decrypt with the right MI.
 *
 * Drives the real dmrAesRxBurst / dmrAesRxLateEntry / dmrAesRxCodecFrame, in the
 * order HR-C6000.c and codecDecode call them.
 *
 * Build: gcc -O2 -DENABLE_AES -Itests/stubs -Iapplication/include -o /tmp/test_rx_bootstrap \
 *          tests/test_rx_bootstrap.c application/source/crypto/dmr_aes_hook.c \
 *          application/source/crypto/dmr_aes.c && /tmp/test_rx_bootstrap
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto/dmr_aes.h"
#include "crypto/dmr_aes_hook.h"
#include "functions/codeplug.h"
#include "hardware/SPI_Flash.h"

bool SPI_Flash_read(uint32_t a, uint8_t *b, int n) { (void)a; memset(b, 0xFF, (size_t)n); return true; }
bool SPI_Flash_write(uint32_t a, uint8_t *b, int n) { (void)a; (void)b; (void)n; return true; }
bool codeplugGetOpenGD77CustomDataBounded(CodeplugCustomDataType_t t, uint8_t *b, int n) { (void)t; (void)b; (void)n; return false; }
bool codeplugSetOpenGD77CustomData(CodeplugCustomDataType_t t, uint8_t *b, int n) { (void)t; (void)b; (void)n; return false; }

static const uint8_t KEY1[32] = {
    0x93,0xa5,0xcf,0x3b,0xda,0xb5,0x58,0xbc, 0xf6,0x1e,0xca,0x57,0x32,0xa8,0x65,0x78,
    0x32,0x39,0x6f,0x67,0x81,0x50,0xe1,0x78, 0x11,0xea,0xa7,0x49,0x1f,0x94,0xb3,0xee };

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void randomBurst(uint8_t *b27) { for (int i = 0; i < 27; i++) { b27[i] = (uint8_t)rnd(); } }

/* Inverse of the nibble read in dmrAesRxLateEntry: OTA bits 71/67/63/59 of each codeword. */
static void stuffNibble(uint8_t *b27, int cw, uint8_t nib)
{
    uint8_t *c = b27 + cw * 9;
    c[8] = (uint8_t)((c[8] & ~0x11) | ((nib >> 3) & 1) | (((nib >> 2) & 1) << 4));
    c[7] = (uint8_t)((c[7] & ~0x11) | ((nib >> 1) & 1) | ((nib & 1) << 4));
}

/* Feed one burst the way the firmware does; return 1 if decryption touched a voice frame. */
static int feedBurst(int seq, const uint8_t *b27)
{
    uint16_t b49[49], orig[49];
    dmrAesRxBurst(seq);
    dmrAesRxLateEntry(seq, b27);
    for (int i = 0; i < 49; i++) { b49[i] = orig[i] = (uint16_t)(rnd() & 1); }
    dmrAesRxCodecFrame(b49, 0);
    return memcmp(b49, orig, sizeof b49) != 0;
}

static void startRx(void)
{
    dmrAesInit();
    dmrAesSetKeyRam(1, KEY1);
}

static int fails = 0;

/* A long CLEAR call (random AMBE bits, no PI) must never engage decryption. */
static void testClearCallNeverDecrypts(void)
{
    const int superframes = 50000;   /* 5 hours of voice; ~565 chance CRC passes */
    int engaged = 0;
    uint8_t b27[27];
    startRx();
    for (int sf = 0; sf < superframes; sf++)
    {
        for (int seq = 1; seq <= 6; seq++)
        {
            randomBurst(b27);
            if (feedBurst(seq, b27)) { engaged++; dmrAesRxEnd(); }
        }
    }
    printf("  %s clear call: decryption engaged %d times in %d superframes\n",
           engaged ? "FAIL" : "PASS", engaged, superframes);
    if (engaged) { fails++; }
}

/* An encrypted call with no PI header must bootstrap from the late entry and then decrypt
 * every frame with the keystream of the true MI. The late entry of superframe k conveys the
 * MI of k+1, so the first one decodes at the start of superframe 1 and a confirming one at 2.
 * With every odd superframe's late entry corrupted, valid ones decode at 1, 3, 5: lock by 3. */
static void testEncryptedRapidCallBootstraps(const char *name, int corruptOddSuperframes, int lockBy)
{
    dmr_aes_ctx_t ref;
    dmr_pi_t pi = { DMR_ALG_AES256, DMR_MFID_DMRA, 1, 0, 1 };
    uint8_t b27[27], frag[7][3], iv[16];
    uint32_t mi[12], next;
    int firstEngaged = -1, wrongFrames = 0;

    mi[0] = 0x332BE4F8u;
    for (int k = 1; k < 12; k++) { dmr_lfsr128d(mi[k - 1], iv, &next); mi[k] = next; }

    startRx();
    for (int sf = 0; sf < 11; sf++)
    {
        dmr_le_mi_build(mi[sf + 1], frag);   /* superframe k conveys MI k+1 (look-ahead) */
        pi.mi = mi[sf];
        dmr_aes_rx_init(&ref, &pi);
        dmr_lfsr128d(mi[sf], ref.iv, &next);
        for (int seq = 1; seq <= 6; seq++)
        {
            uint16_t got[49], want[49];
            randomBurst(b27);
            if (!(corruptOddSuperframes && (sf & 1)))   /* a corrupted superframe keeps random bits */
            {
                for (int cw = 0; cw < 3; cw++) { stuffNibble(b27, cw, frag[seq][cw]); }
            }
            dmrAesRxBurst(seq);
            dmrAesRxLateEntry(seq, b27);
            for (int i = 0; i < 49; i++) { got[i] = want[i] = (uint16_t)(rnd() & 1); }
            dmrAesRxCodecFrame(got, 1);
            dmr_aes_voice_frame(&ref, want, (size_t)(seq - 1) * 168 + 56);
            if (memcmp(got, want, sizeof got) == 0 && firstEngaged < 0) { firstEngaged = sf; }
            if (firstEngaged >= 0 && memcmp(got, want, sizeof got) != 0) { wrongFrames++; }
        }
    }
    int ok = (firstEngaged >= 1) && (firstEngaged <= lockBy) && (wrongFrames == 0);
    printf("  %s %s: decrypting from superframe %d (limit %d), %d wrong frames after\n",
           ok ? "PASS" : "FAIL", name, firstEngaged, lockBy, wrongFrames);
    if (!ok) { fails++; }
}

/* A clear call right after an encrypted one must not decrypt. The call boundary (Terminator,
 * then the next call's Voice LC Header) reaches the hook as dmrAesRxEnd; the previous call's
 * last late entry must not be taken as a confirmation at the clear call's first burst. */
static void testClearCallAfterEncryptedCall(void)
{
    const int calls = 300;
    uint8_t b27[27], frag[7][3], iv[16];
    int engaged = 0;
    startRx();
    for (int call = 0; call < calls; call++)
    {
        uint32_t mi = rnd() | 1u, next;
        for (int sf = 0; sf < 4; sf++)
        {
            dmr_lfsr128d(mi, iv, &next);
            dmr_le_mi_build(next, frag);
            for (int seq = 1; seq <= 6; seq++)
            {
                randomBurst(b27);
                for (int cw = 0; cw < 3; cw++) { stuffNibble(b27, cw, frag[seq][cw]); }
                feedBurst(seq, b27);
            }
            mi = next;
        }
        dmrAesRxEnd();   /* Terminator */
        dmrAesRxEnd();   /* next call's Voice LC Header */
        for (int sf = 0; sf < 3; sf++)
        {
            for (int seq = 1; seq <= 6; seq++)
            {
                randomBurst(b27);
                if (feedBurst(seq, b27)) { engaged++; }
            }
        }
        dmrAesRxEnd();
        dmrAesRxEnd();
    }
    printf("  %s clear call after an encrypted call: decrypted %d frames in %d calls\n",
           engaged ? "FAIL" : "PASS", engaged, calls);
    if (engaged) { fails++; }
}

int main(void)
{
    testClearCallNeverDecrypts();
    testClearCallAfterEncryptedCall();
    testEncryptedRapidCallBootstraps("encrypted rapid call", 0, 2);
    testEncryptedRapidCallBootstraps("encrypted call, late entry lost every other superframe", 1, 3);
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}

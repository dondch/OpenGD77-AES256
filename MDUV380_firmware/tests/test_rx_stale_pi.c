/* Host test of the AES RX PI-header seeding in dmr_aes_hook.c across call boundaries.
 *
 * HR-C6000.c reads the LC page (0x02) of the chip on every voice burst and hands every
 * CRC-valid LC to dmrAesRxPI(). The chip keeps the PI header of an encrypted call in that page
 * for the whole call, and on the first bursts of the NEXT call it still returns the previous
 * call's LC (OpenGD77 itself skips the first frames' LC for the caller display because of this).
 * So a clear call that follows an encrypted one is handed the encrypted call's PI header once
 * more. Decryption must not engage on that: a PI whose MI is the one the call that just ended
 * was seeded with is stale. A new call always carries a fresh (random) MI.
 *
 * Build: gcc -O2 -Itests/stubs -Iapplication/include -o /tmp/test_rx_stale_pi \
 *          tests/test_rx_stale_pi.c application/source/crypto/dmr_aes_hook.c \
 *          application/source/crypto/dmr_aes.c && /tmp/test_rx_stale_pi
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto/dmr_aes.h"
#include "crypto/dmr_aes_hook.h"
#include "functions/codeplug.h"
#include "functions/ticks.h"
#include "hardware/SPI_Flash.h"

bool SPI_Flash_read(uint32_t a, uint8_t *b, int n) { (void)a; memset(b, 0xFF, (size_t)n); return true; }
bool SPI_Flash_write(uint32_t a, uint8_t *b, int n) { (void)a; (void)b; (void)n; return true; }
bool codeplugGetOpenGD77CustomDataBounded(CodeplugCustomDataType_t t, uint8_t *b, int n) { (void)t; (void)b; (void)n; return false; }
bool codeplugSetOpenGD77CustomData(CodeplugCustomDataType_t t, uint8_t *b, int n) { (void)t; (void)b; (void)n; return false; }
uint32_t ticksGetMillis(void) { static uint32_t t = 0; return t += 60; }

static const uint8_t KEY1[32] = {
    0x93,0xa5,0xcf,0x3b,0xda,0xb5,0x58,0xbc, 0xf6,0x1e,0xca,0x57,0x32,0xa8,0x65,0x78,
    0x32,0x39,0x6f,0x67,0x81,0x50,0xe1,0x78, 0x11,0xea,0xa7,0x49,0x1f,0x94,0xb3,0xee };

static uint32_t rng = 0x2545F491u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int fails = 0;
static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { fails++; }
}

static void piLc(uint32_t mi, uint8_t key, uint8_t *lc12)
{
    memset(lc12, 0, 12);
    dmr_pi_build(DMR_ALG_AES256, key, mi, lc12);
}

/* One voice burst as the firmware sees it: ISR burst/late-entry hooks, then the chip's LC page
 * (the PI the chip currently holds, or NULL for a non-PI LC), then codecDecode of frame 0.
 * Returns 1 if decryption changed the frame. Note the order: HR-C6000.c reads the LC page AFTER
 * dmrAesRxBurst(), so a PI read on burst N takes effect (IV generated) from burst N+1. */
static int burst(int seq, const uint8_t *lcPi)
{
    uint8_t b27[27];
    uint16_t b49[49], orig[49];
    for (int i = 0; i < 27; i++) { b27[i] = (uint8_t)rnd(); }
    dmrAesRxBurst(seq);
    dmrAesRxLateEntry(seq, b27);
    if (lcPi) { dmrAesRxPI(lcPi, 12); }
    for (int i = 0; i < 49; i++) { b49[i] = orig[i] = (uint16_t)(rnd() & 1); }
    b49[24] = orig[24] = 1;   /* never a comfort-noise frame */
    dmrAesRxCodecFrame(b49, 0);
    return memcmp(b49, orig, sizeof b49) != 0;
}

/* Does frame 0 of burst seq (first superframe of a call) decrypt with the keystream of MI mi?
 * The PI must already have been read on an earlier burst of this call. */
static int burstDecryptsWith(int seq, uint32_t mi)
{
    uint8_t b27[27];
    uint16_t got[49], want[49];
    dmr_aes_ctx_t ref;
    dmr_pi_t p = { DMR_ALG_AES256, DMR_MFID_DMRA, 1, mi, 1 };
    uint32_t next;
    dmr_aes_rx_init(&ref, &p);
    dmr_lfsr128d(p.mi, ref.iv, &next);
    for (int i = 0; i < 27; i++) { b27[i] = (uint8_t)rnd(); }
    dmrAesRxBurst(seq);
    dmrAesRxLateEntry(seq, b27);
    for (int i = 0; i < 49; i++) { got[i] = want[i] = (uint16_t)(rnd() & 1); }
    got[24] = want[24] = 1;
    dmrAesRxCodecFrame(got, 0);
    dmr_aes_voice_frame(&ref, want, (size_t)(seq - 1) * 168);
    return memcmp(got, want, sizeof got) == 0;
}

/* An encrypted call seeded from its PI header, CRC-valid Terminator at the end. */
static void encryptedCall(uint32_t mi, int superframes)
{
    uint8_t pi[12];
    piLc(mi, 1, pi);
    dmrAesRxEnd();                       /* Voice LC Header */
    for (int sf = 0; sf < superframes; sf++)
    {
        for (int seq = 1; seq <= 6; seq++) { burst(seq, pi); }   /* the chip re-surfaces the PI every burst */
    }
    dmrAesRxEnd();                       /* Terminator */
}

static void testEncryptedCallDecrypts(void)
{
    uint8_t pi[12];
    dmrAesInit();
    dmrAesSetKeyRam(1, KEY1);
    piLc(0x1D6F0A21u, 1, pi);
    dmrAesRxEnd();
    check(burst(1, pi) == 0, "burst A: the PI is read after the burst hooks, so A itself is passed clear");
    check(burstDecryptsWith(2, 0x1D6F0A21u), "PI-seeded call decrypts from burst B");
    check(burstDecryptsWith(3, 0x1D6F0A21u), "and keeps decrypting");
    dmrAesRxEnd();
}

static void testStalePiOnTheNextClearCall(void)
{
    const int calls = 200;
    int engaged = 0;
    dmrAesInit();
    dmrAesSetKeyRam(1, KEY1);
    for (int c = 0; c < calls; c++)
    {
        uint32_t mi = rnd();
        uint8_t stale[12];
        piLc(mi, 1, stale);
        encryptedCall(mi, 2);
        /* clear call: Voice LC Header, then the chip's first LC read still returns the old PI */
        dmrAesRxEnd();
        for (int sf = 0; sf < 2; sf++)
        {
            for (int seq = 1; seq <= 6; seq++)
            {
                if (burst(seq, (sf == 0 && seq <= 2) ? stale : NULL)) { engaged++; }
            }
        }
        dmrAesRxEnd();
    }
    printf("  %s clear call after an encrypted call, old PI re-fed: decrypted %d frames in %d calls\n",
           engaged ? "FAIL" : "PASS", engaged, calls);
    if (engaged) { fails++; }
}

static void testFreshPiOnTheNextCallSeeds(void)
{
    uint8_t pi2[12];
    dmrAesInit();
    dmrAesSetKeyRam(1, KEY1);
    encryptedCall(0x0BADF00Du, 2);
    piLc(0x600DCAFEu, 1, pi2);
    dmrAesRxEnd();
    burst(1, pi2);
    check(burstDecryptsWith(2, 0x600DCAFEu), "a second encrypted call with a new MI decrypts from burst B");
    dmrAesRxEnd();
}

static void testStaleThenFreshPiOnARapidEncryptedCall(void)
{
    uint8_t stale[12], fresh[12];
    dmrAesInit();
    dmrAesSetKeyRam(1, KEY1);
    encryptedCall(0x11112222u, 2);
    piLc(0x11112222u, 1, stale);
    piLc(0x33334444u, 1, fresh);
    dmrAesRxEnd();                                   /* new call's Voice LC Header */
    burst(1, stale);                                 /* first LC read: the chip lags one call behind */
    burst(2, fresh);                                 /* second read: this call's own PI */
    check(burstDecryptsWith(3, 0x33334444u), "rapid second call: the fresh PI seeds, not the stale one");
    check(burstDecryptsWith(4, 0x33334444u), "and the call stays locked on the fresh MI");
    dmrAesRxEnd();
}

static void testOwnTxPiIsStaleForRx(void)
{
    /* After our own encrypted TX the chip's LC page can still hold the PI we wrote. */
    uint8_t ours[12];
    int engaged = 0;
    dmrAesInit();
    dmrAesSetKeyRam(1, KEY1);
    dmrAesTxStart(1, 0x7777AAAAu);
    dmrAesTxBuildPI(ours);
    memset(ours + 7, 0, 5);
    dmrAesTxEnd();
    dmrAesRxEnd();                                   /* an incoming clear call */
    for (int seq = 1; seq <= 6; seq++) { if (burst(seq, (seq == 1) ? ours : NULL)) { engaged++; } }
    check(engaged == 0, "our own TX PI header re-read by RX does not engage decryption");
    dmrAesRxEnd();
}

static void testSameMiTwiceWithACallBetweenIsAccepted(void)
{
    /* The stale guard applies to the call that JUST ended: a different PI in between clears it. */
    uint8_t a[12];
    dmrAesInit();
    dmrAesSetKeyRam(1, KEY1);
    encryptedCall(0xAAAA0001u, 1);
    encryptedCall(0xBBBB0002u, 1);
    piLc(0xAAAA0001u, 1, a);
    dmrAesRxEnd();
    burst(1, a);
    check(burstDecryptsWith(2, 0xAAAA0001u), "an MI seen two calls ago is not stale");
    dmrAesRxEnd();
}

int main(void)
{
    testEncryptedCallDecrypts();
    testStalePiOnTheNextClearCall();
    testFreshPiOnTheNextCallSeeds();
    testStaleThenFreshPiOnARapidEncryptedCall();
    testOwnTxPiIsStaleForRx();
    testSameMiTwiceWithACallBetweenIsAccepted();
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}

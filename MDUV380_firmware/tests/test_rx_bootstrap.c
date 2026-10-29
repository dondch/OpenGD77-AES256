/* Host test of the AES RX late-entry bootstrap in dmr_aes_hook.c (issue #2).
 *
 * The RX path reads 4 "late-entry MI" bits out of every received AMBE codeword,
 * clear calls included. On a CLEAR call those bits are ordinary voice data, and
 * they pass the Golay(24,12)+CRC4 check by chance on ~1% of superframes. Decryption
 * must never engage on that. A real encrypted call with no PI header (rapid re-PTT)
 * must still bootstrap from the in-band late entry and decrypt with the right MI.
 *
 * Drives the real dmrAesRxBurst / dmrAesRxLateEntry / dmrAesRxEmbSb / dmrAesRxCodecFrame, in
 * the order HR-C6000.c and codecDecode call them. The scenarios at the end cover the key the
 * late-entry bootstrap picks (issue #11): the transmitter's burst-F EMB announcement first,
 * then the channel's key, the last call's key and the lowest loaded slot.
 *
 * Build: gcc -O2 -Itests/stubs -Iapplication/include -o /tmp/test_rx_bootstrap \
 *          tests/test_rx_bootstrap.c application/source/crypto/dmr_aes_hook.c \
 *          application/source/crypto/dmr_aes.c && /tmp/test_rx_bootstrap
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
uint32_t ticksGetMillis(void) { static uint32_t t = 0; return t += 60; }   /* for the -DDMR_AES_DIAG_RX build */

static const uint8_t KEY1[32] = {
    0x93,0xa5,0xcf,0x3b,0xda,0xb5,0x58,0xbc, 0xf6,0x1e,0xca,0x57,0x32,0xa8,0x65,0x78,
    0x32,0x39,0x6f,0x67,0x81,0x50,0xe1,0x78, 0x11,0xea,0xa7,0x49,0x1f,0x94,0xb3,0xee };

static const uint8_t KEY2[32] = {
    0x1c,0x7e,0x02,0xb9,0x44,0x8d,0xf0,0x2a, 0x63,0xd1,0x9b,0x05,0xee,0x37,0xc8,0x41,
    0xa9,0x5f,0x16,0xd4,0x7b,0x20,0xcc,0x8e, 0x02,0x93,0x6a,0xf5,0x58,0xb7,0x3d,0x11 };

/* The channel's configured key, as HR-C6000.c would resolve it: overrides the hook's weak default. */
static uint8_t s_keyHint = 0;
uint8_t dmrAesRxKeyHint(void) { return s_keyHint; }

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
    dmrAesSetKeyRam(2, KEY2);
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

/* One encrypted call as the firmware sees it, with no PI header unless piSeeded: it must
 * bootstrap from the late entry and then decrypt every frame with the keystream of the true MI
 * under the transmitter's key. The late entry of superframe k conveys the MI of k+1, so the
 * first one decodes at the start of superframe 1 and, with no LC seen, the confirming one that
 * activates decryption at 2. With every odd superframe's late entry corrupted, valid ones decode
 * at 1, 3, 5: lock at 3. The block of burst F reaches the hook a burst before each wrap, so the
 * announced key is known from the end of superframe 0, ahead of the bootstrap.
 *
 * The transmitter's burst-F EMB Late Entry Single Block (alg + key id) is fed on every burst F
 * the way HR-C6000.c does, through dmrAesRxEmbSb with the raw 4 octets, so a damaged block
 * goes through the real decoder. The key the bootstrap picks is not observable directly: a
 * frame decrypts correctly only under the transmitter's key, so the first correct frame (and
 * none wrong after it) tells which key engaged and when. */
typedef struct
{
    const char *name;
    int corruptOddSuperframes;   /* late entry lost every other superframe */
    uint8_t keyId;               /* the transmitter's key (1 or 2 are loaded, 5 is not) */
    uint8_t hint;                /* the channel's key, dmrAesRxKeyHint(); 0 = none */
    uint8_t lastKey;             /* key of a PI-seeded call that preceded this one; 0 = none */
    uint8_t embKey, embAlg;      /* the burst-F EMB announcement; embKey 0 = no block sent */
    int embFrom;                 /* first superframe whose block arrives undamaged (earlier ones carry a flipped bit) */
    int piSeeded;                /* the call's PI header (under keyId) was seen before the voice */
    int expectSf, expectSeq;     /* first correctly decrypted frame (superframe, burst); expectSf -1 = never */
} scenario_t;

/* A PI-seeded call under keyId, then its Terminator and the next call's Voice LC Header: leaves
 * the hook remembering keyId as "the last call's key". */
static void primeLastCall(uint8_t keyId)
{
    uint8_t lc[12], b27[27];
    memset(lc, 0, sizeof lc);
    dmr_pi_build(DMR_ALG_AES256, keyId, 0x5A5A1234u, lc);
    dmrAesRxPI(lc, 12);
    randomBurst(b27);
    feedBurst(1, b27);
    dmrAesRxEnd();
    dmrAesRxEnd();
}

static void runScenario(const scenario_t *sc)
{
    dmr_aes_ctx_t ref;
    dmr_pi_t pi = { DMR_ALG_AES256, DMR_MFID_DMRA, sc->keyId, 0, 1 };
    s_keyHint = sc->hint;
    uint8_t b27[27], frag[7][3], iv[16], sb[4];
    uint32_t mi[12], next;
    int firstSf = -1, firstSeq = -1, wrongFrames = 0;

    mi[0] = 0x332BE4F8u;
    for (int k = 1; k < 12; k++) { dmr_lfsr128d(mi[k - 1], iv, &next); mi[k] = next; }

    startRx();
    if (sc->lastKey) { primeLastCall(sc->lastKey); }
    if (sc->piSeeded)
    {
        uint8_t lc[12];
        memset(lc, 0, sizeof lc);
        dmr_pi_build(DMR_ALG_AES256, sc->keyId, mi[0], lc);
        dmrAesRxPI(lc, 12);
    }
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
            if (!(sc->corruptOddSuperframes && (sf & 1)))   /* a corrupted superframe keeps random bits */
            {
                for (int cw = 0; cw < 3; cw++) { stuffNibble(b27, cw, frag[seq][cw]); }
            }
            dmrAesRxBurst(seq);
            dmrAesRxLateEntry(seq, b27);
            if ((seq == 6) && sc->embKey)
            {
                dmr_emb_sb_build(sc->embKey, sc->embAlg, sb);
                if (sf < sc->embFrom) { sb[1] ^= 0x08; }   /* one flipped bit: syndrome != 0 */
                dmrAesRxEmbSb(sb, 1);
            }
            for (int i = 0; i < 49; i++) { got[i] = want[i] = (uint16_t)(rnd() & 1); }
            dmrAesRxCodecFrame(got, 1);
            dmr_aes_voice_frame(&ref, want, (size_t)(seq - 1) * 168 + 56);
            int right = (memcmp(got, want, sizeof got) == 0);
            if (right && firstSf < 0) { firstSf = sf; firstSeq = seq; }
            if (firstSf >= 0 && !right) { wrongFrames++; }
        }
    }
    int ok = (firstSf == sc->expectSf) && ((sc->expectSf < 0) || (firstSeq == sc->expectSeq)) && (wrongFrames == 0);
    if (firstSf < 0)
    {
        printf("  %s %s: never decrypted (expected %s)\n", ok ? "PASS" : "FAIL", sc->name,
               (sc->expectSf < 0) ? "never" : "a lock");
    }
    else
    {
        printf("  %s %s: decrypting from superframe %d burst %d (expected %d/%d), %d wrong frames after\n",
               ok ? "PASS" : "FAIL", sc->name, firstSf, firstSeq, sc->expectSf, sc->expectSeq, wrongFrames);
    }
    if (!ok) { fails++; }
    s_keyHint = 0;
}

/* The burst-F EMB announcement decides the late-entry key (issue #11). */
static const scenario_t scenarios[] = {
    /* The pre-EMB chain, unchanged when no block is received. */
    { "encrypted rapid call",                                        0, 1, 0, 0, 0, 0, 0, 0, 2, 1 },
    { "encrypted call, late entry lost every other superframe",      1, 1, 0, 0, 0, 0, 0, 0, 3, 1 },
    /* keys in slots 1 and 2, the channel on key 2, joined by late entry (no PI): the late entry
     * carries no key id; without a block the bootstrap takes the channel's key, not the lowest slot. */
    { "late entry on a key-2 channel, no EMB block",                 0, 2, 2, 0, 0, 0, 0, 0, 2, 1 },
    { "late entry on a key-1 channel with key 2 also loaded",        0, 1, 1, 0, 0, 0, 0, 0, 2, 1 },
    { "late entry, no channel key, lowest slot fallback",            0, 1, 0, 0, 0, 0, 0, 0, 2, 1 },
    /* The EMB announces key 2 while the channel says key 1 and the last call used key 1 (the radio
     * sits on channel 2 while A transmits on channel 3): the announced key wins. */
    { "EMB key 2, channel key 1, last call key 1",                   0, 2, 1, 1, 2, 5, 0, 0, 2, 1 },
    { "EMB key 2, no channel key, last call key 1",                  0, 2, 0, 1, 2, 5, 0, 0, 2, 1 },
    /* Without the block the same call locks onto key 1 and stays noise: the control. */
    { "no EMB block, channel key 1, last call key 1, sender key 2",  0, 2, 1, 1, 0, 0, 0, 0, -1, 0 },
    /* AES-128 announced: not ours, ignored, the chain picks the channel's key. */
    { "EMB announces alg 4 (AES-128): ignored, channel key used",    0, 1, 1, 1, 2, 4, 0, 0, 2, 1 },
    /* An unloaded slot announced: the chain picks the channel's key. */
    { "EMB announces unloaded slot 5: ignored, channel key used",    0, 1, 1, 1, 5, 5, 0, 0, 2, 1 },
    /* Every block damaged (one flipped bit): dropped, the chain picks the channel's key. */
    { "corrupted EMB block every superframe: dropped",               0, 1, 1, 1, 2, 5, 99, 0, 2, 1 },
    { "corrupted EMB block every superframe, sender on key 2: noise", 0, 2, 1, 1, 2, 5, 99, 0, -1, 0 },
    /* The blocks of superframes 0 and 1 are lost: the bootstrap guesses the channel's key 1 at
     * superframe 2, the block of its burst F announces key 2 and the stream switches at once:
     * right from burst 6 of superframe 2, one superframe of noise instead of a whole call. */
    { "first EMB blocks lost, bootstrap on key 1, rekeyed at burst F", 0, 2, 1, 1, 2, 5, 2, 0, 2, 6 },
    /* A PI-seeded call knows its key: an announcement of another key is ignored. */
    { "PI-seeded on key 1, EMB announces key 2: PI wins",            0, 1, 0, 0, 2, 5, 0, 1, 0, 1 },
};

/* The announcement of one call must not seed the next: call 1 announces key 2 (and is decrypted
 * with it), call 2 sends no block and is on key 1 with the channel on key 1. If the stale key 2
 * survived the call boundary it would be tried first and call 2 would be noise. */
static void testEmbDoesNotOutliveItsCall(void)
{
    scenario_t call1 = { "call 1 announces key 2", 0, 2, 0, 0, 2, 5, 0, 0, 2, 1 };
    scenario_t call2 = { "call 2, no block, key 1 on a key-1 channel (stale EMB key 2 must not seed it)", 0, 1, 1, 0, 0, 0, 0, 0, 2, 1 };
    runScenario(&call1);
    /* Terminator, then the next call's Voice LC Header, without re-initialising the hook. */
    dmrAesRxEnd();
    dmrAesRxEnd();
    /* runScenario re-initialises; drive call 2 by hand on the live state instead. */
    dmr_aes_ctx_t ref;
    dmr_pi_t pi = { DMR_ALG_AES256, DMR_MFID_DMRA, call2.keyId, 0, 1 };
    s_keyHint = call2.hint;
    uint8_t b27[27], frag[7][3], iv[16];
    uint32_t mi[12], next;
    int firstSf = -1, firstSeq = -1, wrongFrames = 0;
    mi[0] = 0x7B1D9C02u;
    for (int k = 1; k < 12; k++) { dmr_lfsr128d(mi[k - 1], iv, &next); mi[k] = next; }
    for (int sf = 0; sf < 6; sf++)
    {
        dmr_le_mi_build(mi[sf + 1], frag);
        pi.mi = mi[sf];
        dmr_aes_rx_init(&ref, &pi);
        dmr_lfsr128d(mi[sf], ref.iv, &next);
        for (int seq = 1; seq <= 6; seq++)
        {
            uint16_t got[49], want[49];
            randomBurst(b27);
            for (int cw = 0; cw < 3; cw++) { stuffNibble(b27, cw, frag[seq][cw]); }
            dmrAesRxBurst(seq);
            dmrAesRxLateEntry(seq, b27);
            for (int i = 0; i < 49; i++) { got[i] = want[i] = (uint16_t)(rnd() & 1); }
            dmrAesRxCodecFrame(got, 1);
            dmr_aes_voice_frame(&ref, want, (size_t)(seq - 1) * 168 + 56);
            int right = (memcmp(got, want, sizeof got) == 0);
            if (right && firstSf < 0) { firstSf = sf; firstSeq = seq; }
            if (firstSf >= 0 && !right) { wrongFrames++; }
        }
    }
    int ok = (firstSf == call2.expectSf) && (firstSeq == call2.expectSeq) && (wrongFrames == 0);
    printf("  %s %s: decrypting from superframe %d burst %d (expected %d/%d), %d wrong frames after\n",
           ok ? "PASS" : "FAIL", call2.name, firstSf, firstSeq, call2.expectSf, call2.expectSeq, wrongFrames);
    if (!ok) { fails++; }
    s_keyHint = 0;
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
    for (unsigned i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) { runScenario(&scenarios[i]); }
    testEmbDoesNotOutliveItsCall();
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}

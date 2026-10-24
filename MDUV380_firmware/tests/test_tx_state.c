/* Host test of the AES TX state in dmr_aes_hook.c.
 *
 * HR-C6000.c starts every DMR call through dmrAesTxStart() at DMR_STATE_TX_START_1 with the
 * key id resolved for that call: 0 means a CLEAR call (no key selected, a data call, or hotspot
 * relay). A previous encrypted call can end without reaching DMR_STATE_TX_END_1 (interrupt
 * timeout, mode change, data-harness slot reset), so dmrAesTxStart(0, ...) must positively
 * turn encryption OFF even though nothing called dmrAesTxEnd(): otherwise the next clear call
 * goes out with the encryption Voice-LC flags, PI headers, late-entry MI and an encrypted
 * codec stream, with the stale MI of the aborted call.
 *
 * Also round-trips the TX codec path into the real RX path (PI seed + late-entry MI chain), the
 * way a stock radio or our own RX decodes our transmission.
 *
 * Build: gcc -O2 -DENABLE_AES -Itests/stubs -Iapplication/include -o /tmp/test_tx_state \
 *          tests/test_tx_state.c application/source/crypto/dmr_aes_hook.c \
 *          application/source/crypto/dmr_aes.c && /tmp/test_tx_state
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

static uint32_t rng = 0x9e3779b9u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int fails = 0;
static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { fails++; }
}

/* A random non-silence, non-comfort-noise 49-bit frame (bit 24 set defeats the CCR test). */
static void randomVoice(uint16_t *b49)
{
    for (int i = 0; i < 49; i++) { b49[i] = (uint16_t)(rnd() & 1); }
    b49[24] = 1;
}

/* Does the TX codec hook leave a frame untouched (clear call)? */
static int txLeavesFrameClear(void)
{
    uint16_t b49[49], orig[49], b72[72], orig72[72];
    randomVoice(b49);
    memcpy(orig, b49, sizeof b49);
    dmrAesTxCodecFrame(b49);
    for (int i = 0; i < 72; i++) { b72[i] = orig72[i] = (uint16_t)(rnd() & 1); }
    dmrAesTxStuffMI(b72);
    return (memcmp(b49, orig, sizeof b49) == 0) && (memcmp(b72, orig72, sizeof b72) == 0);
}

static void testNoEncryptionBeforeAnyCall(void)
{
    dmrAesInit();
    dmrAesSetKeyRam(1, KEY1);
    check(!dmrAesTxActive() && txLeavesFrameClear(), "boot state: TX hook is a passthrough");
}

static void testKeyZeroStartEndsAnAbortedEncryptedCall(void)
{
    uint8_t pi[7], emb[4];
    dmrAesInit();
    dmrAesSetKeyRam(1, KEY1);

    dmrAesTxStart(1, 0x5A5A1234u);
    check(dmrAesTxActive() && (dmrAesTxBuildPI(pi) == 7), "encrypted call starts with key 1");
    /* ... the call is aborted without DMR_STATE_TX_END_1 (no dmrAesTxEnd) ... */

    dmrAesTxStart(0, 0);   /* next key-up resolves to a CLEAR call */
    check(!dmrAesTxActive(), "key id 0 at the next key-up turns encryption off");
    check(dmrAesTxBuildPI(pi) == 0, "no PI header is built for the clear call");
    check(dmrAesTxEmbSb(emb) == 0, "no late-entry single block for the clear call");
    check(txLeavesFrameClear(), "codec frames of the clear call are not encrypted");
}

static void testKeyZeroIsOffEvenWhenSlotZeroHoldsAKey(void)
{
    /* keyId 0 is the "encrypted TX disabled" sentinel everywhere (dmrAesTxKeyId, the channel
     * encrypt byte, the hotspot/data gates). A key loaded into slot 0 over USB must not turn it
     * into a real key id on TX. */
    dmrAesInit();
    dmrAesSetKeyRam(0, KEY1);
    dmrAesTxStart(0, 0x11111111u);
    check(!dmrAesTxActive() && txLeavesFrameClear(), "key id 0 never starts an encrypted call");
}

/* Encode one frame through the TX path and hand its late-entry nibble to the RX path, in the
 * order codecEncodeBlock -> HR-C6000 TX -> HR-C6000 RX ISR -> codecDecode. */
static void testTxDecodesOnOwnRx(void)
{
    uint8_t pi[12] = { 0 }, b27[27];
    int wrong = 0, frames = 0;
    dmrAesInit();
    dmrAesSetKeyRam(1, KEY1);

    dmrAesTxStart(1, 0x332BE4F8u);
    check(dmrAesTxBuildPI(pi) == 7, "PI header built");
    dmrAesRxEnd();                 /* receiver sees our Voice LC Header */
    dmrAesRxPI(pi, 12);            /* ... and our PI header */

    for (int f = 0; f < 18 * 4; f++)
    {
        uint16_t plain[49], enc[49], b72[72];
        int seq = ((f / 3) % 6) + 1, cw = f % 3;
        randomVoice(plain);
        memcpy(enc, plain, sizeof plain);
        dmrAesTxCodecFrame(enc);
        memset(b72, 0, sizeof b72);
        dmrAesTxStuffMI(b72);
        if (cw == 0) { memset(b27, 0, sizeof b27); }
        /* the 4 MI bits of codeword cw: OTA bits 71/67/63/59 -> bytes 8/8/7/7 of the codeword */
        b27[cw * 9 + 8] |= (uint8_t)((b72[71] & 1) | ((b72[67] & 1) << 4));
        b27[cw * 9 + 7] |= (uint8_t)((b72[63] & 1) | ((b72[59] & 1) << 4));
        if (cw == 2)
        {
            /* burst complete: the RX ISR sees it, then codecDecode runs its 3 frames. We only
             * check the last frame of the burst here (the others were encrypted the same way). */
            dmrAesRxBurst(seq);
            dmrAesRxLateEntry(seq, b27);
            dmrAesRxCodecFrame(enc, cw);
            frames++;
            if (memcmp(enc, plain, sizeof plain) != 0) { wrong++; }
        }
    }
    dmrAesTxEnd();
    printf("  %s own TX decodes on own RX: %d of %d checked frames wrong\n", wrong ? "FAIL" : "PASS", wrong, frames);
    if (wrong) { fails++; }
}

int main(void)
{
    testNoEncryptionBeforeAnyCall();
    testKeyZeroStartEndsAnAbortedEncryptedCall();
    testKeyZeroIsOffEvenWhenSlotZeroHoldsAKey();
    testTxDecodesOnOwnRx();
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}

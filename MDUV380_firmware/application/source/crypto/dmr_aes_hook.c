#include "crypto/dmr_aes_hook.h"
#ifdef ENABLE_AES
#include "crypto/dmr_aes.h"
#include "functions/codeplug.h"
#include <stddef.h>
#include <string.h>

#define DMR_AMBE_BURST 27

/* AES key store block: see dmrAesLoadKeys. */
#define AESK_HDR_LEN   8
#define AESK_ENTRY_LEN 36
#define AESK_SLOTS     DMR_AES_MAX_KEYS
#define AESK_BLOCK_LEN (AESK_HDR_LEN + AESK_SLOTS * AESK_ENTRY_LEN)

static dmr_aes_ctx_t s_rx DMR_AES_CCM, s_tx DMR_AES_CCM;
static size_t        s_rxOff DMR_AES_CCM, s_txOff DMR_AES_CCM;
static int           s_rxActive DMR_AES_CCM, s_txActive DMR_AES_CCM, s_keysLoaded DMR_AES_CCM;
static uint32_t      s_txPiMi DMR_AES_CCM;   /* MI to advertise in the PI header (pre-advance seed) */
static uint8_t       s_txKeyId DMR_AES_CCM;  /* selected TX key (AESK header byte 5); 0 = enc TX off */

void dmrAesLoadKeys(void)
{
    static uint8_t blk[AESK_BLOCK_LEN] DMR_AES_CCM;
    s_keysLoaded = 1;
    dmr_aes_clear_keys();
    s_txKeyId = 0;
    if (!codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk)) { return; }
    if (memcmp(blk, "AESK", 4) != 0) { return; }
    s_txKeyId = blk[5];   /* active TX key selector (0 = encrypted TX disabled) */
    for (int i = 0; i < AESK_SLOTS; i++)
    {
        uint8_t *e = blk + AESK_HDR_LEN + i * AESK_ENTRY_LEN;
        if (e[0] == 1) { dmr_aes_set_key(e[1], e + 4); }
    }
}

uint8_t dmrAesTxKeyId(void)
{
    if (!s_keysLoaded) { dmrAesLoadKeys(); }
    return s_txKeyId;
}

int dmrAesSetTxKeyId(uint8_t keyId)
{
    static uint8_t blk[AESK_BLOCK_LEN] DMR_AES_CCM;
    if (!codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk) || memcmp(blk, "AESK", 4) != 0)
    {
        memset(blk, 0, sizeof(blk)); memcpy(blk, "AESK", 4); blk[4] = 1;
    }
    blk[5] = keyId;
    int ok = codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk, AESK_BLOCK_LEN) ? 1 : 0;
    dmrAesLoadKeys();
    return ok;
}

int dmrAesStoreKey(uint8_t keyId, const uint8_t *key32)
{
    static uint8_t blk[AESK_BLOCK_LEN] DMR_AES_CCM;
    int slot = -1, freeSlot = -1;
    if (!codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk) || memcmp(blk, "AESK", 4) != 0)
    {
        memset(blk, 0, sizeof(blk)); memcpy(blk, "AESK", 4); blk[4] = 1;
    }
    for (int i = 0; i < AESK_SLOTS; i++)
    {
        uint8_t *e = blk + AESK_HDR_LEN + i * AESK_ENTRY_LEN;
        if ((e[0] == 1) && (e[1] == keyId)) { slot = i; break; }
        if ((freeSlot < 0) && (e[0] == 0))  { freeSlot = i; }
    }
    if (slot < 0) { slot = freeSlot; }
    if (slot < 0) { return 0; }
    uint8_t *e = blk + AESK_HDR_LEN + slot * AESK_ENTRY_LEN;
    e[0] = 1; e[1] = keyId; memcpy(e + 4, key32, 32);
    int ok = codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk, AESK_BLOCK_LEN) ? 1 : 0;
    dmrAesLoadKeys();
    return ok;
}

/* ---- RX (validated on-air: DSD-FME decrypts the same scheme) ---- */
void dmrAesRxPI(const uint8_t *pi, int len)
{
    dmr_pi_t p;
    if (!s_keysLoaded) { dmrAesLoadKeys(); }
    if (dmr_pi_parse(pi, (size_t)len, &p) && p.valid)
    {
        /* Just seed the MI from the PI; the IV is generated at each superframe start
         * (frame A) below, so superframe 0 uses IV=f(PI.mi). */
        s_rxActive = (dmr_aes_rx_init(&s_rx, &p) == 0);
        s_rxOff = 0;
    }
}
void dmrAesRxVoice(uint8_t *ambe, int seq)
{
    if (!s_rxActive) { return; }
    /* RX voice bursts are numbered 1..6 (frame A = 1 = superframe start), NOT 0..5.
     * Regenerate the IV and reset the keystream offset on frame A. */
    if (seq == 1) { dmr_aes_superframe(&s_rx); s_rxOff = 0; }
    s_rxOff = dmr_aes_crypt_frame(&s_rx, ambe, DMR_AMBE_BURST, s_rxOff);
}
void dmrAesRxEnd(void) { s_rxActive = 0; }

/* ---- TX (symmetric: OFB crypt is identical to RX) ----
 * NOTE(flash): exact PI-header burst scheduling + keystream octet base are confirmed at bench. */
void dmrAesTxStart(uint8_t keyId, uint32_t miSeed)
{
    if (!s_keysLoaded) { dmrAesLoadKeys(); }
    s_txActive = (dmr_aes_tx_init(&s_tx, DMR_ALG_AES256, keyId, miSeed) == 0);
    s_txOff = 0;
    /* The PI header advertises the pre-advance MI: the receiver's rx_init seeds its
     * MI from this value and then runs the same initial superframe() advance we do
     * below, so our TX and the (on-air validated) RX path stay bit-for-bit aligned. */
    s_txPiMi = miSeed;
    if (s_txActive) { dmr_aes_superframe(&s_tx); }
}
int dmrAesTxBuildPI(uint8_t *piOut7)   /* build PI header LC to emit at call start / late entry */
{
    if (!s_txActive) { return 0; }
    dmr_pi_build(DMR_ALG_AES256, s_tx.key_id, s_txPiMi, piOut7);
    return 7;
}
void dmrAesTxVoice(uint8_t *ambe, int seq)
{
    if (!s_txActive) { return; }
    if (seq == 0) { dmr_aes_superframe(&s_tx); s_txOff = 0; }
    s_txOff = dmr_aes_crypt_frame(&s_tx, ambe, DMR_AMBE_BURST, s_txOff);
}
int  dmrAesTxActive(void) { return s_txActive; }
void dmrAesTxEnd(void)    { s_txActive = 0; }
#endif

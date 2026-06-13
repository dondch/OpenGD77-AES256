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

static dmr_aes_ctx_t s_rx, s_tx;
static size_t        s_rxOff, s_txOff;
static int           s_rxActive, s_txActive, s_keysLoaded;

void dmrAesLoadKeys(void)
{
    static uint8_t blk[AESK_BLOCK_LEN];
    s_keysLoaded = 1;
    dmr_aes_clear_keys();
    if (!codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk)) { return; }
    if (memcmp(blk, "AESK", 4) != 0) { return; }
    for (int i = 0; i < AESK_SLOTS; i++)
    {
        uint8_t *e = blk + AESK_HDR_LEN + i * AESK_ENTRY_LEN;
        if (e[0] == 1) { dmr_aes_set_key(e[1], e + 4); }
    }
}

int dmrAesStoreKey(uint8_t keyId, const uint8_t *key32)
{
    static uint8_t blk[AESK_BLOCK_LEN];
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
        s_rxActive = (dmr_aes_rx_init(&s_rx, &p) == 0);
        s_rxOff = 0;
        if (s_rxActive) { dmr_aes_superframe(&s_rx); }
    }
}
void dmrAesRxVoice(uint8_t *ambe, int seq)
{
    if (!s_rxActive) { return; }
    if (seq == 0) { dmr_aes_superframe(&s_rx); s_rxOff = 0; }
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
    if (s_txActive) { dmr_aes_superframe(&s_tx); }
}
int dmrAesTxBuildPI(uint8_t *piOut7)   /* build PI header LC to emit at call start / late entry */
{
    if (!s_txActive) { return 0; }
    dmr_pi_build(DMR_ALG_AES256, s_tx.key_id, s_tx.mi, piOut7);
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

#include "crypto/dmr_aes_hook.h"
#ifdef ENABLE_AES
#include "crypto/dmr_aes.h"
#include "functions/codeplug.h"
#include <stddef.h>
#include <string.h>

/* DMR voice burst = 27 bytes (AMBE_AUDIO_LENGTH) = 3x 9-byte AMBE frames. */
#define DMR_AMBE_BURST 27

/* AES key store: OpenGD77 custom-data block type CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS.
 * Fixed-size block (collision-safe, firmware-managed region; no codeplug-offset guessing).
 *   [0..3] magic "AESK"  [4] version(1)  [5..7] reserved
 *   then AESK_SLOTS entries of 36 bytes: [0]=valid(1=used)  [1]=keyId  [2..3]=rsvd  [4..35]=32-byte key
 * The block is always the same length so it can be rewritten in place. */
#define AESK_HDR_LEN   8
#define AESK_ENTRY_LEN 36
#define AESK_SLOTS     DMR_AES_MAX_KEYS
#define AESK_BLOCK_LEN (AESK_HDR_LEN + AESK_SLOTS * AESK_ENTRY_LEN)

static dmr_aes_ctx_t s_rx;
static size_t        s_rxOff;
static int           s_rxActive;
static int           s_keysLoaded;

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
        if (e[0] == 1) { dmr_aes_set_key(e[1], e + 4); }   /* e[1]=keyId, e+4=32-byte key */
    }
}

/* Store/replace one key (by keyId) in the codeplug custom-data block, then reload. */
int dmrAesStoreKey(uint8_t keyId, const uint8_t *key32)
{
    static uint8_t blk[AESK_BLOCK_LEN];
    int slot = -1, freeSlot = -1;
    if (!codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk) || memcmp(blk, "AESK", 4) != 0)
    {
        memset(blk, 0, sizeof(blk));
        memcpy(blk, "AESK", 4);
        blk[4] = 1;
    }
    for (int i = 0; i < AESK_SLOTS; i++)
    {
        uint8_t *e = blk + AESK_HDR_LEN + i * AESK_ENTRY_LEN;
        if ((e[0] == 1) && (e[1] == keyId)) { slot = i; break; }
        if ((freeSlot < 0) && (e[0] == 0))  { freeSlot = i; }
    }
    if (slot < 0) { slot = freeSlot; }
    if (slot < 0) { return 0; }   /* store full */
    uint8_t *e = blk + AESK_HDR_LEN + slot * AESK_ENTRY_LEN;
    e[0] = 1; e[1] = keyId; memcpy(e + 4, key32, 32);
    int ok = codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk, AESK_BLOCK_LEN) ? 1 : 0;
    dmrAesLoadKeys();
    return ok;
}

/* Called on each decoded LC burst; loads keys on first use, acts only on a DMRA PI header. */
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
#endif

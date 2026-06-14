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
static size_t        s_rxBurstBase DMR_AES_CCM;  /* absolute keystream bit offset of the current burst's frame 0 */
static int           s_rxBurstEnc DMR_AES_CCM;   /* 1 if the current burst is an active encrypted stream */

/* Shared scratch for the (non-reentrant, foreground-only) key-store helpers.
 * One buffer instead of three per-function statics — keeps CCM usage down so
 * adding the diag below does not push _eccmram into the codec's CCM scratch. */
static uint8_t       s_aesBlk[AESK_BLOCK_LEN] DMR_AES_CCM;

/* RX diagnostics, read over USB (CPS cmd 0x84) to debug on-air alignment without
 * guess-and-flash: are PIs parsed? what MI? are the voice hooks firing? */
static struct {
    uint32_t lcCrcOk;     /* CRC-valid LCs offered to dmrAesRxPI */
    uint32_t piValid;     /* PIs that parsed as valid DMRA (MFID 0x10, alg<0x26) */
    uint32_t rxInit;      /* successful rx_init (key found, stream armed) */
    uint32_t lastMi;      /* last valid PI's 32-bit MI */
    uint32_t burstCnt;    /* dmrAesRxBurst calls */
    uint32_t codecCnt;    /* dmrAesRxCodecFrame calls */
    uint8_t  lastLc[8];   /* first 8 bytes of the last CRC-valid LC offered */
    uint8_t  rxActive;    /* current s_rxActive */
    uint8_t  lastAlg;     /* last PI alg id byte */
    uint8_t  lastKeyId;   /* last PI key id byte */
    uint8_t  keysLoaded;  /* s_keysLoaded flag */
    uint16_t haveMask;    /* loaded-key slot bitmask (bit i = slot i) */
    uint8_t  key1[4];     /* first 4 bytes of slot-1 key (verify load) */
    uint8_t  pad;
} s_diag DMR_AES_CCM;

/* Ring of distinct (deduped) non-zero LCs offered to dmrAesRxPI during reception —
 * lets us SEE the real DMRA PI header's byte layout (read via CPS cmd 0x85). */
#define LCRING_N 8
#define LCRING_W 12
static uint8_t  s_lcRing[LCRING_N][LCRING_W] DMR_AES_CCM;
static uint8_t  s_lcRingCnt DMR_AES_CCM;
static uint32_t      s_txPiMi DMR_AES_CCM;   /* MI to advertise in the PI header (pre-advance seed) */
static uint8_t       s_txKeyId DMR_AES_CCM;  /* selected TX key (AESK header byte 5); 0 = enc TX off */

/* Zero the AES state at boot. REQUIRED: this state lives in .ccmram, which the
 * startup code does NOT initialize (it only copies .data and zeroes .bss), so at
 * power-on s_keysLoaded / s_rxActive / s_keys etc. are garbage — which leaves keys
 * unloaded and the decrypt stream falsely "active", producing garbled encrypted RX.
 * Standard OpenGD77 .ccmram vars tolerate this (all write-before-read); ours don't. */
void dmrAesInit(void)
{
    memset(&s_rx, 0, sizeof s_rx);
    memset(&s_tx, 0, sizeof s_tx);
    s_rxOff = s_txOff = 0;
    s_rxActive = s_txActive = s_keysLoaded = 0;
    s_rxBurstBase = 0;
    s_rxBurstEnc = 0;
    s_txPiMi = 0;
    s_txKeyId = 0;
    memset(&s_diag, 0, sizeof s_diag);
    memset(s_lcRing, 0, sizeof s_lcRing);
    s_lcRingCnt = 0;
    dmr_aes_clear_keys();   /* zeroes the s_keys/s_have store in dmr_aes.c */
}

/* Load a key straight into RAM (bypasses the flash custom-data store, which needs
 * an initialized "OpenGD77" region that may be absent). Marks keys as loaded so the
 * lazy flash-load in dmrAesRxPI won't clear it. For bench testing / when the store
 * is unavailable. */
void dmrAesSetKeyRam(uint8_t keyId, const uint8_t *key32)
{
    dmr_aes_set_key(keyId, key32);
    s_keysLoaded = 1;
}

void dmrAesLoadKeys(void)
{
    uint8_t *blk = s_aesBlk;
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
    uint8_t *blk = s_aesBlk;
    if (!codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk) || memcmp(blk, "AESK", 4) != 0)
    {
        memset(blk, 0, AESK_BLOCK_LEN); memcpy(blk, "AESK", 4); blk[4] = 1;
    }
    blk[5] = keyId;
    int ok = codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk, AESK_BLOCK_LEN) ? 1 : 0;
    dmrAesLoadKeys();
    return ok;
}

int dmrAesStoreKey(uint8_t keyId, const uint8_t *key32)
{
    uint8_t *blk = s_aesBlk;
    int slot = -1, freeSlot = -1;
    if (!codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk) || memcmp(blk, "AESK", 4) != 0)
    {
        memset(blk, 0, AESK_BLOCK_LEN); memcpy(blk, "AESK", 4); blk[4] = 1;
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

/* ---- RX --------------------------------------------------------------------
 * Validated against DSD-FME (ground truth) on 690 captured frames: the OFB
 * keystream is applied to the 49 DECODED AMBE voice bits (in codecDecode), not
 * the 27 raw FEC octets. dmrAesRxPI seeds the MI once at call start; the IV is
 * regenerated and the MI advanced at each superframe (burst A, seq==1) via the
 * self-sustaining LFSR chain — same as DSD-FME's per-PI LFSR128d advance, so we
 * stay aligned with the over-the-air MI without depending on PI re-parse timing. */
void dmrAesRxPI(const uint8_t *pi, int len)
{
    dmr_pi_t p;
    if (!s_keysLoaded) { dmrAesLoadKeys(); }
    s_diag.lcCrcOk++;
    for (int i = 0; i < 8 && i < len; i++) { s_diag.lastLc[i] = pi[i]; }
    /* capture distinct non-zero LCs (PI candidates) for offline inspection */
    if (len >= LCRING_W)
    {
        int nz = 0; for (int i = 0; i < LCRING_W; i++) { if (pi[i]) { nz = 1; break; } }
        if (nz)
        {
            int dup = 0;
            for (int r = 0; r < s_lcRingCnt; r++) { if (memcmp(s_lcRing[r], pi, LCRING_W) == 0) { dup = 1; break; } }
            if (!dup && s_lcRingCnt < LCRING_N) { memcpy(s_lcRing[s_lcRingCnt++], pi, LCRING_W); }
        }
    }
    if (dmr_pi_parse(pi, (size_t)len, &p) && p.valid)
    {
        s_diag.piValid++;
        s_diag.lastMi = p.mi; s_diag.lastAlg = p.alg_id; s_diag.lastKeyId = p.key_id;
        /* Seed only at call start. The embedded PI may be re-parsed every voice frame;
         * re-seeding mid-call would reset the MI chain and desync the keystream. */
        if (!s_rxActive)
        {
            s_rxActive = (dmr_aes_rx_init(&s_rx, &p) == 0);
            if (s_rxActive) { s_diag.rxInit++; }
            s_rxBurstBase = 0;
            s_rxBurstEnc = 0;
        }
    }
    s_diag.rxActive = (uint8_t)s_rxActive;
}
/* Called per voice burst from the HR-C6000 ISR. seq is the 1..6 burst sequence
 * (A..F); each burst carries 3 AMBE frames, 6 bursts = one 18-frame superframe. */
void dmrAesRxBurst(int seq)
{
    s_diag.burstCnt++;
    if (!s_rxActive) { s_rxBurstEnc = 0; return; }
    s_rxBurstEnc = 1;
    if (seq == 1) { dmr_aes_superframe(&s_rx); }   /* new superframe: IV=f(MI), MI advances */
    s_rxBurstBase = (size_t)((seq >= 1 ? seq - 1 : 0)) * (3 * 56);  /* frame 0 of this burst */
}
/* Called per decoded AMBE frame from codecDecode (idxInBurst 0..2). Applies the
 * OFB keystream to the 49-bit decoded voice vector in place. */
void dmrAesRxCodecFrame(uint16_t *b49, int idxInBurst)
{
    s_diag.codecCnt++;
    if (!s_rxActive || !s_rxBurstEnc) { return; }
    dmr_aes_voice_frame(&s_rx, b49, s_rxBurstBase + (size_t)idxInBurst * 56);
}
void dmrAesRxEnd(void) { s_rxActive = 0; s_rxBurstEnc = 0; s_diag.rxActive = 0; }

/* Copy the RX diag block out for the USB CPS read (cmd 0x84). Returns byte count. */
int dmrAesGetDiag(uint8_t *out)
{
    s_diag.keysLoaded = (uint8_t)s_keysLoaded;
    s_diag.haveMask = (uint16_t)dmr_aes_have_mask();
    dmr_aes_peek_key(1, s_diag.key1);
    memcpy(out, &s_diag, sizeof(s_diag));
    return (int)sizeof(s_diag);
}

/* Copy the captured LC ring (cmd 0x85): [0]=count, then count*12 raw LC bytes. */
int dmrAesGetLcRing(uint8_t *out)
{
    out[0] = s_lcRingCnt;
    memcpy(out + 1, s_lcRing, (size_t)s_lcRingCnt * LCRING_W);
    return 1 + (int)s_lcRingCnt * LCRING_W;
}

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

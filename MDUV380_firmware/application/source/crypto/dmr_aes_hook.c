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
static int           s_rxIvReady DMR_AES_CCM;    /* 0 = (re)generate IV from s_rx.mi on the next burst */
static int           s_rxLastSeq DMR_AES_CCM;    /* previous burst's seq, to detect superframe wrap */
static uint32_t      s_rxNextMi DMR_AES_CCM;     /* LFSR-advanced MI for the next superframe */
static uint32_t      s_rxInitMi DMR_AES_CCM;     /* the current call's CONSTANT initial MI (from its PI) */

/* Shared scratch for the (non-reentrant, foreground-only) key-store helpers. One
 * buffer instead of three per-function statics keeps CCM usage down. */
static uint8_t       s_aesBlk[AESK_BLOCK_LEN] DMR_AES_CCM;
static uint32_t      s_txPiMi DMR_AES_CCM;   /* MI to advertise in the PI header (the call's initial MI) */
static uint8_t       s_txKeyId DMR_AES_CCM;  /* selected TX key (AESK header byte 5); 0 = enc TX off */
static uint32_t      s_txFrameCnt DMR_AES_CCM; /* free-running encoded voice frame index (encode==transmit order) */
static int           s_txIvReady DMR_AES_CCM;  /* 0 = generate IV from s_tx.mi on the next encoded frame */
static uint32_t      s_txNextMi DMR_AES_CCM;   /* LFSR-advanced MI for the next TX superframe */
static uint8_t       s_txFrag[7][3] DMR_AES_CCM; /* Late-Entry MI fragment nibbles for the call (vc 1..6, cw 0..2) */

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
    s_rxIvReady = 0;
    s_rxLastSeq = -1;
    s_rxNextMi = 0;
    s_rxInitMi = 0;
    s_txPiMi = 0;
    s_txKeyId = 0;
    s_txFrameCnt = 0;
    s_txIvReady = 0;
    s_txNextMi = 0;
    memset(s_txFrag, 0, sizeof s_txFrag);
    dmr_aes_clear_keys();   /* zeroes the s_keys/s_have store in dmr_aes.c */
}

/* Load a key straight into RAM (bypasses the flash custom-data store). Marks keys
 * as loaded so the lazy flash-load in dmrAesRxPI won't clear it. For bench use. */
void dmrAesSetKeyRam(uint8_t keyId, const uint8_t *key32)
{
    dmr_aes_set_key(keyId, key32);
    s_keysLoaded = 1;
}

/* Load keys from the OpenGD77 custom-data AES_KEYS block (written by the CPS / the
 * aes_key_store tool). Lazy: called on first PI when keys aren't loaded yet. */
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
 * The OFB keystream is applied to the 49 DECODED AMBE voice bits (in codecDecode),
 * not the 27 raw FEC octets — validated against DSD-FME (ground truth) on 690 frames.
 * The PI carries the call's CONSTANT initial 32-bit MI; the receiver advances it per
 * superframe via the LFSR. So we seed the MI when it changes (a new call) and advance
 * it on each superframe wrap; the per-frame keystream offset is absolute (seq-based). */
void dmrAesRxPI(const uint8_t *pi, int len)
{
    dmr_pi_t p;
    if (!s_keysLoaded) { dmrAesLoadKeys(); }
    if (dmr_pi_parse(pi, (size_t)len, &p) && p.valid)
    {
        /* (Re)seed ONLY when the MI changes — i.e. a genuinely new call (whose RX_END
         * we may have missed). A same-MI re-sent PI (late entry) must be ignored: the
         * MI is constant for the whole call, so re-seeding mid-call would reset the
         * advance back to the initial value and garble the rest of the call. */
        if (!s_rxActive || (p.mi != s_rxInitMi))
        {
            s_rxActive = (dmr_aes_rx_init(&s_rx, &p) == 0);  /* load key for keyId + seed MI */
            if (s_rxActive)
            {
                s_rxInitMi = p.mi;
                s_rxIvReady = 0;     /* generate IV from the seeded MI on the next burst */
                s_rxLastSeq = -1;
            }
        }
    }
}
/* Called per voice burst from the HR-C6000 ISR. seq is the 1..6 burst sequence
 * (A..F); each burst carries 3 AMBE frames, 6 bursts = one 18-frame superframe. */
void dmrAesRxBurst(int seq)
{
    if (!s_rxActive) { s_rxBurstEnc = 0; return; }
    s_rxBurstEnc = 1;
    if (!s_rxIvReady)
    {
        /* First burst after a (re)seed: generate the IV for the CURRENT superframe from
         * s_rx.mi. Works whether we joined at burst A or mid-superframe (the absolute
         * seq-based offset below covers the partial superframe). */
        dmr_lfsr128d(s_rx.mi, s_rx.iv, &s_rxNextMi);
        s_rxIvReady = 1;
    }
    else if (s_rxLastSeq >= 0 && seq < s_rxLastSeq)
    {
        /* seq wrapped (e.g. 6->1) = crossed into a new superframe: advance the MI via the
         * LFSR. Using seq<lastSeq (not seq==1) stays correct even if burst A is dropped. */
        s_rx.mi = s_rxNextMi;
        dmr_lfsr128d(s_rx.mi, s_rx.iv, &s_rxNextMi);
    }
    s_rxLastSeq = seq;
    s_rxBurstBase = (size_t)((seq >= 1 ? seq - 1 : 0)) * (3 * 56);  /* frame 0 of this burst */
}
/* Called per decoded AMBE frame from codecDecode (idxInBurst 0..2). Applies the
 * OFB keystream to the 49-bit decoded voice vector in place. */
void dmrAesRxCodecFrame(uint16_t *b49, int idxInBurst)
{
    if (!s_rxActive || !s_rxBurstEnc) { return; }
    dmr_aes_voice_frame(&s_rx, b49, s_rxBurstBase + (size_t)idxInBurst * 56);
}
void dmrAesRxEnd(void) { s_rxActive = 0; s_rxBurstEnc = 0; }

/* ---- TX (mirror of RX: encrypt the 49 AMBE params at the codec layer) ---- */
void dmrAesTxStart(uint8_t keyId, uint32_t miSeed)
{
    if (!s_keysLoaded) { dmrAesLoadKeys(); }
    s_txActive = (dmr_aes_tx_init(&s_tx, DMR_ALG_AES256, keyId, miSeed) == 0); /* sets s_tx.mi = miSeed */
    s_txOff = 0;
    s_txPiMi = miSeed;     /* the call's CONSTANT initial MI: advertised for late entry, advanced by RX */
    s_txFrameCnt = 0;
    s_txIvReady = 0;
    memset(s_txFrag, 0, sizeof s_txFrag);  /* built per-superframe in dmrAesTxCodecFrame (conveys the NEXT MI) */
}
int dmrAesTxBuildPI(uint8_t *piOut7)   /* build PI header LC to emit at call start / late entry */
{
    if (!s_txActive) { return 0; }
    dmr_pi_build(DMR_ALG_AES256, s_tx.key_id, s_txPiMi, piOut7);
    return 7;
}
/* Encrypt one encoded voice frame's 49 AMBE params in place, during codecEncodeBlock
 * (after AMBE_ENCODE, before AMBE_ENCODE_ECC). The encode order equals the transmit
 * order, so a free-running frame counter gives the superframe position: IV=lfsr(MI),
 * MI advances each 18-frame superframe, offset=(frame%18)*56, silence frames skipped —
 * exactly what the receiver (a stock TYT, or our own RX) expects. */
void dmrAesTxCodecFrame(uint16_t *b49)
{
    if (!s_txActive) { return; }
    if (!s_txIvReady)
    {
        dmr_lfsr128d(s_tx.mi, s_tx.iv, &s_txNextMi);
        /* LOOK-AHEAD conveyance: the receiver assembles the late-entry MI over a whole
         * superframe and applies it (as the IV) to the NEXT superframe — DSD-FME dmr_le.c
         * assembles at vc==6, then LFSR128d packs aes_iv and that drives the next superframe;
         * the HR-C6000 chip path mirrors this. So we encrypt THIS superframe with LFSR(s_tx.mi)
         * but must convey s_txNextMi (the NEXT superframe's MI), so the receiver's IV for the
         * next superframe == our encryption IV for the next superframe. (Conveying s_tx.mi here
         * decodes one superframe early -> garble; confirmed on-air via the constant-plaintext
         * diagnostic = encryption ran one LFSR step ahead of the conveyed MI.) */
        dmr_le_mi_build(s_txNextMi, s_txFrag);
        s_txIvReady = 1;
    }
    else if ((s_txFrameCnt % 18) == 0)
    {
        s_tx.mi = s_txNextMi;
        dmr_lfsr128d(s_tx.mi, s_tx.iv, &s_txNextMi);
        dmr_le_mi_build(s_txNextMi, s_txFrag);   /* convey the NEXT superframe's MI (look-ahead, see above) */
    }
    dmr_aes_voice_frame(&s_tx, b49, (size_t)(s_txFrameCnt % 18) * 56);
    s_txFrameCnt++;
}
/* Stuff the DMRA "Late Entry MI" into the post-ECC AMBE codeword. Called in
 * codecEncodeBlock AFTER AMBE_ENCODE_ECC (bitbuffer_encode now holds the 72-bit
 * codeword) and BEFORE the byte packing. The 4 MI bits overwrite ambe_fr[3][0..3]
 * = OTA bits 71/67/63/59 — plaintext signalling, on top of the encrypted voice —
 * which a stock TYT (and DSD-FME) read to bootstrap the keystream. Uses the same
 * free-running counter dmrAesTxCodecFrame just advanced: f = s_txFrameCnt-1. */
void dmrAesTxStuffMI(uint16_t *b72)
{
    if (!s_txActive) { return; }
    uint32_t f = s_txFrameCnt - 1;          /* the codeword dmrAesTxCodecFrame just processed */
    int cw = (int)(f % 3);                   /* codeword index within the burst (0..2) */
    int vc = (int)((f / 3) % 6) + 1;         /* voice frame within the superframe (1..6) */
    uint8_t nib = s_txFrag[vc][cw];
    b72[71] = (nib >> 3) & 1u;               /* ambe_fr[3][0] */
    b72[67] = (nib >> 2) & 1u;               /* ambe_fr[3][1] */
    b72[63] = (nib >> 1) & 1u;               /* ambe_fr[3][2] */
    b72[59] = (nib >> 0) & 1u;               /* ambe_fr[3][3] */
}
/* The old 27-byte FEC-octet TX crypt is gone — encryption is at the codec layer above. */
void dmrAesTxVoice(uint8_t *ambe, int seq) { (void)ambe; (void)seq; }
int  dmrAesTxActive(void) { return s_txActive; }
void dmrAesTxEnd(void)    { s_txActive = 0; }

/* DMRA "Late Entry Single Block": the burst-F EMB that announces the encryption alg + key,
 * which a stock TYT (and our own RX via the chip's reconstructed PI-LC) reads to auto-engage
 * decryption — without it, only DSD-FME with a forced alg (-M 25) can decode our TX. The chip
 * does NOT BPTC-encode its 0x29/0x2A enc registers in this mode (it emits page-0x02 0x29..0x2C
 * RAW into the burst-F EMB data), so we supply the 4 already-BPTC(16x2)-encoded octets to write
 * to 0x29..0x2C. Payload = (KeyID<<3 | ALG): ALG 5 = AES256 (DSD prints 5+0x20=0x25). The encode
 * (Hamming(16,11,4) systematic + even-parity duplicate + RC interleave) is verified offline vs
 * dsd-fme BPTC_16x2_Extract_Data. Currently KEY ID 1 only (payload 0x00D); 0 if no enc TX. */
int dmrAesTxEmbSb(uint8_t out4[4])
{
    if (!s_txActive) { return 0; }
    static const uint8_t le_sb_keyid1_aes256[4] = { 0x44, 0x42, 0x88, 0x81 }; /* BPTC of 0x00D */
    out4[0] = le_sb_keyid1_aes256[0];
    out4[1] = le_sb_keyid1_aes256[1];
    out4[2] = le_sb_keyid1_aes256[2];
    out4[3] = le_sb_keyid1_aes256[3];
    return 1;
}
#endif

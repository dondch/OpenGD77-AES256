#include "crypto/dmr_aes_hook.h"
#ifdef ENABLE_AES
#include "crypto/dmr_aes.h"
#include "functions/codeplug.h"
#include "hardware/SPI_Flash.h"
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
static uint8_t       s_rxFrag[7][3] DMR_AES_CCM; /* Late-Entry MI fragment nibbles read from the AMBE bits */
static uint8_t       s_rxKeyId DMR_AES_CCM;      /* keyId of the last successful seed (late-entry bootstrap fallback) */
static int           s_rxPiSeeded DMR_AES_CCM;   /* 1 = call seeded from the chip PI-LC (use pure self-advance,
                                                  * stable & RF-independent); 0 = late-entry-bootstrapped rapid call
                                                  * (adopt diverging late entries so a wrong bootstrap self-corrects) */

/* Shared scratch for the (non-reentrant, foreground-only) key-store helpers. One
 * buffer instead of three per-function statics keeps CCM usage down. */
static uint8_t       s_aesBlk[AESK_BLOCK_LEN] DMR_AES_CCM;
static uint32_t      s_txPiMi DMR_AES_CCM;   /* MI to advertise in the PI header (the call's initial MI) */
static uint8_t       s_txKeyId DMR_AES_CCM;  /* selected TX key (AESK header byte 5); 0 = enc TX off */
static uint32_t      s_txFrameCnt DMR_AES_CCM; /* free-running encoded voice frame index (encode==transmit order) */
static int           s_txIvReady DMR_AES_CCM;  /* 0 = generate IV from s_tx.mi on the next encoded frame */
static uint32_t      s_txNextMi DMR_AES_CCM;   /* LFSR-advanced MI for the next TX superframe */
static uint8_t       s_txFrag[7][3] DMR_AES_CCM; /* Late-Entry MI fragment nibbles for the call (vc 1..6, cw 0..2) */

#ifdef DMR_AES_DIAG_PATTERN
/* ---- DIAGNOSTIC (compile-time): inject a known bit pattern as the voice params,
 * UNENCRYPTED, to recover the codec encode->decode param permutation P offline from
 * an over-the-air decode. No on-radio buffer (a CCM buffer collides with the codec).
 * Pattern cycles in 7 blocks of BLK frames: phases 0..5 set b[i]=(i>>phase)&1, phase 6
 * = all-zero marker. Offline: segment by the marker, read P[j] = sum_k(ambe_d_k[j]<<k). */
#define DMR_AES_DIAG_BLK 40
static uint32_t s_diagCtr DMR_AES_CCM;
/* Override b49 with the current pattern; returns 1 (caller then skips encryption). */
int dmrAesDiagPattern(uint16_t *b49)
{
    uint32_t blk = s_diagCtr / DMR_AES_DIAG_BLK;
    int phase = (int)(blk % 7);
    for (int i = 0; i < 49; i++)
    {
        b49[i] = (phase < 6) ? (uint16_t)((i >> phase) & 1) : 0;
    }
    s_diagCtr++;
    return 1;
}
#endif

#ifdef DMR_AES_DIAG_ENCPAT
/* ---- DIAGNOSTIC (compile-time): set a CONSTANT, known, non-silence/non-CCR plaintext
 * as the 49 voice params, which the caller then ENCRYPTS via dmrAesTxCodecFrame. With a
 * constant plaintext the received ciphertext IS the firmware's applied keystream:
 *   enc[i] = const[i] ^ firmware_ks[i]   ->   firmware_ks[i] = enc[i] ^ const[i].
 * Offline we compute our_ks = AES-256-OFB(IV recovered from the conveyed MI) and compare,
 * which pinpoints whether the TX keystream is wrong by key, IV, or offset. const = all-ones
 * (bits 24..43 = 1 -> not CCR; not the silence vector -> not skipped). */
int dmrAesDiagConstPattern(uint16_t *b49)
{
    for (int i = 0; i < 49; i++) { b49[i] = 1; }
    return 1;
}
#endif

#ifdef DMR_AES_DIAG_RX
/* ---- RX state-machine DIAGNOSTIC (compile-time, DMR_AES_DIAG_RX) -------------
 * A small CCM ring of the AES-RX decrypt events, dumped over USB CDC (CPS 0x84)
 * AFTER a call (CPS mode suspends live RX, but the ISR records regardless). Lets
 * the rapid-re-PTT garble be READ OUT instead of guessed at: chip-LC PI seeds (and
 * the MI they carry), superframe wraps (late-entry-decode MI vs the LFSR self-advance
 * fallback), the IV (re)gen at a (re)seed, and the new-call resets — all timestamped.
 * NOT committed: strip with the rest of the DMR_AES_DIAG_* scaffolding before commit. */
#include "functions/ticks.h"
#define RXD_RING 24
typedef struct {            /* 16 bytes */
    uint8_t  type;          /* 1=PI 2=WRAP 3=LCRESET 4=RXEND 5=IVGEN 6=BOOTSTRAP 7=RESEED 8=LATEENTRY */
    uint8_t  flags;         /* see the recording sites */
    uint8_t  seq;           /* burst seq (rxDataType & 7) for WRAP/IVGEN/BOOTSTRAP/RESEED */
    int8_t   lastSeq;       /* s_rxLastSeq before the wrap update */
    uint16_t ts;            /* ticksGetMillis() & 0xFFFF (gap timing across the boundary) */
    uint16_t pad;
    uint32_t mi;            /* PI: parsed p.mi; WRAP/IVGEN/BOOTSTRAP/RESEED: the resulting s_rx.mi */
    uint32_t aux;           /* PI/IVGEN: s_rxInitMi; WRAP/BOOTSTRAP/RESEED: the decoded late-entry MI */
} rxd_evt_t;
static rxd_evt_t s_rxd[RXD_RING] DMR_AES_CCM;
static uint16_t  s_rxdW DMR_AES_CCM;        /* ring write index (free-running) */
static uint16_t  s_rxdCnt[9] DMR_AES_CCM;   /* per-type aggregate counters (survive ring wrap); [8]=LATEENTRY */
static uint16_t  s_rxdLe[2] DMR_AES_CCM;    /* [0]=late-entry decode OK, [1]=fail */
static uint16_t  s_rxdMisc[3] DMR_AES_CCM;  /* [0]=LC reads, [1]=valid PI parses, [2]=RxBurst calls */
static uint32_t  s_rxdLastPiMi DMR_AES_CCM; /* PI dedup: last logged PI MI */
/* Raw-burst capture state (crack the RX late-entry bit layout — see dmrAesGetCapData). */
#define CAP_BURSTS 6
static uint8_t   s_capState DMR_AES_CCM;     /* 0 idle, 1 armed, 2 capturing, 3 done */
static uint8_t   s_capN DMR_AES_CCM;
static uint8_t   s_capBuf[CAP_BURSTS][27] DMR_AES_CCM;
static uint8_t   s_capSeq[CAP_BURSTS] DMR_AES_CCM;
static uint8_t   s_capExp[7][3] DMR_AES_CCM;  /* expected frag (dmr_le_mi_build of the true MI) */
static uint32_t  s_capMi DMR_AES_CCM;         /* the true MI the captured superframe conveys */

static void rxd_log(uint8_t type, uint8_t flags, int seq, int lastSeq, uint32_t mi, uint32_t aux)
{
    rxd_evt_t *e = &s_rxd[s_rxdW % RXD_RING];
    e->type = type; e->flags = flags;
    e->seq = (uint8_t)seq; e->lastSeq = (int8_t)lastSeq;
    e->ts = (uint16_t)(ticksGetMillis() & 0xFFFF); e->pad = 0;
    e->mi = mi; e->aux = aux;
    s_rxdW++;
    if (type < 9) { s_rxdCnt[type]++; }
}
/* Called from HR-C6000.c at the two dmrAesRxEnd sites (LCRESET=3, RXEND=4). */
void dmrAesDiagRxMark(uint8_t type) { rxd_log(type, (uint8_t)(s_rxActive ? 1 : 0), 0, s_rxLastSeq, s_rx.mi, s_rxInitMi); }
/* Called from HR-C6000.c hrc6000SysPostAccessInt: the chip's native Late-Entry interrupt
 * (reg 0x82 bit4 / SYS_INT_POST_ACCESS) fired. seq = slotState at the moment it fired;
 * flags bit0 = gatePassed (OpenGD77's IDLE gate let the handler act), bit1 = s_rxActive
 * (a stale AES decrypt from the previous call was still running). Decides whether the
 * late-entry interrupt is a usable rapid-RX new-call trigger (esp. when slotState != IDLE). */
void dmrAesDiagLateEntry(uint8_t slotState, uint8_t gatePassed)
{
    rxd_log(8 /*LATEENTRY*/, (uint8_t)((gatePassed ? 1 : 0) | (s_rxActive ? 2 : 0)),
            (int)slotState, s_rxLastSeq, s_rx.mi, s_rxInitMi);
}
void dmrAesResetRxDiag(void)
{
    memset(s_rxd, 0, sizeof s_rxd);
    s_rxdW = 0; s_rxdLastPiMi = 0;
    memset(s_rxdCnt, 0, sizeof s_rxdCnt);
    memset(s_rxdLe, 0, sizeof s_rxdLe);
    memset(s_rxdMisc, 0, sizeof s_rxdMisc);
    s_capState = 0; s_capN = 0;
}
/* Copy a self-describing snapshot into out (<= max). Header (32 bytes, LE):
 *  [0]=0xA5 magic, [1]=ring size, [2]=record size, [3]=0, [4..5]=write index,
 *  [6..19]=cnt[1..7] (u16 each), [20..21]=leOk, [22..23]=leFail, [24..25]=LC reads,
 *  [26..27]=valid PI parses, [28..29]=RxBurst calls, [30..31]=LATEENTRY count, then the
 *  raw ring (RXD_RING * 16 bytes, ring order; the host reorders chronologically). */
int dmrAesGetRxDiag(uint8_t *out, int max)
{
    int hdr = 32, body = (int)sizeof s_rxd, n = hdr + body;
    if (n > max) { return 0; }
    memset(out, 0, hdr);
    out[0] = 0xA5; out[1] = RXD_RING; out[2] = (uint8_t)sizeof(rxd_evt_t);
    out[4] = (uint8_t)(s_rxdW & 0xFF); out[5] = (uint8_t)((s_rxdW >> 8) & 0xFF);
    for (int i = 1; i <= 7; i++) { out[6 + (i - 1) * 2] = (uint8_t)(s_rxdCnt[i] & 0xFF); out[7 + (i - 1) * 2] = (uint8_t)((s_rxdCnt[i] >> 8) & 0xFF); }
    out[20] = (uint8_t)(s_rxdLe[0] & 0xFF); out[21] = (uint8_t)((s_rxdLe[0] >> 8) & 0xFF);
    out[22] = (uint8_t)(s_rxdLe[1] & 0xFF); out[23] = (uint8_t)((s_rxdLe[1] >> 8) & 0xFF);
    for (int i = 0; i < 3; i++) { out[24 + i * 2] = (uint8_t)(s_rxdMisc[i] & 0xFF); out[25 + i * 2] = (uint8_t)((s_rxdMisc[i] >> 8) & 0xFF); }
    out[30] = (uint8_t)(s_rxdCnt[8] & 0xFF); out[31] = (uint8_t)((s_rxdCnt[8] >> 8) & 0xFF); /* LATEENTRY count */
    memcpy(out + hdr, s_rxd, body);
    return n;
}

/* ---- Raw-burst capture (crack the RX late-entry bit layout) ------------------
 * cw=0 of each burst decodes correctly but cw=1/cw=2 do not, so the chip's 27-byte
 * AMBE buffer is NOT three plain 9-byte MSB-first codewords. Capture one clean
 * superframe's six raw bursts + the MI its late-entry should encode (the LFSR
 * self-advance truth on a clear call) + the firmware-built expected fragment, and
 * solve the actual bit mapping offline. */
void dmrAesDiagCapArm(void) { s_capState = 1; s_capN = 0; }
int dmrAesGetCapData(uint8_t *out, int max)
{
    int o = 0, v, c, i;
    int n = 2 + 4 + CAP_BURSTS + 18 + CAP_BURSTS * 27;
    if (n > max) { return 0; }
    out[o++] = s_capState; out[o++] = s_capN;
    out[o++] = (uint8_t)s_capMi; out[o++] = (uint8_t)(s_capMi >> 8);
    out[o++] = (uint8_t)(s_capMi >> 16); out[o++] = (uint8_t)(s_capMi >> 24);
    for (i = 0; i < CAP_BURSTS; i++) { out[o++] = s_capSeq[i]; }
    for (v = 1; v <= 6; v++) { for (c = 0; c < 3; c++) { out[o++] = s_capExp[v][c]; } }
    for (i = 0; i < CAP_BURSTS; i++) { memcpy(out + o, s_capBuf[i], 27); o += 27; }
    return o;
}
#endif

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
    memset(s_rxFrag, 0, sizeof s_rxFrag);
    s_rxKeyId = 0;
    s_rxPiSeeded = 0;
    s_txPiMi = 0;
    s_txKeyId = 0;
    s_txFrameCnt = 0;
    s_txIvReady = 0;
    s_txNextMi = 0;
    memset(s_txFrag, 0, sizeof s_txFrag);
#ifdef DMR_AES_DIAG_PATTERN
    s_diagCtr = 0;
#endif
#ifdef DMR_AES_DIAG_RX
    dmrAesResetRxDiag();
#endif
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

/* Ensure the OpenGD77 custom-data region carries its "OpenGD77" magic, so the
 * codeplugSet/GetOpenGD77CustomData block chain works. On a radio whose region
 * was never initialized it reads all-0xFF (no magic) and dmrAesStoreKey/SetTxKeyId
 * silently fail (the host aes_key_store.py creates the magic; the on-radio menu
 * must be able to do it too). Writing the 12-byte magic leaves offset 12 onward
 * as a CODEPLUG_CUSTOM_DATA_TYPE_EMPTY (0xFFFFFFFF) block, which the append path
 * then fills. SPI_Flash_write does a read-modify-write of the whole 4 KB sector,
 * so the rest of the region is preserved. MUST run in the UI/main-loop task
 * (SPI_Flash_write uses osDelay) — never from the CPS critical section. */
int dmrAesEnsureCustomDataRegion(void)
{
    uint8_t hdr[12];
    SPI_Flash_read(FLASH_ADDRESS_OFFSET + 0, hdr, 12);
    if (memcmp(hdr, "OpenGD77", 8) == 0) { return 1; }   /* already initialized */
    memcpy(hdr, "OpenGD77", 8);
    hdr[8] = hdr[9] = hdr[10] = hdr[11] = 0xFF;
    return SPI_Flash_write(FLASH_ADDRESS_OFFSET + 0, hdr, 12) ? 1 : 0;
}

/* Clear the stored key(s) for keyId. Returns 1 on success (incl. nothing to do). */
int dmrAesClearKey(uint8_t keyId)
{
    uint8_t *blk = s_aesBlk;
    int changed = 0;
    if (!codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk) || memcmp(blk, "AESK", 4) != 0)
    {
        return 1;   /* no block -> nothing stored for any keyId */
    }
    for (int i = 0; i < AESK_SLOTS; i++)
    {
        uint8_t *e = blk + AESK_HDR_LEN + i * AESK_ENTRY_LEN;
        if ((e[0] == 1) && (e[1] == keyId)) { memset(e, 0, AESK_ENTRY_LEN); changed = 1; }
    }
    if (!changed) { return 1; }
    int ok = codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk, AESK_BLOCK_LEN) ? 1 : 0;
    dmrAesLoadKeys();
    return ok;
}

/* Bitmask of stored key ids (bit k set => keyId k has a key), for the menu's
 * set/empty display. Never returns key material. keyId 0 is the "off" sentinel. */
uint16_t dmrAesGetKeyMask(void)
{
    uint8_t *blk = s_aesBlk;
    uint16_t mask = 0;
    if (!codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk) || memcmp(blk, "AESK", 4) != 0)
    {
        return 0;
    }
    for (int i = 0; i < AESK_SLOTS; i++)
    {
        uint8_t *e = blk + AESK_HDR_LEN + i * AESK_ENTRY_LEN;
        if ((e[0] == 1) && (e[1] < 16)) { mask |= (uint16_t)(1u << e[1]); }
    }
    return mask;
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
#ifdef DMR_AES_DIAG_RX
    s_rxdMisc[0]++;   /* every CRC-valid LC handed to dmrAesRxPI (a seed/parse opportunity) */
#endif
    if (dmr_pi_parse(pi, (size_t)len, &p) && p.valid)
    {
        /* Seed only when NOT already active. The per-superframe MI is now driven by the
         * Late-Entry MI read directly from the AMBE bits (dmrAesRxBurst), which tracks the
         * transmitter and jumps immediately on a new call. We must NOT reseed on a chip-LC
         * MI change mid-call: that resets s_rxLastSeq and kills the late-entry wrap-resync,
         * and on a rapid call the HR-C6000 re-feeds the previous call's (laggy) LC. A new
         * call is re-detected via the Voice LC Header reset (dmrAesRxEnd) + the late entry. */
#ifdef DMR_AES_DIAG_RX
        int wasActive = s_rxActive;
        int seeded = 0;
#endif
        if (!s_rxActive)
        {
            s_rxActive = (dmr_aes_rx_init(&s_rx, &p) == 0);  /* load key for keyId + seed MI */
            if (s_rxActive)
            {
                s_rxInitMi = p.mi;
                s_rxKeyId = p.key_id;  /* remember for late-entry bootstrap of a later rapid call */
                s_rxPiSeeded = 1;      /* chip PI-LC seed: a normal call -> pure self-advance, stable */
                s_rxIvReady = 0;     /* generate IV from the seeded MI on the next burst */
                s_rxLastSeq = -1;
#ifdef DMR_AES_DIAG_RX
                seeded = 1;
#endif
            }
        }
#ifdef DMR_AES_DIAG_RX
        s_rxdMisc[1]++;   /* valid PI parses (so LCreads - this = non-PI LCs the chip fed) */
        /* Log a PI whenever it (re)seeded, whenever it was seen while IDLE (a seed
         * opportunity — the key rapid-re-PTT signal), or when its MI changed. The chip
         * re-surfaces the same PI-LC every burst mid-call, so the MI-change filter alone
         * suppresses those without hiding the new-call transitions. */
        if (seeded || !wasActive || (p.mi != s_rxdLastPiMi))
        {
            rxd_log(1, (uint8_t)(0x01 | (seeded ? 0x02 : 0) | (wasActive ? 0x04 : 0)),
                    0, s_rxLastSeq, p.mi, s_rxInitMi);
            s_rxdLastPiMi = p.mi;
        }
#endif
    }
}
/* Called per voice burst from the HR-C6000 ISR. seq is the 1..6 burst sequence
 * (A..F); each burst carries 3 AMBE frames, 6 bursts = one 18-frame superframe. */
void dmrAesRxBurst(int seq)
{
#ifdef DMR_AES_DIAG_RX
    s_rxdMisc[2]++;   /* every RxBurst call, regardless of decrypt state */
#endif
    int wrapped = (s_rxLastSeq >= 0 && seq < s_rxLastSeq);

    if (s_rxActive && !s_rxIvReady)
    {
        /* First burst after a PI (re)seed: generate the IV for the CURRENT superframe from
         * s_rx.mi. Works whether we joined at burst A or mid-superframe (the absolute
         * seq-based offset below covers the partial superframe). */
        dmr_lfsr128d(s_rx.mi, s_rx.iv, &s_rxNextMi);
        s_rxIvReady = 1;
#ifdef DMR_AES_DIAG_RX
        rxd_log(5, 0, seq, s_rxLastSeq, s_rx.mi, s_rxInitMi);   /* IVGEN: first IV of this (re)seed */
#endif
    }
    else if (wrapped)
    {
        /* Superframe boundary. Decode the Late-Entry MI assembled from the AMBE bits of the
         * just-completed superframe (Golay(24,12)+CRC4 over ambe_fr[3][0..3], like dsd-fme).
         * It conveys the NEXT superframe's MI (look-ahead) == the MI we are entering now, and
         * == the LFSR self-advance prediction (s_rxNextMi) on a continuing call. ADOPT a
         * crc-valid late entry that DIVERGES from the prediction IMMEDIATELY (single-trust,
         * like dsd-fme / a stock TYT): that is a genuinely new MI stream — a rapid re-PTT call
         * the chip never surfaced a PI-LC for, or a mid-call resync. A wrong adopt (rare crc4
         * fluke) self-corrects on the very next crc-valid late entry, so there is NO multi-
         * superframe "confirm" wait (the previous design's wait is what stalled rapid calls,
         * and it chained the OLD call's stale late entry into a wrong bootstrap). When the late
         * entry agrees with the prediction or fails to decode, keep the proven self-advance —
         * so PI-seeded normal calls are unaffected. The prediction advances every superframe
         * even while idle, so a new call's MI stands out the instant its late entry decodes. */
#ifdef DMR_AES_DIAG_RX
        /* Raw-burst capture state machine: arm -> (next wrap, if active) start capturing this
         * superframe -> (following wrap) the now-complete superframe's late entry encodes the
         * pre-wrap s_rxNextMi (look-ahead), so snapshot it + the expected frag. */
        if (s_capState == 1 && s_rxActive) { s_capState = 2; s_capN = 0; }
        else if (s_capState == 2 && s_capN >= CAP_BURSTS) { s_capMi = s_rxNextMi; dmr_le_mi_build(s_capMi, s_capExp); s_capState = 3; }
#endif
        uint32_t leMi;
        int leok = dmr_le_mi_decode(s_rxFrag, &leMi);
        int diverge = (leok && (leMi != s_rxNextMi));
        uint32_t mi = diverge ? leMi : s_rxNextMi;
#ifdef DMR_AES_DIAG_RX
        s_rxdLe[leok ? 0 : 1]++;
#endif
        if (!s_rxActive)
        {
            /* BOOTSTRAP a rapid call: only a DIVERGING late entry marks a genuinely new call,
             * not the previous call's residual stream the chip may still be feeding. Reuse the
             * last call's keyId (rapid calls share the channel/key); fall back to any loaded key. */
            int act = 0;
            if (diverge)
            {
                dmr_pi_t p;
                p.alg_id = DMR_ALG_AES256; p.mfid = DMR_MFID_DMRA;
                p.key_id = s_rxKeyId; p.mi = leMi; p.valid = 1;
                act = (dmr_aes_rx_init(&s_rx, &p) == 0);   /* sets s_rx.mi = leMi + loads key */
                if (!act) { int fk = dmr_aes_first_keyid(); if (fk >= 0) { p.key_id = (uint8_t)fk; act = (dmr_aes_rx_init(&s_rx, &p) == 0); } }
                if (act) { s_rxActive = 1; s_rxInitMi = leMi; s_rxKeyId = s_rx.key_id; s_rxPiSeeded = 0; }
            }
            if (!act) { s_rx.mi = mi; }   /* rx_init already set s_rx.mi = leMi when act; else keep predicting */
            dmr_lfsr128d(s_rx.mi, s_rx.iv, &s_rxNextMi);  /* IV for the entering superframe (used once active) */
            s_rxIvReady = 1;
#ifdef DMR_AES_DIAG_RX
            rxd_log(6, (uint8_t)((leok ? 0x01 : 0) | (diverge ? 0x08 : 0) | (act ? 0x10 : 0)),
                    seq, s_rxLastSeq, s_rx.mi, leMi);
#endif
        }
        else
        {
            /* PI-seeded normal calls use the PROVEN, RF-independent self-advance and IGNORE the
             * late entry entirely (its FEC-unprotected bits are noisy; a rare crc4 false-accept
             * must never corrupt a clean call). Only a late-entry-BOOTSTRAPPED rapid call adopts a
             * diverging late entry, so a wrong bootstrap self-corrects on the next valid one. */
            int adopt = (diverge && !s_rxPiSeeded);
            if (adopt) { s_rxInitMi = leMi; }
            s_rx.mi = adopt ? leMi : s_rxNextMi;
            dmr_lfsr128d(s_rx.mi, s_rx.iv, &s_rxNextMi);
#ifdef DMR_AES_DIAG_RX
            rxd_log(adopt ? 7 : 2, (uint8_t)((leok ? 0x01 : 0) | (diverge ? 0x08 : 0)), seq, s_rxLastSeq, s_rx.mi, leMi);
#endif
        }
    }

    if (wrapped || s_rxLastSeq < 0) { memset(s_rxFrag, 0, sizeof s_rxFrag); } /* new superframe */
    s_rxLastSeq = seq;
    s_rxBurstEnc = s_rxActive ? 1 : 0;
    s_rxBurstBase = (size_t)((seq >= 1 ? seq - 1 : 0)) * (3 * 56);  /* frame 0 of this burst */
}
/* Read the Late-Entry MI nibbles from one received voice burst's 3 AMBE codewords and
 * store them for the superframe MI decode. Called per burst (seq 1..6) from the ISR with
 * the 27-byte AMBE (3 x 9-byte OTA codewords). The 4 MI bits are OTA bits 71/67/63/59 of
 * each codeword (= ambe_fr[3][0..3]) — the same bits dmrAesTxStuffMI writes on TX. Collected
 * for EVERY burst (not gated on an active decrypt) so a rapid new call can be bootstrapped. */
void dmrAesRxLateEntry(int seq, const uint8_t *ambe27)
{
    if (seq < 1 || seq > 6) { return; }
    for (int cw = 0; cw < 3; cw++)
    {
        const uint8_t *c = ambe27 + cw * 9;        /* one 9-byte (72-bit) codeword */
        uint8_t nib = (uint8_t)(((c[8] & 0x01) << 3) |   /* OTA bit 71 */
                                ((c[8] & 0x10) ? 0x04 : 0) |  /* OTA bit 67 */
                                ((c[7] & 0x01) << 1) |        /* OTA bit 63 */
                                ((c[7] & 0x10) ? 0x01 : 0));  /* OTA bit 59 */
        s_rxFrag[seq][cw] = nib;
    }
#ifdef DMR_AES_DIAG_RX
    if (s_capState == 2 && s_capN < CAP_BURSTS) { memcpy(s_capBuf[s_capN], ambe27, 27); s_capSeq[s_capN] = (uint8_t)seq; s_capN++; }
#endif
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
 * RAW into the burst-F EMB data), so we supply the 4 already-BPTC(16x2)-encoded octets via
 * dmr_emb_sb_build (key_id 1 / AES256 -> 44 42 88 81). Returns 0 if no encrypted TX is active. */
int dmrAesTxEmbSb(uint8_t out4[4])
{
    if (!s_txActive) { return 0; }
    dmr_emb_sb_build(s_tx.key_id, (uint8_t)(s_tx.alg_id & 0x07), out4);
    return 1;
}
#endif

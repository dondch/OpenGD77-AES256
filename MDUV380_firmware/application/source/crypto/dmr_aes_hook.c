#include "crypto/dmr_aes_hook.h"
#include "crypto/dmr_aes.h"
#include "functions/codeplug.h"
#include "hardware/SPI_Flash.h"
#include <stddef.h>
#include <string.h>

#define DMR_AMBE_BURST 27

/* AES key store.
 *
 * Key MATERIAL lives in the STOCK TYT MD-UV390 AES key table in SPI flash, so keys
 * survive a fork<->stock firmware swap on the same radio and the regular CHIRP
 * Read/Write carries them (no separate key path). The table + wrap were reverse-
 * engineered from the stock firmware and verified byte-exact against a real KEY1
 * capture (see the stock-tyt-aes-key-storage note).
 *   - table base 0xD9F9C, 100-byte entries, entry keyId = base + 0x64*keyId;
 *   - entry = [type:4][name UTF-16LE:32][wrapped key:32][pad:32];
 *   - type 0x04 (or 0x07) = AES-256; keyId 0 slot is unused; fork uses keyId 1..15.
 *   - WRAP a 32-byte key to store it: byte-reverse it, then AES-256-ECB-ENCRYPT the
 *     two 16-byte halves under keyA (bytes 0..15) and keyB (bytes 16..31). READ =
 *     inverse (ECB-DECRYPT halves under keyA/keyB, then un-reverse).
 *   - a stock "unset" slot carries type 0x04 + default name + the wrapped BLANK key
 *     (unwraps to 00..0001), which we treat as empty.
 *
 * The global TX-key selector (a fork-only setting with no stock equivalent) stays in
 * the fork's OpenGD77 custom-data AESK header (below); its block size is unchanged. */
#define AESK_HDR_LEN   8
#define AESK_ENTRY_LEN 36
#define AESK_SLOTS     DMR_AES_MAX_KEYS
#define AESK_BLOCK_LEN (AESK_HDR_LEN + AESK_SLOTS * AESK_ENTRY_LEN)

/* Stock key-table geometry (raw SPI-flash addresses; SPI_Flash_read/write take them
 * verbatim, and SPI_Flash_write does a full sector read-modify-erase-write so a
 * 100-byte entry write preserves the rest of the table). keyId 1..15 all fall inside
 * the single 4 KB sector at 0xDA000, so no entry write ever crosses a sector. */
#define STOCK_KEY_TABLE_BASE   0xD9F9Cu
#define STOCK_KEY_ENTRY_LEN    0x64        /* 100 bytes */
#define STOCK_KEY_TYPE_OFF     0
#define STOCK_KEY_NAME_OFF     4           /* UTF-16LE, 32 bytes */
#define STOCK_KEY_KEY_OFF      36          /* wrapped 32-byte key */
#define STOCK_KEY_TYPE_AES256  0x04
#define STOCK_KEY_TYPE_AES256B 0x07        /* also AES-256 per the RE */
#define STOCK_KEY_MIN_ID       1
#define STOCK_KEY_MAX_ID       15          /* fork menu exposes keyId 1..15 */
#define stockKeyEntryAddr(id)  (STOCK_KEY_TABLE_BASE + STOCK_KEY_ENTRY_LEN * (uint32_t)(id))

/* Fixed GLOBAL wrap constants (identical on every radio; NOT device/UID-derived).
 * These are const -> .rodata (flash); do NOT place them in .aes_ccmram, which the
 * startup code does not initialize (see dmrAesInit) so their initializers would be
 * lost. Only write-before-read RAM state belongs in CCM. */
static const uint8_t s_stockKeyA[32] = {
    0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef, 0x23,0x45,0x67,0x89,0xab,0xcd,0xef,0x01,
    0x45,0x67,0x89,0xab,0xcd,0xef,0x01,0x23, 0x67,0x89,0xab,0xcd,0xef,0x01,0x23,0x45 };
static const uint8_t s_stockKeyB[32] = {
    0x45,0x67,0x89,0xab,0xcd,0xef,0x01,0x23, 0x67,0x89,0xab,0xcd,0xef,0x01,0x23,0x45,
    0x15,0x32,0x3a,0x3c,0x3f,0x48,0x60,0x62, 0x65,0x66,0x7f,0x06,0x07,0x08,0x09,0x0a };
/* Stock's "unset slot" key (a wrapped copy of this is what stock writes for a blank
 * slot; it unwraps to this). We both write it on clear and treat it as empty on read. */
static const uint8_t s_stockBlankKey[32] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0x01 };

static dmr_aes_ctx_t s_rx DMR_AES_CCM, s_tx DMR_AES_CCM;
static size_t        s_rxOff DMR_AES_CCM, s_txOff DMR_AES_CCM;
static int           s_rxActive DMR_AES_CCM, s_txActive DMR_AES_CCM, s_keysLoaded DMR_AES_CCM;
static size_t        s_rxBurstBase DMR_AES_CCM;  /* absolute keystream bit offset of the current burst's frame 0 */
static int           s_rxBurstEnc DMR_AES_CCM;   /* 1 if the current burst is an active encrypted stream */
static int           s_rxIvReady DMR_AES_CCM;    /* 0 = (re)generate IV from s_rx.mi on the next burst */
static int           s_rxLastSeq DMR_AES_CCM;    /* previous burst's seq, to detect superframe wrap */
static uint32_t      s_rxNextMi DMR_AES_CCM;     /* LFSR-advanced MI for the next superframe */
static int           s_rxPredReal DMR_AES_CCM;   /* 1 = s_rxNextMi descends from a real MI (a PI seed or a
                                                  * crc-valid late entry), not from the zeroed boot state */
static uint32_t      s_rxInitMi DMR_AES_CCM;     /* the current call's CONSTANT initial MI (from its PI) */
static uint8_t       s_rxFrag[7][3] DMR_AES_CCM; /* Late-Entry MI fragment nibbles read from the AMBE bits */
static uint8_t       s_rxKeyId DMR_AES_CCM;      /* keyId of the last successful seed (late-entry bootstrap fallback) */
static uint8_t       s_rxEmbKey DMR_AES_CCM;     /* AES-256 key id the current stream announces in its burst-F EMB
                                                  * Late Entry Single Block (dmrAesRxEmbSb), 0 = none received yet;
                                                  * tried first by the late-entry bootstrap; cleared at every call
                                                  * boundary (dmrAesRxEnd) so it never seeds the next call */
static int           s_rxPiSeeded DMR_AES_CCM;   /* 1 = call seeded from the chip PI-LC (use pure self-advance,
                                                  * stable & RF-independent); 0 = late-entry-bootstrapped rapid call
                                                  * (adopt diverging late entries so a wrong bootstrap self-corrects) */
static uint32_t      s_rxLastPiMi DMR_AES_CCM;   /* MI of the last valid PI the chip handed us (any state) */
static int           s_rxLastPiValid DMR_AES_CCM;
static uint32_t      s_rxStaleMi DMR_AES_CCM;    /* MI of the PI that belongs to the stream that just ENDED (our own
                                                  * TX included): the chip's LC page still holds it and re-surfaces it
                                                  * on the first bursts of the next call, which must not seed on it */
static int           s_rxStaleValid DMR_AES_CCM;
static int           s_rxLcSeen DMR_AES_CCM;      /* a CRC-valid voice LC (group/private call) was handed to us since
                                                  * the last call boundary: the chip's embedded-LC page of THIS stream */
static int           s_rxLcPrivacy DMR_AES_CCM;   /* ...and its service options carry the Privacy bit: the transmitter
                                                  * says the call is encrypted (a stock radio sets it on every encrypted
                                                  * call, and never on a clear one) */

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
static uint16_t  s_rxdFrames[3] DMR_AES_CCM;  /* decoded frames while decrypting: [0] left alone as
                                               * silence, [1] left alone as comfort noise, [2] XORed */
static uint8_t   s_rxdEmbN DMR_AES_CCM;       /* EMBSB events logged this call (the undecodable ones are capped) */
static uint8_t   s_rxdSync0N DMR_AES_CCM;     /* SYNC0 events logged this call (capped) */
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
/* Called from HR-C6000.c hrc6000SysReceivedDataInt for every received-data interrupt whose
 * reg 0x51 SyncClass is 0 ("Sync Header"; the chip's comment says the burst-F EMB parsing
 * completion is reported this way). seq = the raw reg 0x51, so the bench can see when, and
 * with which data type, such interrupts come. Capped per call so they cannot flood the ring. */
void dmrAesDiagSyncClass0(uint8_t reg51)
{
    if (s_rxdSync0N >= 4) { return; }
    s_rxdSync0N++;
    rxd_log(12 /*SYNC0*/, (uint8_t)(s_rxActive ? 1 : 0), (int)reg51, s_rxLastSeq, s_rx.mi, s_rxInitMi);
}
void dmrAesResetRxDiag(void)
{
    memset(s_rxd, 0, sizeof s_rxd);
    s_rxdW = 0; s_rxdLastPiMi = 0;
    s_rxdEmbN = s_rxdSync0N = 0;
    memset(s_rxdCnt, 0, sizeof s_rxdCnt);
    memset(s_rxdLe, 0, sizeof s_rxdLe);
    memset(s_rxdMisc, 0, sizeof s_rxdMisc);
    memset(s_rxdFrames, 0, sizeof s_rxdFrames);
    s_capState = 0; s_capN = 0;
}
/* Copy a self-describing snapshot into out (<= max). Header (40 bytes, LE):
 *  [0]=0xA5 magic, [1]=ring size, [2]=record size, [3]=header length, [4..5]=write index,
 *  [6..19]=cnt[1..7] (u16 each), [20..21]=leOk, [22..23]=leFail, [24..25]=LC reads,
 *  [26..27]=valid PI parses, [28..29]=RxBurst calls, [30..31]=LATEENTRY count, then the
 *  [32..37]=frames (silence-skipped, ccr-skipped, XORed; u16 each), then the
 *  raw ring (RXD_RING * 16 bytes, ring order; the host reorders chronologically). */
int dmrAesGetRxDiag(uint8_t *out, int max)
{
    int hdr = 40, body = (int)sizeof s_rxd, n = hdr + body;
    if (n > max) { return 0; }
    memset(out, 0, hdr);
    out[0] = 0xA5; out[1] = RXD_RING; out[2] = (uint8_t)sizeof(rxd_evt_t); out[3] = (uint8_t)hdr;
    out[4] = (uint8_t)(s_rxdW & 0xFF); out[5] = (uint8_t)((s_rxdW >> 8) & 0xFF);
    for (int i = 1; i <= 7; i++) { out[6 + (i - 1) * 2] = (uint8_t)(s_rxdCnt[i] & 0xFF); out[7 + (i - 1) * 2] = (uint8_t)((s_rxdCnt[i] >> 8) & 0xFF); }
    out[20] = (uint8_t)(s_rxdLe[0] & 0xFF); out[21] = (uint8_t)((s_rxdLe[0] >> 8) & 0xFF);
    out[22] = (uint8_t)(s_rxdLe[1] & 0xFF); out[23] = (uint8_t)((s_rxdLe[1] >> 8) & 0xFF);
    for (int i = 0; i < 3; i++) { out[24 + i * 2] = (uint8_t)(s_rxdMisc[i] & 0xFF); out[25 + i * 2] = (uint8_t)((s_rxdMisc[i] >> 8) & 0xFF); }
    out[30] = (uint8_t)(s_rxdCnt[8] & 0xFF); out[31] = (uint8_t)((s_rxdCnt[8] >> 8) & 0xFF); /* LATEENTRY count */
    for (int i = 0; i < 3; i++) { out[32 + i * 2] = (uint8_t)(s_rxdFrames[i] & 0xFF); out[33 + i * 2] = (uint8_t)((s_rxdFrames[i] >> 8) & 0xFF); }
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
    s_rxPredReal = 0;
    s_rxInitMi = 0;
    memset(s_rxFrag, 0, sizeof s_rxFrag);
    s_rxKeyId = 0;
    s_rxEmbKey = 0;
    s_rxPiSeeded = 0;
    s_rxLastPiMi = s_rxStaleMi = 0;
    s_rxLastPiValid = s_rxStaleValid = 0;
    s_rxLcSeen = s_rxLcPrivacy = 0;
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

/* ---- stock key-table wrap + entry access (foreground/main-loop only) --------
 * All of these run in the UI/main-loop or CPS thread context (never an ISR): they
 * touch SPI flash (SPI_Flash_write uses osDelay) and reuse the shared s_aesBlk
 * scratch, matching the existing non-reentrant key-store helpers. */

/* Byte-reverse key32 then AES-256-ECB-ENCRYPT the two halves under keyA/keyB. */
static void stockWrapKey(const uint8_t *key32, uint8_t *wrapped32)
{
    for (int i = 0; i < 32; i++) { wrapped32[i] = key32[31 - i]; }
    aes256_ecb_encrypt(s_stockKeyA, wrapped32);
    aes256_ecb_encrypt(s_stockKeyB, wrapped32 + 16);
}

/* Inverse of stockWrapKey: ECB-DECRYPT the halves under keyA/keyB, then un-reverse. */
static void stockUnwrapKey(const uint8_t *wrapped32, uint8_t *key32)
{
    uint8_t tmp[32];
    memcpy(tmp, wrapped32, 16);
    memcpy(tmp + 16, wrapped32 + 16, 16);
    aes256_ecb_decrypt(s_stockKeyA, tmp);
    aes256_ecb_decrypt(s_stockKeyB, tmp + 16);
    for (int i = 0; i < 32; i++) { key32[i] = tmp[31 - i]; }
}

/* A key is "present" (a real, usable key) unless it is all-zero or the stock BLANK
 * sentinel — both of which a stock radio leaves in an unset slot. */
static int stockKeyIsPresent(const uint8_t *key32)
{
    static const uint8_t zero[32] = { 0 };
    if (memcmp(key32, zero, 32) == 0) { return 0; }
    if (memcmp(key32, s_stockBlankKey, 32) == 0) { return 0; }
    return 1;
}

/* Default UTF-16LE slot name = decimal keyId ("1".."15"), matching stock defaults. */
static void stockDefaultName(uint8_t keyId, uint8_t *name32)
{
    char dec[4];
    int n = 0;
    if (keyId >= 10) { dec[n++] = (char)('0' + keyId / 10); }
    dec[n++] = (char)('0' + keyId % 10);
    memset(name32, 0, 32);
    for (int i = 0; i < n; i++) { name32[i * 2] = (uint8_t)dec[i]; name32[i * 2 + 1] = 0; }
}

/* Read + unwrap one stock table entry into keyOut[32]. Returns 1 iff the slot holds
 * a present (non-blank) AES-256 key. Uses s_aesBlk as the entry scratch. */
static int stockKeyRead(uint8_t keyId, uint8_t *keyOut)
{
    uint8_t *e = s_aesBlk;
    if (!SPI_Flash_read(stockKeyEntryAddr(keyId), e, STOCK_KEY_ENTRY_LEN)) { return 0; }
    if ((e[STOCK_KEY_TYPE_OFF] != STOCK_KEY_TYPE_AES256) &&
        (e[STOCK_KEY_TYPE_OFF] != STOCK_KEY_TYPE_AES256B)) { return 0; }
    stockUnwrapKey(e + STOCK_KEY_KEY_OFF, keyOut);
    return stockKeyIsPresent(keyOut);
}

/* Write one stock table entry: type = AES-256, name (preserved if the slot already
 * had one, else default), the wrapped key, pad = 0. Returns 1 on success. */
static int stockKeyWrite(uint8_t keyId, const uint8_t *key32)
{
    uint8_t *e = s_aesBlk;
    uint8_t  name[32];
    int      haveName;
    if (!SPI_Flash_read(stockKeyEntryAddr(keyId), e, STOCK_KEY_ENTRY_LEN)) { return 0; }
    haveName = ((e[STOCK_KEY_TYPE_OFF] == STOCK_KEY_TYPE_AES256) ||
                (e[STOCK_KEY_TYPE_OFF] == STOCK_KEY_TYPE_AES256B)) &&
               (e[STOCK_KEY_NAME_OFF] != 0x00) && (e[STOCK_KEY_NAME_OFF] != 0xFF);
    memcpy(name, e + STOCK_KEY_NAME_OFF, 32);
    memset(e, 0, STOCK_KEY_ENTRY_LEN);
    e[STOCK_KEY_TYPE_OFF] = STOCK_KEY_TYPE_AES256;
    if (haveName) { memcpy(e + STOCK_KEY_NAME_OFF, name, 32); }
    else          { stockDefaultName(keyId, e + STOCK_KEY_NAME_OFF); }
    stockWrapKey(key32, e + STOCK_KEY_KEY_OFF);
    return SPI_Flash_write(stockKeyEntryAddr(keyId), e, STOCK_KEY_ENTRY_LEN) ? 1 : 0;
}

/* Load a key straight into RAM (bypasses the flash custom-data store). Marks keys
 * as loaded so the lazy flash-load in dmrAesRxPI won't clear it. For bench use. */
void dmrAesSetKeyRam(uint8_t keyId, const uint8_t *key32)
{
    dmr_aes_set_key(keyId, key32);
    s_keysLoaded = 1;
}

/* Load keys into the RAM store. Key MATERIAL comes from the stock TYT key table in
 * SPI flash (unwrapped per entry); the global TX-key selector comes from the fork's
 * OpenGD77 custom-data AESK header. Called at boot (eager) and after every key edit. */
void dmrAesLoadKeys(void)
{
    uint8_t key[DMR_AES_KEY_BYTES];
    s_keysLoaded = 1;
    dmr_aes_clear_keys();
    s_txKeyId = 0;
    /* TX-key selector (fork-only; no stock equivalent). Read it before the loop below
     * reuses s_aesBlk. Absent block -> selector stays 0 (encrypted TX disabled). */
    if (codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, s_aesBlk, AESK_BLOCK_LEN) &&
        (memcmp(s_aesBlk, "AESK", 4) == 0))
    {
        s_txKeyId = s_aesBlk[5];
    }
    for (uint8_t id = STOCK_KEY_MIN_ID; id <= STOCK_KEY_MAX_ID; id++)
    {
        if (stockKeyRead(id, key)) { dmr_aes_set_key(id, key); }
    }
}

uint8_t dmrAesTxKeyId(void)
{
    /* Keys are eager-loaded at boot (applicationMain) and reloaded by the thread-context
     * CPS/menu writers; never lazy-load here — this runs in the HR-C6000 timeslot ISR. */
    return s_txKeyId;
}

int dmrAesSetTxKeyId(uint8_t keyId)
{
    uint8_t *blk = s_aesBlk;
    if (!codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, blk, AESK_BLOCK_LEN) || memcmp(blk, "AESK", 4) != 0)
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
    if ((keyId < STOCK_KEY_MIN_ID) || (keyId > STOCK_KEY_MAX_ID)) { return 0; }
    int ok = stockKeyWrite(keyId, key32);
    dmrAesLoadKeys();
    return ok;
}

/* Ensure the OpenGD77 custom-data region carries its "OpenGD77" magic, so the
 * codeplugSet/GetOpenGD77CustomData block chain works. Only the TX-key selector
 * (dmrAesSetTxKeyId, AESK header) needs this now — key material lives in the stock
 * table and no longer depends on the OpenGD77 magic. On a radio whose region was
 * never initialized it reads all-0xFF (no magic) and dmrAesSetTxKeyId would
 * silently fail. Writing the 12-byte magic leaves offset 12 onward
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

/* Clear the stored key for keyId by writing the stock "unset slot" marker (type
 * AES-256, default name, wrapped BLANK key), so a stock radio sees a normal empty
 * slot. Returns 1 on success (incl. nothing to do). */
int dmrAesClearKey(uint8_t keyId)
{
    if ((keyId < STOCK_KEY_MIN_ID) || (keyId > STOCK_KEY_MAX_ID)) { return 1; }
    int ok = stockKeyWrite(keyId, s_stockBlankKey);
    dmrAesLoadKeys();
    return ok;
}

/* Bitmask of stored key ids (bit k set => keyId k has a key), for the menu's
 * set/empty display. Never returns key material. keyId 0 is the "off" sentinel. */
uint16_t dmrAesGetKeyMask(void)
{
    uint8_t  key[DMR_AES_KEY_BYTES];
    uint16_t mask = 0;
    for (uint8_t id = STOCK_KEY_MIN_ID; id <= STOCK_KEY_MAX_ID; id++)
    {
        if (stockKeyRead(id, key)) { mask |= (uint16_t)(1u << id); }
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
    /* ISR context (HR-C6000 sys-interrupt): never lazy-load keys here — they are
     * eager-loaded at boot. An unloaded store simply yields clear passthrough. */
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
        /* STALE PI: HR-C6000.c reads the chip's LC page on every voice burst, and the chip keeps
         * returning the PREVIOUS transmission's LC on the first bursts of a new one (OpenGD77 skips
         * those frames for the caller display for the same reason). After an encrypted call that
         * page holds its PI header, so the next call, clear or not, is handed that PI again right
         * after the Voice LC Header reset. Seeding on it would decrypt a clear call into noise, or
         * lock a rapid encrypted call onto the old MI and ignore its own PI. A real new call always
         * carries a fresh MI, so a PI equal to the one the just-ended stream was seeded with (our own
         * TX included) is ignored until a different PI has seeded. */
        int stale = (!s_rxActive && s_rxStaleValid && (p.mi == s_rxStaleMi));
        s_rxLastPiMi = p.mi;
        s_rxLastPiValid = 1;
        if (!s_rxActive && !stale)
        {
            s_rxActive = (dmr_aes_rx_init(&s_rx, &p) == 0);  /* load key for keyId + seed MI */
            if (s_rxActive)
            {
                s_rxInitMi = p.mi;
                s_rxKeyId = p.key_id;  /* remember for late-entry bootstrap of a later rapid call */
                s_rxPiSeeded = 1;      /* chip PI-LC seed: a normal call -> pure self-advance, stable */
                s_rxIvReady = 0;     /* generate IV from the seeded MI on the next burst */
                s_rxLastSeq = -1;
                s_rxStaleValid = 0;  /* a new stream seeded: the previous one's PI is history */
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
            rxd_log(1, (uint8_t)(0x01 | (seeded ? 0x02 : 0) | (wasActive ? 0x04 : 0) | (stale ? 0x08 : 0)),
                    0, s_rxLastSeq, p.mi, s_rxInitMi);
            s_rxdLastPiMi = p.mi;
        }
#endif
    }
    else if ((len >= 3) && (((pi[0] & 0x3F) == 0x00) || ((pi[0] & 0x3F) == 0x03)) && ((pi[1] == 0x00) || (pi[1] == DMR_MFID_DMRA)))
    {
        /* A voice LC (FLCO group or private call, standard or DMRA feature set): the embedded LC
         * the chip reassembles from bursts B..E of every superframe, so it is there within one
         * superframe of joining a call, PI header or not. Its service options Privacy bit (0x40)
         * is the transmitter's own statement that the voice is encrypted: a stock radio sets it on
         * every encrypted call and never on a clear one. The late-entry bootstrap below relies on
         * it, and a bootstrapped stream is dropped when the LC of the stream says clear. The chip
         * can hand us the PREVIOUS call's LC on the first bursts of a new one; a wrong early read
         * is replaced by the fresh LC a superframe later, and the bootstrap self-corrects with it. */
        int privacy = ((pi[2] & 0x40) != 0);
        s_rxLcSeen = 1;
        s_rxLcPrivacy = privacy;
        if (s_rxActive && !s_rxPiSeeded && !privacy)
        {
            /* Bootstrapped from the voice bits, but the stream's own LC says clear: a chance
             * late-entry match on clear voice (issue #2), or the stale LC case above resolving. */
            s_rxActive = 0;
            s_rxBurstEnc = 0;
#ifdef DMR_AES_DIAG_RX
            rxd_log(10, 0, 0, s_rxLastSeq, s_rx.mi, s_rxInitMi);   /* LCCLEAR: dropped a bootstrap */
#endif
        }
    }
}
/* Key id to try first when a call is bootstrapped from its late entry (which carries no key
 * id). The firmware's definition (HR-C6000.c) resolves the current channel's encrypt byte,
 * falling back to the global TX key selector; this weak default (host tests, no channel)
 * says "no preference". 0 = none; ISR context, so it must not touch SPI flash. */
__attribute__((weak)) uint8_t dmrAesRxKeyHint(void) { return 0; }

/* Called per voice burst from the HR-C6000 ISR. seq is the 1..6 burst sequence
 * (A..F); each burst carries 3 AMBE frames, 6 bursts = one 18-frame superframe. */
void dmrAesRxBurst(int seq)
{
#ifdef DMR_AES_DIAG_RX
    s_rxdMisc[2]++;   /* every RxBurst call, regardless of decrypt state */
#endif
    /* Guard the burst sequence (1..6 = A..F, raw rxDataType&7 from the chip). A stray 0
     * fakes a superframe wrap (advancing the MI mid-stream); a 7 sets an out-of-range
     * keystream base. Matches the same guard in dmrAesRxLateEntry. */
    if (seq < 1 || seq > 6) { return; }
    int wrapped = (s_rxLastSeq >= 0 && seq < s_rxLastSeq);

    if (s_rxActive && !s_rxIvReady)
    {
        /* First burst after a PI (re)seed: generate the IV for the CURRENT superframe from
         * s_rx.mi. Works whether we joined at burst A or mid-superframe (the absolute
         * seq-based offset below covers the partial superframe). */
        dmr_lfsr128d(s_rx.mi, s_rx.iv, &s_rxNextMi);
        s_rxPredReal = 1;
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
            /* BOOTSTRAP a call the chip surfaced no PI header for (a rapid re-PTT after the
             * previous call, or one joined late: the preamble was missed). When the stream's own
             * LC has been seen, it decides: Privacy set, and the FIRST crc-valid late entry
             * activates (a stock radio bootstraps the same way); Privacy clear, and nothing
             * does, however the voice bits look. Without an LC yet, a single crc-valid late
             * entry is not enough: on a CLEAR call these bits are ordinary AMBE voice data and
             * pass Golay+CRC4 by chance on ~1% of superframes, which engaged decryption on
             * unencrypted voice (issue #2); so it must CONFIRM the prediction (equal the LFSR
             * self-advance of an earlier real MI, a chance match of ~2^-36): the first valid late
             * entry diverges and becomes the prediction, the next valid one activates. That wait
             * cost a second or more of noise at the start of every rapid call, and a whole call
             * when the bits kept failing (bench, 2026-10-24). Failed decodes in between keep
             * self-advancing the prediction, so a noisy call still locks. The key: the channel's
             * configured key, else the last call's keyId, else any loaded key (see below). */
            int act = 0;
            if (leok && (s_rxLcSeen ? s_rxLcPrivacy : (!diverge && s_rxPredReal)))
            {
                dmr_pi_t p;
                p.alg_id = DMR_ALG_AES256; p.mfid = DMR_MFID_DMRA;
                p.mi = leMi; p.valid = 1;
                /* The late entry carries no key id, and OFB gives no way to tell a wrong key
                 * from a right one: whichever key engages here stays for the whole call. The
                 * transmitter does announce its key: the burst-F EMB Late Entry Single Block,
                 * read by dmrAesRxEmbSb and kept in s_rxEmbKey for this stream. Try that FIRST
                 * (issue #11: the channel's key is wrong when the radio sits on another
                 * channel, or on one with no key), then the key the current channel is
                 * configured with (a channel on key 2 late-entered with key 1, the lowest
                 * slot, and stayed noise), then the key of the last call, then any loaded key.
                 * An announced slot that holds no key falls through to the chain. */
                uint8_t hint = dmrAesRxKeyHint();
                if (s_rxEmbKey != 0) { p.key_id = s_rxEmbKey; act = (dmr_aes_rx_init(&s_rx, &p) == 0); }
                if (!act && (hint != 0) && (hint < DMR_AES_MAX_KEYS)) { p.key_id = hint; act = (dmr_aes_rx_init(&s_rx, &p) == 0); }
                if (!act) { p.key_id = s_rxKeyId; act = (dmr_aes_rx_init(&s_rx, &p) == 0); }   /* sets s_rx.mi = leMi + loads key */
                if (!act) { int fk = dmr_aes_first_keyid(); if (fk >= 0) { p.key_id = (uint8_t)fk; act = (dmr_aes_rx_init(&s_rx, &p) == 0); } }
                if (act) { s_rxActive = 1; s_rxInitMi = leMi; s_rxKeyId = s_rx.key_id; s_rxPiSeeded = 0; }
            }
            if (!act) { s_rx.mi = mi; }   /* rx_init already set s_rx.mi = leMi when act; else keep predicting */
            dmr_lfsr128d(s_rx.mi, s_rx.iv, &s_rxNextMi);  /* IV for the entering superframe (used once active) */
            s_rxPredReal = s_rxPredReal || leok;   /* when leok, s_rx.mi == leMi */
            s_rxIvReady = 1;
#ifdef DMR_AES_DIAG_RX
            rxd_log(6, (uint8_t)((leok ? 0x01 : 0) | (diverge ? 0x08 : 0) | (act ? 0x10 : 0) | (s_rxLcSeen ? 0x20 : 0) | (s_rxLcPrivacy ? 0x40 : 0) | (s_rxEmbKey ? 0x80 : 0)),
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
            s_rxPredReal = 1;
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
/* The burst-F EMB of every DMRA encrypted call carries the "Late Entry Single Block": a
 * BPTC(16x2) codeword announcing the algorithm and key id, which is exactly what the in-AMBE
 * late-entry MI lacks. HR-C6000.c reads the 4 octets the chip received back from page 0x02
 * regs 0x29..0x2C (the registers our TX side writes the block to; the chip emits them raw)
 * on burst F and hands them here while dmrAesRxWantEmbSb says a bootstrap may still need
 * them. Called from the ISR: the decode is ~100 bit operations, no flash, no AES. A clean
 * AES-256 announcement of a key slot 1..15 is remembered for the stream (s_rxEmbKey) and
 * tried first by the late-entry bootstrap in dmrAesRxBurst. A stream already bootstrapped on
 * a guessed key (its first announcement was lost) switches to the announced key at once, MI
 * track untouched, so a wrong guess costs at most one superframe. A PI-seeded stream knows
 * its key from the PI and ignores the block. Other algorithms, key id 0 and a damaged block
 * (syndrome or parity) leave the state untouched, and the next burst F is read again.
 * src is a diagnostic tag only (the chip's reg 0x51 SyncClass of the interrupt it came on). */
void dmrAesRxEmbSb(const uint8_t sb4[4], uint8_t src)
{
    uint8_t key = 0, alg = 0;
    int ok = dmr_emb_sb_decode(sb4, &key, &alg);
    int usable = ok && (alg == (DMR_ALG_AES256 & 0x07)) && (key != 0) && (key < DMR_AES_MAX_KEYS);
    int rekeyed = 0;
    if (usable && (s_rxEmbKey == 0))
    {
        s_rxEmbKey = key;
        if (s_rxActive && !s_rxPiSeeded && (s_rx.key_id != key) && (dmr_aes_ctx_set_key(&s_rx, key) == 0))
        {
            s_rxKeyId = key;
            rekeyed = 1;
        }
    }
#ifdef DMR_AES_DIAG_RX
    /* EMBSB: mi = the raw 4 octets (big-endian), aux = key | alg << 8 when decodable, seq = src.
     * Every usable block is logged (one per call); the others are capped per call. */
    if (usable || (s_rxdEmbN < 3))
    {
        s_rxdEmbN++;
        rxd_log(11 /*EMBSB*/, (uint8_t)((ok ? 0x01 : 0) | (usable ? 0x02 : 0) | (rekeyed ? 0x04 : 0) | (s_rxActive ? 0x10 : 0) | (s_rxPiSeeded ? 0x20 : 0)),
                (int)src, s_rxLastSeq,
                ((uint32_t)sb4[0] << 24) | ((uint32_t)sb4[1] << 16) | ((uint32_t)sb4[2] << 8) | sb4[3],
                ok ? ((uint32_t)key | ((uint32_t)alg << 8)) : 0);
    }
#else
    (void)src; (void)rekeyed;
#endif
}
/* Whether HR-C6000.c should read the burst-F EMB block for this stream: only while the
 * announcement is still unknown and could matter (no decrypt running, or one bootstrapped
 * from the late entry on a guessed key). A PI-seeded call never needs it. */
int dmrAesRxWantEmbSb(void)
{
    return (s_rxEmbKey == 0) && (!s_rxActive || !s_rxPiSeeded);
}
/* Called per decoded AMBE frame from codecDecode (idxInBurst 0..2). Applies the
 * OFB keystream to the 49-bit decoded voice vector in place. */
void dmrAesRxCodecFrame(uint16_t *b49, int idxInBurst)
{
    if (!s_rxActive || !s_rxBurstEnc)
    {
        /* The stream's LC says encrypted but we have no keystream yet (PI header missed,
         * late entry not decoded): play silence, not the ciphertext as noise. */
        if (!s_rxActive && s_rxLcSeen && s_rxLcPrivacy) { dmr_ambe_set_silence(b49); }
        return;
    }
#ifdef DMR_AES_DIAG_RX
    /* Which way dmr_aes_voice_frame will go with this frame (issue #4: do the firmware's
     * inserted SILENCE_AUDIO frames get left alone?). Same tests, same order, as inside it. */
    s_rxdFrames[dmr_ambe_is_silence(b49) ? 0 : (dmr_ambe_is_ccr(b49) ? 1 : 2)]++;
#endif
    dmr_aes_voice_frame(&s_rx, b49, s_rxBurstBase + (size_t)idxInBurst * 56);
}
/* Call boundary (CRC-valid Terminator, or the next call's Voice LC Header). Also drop the
 * late-entry fragments and the superframe position: otherwise the next call's first burst
 * reads as a superframe wrap and decodes the PREVIOUS call's last late entry, which matches
 * the prediction and would engage decryption on whatever follows, clear voice included. */
void dmrAesRxEnd(void)
{
    s_rxActive = 0;
    s_rxBurstEnc = 0;
    s_rxLastSeq = -1;
    memset(s_rxFrag, 0, sizeof s_rxFrag);
    s_rxEmbKey = 0;      /* the ended stream's key announcement must not seed the next */
#ifdef DMR_AES_DIAG_RX
    s_rxdEmbN = s_rxdSync0N = 0;
#endif
    s_rxLcSeen = 0;      /* the next stream's LC decides for the next stream */
    s_rxLcPrivacy = 0;
    /* The PI the chip handed us during this stream is now stale (see dmrAesRxPI). Only the
     * most recent one: a PI seen before an intervening stream is accepted again. */
    if (s_rxLastPiValid)
    {
        s_rxStaleMi = s_rxLastPiMi;
        s_rxStaleValid = 1;
        s_rxLastPiValid = 0;
    }
}

/* ---- TX (mirror of RX: encrypt the 49 AMBE params at the codec layer) ---- */
void dmrAesTxStart(uint8_t keyId, uint32_t miSeed)
{
    /* ISR context (HR-C6000 timeslot interrupt): keys are eager-loaded at boot; never
     * do the blocking SPI-flash load here. Unloaded -> dmr_aes_tx_init fails -> clear TX.
     * Called at EVERY key-up (DMR_STATE_TX_START_1), keyId 0 meaning a CLEAR call (no key
     * selected, a data call, hotspot relay). It must positively end any encrypted state: a
     * previous encrypted call can be aborted without DMR_STATE_TX_END_1 (interrupt timeout,
     * mode change, slot reset), and a stale s_txActive would otherwise make the next clear
     * call go out with the encryption LC flags, PI headers and an encrypted codec stream.
     * keyId 0 is the "off" sentinel everywhere, never a key slot, even if slot 0 holds a key. */
    s_txActive = (keyId != 0) && (dmr_aes_tx_init(&s_tx, DMR_ALG_AES256, keyId, miSeed) == 0); /* sets s_tx.mi = miSeed */
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
 * MI advances each 18-frame superframe, offset=(frame%18)*56 — exactly what the receiver
 * (a stock TYT, or our own RX) expects. Every frame is encrypted, silence included, as the
 * stock TYT does (see dmr_aes_voice_frame_tx). */
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
    dmr_aes_voice_frame_tx(&s_tx, b49, (size_t)(s_txFrameCnt % 18) * 56);
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
    /* s_txFrameCnt==0 would underflow f to 0xFFFFFFFF (stale frag nibble): can happen if an
     * ISR dmrAesTxStart resets the counter between the encode and this stuff. Guard both. */
    if (!s_txActive || s_txFrameCnt == 0) { return; }
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
void dmrAesTxEnd(void)
{
    if (s_txActive)
    {
        /* Our own PI header may still sit in the chip's LC page and be re-read by RX. */
        s_rxStaleMi = s_txPiMi;
        s_rxStaleValid = 1;
    }
    s_txActive = 0;
}

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

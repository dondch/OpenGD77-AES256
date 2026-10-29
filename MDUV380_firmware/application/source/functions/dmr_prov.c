/*
 * dmr_prov.c — secure remote provisioning over DMR data. See dmr_prov.h and PROVISIONING.md.
 *
 * Compiles to nothing unless built -DENABLE_REMOTE_PROVISION (which needs ENABLE_DMR_DATA).
 * This file is the protocol and the two state machines (slave: stage/commit/reply; master:
 * send/ACK/STATUS) and is host-testable: everything that touches the codeplug, the UI, the
 * battery or the feature arena goes through the dmrProvApply* functions in dmr_prov_apply.c,
 * which the host test replaces with stubs.
 *
 * Context rules: dmrProvRxPdu, dmrProvTick, dmrProvInit/Reload and dmrProvJobLoad run in the
 * main loop task (or the CPS handler, which is the same task). dmrProvIsMasterId,
 * dmrProvAckAllowed and dmrProvRxResponse run in the HR-C6000 ISR and only read or set
 * single bytes/words. Flash is written only from dmrProvTick / dmrProvRxPdu (main loop).
 */
#include "functions/dmr_prov.h"

#if defined(ENABLE_REMOTE_PROVISION)

#include "functions/dmr_sms.h"
#include "functions/dmr_data.h"
#include "functions/trx.h"
#include "functions/ticks.h"
#include "crypto/dmr_aes.h"
#include "crypto/dmr_cmac.h"
#include <string.h>
#include <stdio.h>

/* ---- tunables -------------------------------------------------------------- */
#define PROV_ACK_TIMEOUT_MS      2000u   /* master: link ACK after the data call ended */
#define PROV_STATUS_TIMEOUT_MS   6000u   /* master: signed STATUS after COMMIT/STATUS/REBOOT */
#define PROV_TRIES               3       /* master: sends of one PDU before giving up */
#define PROV_TXN_TIMEOUT_MS      120000u /* slave: idle time before a staged transaction is dropped */
#define PROV_HOLDOFF_FAILS       4       /* slave: consecutive MAC failures that start the hold-off */
#define PROV_HOLDOFF_MS          10000u
#define PROV_TX_SETTLE_MS        400u    /* a loaded data call keys ~100 ms later; wait before polling "done" */
#define PROV_REPLY_PDU_MAX       80      /* STATUS reply: 10 + 24 + 16 = 50 -> 60 with CRC and padding */
#define PROV_BLK_PAYLOAD         10      /* confirmed rate-1/2 block payload */
#define PROV_PREAMBLE            6
#define PROV_PDU_SCRATCH         384     /* master: PDU assembly at the top of the arena */

/* burst type bytes (HR-C6000 page 0x04 reg 0x50), as in dmr_sms.c */
#define DTB_CSBK         0x30
#define DTB_DATA_HEADER  0x60
#define DTB_RATE12_DATA  0x70

/* PROV block offsets (PROVISIONING.md §2.1) */
#define BLK_MAGIC     0
#define BLK_VERSION   4
#define BLK_NMASTERS  5
#define BLK_KEY       8
#define BLK_MASTERS   40
#define BLK_OWNSEQ    72

/* ---- state ------------------------------------------------------------------ *
 * CCM (DMR_AES_CCM) is not zeroed at boot: everything here is set by dmrProvInit. */
static uint8_t  s_blk[DMR_PROV_BLOCK_LEN] DMR_AES_CCM;
static uint8_t  s_kEnc[32] DMR_AES_CCM;
static uint8_t  s_kMac[32] DMR_AES_CCM;
static uint8_t  s_paired DMR_AES_CCM;
static uint8_t  s_nMasters DMR_AES_CCM;
static volatile uint32_t s_masterId[DMR_PROV_MAX_MASTERS] DMR_AES_CCM;
static uint32_t s_lastSeq[DMR_PROV_MAX_MASTERS] DMR_AES_CCM;    /* persisted (flash copy) */
static uint32_t s_lastSeen[DMR_PROV_MAX_MASTERS] DMR_AES_CCM;   /* RAM, >= s_lastSeq */
static uint32_t s_ownSeqNext DMR_AES_CCM;        /* master: next sequence number to use */
static uint32_t s_ownSeqReserved DMR_AES_CCM;    /* master: flash says we may use anything below this */

/* slave */
static uint8_t  s_txnActive DMR_AES_CCM;
static uint8_t  s_txn DMR_AES_CCM;
static uint8_t  s_stageCount DMR_AES_CCM;
static uint16_t s_stageUsed DMR_AES_CCM;
static uint8_t  s_stageErr DMR_AES_CCM;          /* a staging failure to report at COMMIT (0 = none) */
static uint8_t *s_stage DMR_AES_CCM;             /* the arena while staging, else NULL */
static uint32_t s_stageLastMs DMR_AES_CCM;
static uint8_t  s_lastCommitMaster DMR_AES_CCM;  /* index of the master of the cached reply */
static uint32_t s_lastCommitSeq DMR_AES_CCM;     /* the request seq the cached reply answers */
static uint32_t s_lastCodeplugCrc DMR_AES_CCM;
static uint8_t  s_replyPdu[PROV_REPLY_PDU_MAX] DMR_AES_CCM;   /* the cached STATUS reply PDU */
static uint8_t  s_replyLen DMR_AES_CCM;
static uint32_t s_replyDst DMR_AES_CCM;
static uint32_t s_replySeq DMR_AES_CCM;
static uint8_t  s_replyPending DMR_AES_CCM;      /* queued for transmission */
static uint32_t s_replyLoadedMs DMR_AES_CCM;
static uint8_t  s_rebootPending DMR_AES_CCM;     /* after the reply has gone out */
static uint16_t s_failedMacs DMR_AES_CCM;
static uint8_t  s_failStreak DMR_AES_CCM;
static uint32_t s_holdoffUntil DMR_AES_CCM;
static volatile uint8_t s_ackInhibit DMR_AES_CCM;
static uint16_t s_rejSeq DMR_AES_CCM, s_rejAddr DMR_AES_CCM, s_rejFrame DMR_AES_CCM, s_commits DMR_AES_CCM;
static uint16_t s_stagedTotal DMR_AES_CCM;

/* master */
enum { M_IDLE = 0, M_NEXT, M_SENDING, M_WAIT_ACK, M_WAIT_STATUS, M_DONE, M_FAILED };
static uint8_t  s_mPhase DMR_AES_CCM;
static uint8_t *s_job DMR_AES_CCM;               /* arena while a job runs */
static uint16_t s_jobLen DMR_AES_CCM;
static uint32_t s_mSlave DMR_AES_CCM;
static uint8_t  s_mTxn DMR_AES_CCM;
static uint8_t  s_mFlags DMR_AES_CCM;
static uint16_t s_mItemOff DMR_AES_CCM;          /* offset of the current item in the job */
static uint8_t  s_mItemIdx DMR_AES_CCM;
static uint8_t  s_mItemCount DMR_AES_CCM;
static uint8_t  s_mTries DMR_AES_CCM;
static uint8_t  s_mCommitSent DMR_AES_CCM;
static uint8_t  s_mCurIsCommit DMR_AES_CCM;      /* the PDU in flight is the implicit COMMIT */
static uint32_t s_mSeqCur DMR_AES_CCM;
static uint32_t s_mT0 DMR_AES_CCM;
static uint32_t s_mCrc DMR_AES_CCM;              /* CRC32 of the items (what COMMIT carries) */
static uint8_t  s_mFailReason DMR_AES_CCM;
static volatile uint8_t  s_ackSeen DMR_AES_CCM;
static volatile uint8_t  s_ackNs DMR_AES_CCM;
static volatile uint32_t s_ackSrc DMR_AES_CCM;
static uint8_t  s_mStatusSeen DMR_AES_CCM;
static uint8_t  s_mStatus[DMR_PROV_STATUS_BODY_LEN] DMR_AES_CCM;   /* last verified STATUS body */
static uint8_t  s_mLastOutcome DMR_AES_CCM;      /* 0 none, 1 done, 2 failed */

/* ---- small helpers ------------------------------------------------------------- */
static uint32_t rd32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void     wr32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void     wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* CRC32 (IEEE 802.3, reflected, == zlib.crc32) of the staged image / job items. */
static uint32_t crc32_ieee(uint32_t crc, const uint8_t *p, int len)
{
    crc = ~crc;
    for (int i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) { crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); }
    }
    return ~crc;
}

/* ETSI TS 102 361-1 B.3.11 CRC-9 of a confirmed block, in the form DSD-FME checks (its
 * dmr_dburst.c, "need to find a sample to confirm this"): register poly 0x059 from 0 over the
 * data bits MSB-first, then the 7 DBSN bits MSB-first; DSD-FME compares the field XOR 0x0F0
 * (the rate-1/2 mask) with the inverted register, so the field on the air is R ^ 0x10F.
 * NOT verified against a real radio: our own receiver does not check it (the HR-C6000 burst
 * CRC gates the block), a wrong convention only shows as a CRC-9 warning in DSD-FME. */
uint16_t dmrProvCrc9(const uint8_t *data, int len, uint8_t dbsn)
{
    uint16_t crc = 0;
    for (int i = 0; i < len * 8 + 7; i++)
    {
        int bit = (i < len * 8) ? ((data[i >> 3] >> (7 - (i & 7))) & 1) : ((dbsn >> (6 - (i - len * 8))) & 1);
        if (((crc >> 8) & 1) ^ bit) { crc = (uint16_t)(((crc << 1) ^ 0x059) & 0x1FF); }
        else                        { crc = (uint16_t)((crc << 1) & 0x1FF); }
    }
    return (uint16_t)(crc ^ 0x10F);
}

static int masterIndex(uint32_t id)
{
    for (int i = 0; i < s_nMasters; i++) { if (s_masterId[i] == id) { return i; } }
    return -1;
}

/* ---- block / keys ----------------------------------------------------------------- */
static void loadBlock(void)
{
    s_paired = 0; s_nMasters = 0;
    memset(s_kEnc, 0, 32); memset(s_kMac, 0, 32);
    for (int i = 0; i < DMR_PROV_MAX_MASTERS; i++) { s_masterId[i] = 0; s_lastSeq[i] = 0; s_lastSeen[i] = 0; }
    s_ownSeqNext = s_ownSeqReserved = 0;
    if (!dmrProvApplyBlockRead(s_blk) || (memcmp(s_blk + BLK_MAGIC, "PROV", 4) != 0) || (s_blk[BLK_VERSION] != 1))
    {
        memset(s_blk, 0, sizeof s_blk);
        return;
    }
    static const uint8_t zero[DMR_PROV_KEY_BYTES] = { 0 };
    if (memcmp(s_blk + BLK_KEY, zero, DMR_PROV_KEY_BYTES) == 0) { return; }   /* no key: not paired */
    s_nMasters = (s_blk[BLK_NMASTERS] > DMR_PROV_MAX_MASTERS) ? DMR_PROV_MAX_MASTERS : s_blk[BLK_NMASTERS];
    for (int i = 0; i < s_nMasters; i++)
    {
        s_masterId[i] = rd32le(s_blk + BLK_MASTERS + i * 8) & 0x00FFFFFFu;
        s_lastSeq[i] = s_lastSeen[i] = rd32le(s_blk + BLK_MASTERS + i * 8 + 4);
    }
    s_ownSeqReserved = rd32le(s_blk + BLK_OWNSEQ);
    s_ownSeqNext = s_ownSeqReserved;
    aes256_kdf108(s_blk + BLK_KEY, "enc", s_kEnc);
    aes256_kdf108(s_blk + BLK_KEY, "mac", s_kMac);
    s_paired = 1;
}

/* Persist the sequence state into the block (the key and master IDs are what flash holds). */
static int saveBlock(void)
{
    for (int i = 0; i < s_nMasters; i++) { wr32le(s_blk + BLK_MASTERS + i * 8 + 4, s_lastSeq[i]); }
    wr32le(s_blk + BLK_OWNSEQ, s_ownSeqReserved);
    return dmrProvApplyBlockWrite(s_blk);
}

static void stageDiscard(void)
{
    if (s_stage != NULL) { dmrProvApplyArenaRelease(); }
    s_stage = NULL; s_txnActive = 0; s_stageCount = 0; s_stageUsed = 0; s_stageErr = 0;
}

static void masterReset(void)
{
    if (s_job != NULL) { dmrProvApplyArenaRelease(); }
    s_job = NULL; s_jobLen = 0; s_mPhase = M_IDLE;
    s_mItemOff = 0; s_mItemIdx = 0; s_mItemCount = 0; s_mTries = 0; s_mCommitSent = 0; s_mCurIsCommit = 0;
    s_ackSeen = 0; s_mStatusSeen = 0;
}

void dmrProvInit(void)
{
    s_stage = NULL; s_job = NULL;          /* CCM garbage must not look like a held arena */
    stageDiscard();
    masterReset();
    s_replyLen = 0; s_replyPending = 0; s_rebootPending = 0; s_replyDst = 0; s_replySeq = 0; s_replyLoadedMs = 0;
    s_lastCommitSeq = 0; s_lastCommitMaster = 0; s_lastCodeplugCrc = 0;
    s_failedMacs = 0; s_failStreak = 0; s_holdoffUntil = 0; s_ackInhibit = 0;
    s_rejSeq = s_rejAddr = s_rejFrame = s_commits = s_stagedTotal = 0;
    s_mLastOutcome = 0; s_mFailReason = 0; s_mSeqCur = 0; s_mT0 = 0; s_mCrc = 0;
    memset(s_mStatus, 0, sizeof s_mStatus);
    s_stageLastMs = 0; s_txn = 0;
    loadBlock();
}

void dmrProvReload(void)
{
    /* A new key or master list invalidates everything in flight. */
    stageDiscard();
    masterReset();
    s_replyLen = 0; s_replyPending = 0; s_lastCommitSeq = 0;
    loadBlock();
}

int dmrProvIsPaired(void) { return s_paired; }

int dmrProvIsMasterId(uint32_t id)
{
    for (int i = 0; i < DMR_PROV_MAX_MASTERS; i++) { if ((i < s_nMasters) && (s_masterId[i] == id)) { return 1; } }
    return 0;
}

int dmrProvAckAllowed(void) { return s_ackInhibit ? 0 : 1; }

/* ---- frame / PDU builders ------------------------------------------------------------ */
int dmrProvBuildPdu(uint32_t master, uint32_t slave, uint32_t seq, uint8_t txn, int reply,
                    const uint8_t *body, int bodyLen, uint8_t *pdu, int pduMax)
{
    if (!s_paired || (bodyLen < 1) || (bodyLen > DMR_PROV_BODY_MAX)) { return -1; }
    int flen = DMR_PROV_HDR_LEN + bodyLen + DMR_PROV_TAG_LEN;
    int total = ((flen + 4 + PROV_BLK_PAYLOAD - 1) / PROV_BLK_PAYLOAD) * PROV_BLK_PAYLOAD;
    if ((total > pduMax) || (total > DMR_PROV_PDU_MAX)) { return -2; }
    uint8_t assoc[8], nonce[13];
    wr32be(assoc, master); wr32be(assoc + 4, slave);
    memcpy(nonce, assoc, 8); wr32be(nonce + 8, seq); nonce[12] = reply ? 1 : 0;
    pdu[0] = DMR_PROV_MAGIC; pdu[1] = DMR_PROV_VERSION; pdu[2] = txn; pdu[3] = reply ? 1 : 0;
    wr32be(pdu + 4, seq);
    pdu[8] = (uint8_t)(bodyLen >> 8); pdu[9] = (uint8_t)bodyLen;
    memcpy(pdu + DMR_PROV_HDR_LEN, body, (size_t)bodyLen);
    aes256_ctr_crypt(s_kEnc, nonce, pdu + DMR_PROV_HDR_LEN, (size_t)bodyLen);
    aes256_cmac2(s_kMac, assoc, 8, pdu, (size_t)(DMR_PROV_HDR_LEN + bodyLen), pdu + DMR_PROV_HDR_LEN + bodyLen);
    memset(pdu + flen, 0, (size_t)(total - flen));
    uint32_t crc = dmrSmsCrc32(pdu, total);
    wr32be(pdu + total - 4, crc);
    return total;
}

int dmrProvBuildBursts(const uint8_t *pdu, int pduLen, uint32_t dst, uint32_t src, uint32_t seq,
                       uint8_t *q, int qMaxBursts)
{
    if ((pduLen <= 0) || (pduLen % PROV_BLK_PAYLOAD) != 0) { return -1; }
    int nblocks = pduLen / PROV_BLK_PAYLOAD;
    int n = 0;
    if (PROV_PREAMBLE + 1 + nblocks > qMaxBursts) { return -2; }
    /* the frame's own length says where the padding starts: POC for the header */
    int plen = ((int)pdu[8] << 8) | pdu[9];
    int poc = pduLen - 4 - (DMR_PROV_HDR_LEN + plen + DMR_PROV_TAG_LEN);
    if (poc < 0) { poc = 0; }
    for (int i = 0; i < PROV_PREAMBLE; i++)
    {
        uint8_t *b = q + n * 13;
        b[0] = DTB_CSBK;
        b[1] = 0xBD; b[2] = 0x00; b[3] = 0x80; b[4] = (uint8_t)((PROV_PREAMBLE - 1 - i) + 1 + nblocks);
        b[5] = (uint8_t)(dst >> 16); b[6] = (uint8_t)(dst >> 8); b[7] = (uint8_t)dst;
        b[8] = (uint8_t)(src >> 16); b[9] = (uint8_t)(src >> 8); b[10] = (uint8_t)src;
        dmrSmsHdrCrc(b + 1, 10, 0xA5A5, b + 11);
        n++;
    }
    {
        uint8_t *b = q + n * 13;
        b[0] = DTB_DATA_HEADER;
        b[1] = (uint8_t)(0x43 | (((poc >> 4) & 0x03) << 4));           /* individual, A, DPF 3 */
        b[2] = (uint8_t)((DMR_PROV_SAP << 4) | (poc & 0x0F));
        b[3] = (uint8_t)(dst >> 16); b[4] = (uint8_t)(dst >> 8); b[5] = (uint8_t)dst;
        b[6] = (uint8_t)(src >> 16); b[7] = (uint8_t)(src >> 8); b[8] = (uint8_t)src;
        b[9] = (uint8_t)(0x80 | (nblocks & 0x7F));
        b[10] = (uint8_t)((seq & 0x07) << 4);                           /* S = 0, N(S), FSN = 0 */
        dmrSmsHdrCrc(b + 1, 10, 0xCCCC, b + 11);
        n++;
    }
    for (int k = 0; k < nblocks; k++)
    {
        uint8_t *b = q + n * 13;
        uint8_t dbsn = (uint8_t)(k & 0x7F);
        uint16_t c9 = dmrProvCrc9(pdu + k * PROV_BLK_PAYLOAD, PROV_BLK_PAYLOAD, dbsn);
        b[0] = DTB_RATE12_DATA;
        b[1] = (uint8_t)((dbsn << 1) | ((c9 >> 8) & 1));
        b[2] = (uint8_t)c9;
        memcpy(b + 3, pdu + k * PROV_BLK_PAYLOAD, PROV_BLK_PAYLOAD);
        n++;
    }
    return n;
}

/* Verify a frame's MAC and decrypt its body in place. pdu/len as received (padding + CRC
 * included). Returns the body length (>= 1) with seq, txn and reply filled, or <0:
 * -1 not a frame, -2 MAC failure. */
static int frameOpen(uint8_t *pdu, int len, uint32_t master, uint32_t slave, uint32_t *seq, uint8_t *txn, int *reply)
{
    if ((len < DMR_PROV_HDR_LEN + DMR_PROV_TAG_LEN + 4) || (pdu[0] != DMR_PROV_MAGIC) || (pdu[1] != DMR_PROV_VERSION)) { return -1; }
    int plen = ((int)pdu[8] << 8) | pdu[9];
    int flen = DMR_PROV_HDR_LEN + plen + DMR_PROV_TAG_LEN;
    if ((plen < 1) || (plen > DMR_PROV_BODY_MAX) || (flen + 4 > len) || (len - 4 - flen > 15) || (pdu[3] & 0xFE)) { return -1; }
    uint8_t assoc[8], nonce[13], tag[16];
    wr32be(assoc, master); wr32be(assoc + 4, slave);
    aes256_cmac2(s_kMac, assoc, 8, pdu, (size_t)(DMR_PROV_HDR_LEN + plen), tag);
    if (dmr_consttime_neq(tag, pdu + DMR_PROV_HDR_LEN + plen, DMR_PROV_TAG_LEN)) { return -2; }
    *seq = rd32be(pdu + 4); *txn = pdu[2]; *reply = pdu[3] & 1;
    memcpy(nonce, assoc, 8); wr32be(nonce + 8, *seq); nonce[12] = (uint8_t)*reply;
    aes256_ctr_crypt(s_kEnc, nonce, pdu + DMR_PROV_HDR_LEN, (size_t)plen);
    return plen;
}

/* ---- slave ---------------------------------------------------------------------------- */
/* Make txn the active transaction: a different one supersedes what was staged, a new one
 * claims the arena (BUSY when another feature, or this radio's own master job, holds it). */
static void stageBegin(uint8_t txn)
{
    if (s_txnActive && (txn != s_txn)) { stageDiscard(); }
    if (!s_txnActive)
    {
        s_txnActive = 1; s_txn = txn; s_stageUsed = 0; s_stageCount = 0; s_stageErr = 0;
        s_stage = (s_job != NULL) ? NULL : dmrProvApplyArenaClaim();
        if (s_stage == NULL) { s_stageErr = DMR_PROV_RES_BUSY; }
    }
    s_stageLastMs = ticksGetMillis();
}

static int stageItem(uint8_t txn, const uint8_t *body, int bodyLen)
{
    int need = 3 + bodyLen - 1;    /* op(1) len(2) args */
    stageBegin(txn);
    if (s_stageErr) { return 0; }
    if ((s_stageUsed + need > DMR_PROV_ARENA_BYTES) || (s_stageCount == 255)) { s_stageErr = DMR_PROV_RES_NO_SPACE; return 0; }
    uint8_t *it = s_stage + s_stageUsed;
    it[0] = body[0]; it[1] = (uint8_t)((bodyLen - 1) & 0xFF); it[2] = (uint8_t)((bodyLen - 1) >> 8);
    memcpy(it + 3, body + 1, (size_t)(bodyLen - 1));
    s_stageUsed = (uint16_t)(s_stageUsed + need);
    s_stageCount++;
    s_stagedTotal++;
    return 1;
}

/* Expected argument length per staged op, -1 for not a staged op. */
static int stagedArgLen(uint8_t op)
{
    switch (op)
    {
        case DMR_PROV_OP_WRITE_CHANNEL: return 2 + 56;
        case DMR_PROV_OP_WRITE_ZONE:    return 1 + 16 + 160;
        case DMR_PROV_OP_SET_VFO:       return 1 + 56;
        case DMR_PROV_OP_SET_SETTING:   return 1 + 4;
        case DMR_PROV_OP_STORE_KEY:     return 1 + 32;
        default: return -1;
    }
}

/* Write one staged item. Returns 1 on success. */
static int applyItem(const uint8_t *it, int *settingsTouched)
{
    uint8_t op = it[0];
    const uint8_t *a = it + 3;
    switch (op)
    {
        case DMR_PROV_OP_WRITE_CHANNEL: return dmrProvApplyChannel((uint16_t)((a[0] << 8) | a[1]), a + 2);
        case DMR_PROV_OP_WRITE_ZONE:    return dmrProvApplyZone(a[0], a + 1);
        case DMR_PROV_OP_SET_VFO:       return dmrProvApplyVfo(a[0], a + 1);
        case DMR_PROV_OP_SET_SETTING:   *settingsTouched = 1; return dmrProvApplySetting(a[0], rd32le(a + 1));
        case DMR_PROV_OP_STORE_KEY:     return dmrProvApplyStoreKey(a[0], a + 1);
        default: return 0;
    }
}

/* Build and cache the STATUS reply for request (seq, txn) from master mIdx, and queue it. */
static void replyStatus(int mIdx, uint32_t seq, uint8_t txn, uint8_t result)
{
    uint8_t body[DMR_PROV_STATUS_BODY_LEN];
    const char *git = dmrProvApplyGitHash();
    body[0] = DMR_PROV_OP_STATUS_REPLY;
    body[1] = result; body[2] = txn;
    wr32be(body + 3, s_lastSeq[mIdx]);
    wr32be(body + 7, s_lastCodeplugCrc);
    memset(body + 11, 0, 8);
    for (int i = 0; i < 8 && git[i]; i++) { body[11 + i] = (uint8_t)git[i]; }
    uint16_t bv = dmrProvApplyBatteryCentivolts();
    body[19] = (uint8_t)(bv >> 8); body[20] = (uint8_t)bv;
    body[21] = s_stageCount;
    body[22] = (uint8_t)(s_failedMacs >> 8); body[23] = (uint8_t)s_failedMacs;
    int n = dmrProvBuildPdu(s_masterId[mIdx], trxDMRID, seq, txn, 1, body, sizeof body, s_replyPdu, sizeof s_replyPdu);
    if (n <= 0) { s_replyLen = 0; return; }
    s_replyLen = (uint8_t)n;
    s_replyDst = s_masterId[mIdx];
    s_replySeq = seq;
    s_lastCommitMaster = (uint8_t)mIdx;
    s_lastCommitSeq = seq;
    s_replyPending = 1;
}

static void slaveCommit(int mIdx, uint32_t seq, uint8_t txn, const uint8_t *args)
{
    uint8_t result = DMR_PROV_RES_OK;
    uint8_t count = args[0];
    uint32_t crc = rd32be(args + 1);
    if (!s_txnActive || (txn != s_txn)) { result = DMR_PROV_RES_TXN_MISMATCH; }
    else if (s_stageErr) { result = s_stageErr; }
    else if ((count != s_stageCount) || (crc32_ieee(0, s_stage, s_stageUsed) != crc)) { result = DMR_PROV_RES_CRC_MISMATCH; }
    if (result == DMR_PROV_RES_OK)
    {
        int settingsTouched = 0, off = 0, done = 0;
        uint8_t rb[DMR_PROV_BODY_MAX];
        uint32_t rcrc = 0;
        for (int i = 0; i < s_stageCount; i++)
        {
            const uint8_t *it = s_stage + off;
            int alen = rd16le(it + 1);
            if (!applyItem(it, &settingsTouched)) { result = DMR_PROV_RES_WRITE_FAILED; break; }
            done++;
            off += 3 + alen;
        }
        if ((result == DMR_PROV_RES_OK) && settingsTouched && !dmrProvApplySettingsSave()) { result = DMR_PROV_RES_WRITE_FAILED; }
        /* read the written records back: the codeplug CRC the master compares against */
        off = 0;
        for (int i = 0; i < done; i++)
        {
            const uint8_t *it = s_stage + off;
            int alen = rd16le(it + 1);
            int n = dmrProvApplyReadBack(it[0], it + 3, alen, rb, (int)sizeof rb);
            if (n > 0) { rcrc = crc32_ieee(rcrc, rb, n); }
            off += 3 + alen;
        }
        s_lastCodeplugCrc = rcrc;
        s_stageCount = (uint8_t)done;
        s_commits++;
    }
    /* the sequence is spent whatever happened to the image: persist, then answer */
    s_lastSeq[mIdx] = seq;
    saveBlock();
    replyStatus(mIdx, seq, txn, result);
    stageDiscard();
}

/* Hand a reply from a slave to the master state machine (pdu already verified+decrypted). */
static void masterStatus(uint32_t src, uint32_t seq, const uint8_t *body, int bodyLen)
{
    if ((s_mPhase != M_SENDING) && (s_mPhase != M_WAIT_ACK) && (s_mPhase != M_WAIT_STATUS)) { return; }
    if ((src != s_mSlave) || (seq != s_mSeqCur) || (bodyLen != DMR_PROV_STATUS_BODY_LEN) || (body[0] != DMR_PROV_OP_STATUS_REPLY)) { return; }
    memcpy(s_mStatus, body, DMR_PROV_STATUS_BODY_LEN);
    s_mStatusSeen = 1;
}

int dmrProvRxPdu(uint8_t *pdu, int len, uint32_t src, uint32_t dst)
{
    if (!s_paired) { return 0; }
    /* framing first (cheap, no key): is this a provisioning frame at all? */
    if ((len < DMR_PROV_HDR_LEN + DMR_PROV_TAG_LEN + 4) || (pdu[0] != DMR_PROV_MAGIC) || (pdu[1] != DMR_PROV_VERSION)) { return 0; }
    if (dst != trxDMRID) { s_rejAddr++; return 1; }
    uint32_t seq; uint8_t txn; int reply;
    if (pdu[3] & 1)
    {
        /* a reply: only the master state machine wants it, and only from its slave */
        if ((s_mPhase == M_IDLE) || (src != s_mSlave)) { s_rejAddr++; return 1; }
        int n = frameOpen(pdu, len, trxDMRID, src, &seq, &txn, &reply);
        if (n == -1) { s_rejFrame++; return 1; }
        if (n == -2) { s_failedMacs++; return 1; }
        masterStatus(src, seq, pdu + DMR_PROV_HDR_LEN, n);
        return 1;
    }
    int mIdx = masterIndex(src);
    if (mIdx < 0) { s_rejAddr++; return 1; }
    int n = frameOpen(pdu, len, src, dst, &seq, &txn, &reply);
    if (n == -1) { s_rejFrame++; return 1; }
    if (n == -2)
    {
        s_failedMacs++;
        if (++s_failStreak >= PROV_HOLDOFF_FAILS) { s_holdoffUntil = ticksGetMillis() + PROV_HOLDOFF_MS; s_ackInhibit = 1; }
        return 1;
    }
    s_failStreak = 0;
    if ((seq == 0xFFFFFFFFu) || (seq <= s_lastSeen[mIdx]))
    {
        /* the master's retry of the request we already answered: the cached reply, as is */
        if (s_replyLen && (seq == s_lastCommitSeq) && (mIdx == s_lastCommitMaster) && (s_replyDst == src)) { s_replyPending = 1; }
        else { s_rejSeq++; }
        return 1;
    }
    s_lastSeen[mIdx] = seq;
    uint8_t *body = pdu + DMR_PROV_HDR_LEN;
    uint8_t op = body[0];
    int alen = stagedArgLen(op);
    if (alen >= 0)
    {
        if (n - 1 != alen) { stageBegin(txn); s_stageErr = DMR_PROV_RES_BAD_OP; return 1; }
        stageItem(txn, body, n);
        return 1;
    }
    switch (op)
    {
        case DMR_PROV_OP_STATUS:
            replyStatus(mIdx, seq, txn, DMR_PROV_RES_OK);   /* seq not persisted (§4) */
            break;
        case DMR_PROV_OP_COMMIT:
            if (n != 1 + 5) { replyStatus(mIdx, seq, txn, DMR_PROV_RES_BAD_OP); break; }
            slaveCommit(mIdx, seq, txn, body + 1);
            break;
        case DMR_PROV_OP_ABORT:
            if (s_txnActive && (txn == s_txn)) { stageDiscard(); }
            break;
        case DMR_PROV_OP_REBOOT:
            stageDiscard();
            s_lastSeq[mIdx] = seq;
            saveBlock();
            replyStatus(mIdx, seq, txn, DMR_PROV_RES_OK);
            s_rebootPending = 1;
            break;
        default:
            replyStatus(mIdx, seq, txn, DMR_PROV_RES_BAD_OP);
            break;
    }
    return 1;
}

void dmrProvRxResponse(uint32_t src, uint32_t dst, uint8_t status)
{
    if ((s_mPhase == M_IDLE) || (dst != trxDMRID) || (src != s_mSlave)) { return; }
    s_ackNs = (uint8_t)(status & 0x07);
    s_ackSrc = src;
    s_ackSeen = 1;
}

/* ---- master ------------------------------------------------------------------------- */
static int nextSeq(uint32_t *out)
{
    if (s_ownSeqNext >= DMR_PROV_SEQ_LIMIT) { return 0; }
    if (s_ownSeqNext >= s_ownSeqReserved)
    {
        s_ownSeqReserved = s_ownSeqNext + DMR_PROV_SEQ_RESERVE;
        if (!saveBlock()) { return 0; }
    }
    *out = s_ownSeqNext++;
    return 1;
}

/* Build the PDU in flight (item or the implicit COMMIT) with s_mSeqCur and load the bursts.
 * Byte-identical on every retry: same op bytes, same seq. Returns 1 when loaded. */
static int masterSend(void)
{
    uint8_t *pdu = s_job + DMR_PROV_ARENA_BYTES - PROV_PDU_SCRATCH;
    uint8_t commit[6];
    const uint8_t *body; int bodyLen;
    if (s_mCurIsCommit)
    {
        commit[0] = DMR_PROV_OP_COMMIT; commit[1] = s_mItemCount; wr32be(commit + 2, s_mCrc);
        body = commit; bodyLen = 6;
    }
    else
    {
        const uint8_t *it = s_job + s_mItemOff;
        int alen = rd16le(it + 1);
        /* the item is op(1) len(2) args: the frame body is op || args, built in place by
         * moving the op byte next to the args (the len field is restored after) */
        uint8_t *b = s_job + s_mItemOff + 2;
        uint8_t keep = b[0];
        b[0] = it[0];
        int n = dmrProvBuildPdu(trxDMRID, s_mSlave, s_mSeqCur, s_mTxn, 0, b, 1 + alen, pdu, PROV_PDU_SCRATCH);
        b[0] = keep;
        if (n <= 0) { return 0; }
        int nb = dmrProvBuildBursts(pdu, n, s_mSlave, trxDMRID, s_mSeqCur, dmrSmsBurstQueueScratch(), DMR_DATA_MAX_BURSTS);
        if (nb <= 0) { return 0; }
        s_ackSeen = 0; s_mStatusSeen = 0;
        dmrDataTxLoad(dmrSmsBurstQueueScratch(), (uint8_t)nb);
        s_mT0 = ticksGetMillis();
        return 1;
    }
    int n = dmrProvBuildPdu(trxDMRID, s_mSlave, s_mSeqCur, s_mTxn, 0, body, bodyLen, pdu, PROV_PDU_SCRATCH);
    if (n <= 0) { return 0; }
    int nb = dmrProvBuildBursts(pdu, n, s_mSlave, trxDMRID, s_mSeqCur, dmrSmsBurstQueueScratch(), DMR_DATA_MAX_BURSTS);
    if (nb <= 0) { return 0; }
    s_ackSeen = 0; s_mStatusSeen = 0;
    dmrDataTxLoad(dmrSmsBurstQueueScratch(), (uint8_t)nb);
    s_mT0 = ticksGetMillis();
    return 1;
}

static int curExpectsStatus(void)
{
    if (s_mCurIsCommit) { return 1; }
    uint8_t op = s_job[s_mItemOff];
    return (op == DMR_PROV_OP_STATUS) || (op == DMR_PROV_OP_REBOOT) || (op == DMR_PROV_OP_COMMIT);
}

static void masterFinish(int ok, uint8_t reason)
{
    char note[48];
    s_mLastOutcome = ok ? 1 : 2;
    s_mFailReason = reason;
    if (ok)
    {
        snprintf(note, sizeof note, "Prov %u: OK res %u crc %s", (unsigned)s_mSlave, s_mStatus[1],
                 (s_mStatusSeen && (rd32be(s_mStatus + 7) == s_mCrc)) ? "match" : "n/a");
    }
    else
    {
        snprintf(note, sizeof note, "Prov %u: FAILED (%s) at %u/%u", (unsigned)s_mSlave,
                 (reason == DMR_PROV_RES_TIMEOUT) ? "no reply" : "error", s_mItemIdx, s_mItemCount);
    }
    dmrProvApplyNotify(note);
    uint8_t keepStatus[DMR_PROV_STATUS_BODY_LEN]; memcpy(keepStatus, s_mStatus, sizeof keepStatus);
    uint8_t seen = s_mStatusSeen;
    masterReset();
    memcpy(s_mStatus, keepStatus, sizeof keepStatus); s_mStatusSeen = seen;
}

static int txDone(uint32_t since)
{
    return ((uint32_t)(ticksGetMillis() - since) >= PROV_TX_SETTLE_MS) && !dmrDataTxActive() && !trxIsTransmitting;
}

static void masterTick(void);
static void masterTick(void)
{
    uint32_t now = ticksGetMillis();
    switch (s_mPhase)
    {
        case M_NEXT:
            if (s_mItemIdx < s_mItemCount)
            {
                s_mCurIsCommit = 0;
            }
            else if ((s_mFlags & 0x01) && !s_mCommitSent)
            {
                s_mCurIsCommit = 1; s_mCommitSent = 1;
            }
            else
            {
                masterFinish(1, 0);
                return;
            }
            if (!nextSeq(&s_mSeqCur)) { masterFinish(0, DMR_PROV_RES_BAD_SEQ); return; }
            s_mTries = 1;
            if (!masterSend()) { masterFinish(0, DMR_PROV_RES_BAD_OP); return; }
            s_mPhase = M_SENDING;
            break;
        case M_SENDING:
            if (txDone(s_mT0)) { s_mPhase = M_WAIT_ACK; s_mT0 = now; }
            break;
        case M_WAIT_ACK:
            if (s_mStatusSeen || (s_ackSeen && (s_ackNs == (s_mSeqCur & 7))))
            {
                if (curExpectsStatus()) { s_mPhase = M_WAIT_STATUS; s_mT0 = now; }
                else
                {
                    s_mItemOff = (uint16_t)(s_mItemOff + 3 + rd16le(s_job + s_mItemOff + 1));
                    s_mItemIdx++;
                    s_mPhase = M_NEXT;
                    masterTick();   /* send the next PDU now, not a loop iteration later */
                }
                break;
            }
            if ((uint32_t)(now - s_mT0) >= PROV_ACK_TIMEOUT_MS)
            {
                if (s_mTries >= PROV_TRIES) { masterFinish(0, DMR_PROV_RES_TIMEOUT); return; }
                s_mTries++;
                if (!masterSend()) { masterFinish(0, DMR_PROV_RES_BAD_OP); return; }
                s_mPhase = M_SENDING;
            }
            break;
        case M_WAIT_STATUS:
            if (s_mStatusSeen)
            {
                if (s_mCurIsCommit || (s_job[s_mItemOff] == DMR_PROV_OP_REBOOT)) { masterFinish(s_mStatus[1] == DMR_PROV_RES_OK, s_mStatus[1]); return; }
                s_mItemOff = (uint16_t)(s_mItemOff + 3 + rd16le(s_job + s_mItemOff + 1));
                s_mItemIdx++;
                s_mPhase = M_NEXT;
                masterTick();
                break;
            }
            if ((uint32_t)(now - s_mT0) >= PROV_STATUS_TIMEOUT_MS)
            {
                if (s_mTries >= PROV_TRIES) { masterFinish(0, DMR_PROV_RES_TIMEOUT); return; }
                s_mTries++;
                if (!masterSend()) { masterFinish(0, DMR_PROV_RES_BAD_OP); return; }
                s_mPhase = M_SENDING;
            }
            break;
        default:
            break;
    }
}

int dmrProvJobLoad(const uint8_t *job, int len)
{
    if ((s_mPhase != M_IDLE) || (s_stage != NULL)) { return -1; }   /* a job runs, or we are staging as a slave */
    if (!s_paired) { return -2; }
    if (len < 6) { return -4; }
    if (len > DMR_PROV_ARENA_BYTES - PROV_PDU_SCRATCH) { return -5; }
    if (s_ownSeqNext >= DMR_PROV_SEQ_LIMIT) { return -6; }
    /* validate the item chain and count it */
    int off = 6, count = 0;
    uint32_t crc = 0;
    while (off < len)
    {
        if (off + 3 > len) { return -4; }
        int alen = rd16le(job + off + 1);
        uint8_t op = job[off];
        if ((off + 3 + alen > len) || (alen + 1 > DMR_PROV_BODY_MAX) || (count == 255)) { return -4; }
        int want = stagedArgLen(op);
        if (want >= 0) { if (alen != want) { return -4; } }
        else if (((op == DMR_PROV_OP_STATUS) || (op == DMR_PROV_OP_REBOOT) || (op == DMR_PROV_OP_ABORT)) && (alen == 0)) { }
        else { return -4; }
        crc = crc32_ieee(crc, job + off, 3 + alen);
        off += 3 + alen;
        count++;
    }
    if (count == 0) { return -4; }
    uint8_t *a = dmrProvApplyArenaClaim();
    if (a == NULL) { return -3; }
    memcpy(a, job, (size_t)len);
    s_job = a; s_jobLen = (uint16_t)len;
    s_mSlave = rd32le(job) & 0x00FFFFFFu;
    s_mTxn = job[4]; s_mFlags = job[5];
    s_mItemOff = 6; s_mItemIdx = 0; s_mItemCount = (uint8_t)count;
    s_mCrc = crc;
    s_mCommitSent = 0; s_mCurIsCommit = 0; s_mTries = 0;
    s_ackSeen = 0; s_mStatusSeen = 0; s_mLastOutcome = 0; s_mFailReason = 0;
    memset(s_mStatus, 0, sizeof s_mStatus);
    s_mPhase = M_NEXT;
    return 0;
}

void dmrProvJobCancel(void)
{
    if (s_mPhase != M_IDLE) { masterReset(); s_mLastOutcome = 2; s_mFailReason = DMR_PROV_RES_TIMEOUT; }
}

int dmrProvJobStatus(uint8_t *out, int max)
{
    int n = 12 + DMR_PROV_STATUS_BODY_LEN;
    if (max < n) { return 0; }
    out[0] = s_mPhase; out[1] = s_mItemIdx; out[2] = s_mItemCount; out[3] = s_mTries;
    out[4] = s_mLastOutcome; out[5] = s_mFailReason; out[6] = s_mStatusSeen; out[7] = s_ackSeen ? (uint8_t)(0x80 | s_ackNs) : 0;
    wr32be(out + 8, s_mSeqCur);
    memcpy(out + 12, s_mStatus, DMR_PROV_STATUS_BODY_LEN);
    return n;
}

/* ---- tick ----------------------------------------------------------------------------- */
void dmrProvTick(void)
{
    if (!s_paired) { return; }
    uint32_t now = ticksGetMillis();
    if (s_ackInhibit && ((int32_t)(now - s_holdoffUntil) >= 0)) { s_ackInhibit = 0; s_failStreak = 0; }
    if (s_txnActive && ((uint32_t)(now - s_stageLastMs) >= PROV_TXN_TIMEOUT_MS)) { stageDiscard(); }
    /* the slave's reply: one data call, only when the radio is free and may transmit */
    if (s_replyPending && s_replyLen && !dmrDataTxActive() && !trxIsTransmitting)
    {
        if (!dmrProvApplyTxAllowed()) { s_replyPending = 0; }
        else
        {
            int nb = dmrProvBuildBursts(s_replyPdu, s_replyLen, s_replyDst, trxDMRID, s_replySeq, dmrSmsBurstQueueScratch(), DMR_DATA_MAX_BURSTS);
            if (nb > 0) { dmrDataTxLoad(dmrSmsBurstQueueScratch(), (uint8_t)nb); s_replyLoadedMs = now; }
            s_replyPending = 0;
        }
    }
    if (s_rebootPending && !s_replyPending && txDone(s_replyLoadedMs))
    {
        s_rebootPending = 0;
        dmrProvApplyReboot();
    }
    if (s_mPhase != M_IDLE) { masterTick(); }
}

/* ---- diagnostics ------------------------------------------------------------------------ */
int dmrProvDiag(uint8_t *out, int max)
{
    int n = 28 + DMR_PROV_MAX_MASTERS * 12 + 8;
    if (max < n) { return 0; }
    out[0] = s_paired; out[1] = s_nMasters; out[2] = s_txnActive; out[3] = s_txn;
    out[4] = s_stageCount; out[5] = (uint8_t)(s_stageUsed >> 8); out[6] = (uint8_t)s_stageUsed; out[7] = s_stageErr;
    out[8] = (uint8_t)(s_failedMacs >> 8); out[9] = (uint8_t)s_failedMacs;
    out[10] = (uint8_t)(s_rejSeq >> 8); out[11] = (uint8_t)s_rejSeq;
    out[12] = (uint8_t)(s_rejAddr >> 8); out[13] = (uint8_t)s_rejAddr;
    out[14] = (uint8_t)(s_rejFrame >> 8); out[15] = (uint8_t)s_rejFrame;
    out[16] = (uint8_t)(s_commits >> 8); out[17] = (uint8_t)s_commits;
    wr32be(out + 18, s_lastCommitSeq);
    wr32be(out + 22, s_lastCodeplugCrc);
    out[26] = s_ackInhibit; out[27] = s_replyPending;
    for (int i = 0; i < DMR_PROV_MAX_MASTERS; i++)
    {
        wr32be(out + 28 + i * 12, s_masterId[i]);
        wr32be(out + 28 + i * 12 + 4, s_lastSeq[i]);
        wr32be(out + 28 + i * 12 + 8, s_lastSeen[i]);
    }
    wr32be(out + 28 + DMR_PROV_MAX_MASTERS * 12, s_ownSeqNext);
    wr32be(out + 28 + DMR_PROV_MAX_MASTERS * 12 + 4, s_ownSeqReserved);
    return n;
}

#endif /* ENABLE_REMOTE_PROVISION */

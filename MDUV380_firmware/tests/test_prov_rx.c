/* Host test of remote provisioning (functions/dmr_prov.c) through the REAL DMR SMS data path
 * (functions/dmr_sms.c): the master's frame and burst builders produce exactly what goes on
 * the air, the bursts are played into dmrSmsRxBurst() as the HR-C6000 ISR does, the main loop
 * tick (dmrSmsRxTick -> dmrProvRxPdu, dmrProvTick) does the rest, and the codeplug writers are
 * stubs that record what would have reached flash.
 *
 * Covers (PROVISIONING.md §10): a multi-PDU transaction committing in order with a verified
 * STATUS reply; replay, reorder, MAC failure, wrong slave ID (redirect), wrong master ID,
 * truncation, transaction splice and a wrong COMMIT CRC all rejected without touching the
 * staging area; the failed-MAC hold-off; the cached reply for a retried COMMIT; the link-layer
 * ACK gate; an encrypted SMS still decoding next to provisioning; the master state machine
 * (job -> PDUs -> ACK -> COMMIT -> signed STATUS, retries, failure).
 *
 * Build: gcc -O2 -DENABLE_DMR_DATA -DENABLE_REMOTE_PROVISION -Itests/stubs -Iapplication/include \
 *          -o /tmp/test_prov_rx tests/test_prov_rx.c application/source/functions/dmr_prov.c \
 *          application/source/functions/dmr_sms.c application/source/crypto/dmr_aes_hook.c \
 *          application/source/crypto/dmr_aes.c application/source/crypto/dmr_cmac.c && /tmp/test_prov_rx
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto/dmr_aes.h"
#include "crypto/dmr_aes_hook.h"
#include "crypto/dmr_cmac.h"
#include "functions/dmr_sms.h"
#include "functions/dmr_data.h"
#include "functions/dmr_prov.h"
#include "functions/codeplug.h"
#include "functions/settings.h"
#include "functions/ticks.h"
#include "functions/trx.h"
#include "hardware/SPI_Flash.h"
#include "user_interface/menuSystem.h"

#define MASTER_ID  1234567u
#define SLAVE_ID   7654321u
#define OTHER_ID   1111111u

/* ---- firmware stand-ins ---------------------------------------------------- */
volatile bool trxTransmissionEnabled = false;
volatile bool trxIsTransmitting = false;
uint32_t trxDMRID = SLAVE_ID;
CodeplugChannel_t *currentChannelData = NULL;

static uint8_t  s_flashBlock[DMR_PROV_BLOCK_LEN];   /* the PROV block "in flash" */
static int      s_flashBlockValid = 0;
static int      s_blockWrites = 0;

bool SPI_Flash_read(uint32_t a, uint8_t *b, int n) { (void)a; memset(b, 0xFF, (size_t)n); return true; }
bool SPI_Flash_write(uint32_t a, uint8_t *b, int n) { (void)a; (void)b; (void)n; return true; }
bool codeplugGetOpenGD77CustomData(CodeplugCustomDataType_t t, uint8_t *b) { (void)t; (void)b; return false; }
bool codeplugGetOpenGD77CustomDataBounded(CodeplugCustomDataType_t t, uint8_t *b, int n) { (void)t; (void)b; (void)n; return false; }
bool codeplugSetOpenGD77CustomData(CodeplugCustomDataType_t t, uint8_t *b, int n) { (void)t; (void)b; (void)n; return true; }
uint8_t codeplugChannelGetFlag(CodeplugChannel_t *c, ChannelFlag_t f) { (void)c; (void)f; return 0; }
static uint32_t s_nowMs = 1000;
uint32_t ticksGetMillis(void) { return s_nowMs; }
static void clockAdvance(uint32_t ms) { s_nowMs += ms; }

static int  notifications = 0;
static char lastNote[80];
void uiNotificationShow(uiNotificationType_t type, uiNotificationID_t id, uint32_t ms, const char *msg, bool now)
{ (void)type; (void)id; (void)ms; (void)now; notifications++; snprintf(lastNote, sizeof lastNote, "%s", msg); }

/* data-TX harness: capture the queued burst stream (count * [type, 12 payload bytes]) */
static uint8_t s_q[DMR_DATA_MAX_BURSTS * 13];
static int     s_qn = 0;
static int     s_txLoads = 0;
void dmrDataTxLoad(const uint8_t *bursts, uint8_t count) { memcpy(s_q, bursts, (size_t)count * 13); s_qn = count; s_txLoads++; }
int  dmrDataTxActive(void) { return 0; }
int  dmrDataTxNextBurst(uint8_t *t, uint8_t *p) { (void)t; (void)p; return 0; }
void dmrDataTxEnd(void) { }

/* ---- the apply layer: records what would have been written -------------------- */
typedef struct { uint8_t op; uint16_t idx; uint8_t data[180]; int len; } written_t;
static written_t s_written[16];
static int s_nWritten = 0;
static int s_settingsSaves = 0, s_reboots = 0, s_arenaBusy = 0, s_arenaHeld = 0, s_txAllowed = 1;
static uint8_t s_arena[DMR_PROV_ARENA_BYTES];

static void record(uint8_t op, uint16_t idx, const uint8_t *d, int len)
{
    if (s_nWritten < 16) { s_written[s_nWritten].op = op; s_written[s_nWritten].idx = idx; memcpy(s_written[s_nWritten].data, d, (size_t)len); s_written[s_nWritten].len = len; }
    s_nWritten++;
}
int  dmrProvApplyChannel(uint16_t index, const uint8_t *r) { if (index == 999) { return 0; } record(DMR_PROV_OP_WRITE_CHANNEL, index, r, 56); return 1; }
int  dmrProvApplyZone(uint8_t zone, const uint8_t *r) { record(DMR_PROV_OP_WRITE_ZONE, zone, r, 176); return 1; }
int  dmrProvApplyVfo(uint8_t vfo, const uint8_t *r) { record(DMR_PROV_OP_SET_VFO, vfo, r, 56); return 1; }
int  dmrProvApplySetting(uint8_t field, uint32_t value) { uint8_t v[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) }; record(DMR_PROV_OP_SET_SETTING, field, v, 4); return 1; }
int  dmrProvApplySettingsSave(void) { s_settingsSaves++; return 1; }
int  dmrProvApplyStoreKey(uint8_t slot, const uint8_t *k) { record(DMR_PROV_OP_STORE_KEY, slot, k, 32); return 1; }
void dmrProvApplyReboot(void) { s_reboots++; }
int  dmrProvApplyReadBack(uint8_t op, const uint8_t *body, int bodyLen, uint8_t *out, int max)
{
    (void)bodyLen; (void)max;
    /* "flash" holds what was written: find the record */
    for (int i = 0; i < s_nWritten && i < 16; i++)
    {
        uint16_t idx = (op == DMR_PROV_OP_WRITE_CHANNEL) ? (uint16_t)((body[0] << 8) | body[1]) : body[0];
        if ((s_written[i].op == op) && (s_written[i].idx == idx) && (op != DMR_PROV_OP_STORE_KEY))
        {
            memcpy(out, s_written[i].data, (size_t)s_written[i].len);
            return s_written[i].len;
        }
    }
    return 0;
}
int  dmrProvApplyTxAllowed(void) { return s_txAllowed; }
void dmrProvApplyNotify(const char *text) { uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 0, text, true); }
uint16_t dmrProvApplyBatteryCentivolts(void) { return 781; }
const char *dmrProvApplyGitHash(void) { return "abc1234"; }
int  dmrProvApplyBlockRead(uint8_t *b) { if (!s_flashBlockValid) { return 0; } memcpy(b, s_flashBlock, DMR_PROV_BLOCK_LEN); return 1; }
int  dmrProvApplyBlockWrite(const uint8_t *b) { memcpy(s_flashBlock, b, DMR_PROV_BLOCK_LEN); s_flashBlockValid = 1; s_blockWrites++; return 1; }
uint8_t *dmrProvApplyArenaClaim(void) { if (s_arenaBusy || s_arenaHeld) { return NULL; } s_arenaHeld = 1; memset(s_arena, 0, sizeof s_arena); return s_arena; }
void dmrProvApplyArenaRelease(void) { s_arenaHeld = 0; }

/* ---- test scaffolding ---------------------------------------------------------- */
static int fails = 0;
static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { fails++; }
}

static const uint8_t PAIR_KEY[32] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 };
static uint8_t s_kEnc[32], s_kMac[32];

static uint32_t rd32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint32_t crc32_ieee(uint32_t crc, const uint8_t *p, int len)
{
    crc = ~crc;
    for (int i = 0; i < len; i++) { crc ^= p[i]; for (int k = 0; k < 8; k++) { crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u))); } }
    return ~crc;
}

/* Write the PROV block "into flash" and boot the module: key, one master, counters 0. */
static void bootAs(uint32_t ownId, uint32_t masterId, uint32_t lastSeq)
{
    memset(s_flashBlock, 0, sizeof s_flashBlock);
    memcpy(s_flashBlock, "PROV", 4); s_flashBlock[4] = 1; s_flashBlock[5] = 1;
    memcpy(s_flashBlock + 8, PAIR_KEY, 32);
    wr32le(s_flashBlock + 40, masterId); wr32le(s_flashBlock + 44, lastSeq);
    s_flashBlockValid = 1;
    trxDMRID = ownId;
    s_arenaHeld = 0; s_arenaBusy = 0; s_txAllowed = 1;
    dmrAesInit(); dmrSmsInit(); dmrProvInit();
    s_nWritten = 0; s_settingsSaves = 0; s_reboots = 0; s_blockWrites = 0; s_qn = 0; s_txLoads = 0; notifications = 0;
    aes256_kdf108(PAIR_KEY, "enc", s_kEnc); aes256_kdf108(PAIR_KEY, "mac", s_kMac);
}

/* diag snapshot fields */
typedef struct { int paired, txnActive, stagedCount, stageErr, failedMacs, rejSeq, rejAddr, rejFrame, commits, ackInhibit; uint32_t lastCommitSeq, cpCrc, lastSeq0, lastSeen0, ownNext, ownRes; } diag_t;
static diag_t diag(void)
{
    uint8_t d[128]; diag_t r;
    int n = dmrProvDiag(d, sizeof d);
    if (n <= 0) { memset(&r, 0, sizeof r); return r; }
    r.paired = d[0]; r.txnActive = d[2]; r.stagedCount = d[4]; r.stageErr = d[7];
    r.failedMacs = (d[8] << 8) | d[9]; r.rejSeq = (d[10] << 8) | d[11]; r.rejAddr = (d[12] << 8) | d[13];
    r.rejFrame = (d[14] << 8) | d[15]; r.commits = (d[16] << 8) | d[17]; r.lastCommitSeq = rd32be(d + 18); r.cpCrc = rd32be(d + 22);
    r.ackInhibit = d[26]; r.lastSeq0 = rd32be(d + 32); r.lastSeen0 = rd32be(d + 36);
    r.ownNext = rd32be(d + 28 + 48); r.ownRes = rd32be(d + 28 + 52);
    return r;
}

/* Play a burst queue into the RX path as the ISR and main loop would; returns 1 when the
 * ISR built a link-layer ACK for it. */
static int play(const uint8_t *q, int n)
{
    uint8_t ack[12]; int acked = 0;
    for (int i = 0; i < n; i++)
    {
        uint8_t type = q[i * 13];
        const uint8_t *p = q + i * 13 + 1;
        if (type == 0x60) { dmrSmsRxBurst(6, p); }
        else if (type == 0x70) { dmrSmsRxBurst(7, p); if (dmrSmsAckTake(ack)) { acked = 1; } }
    }
    dmrSmsRxReset();
    dmrSmsRxTick();
    dmrProvTick();
    return acked;
}

/* Build a master request as the master does and return its burst count in q. */
static int request(uint8_t *q, uint32_t master, uint32_t slave, uint32_t seq, uint8_t txn, const uint8_t *body, int bodyLen)
{
    uint8_t pdu[DMR_PROV_PDU_MAX];
    int n = dmrProvBuildPdu(master, slave, seq, txn, 0, body, bodyLen, pdu, sizeof pdu);
    if (n <= 0) { return n; }
    return dmrProvBuildBursts(pdu, n, slave, master, seq, q, DMR_DATA_MAX_BURSTS);
}

/* The PDU carried by a captured burst queue (confirmed rate-1/2 blocks: 10 bytes behind
 * the DBSN/CRC-9 field). Returns its length. */
static int pduFromBursts(const uint8_t *q, int n, uint8_t *pdu, uint8_t *hdr)
{
    int len = 0;
    for (int i = 0; i < n; i++)
    {
        uint8_t type = q[i * 13];
        const uint8_t *p = q + i * 13 + 1;
        if (type == 0x60 && hdr) { memcpy(hdr, p, 12); }
        if (type == 0x70) { memcpy(pdu + len, p + 2, 10); len += 10; }
    }
    return len;
}

/* Verify + decrypt a frame with the test's own keys. Returns the body length, <0 on failure. */
static int openFrame(uint8_t *pdu, int len, uint32_t master, uint32_t slave, uint32_t *seq, uint8_t *txn, int *reply)
{
    if ((len < 30) || (pdu[0] != 0x50) || (pdu[1] != 0x01)) { return -1; }
    int plen = (pdu[8] << 8) | pdu[9];
    if (10 + plen + 16 + 4 > len) { return -1; }
    uint8_t assoc[8], nonce[13], tag[16];
    wr32be(assoc, master); wr32be(assoc + 4, slave);
    aes256_cmac2(s_kMac, assoc, 8, pdu, (size_t)(10 + plen), tag);
    if (memcmp(tag, pdu + 10 + plen, 16) != 0) { return -2; }
    *seq = rd32be(pdu + 4); *txn = pdu[2]; *reply = pdu[3] & 1;
    memcpy(nonce, assoc, 8); wr32be(nonce + 8, *seq); nonce[12] = (uint8_t)*reply;
    aes256_ctr_crypt(s_kEnc, nonce, pdu + 10, (size_t)plen);
    return plen;
}

/* item helpers (op || args), and the staged-image bytes for the COMMIT crc */
static int itemChannel(uint8_t *body, uint16_t idx, uint8_t fill)
{
    body[0] = DMR_PROV_OP_WRITE_CHANNEL; body[1] = (uint8_t)(idx >> 8); body[2] = (uint8_t)idx;
    for (int i = 0; i < 56; i++) { body[3 + i] = (uint8_t)(fill + i); }
    return 59;
}
static int itemZone(uint8_t *body, uint8_t zone)
{
    body[0] = DMR_PROV_OP_WRITE_ZONE; body[1] = zone;
    memset(body + 2, 0xFF, 16); memcpy(body + 2, "Fleet", 5);
    for (int i = 0; i < 80; i++) { body[18 + 2 * i] = (uint8_t)((i < 3) ? (i + 1) : 0); body[19 + 2 * i] = 0; }
    return 178;
}
static int itemSetting(uint8_t *body, uint8_t field, uint32_t v) { body[0] = DMR_PROV_OP_SET_SETTING; body[1] = field; wr32le(body + 2, v); return 6; }
static int itemKey(uint8_t *body, uint8_t slot) { body[0] = DMR_PROV_OP_STORE_KEY; body[1] = slot; for (int i = 0; i < 32; i++) { body[2 + i] = (uint8_t)(0xA0 + i); } return 34; }
static int itemCommit(uint8_t *body, uint8_t count, uint32_t crc) { body[0] = DMR_PROV_OP_COMMIT; body[1] = count; wr32be(body + 2, crc); return 6; }
static uint32_t stageCrc(uint32_t crc, const uint8_t *body, int bodyLen)
{
    uint8_t it[3] = { body[0], (uint8_t)((bodyLen - 1) & 0xFF), (uint8_t)((bodyLen - 1) >> 8) };
    crc = crc32_ieee(crc, it, 3);
    return crc32_ieee(crc, body + 1, bodyLen - 1);
}

/* The slave's last transmitted reply, verified with the test keys. Returns body length. */
static int lastReply(uint8_t *body, uint32_t *seq, uint8_t *txn)
{
    uint8_t pdu[DMR_PROV_PDU_MAX], hdr[12]; int reply = 0;
    int n = pduFromBursts(s_q, s_qn, pdu, hdr);
    if (n <= 0) { return -1; }
    uint32_t dst = ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 8) | hdr[4];
    uint32_t src = ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];
    if ((dst != MASTER_ID) || (src != SLAVE_ID) || ((hdr[1] >> 4) != 9) || ((hdr[0] & 0x4F) != 0x43)) { return -3; }
    int r = openFrame(pdu, n, MASTER_ID, SLAVE_ID, seq, txn, &reply);
    if (r < 0) { return r; }
    if (!reply) { return -4; }
    memcpy(body, pdu + 10, (size_t)r);
    return r;
}

/* ---- tests ------------------------------------------------------------------------------- */
static uint32_t s_seq = 0;
static uint32_t nextSeq(void) { return ++s_seq; }

static void testTransactionCommits(void)
{
    uint8_t q[DMR_DATA_MAX_BURSTS * 13], body[DMR_PROV_BODY_MAX];
    uint32_t crc = 0;
    int n, acked;
    bootAs(SLAVE_ID, MASTER_ID, 0); s_seq = 0;

    n = itemChannel(body, 5, 0x10); crc = stageCrc(crc, body, n);
    acked = play(q, request(q, MASTER_ID, SLAVE_ID, nextSeq(), 7, body, n));
    check(acked, "a provisioning PDU from the master is link-acknowledged by the ISR");
    check(diag().txnActive == 1 && diag().stagedCount == 1 && s_nWritten == 0, "first item staged, nothing written yet");
    n = itemChannel(body, 300, 0x40); crc = stageCrc(crc, body, n);
    play(q, request(q, MASTER_ID, SLAVE_ID, nextSeq(), 7, body, n));
    n = itemZone(body, 2); crc = stageCrc(crc, body, n);
    play(q, request(q, MASTER_ID, SLAVE_ID, nextSeq(), 7, body, n));
    n = itemSetting(body, 3, 0x11223344); crc = stageCrc(crc, body, n);
    play(q, request(q, MASTER_ID, SLAVE_ID, nextSeq(), 7, body, n));
    n = itemKey(body, 4); crc = stageCrc(crc, body, n);
    play(q, request(q, MASTER_ID, SLAVE_ID, nextSeq(), 7, body, n));
    check(diag().stagedCount == 5 && s_nWritten == 0 && s_blockWrites == 0, "five items staged, nothing written, no flash");

    uint32_t commitSeq = nextSeq();
    n = itemCommit(body, 5, crc);
    s_txLoads = 0;
    play(q, request(q, MASTER_ID, SLAVE_ID, commitSeq, 7, body, n));
    check(s_nWritten == 5, "COMMIT writes the five items");
    check(s_written[0].op == DMR_PROV_OP_WRITE_CHANNEL && s_written[0].idx == 5 && s_written[0].data[0] == 0x10 && s_written[0].data[55] == 0x10 + 55, "channel 5 written with its 56 bytes");
    check(s_written[1].op == DMR_PROV_OP_WRITE_CHANNEL && s_written[1].idx == 300 && s_written[1].data[0] == 0x40, "channel 300 written second");
    check(s_written[2].op == DMR_PROV_OP_WRITE_ZONE && s_written[2].idx == 2 && memcmp(s_written[2].data, "Fleet", 5) == 0 && s_written[2].data[16] == 1 && s_written[2].data[18] == 2, "zone 2 written third with its name and channels");
    check(s_written[3].op == DMR_PROV_OP_SET_SETTING && s_written[3].idx == 3 && s_written[3].data[0] == 0x44 && s_written[3].data[3] == 0x11 && s_settingsSaves == 1, "setting 3 written and the settings saved once");
    check(s_written[4].op == DMR_PROV_OP_STORE_KEY && s_written[4].idx == 4 && s_written[4].data[0] == 0xA0, "voice key stored in slot 4");
    check(s_blockWrites == 1 && diag().lastSeq0 == commitSeq && diag().lastCommitSeq == commitSeq, "the COMMIT seq is persisted in the PROV block");
    check(diag().txnActive == 0 && !s_arenaHeld && diag().commits == 1, "transaction closed, arena released");
    check(s_txLoads == 1, "exactly one reply transmission");

    uint8_t rb[32]; uint32_t rseq; uint8_t rtxn;
    int rl = lastReply(rb, &rseq, &rtxn);
    check(rl == DMR_PROV_STATUS_BODY_LEN && rb[0] == DMR_PROV_OP_STATUS_REPLY, "the reply is a STATUS_REPLY that verifies under the master/slave MAC");
    check(rseq == commitSeq && rtxn == 7 && rb[1] == DMR_PROV_RES_OK && rb[2] == 7, "reply: result OK, bound to the COMMIT's seq and txn");
    check(rd32be(rb + 3) == commitSeq, "reply carries the persisted lastSeq");
    /* codeplug CRC: over the records read back (channel 5, channel 300, zone, setting) */
    uint32_t want = 0;
    want = crc32_ieee(want, s_written[0].data, 56); want = crc32_ieee(want, s_written[1].data, 56);
    want = crc32_ieee(want, s_written[2].data, 176); want = crc32_ieee(want, s_written[3].data, 4);
    check(rd32be(rb + 7) == want, "reply codeplug CRC = CRC32 of the committed records read back");
    check(memcmp(rb + 11, "abc1234", 7) == 0 && ((rb[19] << 8) | rb[20]) == 781 && rb[21] == 5, "reply carries git hash, battery, staged count");
    /* the reply's own framing: individual confirmed data header, SAP 9, N(S) = seq & 7 */
    uint8_t pdu[DMR_PROV_PDU_MAX], hdr[12];
    int pl = pduFromBursts(s_q, s_qn, pdu, hdr);
    check((pl % 10) == 0 && (hdr[9] >> 4) == (commitSeq & 7) && (hdr[8] & 0x7F) == pl / 10, "reply header: block count and N(S) match");
    /* each block's CRC-9 and DBSN as the builder defines them */
    int c9ok = 1, b = 0;
    for (int i = 0; i < s_qn; i++)
    {
        const uint8_t *p = s_q + i * 13 + 1;
        if (s_q[i * 13] != 0x70) { continue; }
        uint16_t c9 = dmrProvCrc9(p + 2, 10, (uint8_t)b);
        if (((p[0] >> 1) != b) || ((p[0] & 1) != ((c9 >> 8) & 1)) || (p[1] != (c9 & 0xFF))) { c9ok = 0; }
        b++;
    }
    check(c9ok, "blocks carry DBSN 0.. and their CRC-9");

    /* the master's retry of the COMMIT (it missed the reply): the cached reply, byte for byte */
    uint8_t keep[DMR_DATA_MAX_BURSTS * 13]; int keepN = s_qn; memcpy(keep, s_q, (size_t)s_qn * 13);
    s_txLoads = 0; s_nWritten = 0;
    play(q, request(q, MASTER_ID, SLAVE_ID, commitSeq, 7, body, n));
    check(s_txLoads == 1 && s_qn == keepN && memcmp(keep, s_q, (size_t)keepN * 13) == 0 && s_nWritten == 0 && s_blockWrites == 1,
          "a retried COMMIT gets the identical cached reply, nothing written again");
}

static void testReplayAndReorder(void)
{
    uint8_t q[DMR_DATA_MAX_BURSTS * 13], body[DMR_PROV_BODY_MAX];
    int n;
    bootAs(SLAVE_ID, MASTER_ID, 100);
    n = itemChannel(body, 1, 0);
    play(q, request(q, MASTER_ID, SLAVE_ID, 101, 1, body, n));
    check(diag().stagedCount == 1, "seq 101 > stored 100 accepted");
    play(q, request(q, MASTER_ID, SLAVE_ID, 101, 1, body, n));
    check(diag().stagedCount == 1 && diag().rejSeq == 1, "replay of seq 101 rejected, staging untouched");
    play(q, request(q, MASTER_ID, SLAVE_ID, 100, 1, body, n));
    check(diag().stagedCount == 1 && diag().rejSeq == 2, "seq 100 (<= stored) rejected");
    play(q, request(q, MASTER_ID, SLAVE_ID, 105, 1, body, n));
    play(q, request(q, MASTER_ID, SLAVE_ID, 103, 1, body, n));
    check(diag().stagedCount == 2 && diag().rejSeq == 3 && diag().lastSeen0 == 105, "a reordered (lower) seq after 105 is rejected");
    play(q, request(q, MASTER_ID, SLAVE_ID, 0xFFFFFFFFu, 1, body, n));
    check(diag().stagedCount == 2 && diag().rejSeq == 4, "seq 0xFFFFFFFF never accepted");
    check(s_blockWrites == 0, "no flash write without a commit");
    /* a reboot forgets lastSeen but keeps lastSeq: 102 is accepted again (re-staged, never committed) */
    s_arenaHeld = 0; dmrProvInit();
    play(q, request(q, MASTER_ID, SLAVE_ID, 102, 1, body, n));
    check(diag().stagedCount == 1 && diag().lastSeq0 == 100, "after a reboot an uncommitted seq re-stages (lastSeq still 100)");
}

static void testMacFailureAndHoldoff(void)
{
    uint8_t q[DMR_DATA_MAX_BURSTS * 13], body[DMR_PROV_BODY_MAX], pdu[DMR_PROV_PDU_MAX];
    int n, acked;
    bootAs(SLAVE_ID, MASTER_ID, 0);
    n = itemChannel(body, 1, 0);
    /* flip one ciphertext bit, re-frame */
    int pl = dmrProvBuildPdu(MASTER_ID, SLAVE_ID, 1, 1, 0, body, n, pdu, sizeof pdu);
    pdu[12] ^= 0x01;
    uint32_t c = dmrSmsCrc32(pdu, pl); wr32be(pdu + pl - 4, c);
    acked = play(q, dmrProvBuildBursts(pdu, pl, SLAVE_ID, MASTER_ID, 1, q, DMR_DATA_MAX_BURSTS));
    check(acked && diag().failedMacs == 1 && diag().txnActive == 0 && diag().stagedCount == 0, "bad MAC: counted, nothing staged (the ISR ACK had already gone out)");
    /* tag bit flip */
    pl = dmrProvBuildPdu(MASTER_ID, SLAVE_ID, 2, 1, 0, body, n, pdu, sizeof pdu);
    pdu[pl - 5] ^= 0x80;   /* last tag byte lives just before the padding/CRC for this size? recompute CRC anyway */
    pdu[10 + n + 15] ^= 0x80;
    c = dmrSmsCrc32(pdu, pl); wr32be(pdu + pl - 4, c);
    play(q, dmrProvBuildBursts(pdu, pl, SLAVE_ID, MASTER_ID, 2, q, DMR_DATA_MAX_BURSTS));
    /* header field change (txn) */
    pl = dmrProvBuildPdu(MASTER_ID, SLAVE_ID, 3, 1, 0, body, n, pdu, sizeof pdu);
    pdu[2] = 9; c = dmrSmsCrc32(pdu, pl); wr32be(pdu + pl - 4, c);
    play(q, dmrProvBuildBursts(pdu, pl, SLAVE_ID, MASTER_ID, 3, q, DMR_DATA_MAX_BURSTS));
    /* sequence field change */
    pl = dmrProvBuildPdu(MASTER_ID, SLAVE_ID, 4, 1, 0, body, n, pdu, sizeof pdu);
    pdu[7] ^= 1; c = dmrSmsCrc32(pdu, pl); wr32be(pdu + pl - 4, c);
    play(q, dmrProvBuildBursts(pdu, pl, SLAVE_ID, MASTER_ID, 4, q, DMR_DATA_MAX_BURSTS));
    check(diag().failedMacs == 4 && diag().stagedCount == 0 && diag().lastSeen0 == 0, "tag, txn and seq tampering all fail the MAC; seq never advanced");
    check(diag().ackInhibit == 1, "four consecutive MAC failures start the ACK hold-off");
    /* a request with the reply flag set is a misaddressed reply (no job runs), not a request */
    pl = dmrProvBuildPdu(MASTER_ID, SLAVE_ID, 4, 1, 0, body, n, pdu, sizeof pdu);
    pdu[3] = 1; c = dmrSmsCrc32(pdu, pl); wr32be(pdu + pl - 4, c);
    play(q, dmrProvBuildBursts(pdu, pl, SLAVE_ID, MASTER_ID, 4, q, DMR_DATA_MAX_BURSTS));
    check(diag().rejAddr == 1 && diag().stagedCount == 0, "a request carrying the reply flag is dropped");
    acked = play(q, request(q, MASTER_ID, SLAVE_ID, 5, 1, body, n));
    check(!acked, "during the hold-off a (good) PDU is not link-acknowledged");
    check(diag().stagedCount == 1, "... but a good PDU is still processed");
    clockAdvance(11000); dmrProvTick();
    acked = play(q, request(q, MASTER_ID, SLAVE_ID, 6, 1, body, n));
    check(diag().ackInhibit == 0 && acked, "the hold-off ends after 10 s and ACKs resume");
}

static void testAddressing(void)
{
    uint8_t q[DMR_DATA_MAX_BURSTS * 13], body[DMR_PROV_BODY_MAX], pdu[DMR_PROV_PDU_MAX];
    int n, acked;
    bootAs(SLAVE_ID, MASTER_ID, 0);
    n = itemChannel(body, 1, 0);
    /* to another slave: the data path drops it before the module sees it */
    acked = play(q, request(q, MASTER_ID, OTHER_ID, 1, 1, body, n));
    check(!acked && diag().stagedCount == 0 && diag().rejAddr == 0 && diag().failedMacs == 0, "a PDU addressed to another slave is ignored entirely");
    /* redirect: a PDU built for OTHER_ID re-addressed on the air to us */
    int pl = dmrProvBuildPdu(MASTER_ID, OTHER_ID, 2, 1, 0, body, n, pdu, sizeof pdu);
    play(q, dmrProvBuildBursts(pdu, pl, SLAVE_ID, MASTER_ID, 2, q, DMR_DATA_MAX_BURSTS));
    check(diag().stagedCount == 0 && diag().failedMacs == 1, "a PDU redirected from another slave's ID fails the MAC");
    /* from a source that is not a configured master, even with a valid MAC for that source */
    pl = dmrProvBuildPdu(OTHER_ID, SLAVE_ID, 3, 1, 0, body, n, pdu, sizeof pdu);
    acked = play(q, dmrProvBuildBursts(pdu, pl, SLAVE_ID, OTHER_ID, 3, q, DMR_DATA_MAX_BURSTS));
    check(!acked && diag().stagedCount == 0 && diag().rejAddr == 1, "a PDU from an unconfigured master ID gets no ACK and is rejected");
    /* a group (talkgroup) PDU: never ours */
    pl = dmrProvBuildPdu(MASTER_ID, SLAVE_ID, 4, 1, 0, body, n, pdu, sizeof pdu);
    int nb = dmrProvBuildBursts(pdu, pl, SLAVE_ID, MASTER_ID, 4, q, DMR_DATA_MAX_BURSTS);
    for (int i = 0; i < nb; i++) { if (q[i * 13] == 0x60) { q[i * 13 + 1] |= 0x80; dmrSmsHdrCrc(q + i * 13 + 1, 10, 0xCCCC, q + i * 13 + 11); } }
    acked = play(q, nb);
    check(!acked && diag().stagedCount == 0, "a PDU sent as a group call is not acted on");
    /* the spoofed master ID from someone without the key: ACKed at the link layer, nothing else */
    memset(pdu, 0x5A, sizeof pdu); pdu[0] = 0x50; pdu[1] = 0x01; pdu[3] = 0; pdu[8] = 0; pdu[9] = 59;
    wr32be(pdu + 86, dmrSmsCrc32(pdu, 90));
    acked = play(q, dmrProvBuildBursts(pdu, 90, SLAVE_ID, MASTER_ID, 5, q, DMR_DATA_MAX_BURSTS));
    check(acked && diag().stagedCount == 0 && diag().failedMacs == 2, "a forged frame under a configured master ID: link ACK only, MAC fails, nothing staged");
}

static void testTruncatedAndMalformed(void)
{
    uint8_t q[DMR_DATA_MAX_BURSTS * 13], body[DMR_PROV_BODY_MAX], pdu[DMR_PROV_PDU_MAX];
    int n;
    bootAs(SLAVE_ID, MASTER_ID, 0);
    n = itemChannel(body, 1, 0);
    int nb = request(q, MASTER_ID, SLAVE_ID, 1, 1, body, n);
    play(q, nb - 1);     /* last block lost */
    check(diag().stagedCount == 0 && diag().failedMacs == 0 && diag().rejFrame == 0, "a PDU missing its last block never completes (CRC32), module not called");
    /* a frame whose length field claims more than the PDU holds */
    int pl = dmrProvBuildPdu(MASTER_ID, SLAVE_ID, 2, 1, 0, body, n, pdu, sizeof pdu);
    pdu[9] = (uint8_t)(n + 40); wr32be(pdu + pl - 4, dmrSmsCrc32(pdu, pl));
    play(q, dmrProvBuildBursts(pdu, pl, SLAVE_ID, MASTER_ID, 2, q, DMR_DATA_MAX_BURSTS));
    check(diag().stagedCount == 0 && diag().rejFrame == 1 && dmrSmsCount(0) == 0, "an inconsistent length is rejected as a bad frame and never reaches the inbox");
    /* a staged op with the wrong argument length poisons the transaction, COMMIT says BAD_OP */
    body[0] = DMR_PROV_OP_WRITE_CHANNEL; body[1] = 0; body[2] = 1;
    play(q, request(q, MASTER_ID, SLAVE_ID, 3, 1, body, 10));
    check(diag().txnActive == 1 && diag().stageErr == DMR_PROV_RES_BAD_OP, "wrong argument length: transaction marked BAD_OP");
    n = itemCommit(body, 0, 0);
    play(q, request(q, MASTER_ID, SLAVE_ID, 4, 1, body, n));
    uint8_t rb[32]; uint32_t rseq; uint8_t rtxn;
    check(lastReply(rb, &rseq, &rtxn) > 0 && rb[1] == DMR_PROV_RES_BAD_OP && s_nWritten == 0, "COMMIT answered BAD_OP, nothing written");
    /* unknown op */
    body[0] = 0x77;
    play(q, request(q, MASTER_ID, SLAVE_ID, 5, 1, body, 1));
    check(lastReply(rb, &rseq, &rtxn) > 0 && rb[1] == DMR_PROV_RES_BAD_OP && rseq == 5, "an unknown op is answered BAD_OP");
}

static void testTransactionIntegrity(void)
{
    uint8_t q[DMR_DATA_MAX_BURSTS * 13], body[DMR_PROV_BODY_MAX], rb[32];
    uint32_t crc = 0, rseq; uint8_t rtxn;
    int n;
    bootAs(SLAVE_ID, MASTER_ID, 0);
    n = itemChannel(body, 1, 0); crc = stageCrc(crc, body, n);
    play(q, request(q, MASTER_ID, SLAVE_ID, 1, 1, body, n));
    /* COMMIT for another transaction id */
    n = itemCommit(body, 1, crc);
    play(q, request(q, MASTER_ID, SLAVE_ID, 2, 2, body, n));
    check(lastReply(rb, &rseq, &rtxn) > 0 && rb[1] == DMR_PROV_RES_TXN_MISMATCH && s_nWritten == 0 && diag().txnActive == 0, "COMMIT with the wrong txn: TXN_MISMATCH, nothing written, image dropped");
    /* an item of a new txn supersedes the staged one */
    n = itemChannel(body, 1, 0); crc = stageCrc(0, body, n);
    play(q, request(q, MASTER_ID, SLAVE_ID, 3, 5, body, n));
    n = itemChannel(body, 2, 0);
    play(q, request(q, MASTER_ID, SLAVE_ID, 4, 6, body, n));
    check(diag().stagedCount == 1 && diag().txnActive == 1, "an item of a different txn restarts staging (spliced transactions cannot merge)");
    /* wrong CRC in COMMIT */
    crc = stageCrc(0, body, n);
    n = itemCommit(body, 1, crc ^ 1);
    play(q, request(q, MASTER_ID, SLAVE_ID, 5, 6, body, n));
    check(lastReply(rb, &rseq, &rtxn) > 0 && rb[1] == DMR_PROV_RES_CRC_MISMATCH && s_nWritten == 0 && diag().txnActive == 0 && diag().lastSeq0 == 5,
          "COMMIT with a wrong image CRC: CRC_MISMATCH, nothing written, seq still spent");
    /* wrong count */
    n = itemChannel(body, 2, 0); crc = stageCrc(0, body, n);
    play(q, request(q, MASTER_ID, SLAVE_ID, 6, 8, body, n));
    n = itemCommit(body, 2, crc);
    play(q, request(q, MASTER_ID, SLAVE_ID, 7, 8, body, n));
    check(lastReply(rb, &rseq, &rtxn) > 0 && rb[1] == DMR_PROV_RES_CRC_MISMATCH && s_nWritten == 0, "COMMIT with the wrong item count is refused");
    /* ABORT drops the image; a timeout drops it too */
    n = itemChannel(body, 3, 0);
    play(q, request(q, MASTER_ID, SLAVE_ID, 8, 9, body, n));
    body[0] = DMR_PROV_OP_ABORT;
    play(q, request(q, MASTER_ID, SLAVE_ID, 9, 9, body, 1));
    check(diag().txnActive == 0 && !s_arenaHeld, "ABORT discards the staged image and releases the arena");
    n = itemChannel(body, 3, 0);
    play(q, request(q, MASTER_ID, SLAVE_ID, 10, 10, body, n));
    clockAdvance(121000); dmrProvTick();
    check(diag().txnActive == 0 && !s_arenaHeld, "a transaction idle for 120 s is discarded");
    /* a failing write stops the commit and says so */
    n = itemChannel(body, 999, 0); crc = stageCrc(0, body, n);
    play(q, request(q, MASTER_ID, SLAVE_ID, 11, 11, body, n));
    n = itemCommit(body, 1, crc);
    play(q, request(q, MASTER_ID, SLAVE_ID, 12, 11, body, n));
    check(lastReply(rb, &rseq, &rtxn) > 0 && rb[1] == DMR_PROV_RES_WRITE_FAILED && rb[21] == 0, "a failing codeplug write answers WRITE_FAILED with 0 items done");
    /* arena held by another feature: BUSY */
    s_arenaBusy = 1;
    n = itemChannel(body, 3, 0); crc = stageCrc(0, body, n);
    play(q, request(q, MASTER_ID, SLAVE_ID, 13, 12, body, n));
    n = itemCommit(body, 1, crc);
    play(q, request(q, MASTER_ID, SLAVE_ID, 14, 12, body, n));
    s_arenaBusy = 0;
    check(lastReply(rb, &rseq, &rtxn) > 0 && rb[1] == DMR_PROV_RES_BUSY, "arena held by another feature: COMMIT answered BUSY");
}

static void testStatusAndReboot(void)
{
    uint8_t q[DMR_DATA_MAX_BURSTS * 13], body[DMR_PROV_BODY_MAX], rb[32];
    uint32_t rseq; uint8_t rtxn;
    bootAs(SLAVE_ID, MASTER_ID, 40);
    body[0] = DMR_PROV_OP_STATUS;
    play(q, request(q, MASTER_ID, SLAVE_ID, 41, 0, body, 1));
    check(lastReply(rb, &rseq, &rtxn) > 0 && rb[1] == DMR_PROV_RES_OK && rseq == 41 && rd32be(rb + 3) == 40 && s_blockWrites == 0,
          "STATUS request: signed reply with the stored lastSeq, nothing persisted");
    /* RX-only / TX-inhibited: the reply is dropped, nothing else transmits */
    s_txAllowed = 0; s_txLoads = 0;
    play(q, request(q, MASTER_ID, SLAVE_ID, 42, 0, body, 1));
    check(s_txLoads == 0, "no reply transmission when TX is not allowed");
    s_txAllowed = 1;
    body[0] = DMR_PROV_OP_REBOOT;
    play(q, request(q, MASTER_ID, SLAVE_ID, 43, 0, body, 1));
    check(lastReply(rb, &rseq, &rtxn) > 0 && rb[1] == DMR_PROV_RES_OK && s_blockWrites == 1 && diag().lastSeq0 == 43 && s_reboots == 0, "REBOOT: seq persisted, reply queued, not rebooted yet");
    clockAdvance(500); dmrProvTick();
    check(s_reboots == 1, "... reboot after the reply has gone out");
    /* a replayed REBOOT after the reboot: rejected (persisted seq) */
    dmrProvInit(); s_reboots = 0; s_txLoads = 0;
    play(q, request(q, MASTER_ID, SLAVE_ID, 43, 0, body, 1));
    clockAdvance(500); dmrProvTick();
    check(s_reboots == 0 && s_txLoads == 0 && diag().rejSeq == 1, "a replayed REBOOT after the reboot is rejected");
    /* a radio that is not paired ignores everything */
    s_flashBlockValid = 0; dmrProvInit();
    check(dmrProvIsPaired() == 0, "no PROV block: not paired");
    play(q, request(q, MASTER_ID, SLAVE_ID, 50, 0, body, 1));
    check(s_txLoads == 0, "unpaired: no reply (and no builder output)");
}

static const uint8_t KEY1[32] = {
    0x93,0xa5,0xcf,0x3b,0xda,0xb5,0x58,0xbc, 0xf6,0x1e,0xca,0x57,0x32,0xa8,0x65,0x78,
    0x32,0x39,0x6f,0x67,0x81,0x50,0xe1,0x78, 0x11,0xea,0xa7,0x49,0x1f,0x94,0xb3,0xee };

static void testSmsCoexists(void)
{
    uint8_t q[DMR_DATA_MAX_BURSTS * 13], body[DMR_PROV_BODY_MAX];
    bootAs(SLAVE_ID, MASTER_ID, 0);
    dmrAesSetKeyRam(1, KEY1);
    dmrSmsDeleteAll(-1);
    /* an encrypted private SMS to us (SAP 9 + ENC header) still decodes */
    s_qn = 0;
    int r = dmrSmsSend("Hello bench", SLAVE_ID, 0, 1);
    check(r == 0, "dmrSmsSend builds an encrypted SMS");
    memcpy(q, s_q, (size_t)s_qn * 13);
    int n = s_qn;
    dmrSmsRxTick();   /* flush the deferred Sent-folder write */
    play(q, n);
    check(dmrSmsCount(0) == 1, "an encrypted SMS (SAP 9 with the ENC header) still reaches the inbox");
    /* a provisioning PDU never reaches the inbox and never notifies */
    notifications = 0;
    n = itemChannel(body, 1, 0);
    play(q, request(q, MASTER_ID, SLAVE_ID, 1, 1, body, n));
    check(dmrSmsCount(0) == 1 && notifications == 0 && diag().stagedCount == 1, "a provisioning PDU is staged, not shown as a message");
}

/* ---- the master ------------------------------------------------------------------------- */
static int jobChannel(uint8_t *job, int off, uint16_t idx, uint8_t fill)
{
    uint8_t body[64]; int n = itemChannel(body, idx, fill);
    job[off] = body[0]; job[off + 1] = (uint8_t)(n - 1); job[off + 2] = 0;
    memcpy(job + off + 3, body + 1, (size_t)(n - 1));
    return off + 3 + (n - 1);
}

static void testMaster(void)
{
    uint8_t job[512], pdu[DMR_PROV_PDU_MAX], hdr[12], st[64];
    uint32_t seq; uint8_t txn; int reply;
    bootAs(MASTER_ID, 0, 0);     /* the master needs the key and its own counter only */
    wr32le(job, SLAVE_ID); job[4] = 3; job[5] = 0x01;
    int len = jobChannel(job, 6, 7, 0x70);
    len = jobChannel(job, len, 8, 0x80);
    uint32_t crc = crc32_ieee(0, job + 6, len - 6);

    check(dmrProvJobLoad(job, 5) == -4 && dmrProvJobLoad(job, len - 1) == -4, "malformed jobs are refused");
    check(dmrProvJobLoad(job, len) == 0 && s_arenaHeld, "a job is queued into the arena");
    check(dmrProvJobLoad(job, len) == -1, "a second job is refused while one runs");
    s_txLoads = 0;
    dmrProvTick();
    check(s_txLoads == 1 && s_blockWrites == 1 && diag().ownRes == 64 && diag().ownNext == 1, "first PDU sent; the counter reservation (64) persisted first");
    int pl = pduFromBursts(s_q, s_qn, pdu, hdr);
    uint32_t dst = ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 8) | hdr[4];
    uint32_t src = ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];
    check(dst == SLAVE_ID && src == MASTER_ID && (hdr[0] & 0x4F) == 0x43 && (hdr[1] >> 4) == 9, "on the air: confirmed individual data header, SAP 9, A set");
    check(s_q[0] == 0x30 && s_q[6 * 13] == 0x60 && s_q[7 * 13] == 0x70, "CSBK preamble, one data header, then blocks");
    int bl = openFrame(pdu, pl, MASTER_ID, SLAVE_ID, &seq, &txn, &reply);
    check(bl == 59 && seq == 0 && txn == 3 && !reply && pdu[10] == DMR_PROV_OP_WRITE_CHANNEL && pdu[12] == 7 && pdu[13] == 0x70, "PDU 1 = WRITE_CHANNEL 7 under seq 0, txn 3, verified with the test keys");
    uint8_t first[DMR_DATA_MAX_BURSTS * 13]; int firstN = s_qn; memcpy(first, s_q, (size_t)s_qn * 13);

    /* no ACK: resent byte-identical, three tries, then failed */
    clockAdvance(500); dmrProvTick();            /* TX done -> wait ACK */
    clockAdvance(2100); dmrProvTick();           /* timeout -> retry 2 */
    check(s_txLoads == 2 && s_qn == firstN && memcmp(first, s_q, (size_t)firstN * 13) == 0, "no ACK: the same bytes are sent again");
    clockAdvance(500); dmrProvTick(); clockAdvance(2100); dmrProvTick();   /* retry 3 */
    clockAdvance(500); dmrProvTick(); clockAdvance(2100); dmrProvTick();   /* give up */
    dmrProvJobStatus(st, sizeof st);
    check(s_txLoads == 3 && st[0] == 0 && st[4] == 2 && st[5] == DMR_PROV_RES_TIMEOUT && !s_arenaHeld && strstr(lastNote, "FAILED") != NULL, "after three tries the job fails, the arena is released, the UI says so");

    /* the happy path: ACK each PDU, answer COMMIT with a signed STATUS */
    s_txLoads = 0; notifications = 0;
    check(dmrProvJobLoad(job, len) == 0, "the job is queued again");
    dmrProvTick();                               /* PDU 1 (seq 3) */
    pl = pduFromBursts(s_q, s_qn, pdu, hdr); openFrame(pdu, pl, MASTER_ID, SLAVE_ID, &seq, &txn, &reply);
    check(seq == 1, "retries of seq 0 did not consume numbers: the next PDU is seq 1");
    clockAdvance(500); dmrProvTick();
    dmrProvRxResponse(SLAVE_ID, MASTER_ID, (uint8_t)(0x08 | (seq & 7)));   /* the slave's link ACK */
    dmrProvTick();                               /* -> PDU 2 */
    pl = pduFromBursts(s_q, s_qn, pdu, hdr); openFrame(pdu, pl, MASTER_ID, SLAVE_ID, &seq, &txn, &reply);
    check(s_txLoads == 2 && seq == 2 && pdu[12] == 8, "ACK received: PDU 2 (channel 8) goes out under seq 2");
    clockAdvance(500); dmrProvTick();
    dmrProvRxResponse(SLAVE_ID, MASTER_ID, (uint8_t)(0x08 | 1));            /* wrong N(S): not ours */
    dmrProvTick();
    check(s_txLoads == 2, "an ACK with another N(S) does not advance");
    dmrProvRxResponse(OTHER_ID, MASTER_ID, (uint8_t)(0x08 | (seq & 7)));   /* wrong source */
    dmrProvTick();
    check(s_txLoads == 2, "an ACK from another radio does not advance");
    dmrProvRxResponse(SLAVE_ID, MASTER_ID, (uint8_t)(0x08 | (seq & 7)));
    dmrProvTick();                               /* -> COMMIT */
    pl = pduFromBursts(s_q, s_qn, pdu, hdr); int cl = openFrame(pdu, pl, MASTER_ID, SLAVE_ID, &seq, &txn, &reply);
    check(s_txLoads == 3 && cl == 6 && pdu[10] == DMR_PROV_OP_COMMIT && pdu[11] == 2 && rd32be(pdu + 12) == crc && seq == 3, "then the implicit COMMIT: count 2 and the CRC32 of the items");
    clockAdvance(500); dmrProvTick();
    dmrProvRxResponse(SLAVE_ID, MASTER_ID, (uint8_t)(0x08 | (seq & 7)));
    dmrProvTick();
    dmrProvJobStatus(st, sizeof st);
    check(st[0] == 4, "waiting for the STATUS reply");
    /* the slave's reply (built as the slave builds it), played into the master's RX path */
    uint8_t body[DMR_PROV_STATUS_BODY_LEN] = { DMR_PROV_OP_STATUS_REPLY, DMR_PROV_RES_OK, 3 };
    wr32be(body + 3, 3); wr32be(body + 7, crc); memcpy(body + 11, "abc1234", 7); body[19] = 3; body[20] = 0x0D; body[21] = 2;
    uint8_t rp[DMR_PROV_PDU_MAX], rq[DMR_DATA_MAX_BURSTS * 13];
    int rl = dmrProvBuildPdu(MASTER_ID, SLAVE_ID, 3, 3, 1, body, sizeof body, rp, sizeof rp);
    /* a reply under the wrong seq first: ignored */
    int rn = dmrProvBuildBursts(rp, rl, MASTER_ID, SLAVE_ID, 3, rq, DMR_DATA_MAX_BURSTS);
    uint8_t wrong[DMR_PROV_PDU_MAX]; int wl = dmrProvBuildPdu(MASTER_ID, SLAVE_ID, 2, 3, 1, body, sizeof body, wrong, sizeof wrong);
    uint8_t wq[DMR_DATA_MAX_BURSTS * 13]; int wn = dmrProvBuildBursts(wrong, wl, MASTER_ID, SLAVE_ID, 2, wq, DMR_DATA_MAX_BURSTS);
    play(wq, wn);
    dmrProvJobStatus(st, sizeof st);
    check(st[0] == 4 && st[6] == 0, "a STATUS reply for another seq is ignored");
    play(rq, rn);
    dmrProvJobStatus(st, sizeof st);
    check(st[0] == 0 && st[4] == 1 && st[6] == 1 && st[12 + 1] == DMR_PROV_RES_OK && rd32be(st + 12 + 7) == crc, "the signed STATUS completes the job: outcome OK, fields readable over USB");
    check(strstr(lastNote, "OK") != NULL && strstr(lastNote, "match") != NULL && !s_arenaHeld, "the UI shows OK and the codeplug CRC match");
    /* a single-shot STATUS job */
    uint8_t sjob[9]; wr32le(sjob, SLAVE_ID); sjob[4] = 1; sjob[5] = 0x02; sjob[6] = DMR_PROV_OP_STATUS; sjob[7] = 0; sjob[8] = 0;
    check(dmrProvJobLoad(sjob, 9) == 0, "a STATUS job is queued");
    s_txLoads = 0; dmrProvTick();
    pl = pduFromBursts(s_q, s_qn, pdu, hdr); openFrame(pdu, pl, MASTER_ID, SLAVE_ID, &seq, &txn, &reply);
    check(s_txLoads == 1 && pdu[10] == DMR_PROV_OP_STATUS && seq == 4, "STATUS request sent under seq 4");
    dmrProvJobCancel();
    check(!s_arenaHeld, "cancel releases the arena");
}

int main(void)
{
    testTransactionCommits();
    testReplayAndReorder();
    testMacFailureAndHoldoff();
    testAddressing();
    testTruncatedAndMalformed();
    testTransactionIntegrity();
    testStatusAndReboot();
    testSmsCoexists();
    testMaster();
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}

/* Host test of the DMR SMS receive path in functions/dmr_sms.c (ENABLE_DMR_DATA + ENABLE_AES).
 *
 * The real dmrSmsSend() builds the on-air burst stream (CSBK preamble, data header, ENC header,
 * rate-1/2 blocks) exactly as a stock TYT does; the test captures that stream from the data-TX
 * harness stub and plays the data bursts back into the RX reassembler (dmrSmsRxBurst, as the
 * HR-C6000 ISR does) and the main-loop decoder (dmrSmsRxTick). On top of the unconfirmed group
 * SMS this fork already sends, it checks what a STOCK radio sends to an individual:
 *   - CONFIRMED delivery (data header DPF 3): each rate-1/2 block carries a 2-byte serial
 *     number + CRC-9 before 10 payload bytes, so the packet must be reassembled from 10-byte
 *     payloads, not 12;
 *   - the message is addressed: a private SMS to another radio's ID is not ours;
 *   - without an ACK from us the sender retransmits the same packet, which must not show up
 *     as a second message;
 *   - an ENC header with the group bit set, or a lost ENC header burst, must still decode.
 *
 * Build: gcc -O2 -DENABLE_AES -DENABLE_DMR_DATA -Itests/stubs -Iapplication/include \
 *          -o /tmp/test_sms_rx tests/test_sms_rx.c application/source/functions/dmr_sms.c \
 *          application/source/crypto/dmr_aes_hook.c application/source/crypto/dmr_aes.c \
 *          && /tmp/test_sms_rx
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto/dmr_aes.h"
#include "crypto/dmr_aes_hook.h"
#include "functions/dmr_sms.h"
#include "functions/dmr_data.h"
#include "functions/codeplug.h"
#include "functions/settings.h"
#include "functions/ticks.h"
#include "functions/trx.h"
#include "hardware/SPI_Flash.h"
#include "user_interface/menuSystem.h"

/* ---- firmware stand-ins ---------------------------------------------------- */
volatile bool trxTransmissionEnabled = false;
volatile bool trxIsTransmitting = false;
uint32_t trxDMRID = 2345678;                 /* our radio ID */
CodeplugChannel_t *currentChannelData = NULL;

bool SPI_Flash_read(uint32_t a, uint8_t *b, int n) { (void)a; memset(b, 0xFF, (size_t)n); return true; }
bool SPI_Flash_write(uint32_t a, uint8_t *b, int n) { (void)a; (void)b; (void)n; return true; }
bool codeplugGetOpenGD77CustomData(CodeplugCustomDataType_t t, uint8_t *b) { (void)t; (void)b; return false; }
bool codeplugGetOpenGD77CustomDataBounded(CodeplugCustomDataType_t t, uint8_t *b, int n) { (void)t; (void)b; (void)n; return false; }
bool codeplugSetOpenGD77CustomData(CodeplugCustomDataType_t t, uint8_t *b, int n) { (void)t; (void)b; (void)n; return true; }
uint8_t codeplugChannelGetFlag(CodeplugChannel_t *c, ChannelFlag_t f) { (void)c; (void)f; return 0; }
static uint32_t s_nowMs = 1000;
uint32_t ticksGetMillis(void) { return s_nowMs += 60; }   /* a tick per call, like a busy main loop */
static void clockAdvance(uint32_t ms) { s_nowMs += ms; }

static int notifications = 0;
void uiNotificationShow(uiNotificationType_t type, uiNotificationID_t id, uint32_t ms, const char *msg, bool now)
{ (void)type; (void)id; (void)ms; (void)msg; (void)now; notifications++; }

/* data-TX harness: capture the queued burst stream (count * [type, 12 payload bytes]) */
static uint8_t s_q[DMR_DATA_MAX_BURSTS * 13];
static int     s_qn = 0;
void dmrDataTxLoad(const uint8_t *bursts, uint8_t count) { memcpy(s_q, bursts, (size_t)count * 13); s_qn = count; }
int  dmrDataTxActive(void) { return 0; }
int  dmrDataTxNextBurst(uint8_t *t, uint8_t *p) { (void)t; (void)p; return 0; }
void dmrDataTxEnd(void) { }

static const uint8_t KEY1[32] = {
    0x93,0xa5,0xcf,0x3b,0xda,0xb5,0x58,0xbc, 0xf6,0x1e,0xca,0x57,0x32,0xa8,0x65,0x78,
    0x32,0x39,0x6f,0x67,0x81,0x50,0xe1,0x78, 0x11,0xea,0xa7,0x49,0x1f,0x94,0xb3,0xee };

static int fails = 0;
static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { fails++; }
}

/* DMR data-PDU CRC32 as on the wire (same definition as dmr_sms.c / tools/dmr_enc_sms.py). */
static uint32_t crc32_dmr(const uint8_t *pdu, int len)
{
    uint32_t crc = 0;
    int nbits = len * 8 - 32, bitno = 0;
    for (int i = 0; i + 1 < len; i += 2)
    {
        for (int pass = 0; pass < 2; pass++)
        {
            uint8_t byte = (pass == 0) ? pdu[i + 1] : pdu[i];
            for (int k = 7; k >= 0; k--)
            {
                if (bitno >= nbits) { goto done; }
                int bit = (byte >> k) & 1;
                crc = (((crc >> 31) & 1) ^ (uint32_t)bit) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
                bitno++;
            }
        }
    }
done:
    return ((crc & 0xFF) << 24) | ((crc & 0xFF00) << 8) | ((crc >> 8) & 0xFF00) | ((crc >> 24) & 0xFF);
}

/* A captured stream, split into what the chip hands the ISR: headers (type 6) and blocks
 * (type 7 = rate 1/2, 12 bytes; type 8 = rate 3/4, 18 bytes). */
typedef struct
{
    uint8_t hdr[4][12]; int nhdr;      /* data header [, ENC header] */
    uint8_t blk[32][18]; int nblk;     /* data blocks */
    int blkType;                       /* 7 or 8 */
} stream_t;

static void capture(stream_t *s)
{
    memset(s, 0, sizeof *s);
    s->blkType = 7;
    for (int i = 0; i < s_qn; i++)
    {
        uint8_t type = s_q[i * 13];
        const uint8_t *p = s_q + i * 13 + 1;
        if (type == 0x60 && s->nhdr < 4) { memcpy(s->hdr[s->nhdr++], p, 12); }
        else if (type == 0x70 && s->nblk < 32) { memcpy(s->blk[s->nblk++], p, 12); }
    }
}

static void playback(const stream_t *s, int skipEncHeader)
{
    for (int i = 0; i < s->nhdr; i++)
    {
        if (skipEncHeader && (s->hdr[i][0] & 0x0F) == 0x0F) { continue; }
        dmrSmsRxBurst(6, s->hdr[i]);
    }
    for (int i = 0; i < s->nblk; i++) { dmrSmsRxBurst(s->blkType, s->blk[i]); }
    dmrSmsRxReset();      /* Terminator */
    dmrSmsRxTick();       /* main loop */
}

/* Send text (as the menu does) and capture the stream a stock radio would receive from us. */
static void sendAndCapture(const char *text, uint32_t dst, int group, uint8_t keyId, stream_t *s)
{
    s_qn = 0;
    int r = dmrSmsSend(text, dst, group, keyId);
    if (r != 0) { printf("  dmrSmsSend returned %d\n", r); }
    capture(s);
    dmrSmsRxTick();       /* flush the deferred Sent-folder write so it doesn't count later */
}

/* Rewrite an unconfirmed rate-1/2 stream as a stock radio's CONFIRMED delivery to one radio:
 * DPF 3, response requested, and block payloads behind a 2-byte DBSN/CRC-9 field. blkType 8
 * gives the rate-3/4 form a stock TYT actually uses for a private SMS (16 payload bytes per
 * block); blkType 7 the rate-1/2 form (10 payload bytes). */
static void toConfirmedRate(const stream_t *in, int plaintextLen, uint32_t dst, int blkType, stream_t *out)
{
    uint8_t ct[400], pdu[400];
    int ctLen = plaintextLen;             /* ciphertext (or cleartext) length == IP packet length */
    int pay = (blkType == 8) ? 16 : 10;   /* payload bytes per confirmed block */
    memset(out, 0, sizeof *out);
    out->blkType = blkType;
    for (int i = 0; i < in->nblk; i++) { memcpy(ct + i * 12, in->blk[i], 12); }
    int total = (((ctLen + 4) + (pay - 1)) / pay) * pay;
    int poc = total - ctLen - 4;
    memcpy(pdu, ct, ctLen);
    memset(pdu + ctLen, 0, (size_t)(total - ctLen));
    uint32_t crc = crc32_dmr(pdu, total);
    pdu[total - 4] = (uint8_t)(crc >> 24); pdu[total - 3] = (uint8_t)(crc >> 16);
    pdu[total - 2] = (uint8_t)(crc >> 8);  pdu[total - 1] = (uint8_t)crc;

    for (int i = 0; i < in->nhdr; i++)
    {
        memcpy(out->hdr[i], in->hdr[i], 12);
        if ((in->hdr[i][0] & 0x0F) == 0x02)             /* the Unconfirmed data header -> Confirmed */
        {
            out->hdr[i][0] = 0x43;                       /* individual, response requested, DPF 3 */
            out->hdr[i][1] = (uint8_t)((in->hdr[i][1] & 0xF0) | (poc & 0x0F));
            out->hdr[i][2] = (uint8_t)(dst >> 16); out->hdr[i][3] = (uint8_t)(dst >> 8); out->hdr[i][4] = (uint8_t)dst;
            out->hdr[i][8] = (uint8_t)(0x80 | ((total / pay) + (in->nhdr > 1 ? 1 : 0)));
            out->hdr[i][9] = 0x00;                       /* S=0, Ns=0, FSN=0 */
        }
    }
    out->nhdr = in->nhdr;
    out->nblk = total / pay;
    for (int b = 0; b < out->nblk; b++)
    {
        uint16_t sn_crc9 = (uint16_t)((b & 0x7F) << 9) | (uint16_t)(0x1A5 ^ b);   /* DBSN + a CRC-9 we don't check */
        out->blk[b][0] = (uint8_t)(sn_crc9 >> 8);
        out->blk[b][1] = (uint8_t)sn_crc9;
        memcpy(out->blk[b] + 2, pdu + b * pay, (size_t)pay);
    }
}

static void toConfirmed(const stream_t *in, int plaintextLen, uint32_t dst, stream_t *out)
{
    toConfirmedRate(in, plaintextLen, dst, 7, out);
}

static int plaintextLen(const char *text) { return 20 + 8 + 10 + 2 * (int)strlen(text); }

static void fresh(void)
{
    dmrAesInit();
    dmrSmsInit();                 /* reloads the key store from (empty) flash ... */
    dmrAesSetKeyRam(1, KEY1);     /* ... so load the bench key afterwards */
    dmrSmsDeleteAll(-1);
    notifications = 0;
}

static int inboxHas(const char *text)
{
    const dmrSmsMessage_t *m = dmrSmsGet(0, 0);
    return (m != NULL) && (m->textLen == (int)strlen(text)) && (memcmp(m->text, text, m->textLen) == 0);
}

static void testUnconfirmedGroupEncryptedRoundTrip(void)
{
    stream_t s;
    fresh();
    sendAndCapture("Hello bench", 9, 1, 1, &s);
    check(s.nhdr == 2 && s.nblk >= 4, "stream has data header + ENC header + blocks");
    playback(&s, 0);
    check(dmrSmsCount(0) == 1 && inboxHas("Hello bench"), "encrypted group SMS decodes into the inbox");
    const dmrSmsMessage_t *m = dmrSmsGet(0, 0);
    check(m && (m->flags & DMR_SMS_FLAG_GROUP) && (m->peerId == trxDMRID), "stored as a group message from the sender");
}

static void testUnconfirmedClearRoundTrip(void)
{
    stream_t s;
    CodeplugChannel_t ch = { 0xFF };     /* channel forces clear */
    fresh();
    currentChannelData = &ch;
    sendAndCapture("clear text", 9, 1, 0, &s);
    currentChannelData = NULL;
    check(s.nhdr == 1, "cleartext stream has no ENC header");
    playback(&s, 0);
    check(dmrSmsCount(0) == 1 && inboxHas("clear text"), "cleartext group SMS decodes into the inbox");
}

static void testConfirmedPrivateEncryptedSms(void)
{
    stream_t s, c;
    fresh();
    sendAndCapture("Hi", trxDMRID, 0, 1, &s);
    toConfirmed(&s, plaintextLen("Hi"), trxDMRID, &c);
    playback(&c, 0);
    check(dmrSmsCount(0) == 1 && inboxHas("Hi"), "confirmed (DPF 3) private encrypted SMS decodes");
    const dmrSmsMessage_t *m = dmrSmsGet(0, 0);
    check(m && !(m->flags & DMR_SMS_FLAG_GROUP), "stored as a private message");
}

static void testConfirmedPrivateClearSms(void)
{
    stream_t s, c;
    CodeplugChannel_t ch = { 0xFF };
    fresh();
    currentChannelData = &ch;
    sendAndCapture("Where are you now?", trxDMRID, 0, 0, &s);
    currentChannelData = NULL;
    toConfirmed(&s, plaintextLen("Where are you now?"), trxDMRID, &c);
    playback(&c, 0);
    check(dmrSmsCount(0) == 1 && inboxHas("Where are you now?"), "confirmed (DPF 3) private cleartext SMS decodes");
}

/* The form the July 2026 stock recon captures show for a private SMS: Confirmed delivery in
 * RATE-3/4 blocks (type 8), 16 payload bytes behind the DBSN/CRC-9 field. */
static void testConfirmedPrivateRate34EncryptedSms(void)
{
    stream_t s, c;
    fresh();
    sendAndCapture("Rate three quarter", trxDMRID, 0, 1, &s);
    toConfirmedRate(&s, plaintextLen("Rate three quarter"), trxDMRID, 8, &c);
    check(c.blkType == 8 && c.nblk >= 3, "rate-3/4 stream built (type 8 blocks)");
    playback(&c, 0);
    check(dmrSmsCount(0) == 1 && inboxHas("Rate three quarter"), "confirmed rate-3/4 private encrypted SMS decodes");
}

static void testConfirmedPrivateRate34ClearSms(void)
{
    stream_t s, c;
    CodeplugChannel_t ch = { 0xFF };
    fresh();
    currentChannelData = &ch;
    sendAndCapture("Hi", trxDMRID, 0, 0, &s);
    currentChannelData = NULL;
    toConfirmedRate(&s, plaintextLen("Hi"), trxDMRID, 8, &c);
    playback(&c, 0);
    check(dmrSmsCount(0) == 1 && inboxHas("Hi"), "confirmed rate-3/4 private cleartext SMS decodes (shortest message)");
}

/* A REAL stock TYT private SMS, from the July 2026 bench recon capture r4_pmc_conf_priv_clean
 * (RADIO3 12343 -> RADIO2 12342, "Hello", clear channel). Data header: individual, response
 * requested, DPF 3 Confirmed, SAP 4, POC 12, F=1 BTF 4, N(S) 0 FSN 8. Then four rate-3/4
 * confirmed blocks whose 16 payload bytes DSD-FME printed (its own CRC checks flagged errors,
 * but the CRC32 of these 64 bytes and the IPv4 header checksum both verify with the firmware's
 * algorithms). The 2-byte DBSN/CRC-9 field in front of each block was not recorded; the
 * receiver does not check it, so any value stands in. In r3 the sender repeated the whole PDU
 * three times per attempt with N(S) 3,3,3 then 4,4 when it got no ACK. */
static const uint8_t RECON_HDR[12] = { 0x43,0x4C,0x00,0x30,0x36,0x00,0x30,0x37,0x84,0x08,0xDB,0x47 };
static const uint8_t RECON_BLK[4][16] = {
    { 0x45,0x00,0x00,0x30,0x00,0x00,0x00,0x00,0x40,0x11,0x02,0x51,0x0C,0x00,0x30,0x37 },
    { 0x0C,0x00,0x30,0x36,0x0F,0xA7,0x0F,0xA7,0x00,0x1C,0xE0,0xE1,0x00,0x12,0xE0,0x00 },
    { 0x9C,0x04,0x0D,0x00,0x0A,0x00,0x48,0x00,0x65,0x00,0x6C,0x00,0x6C,0x00,0x6F,0x00 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0x63,0x53,0x4E,0x5D } };

/* One copy of the recon "Hello" as the chip delivers it: header with N(S) ns from source src,
 * four rate-3/4 blocks; payload byte badByte of block badBlock (if badBlock >= 0) XORed with
 * badXor, the way the r3 bench replay showed bit errors that pass the chip's CRC flag
 * (block 1: 0x0C -> 0x1C; block 3 differed too). Ends with the Terminator and a main-loop tick. */
static void playReconCopy(uint8_t ns, uint32_t src, int badBlock, int badByte, uint8_t badXor)
{
    uint8_t hdr[12];
    memcpy(hdr, RECON_HDR, 12);
    hdr[5] = (uint8_t)(src >> 16); hdr[6] = (uint8_t)(src >> 8); hdr[7] = (uint8_t)src;
    hdr[9] = (uint8_t)((hdr[9] & 0x8F) | ((ns & 0x07) << 4));   /* S | N(S) | FSN */
    dmrSmsRxBurst(6, hdr);
    for (int b = 0; b < 4; b++)
    {
        uint8_t raw[18] = { (uint8_t)(b << 1), 0x5A };   /* stand-in DBSN/CRC-9 field */
        memcpy(raw + 2, RECON_BLK[b], 16);
        if (b == badBlock) { raw[2 + badByte] ^= badXor; }
        dmrSmsRxBurst(8, raw);
    }
    dmrSmsRxReset();
    dmrSmsRxTick();
}

static void playReconPrivateHello(uint8_t ns)
{
    playReconCopy(ns, 12343, -1, 0, 0);
}

/* r3 on the bench: a stock radio that gets no ACK sends the whole PDU again, and the HR-C6000
 * passes rate-3/4 blocks with bit errors as CRC-valid (its flag does not cover the per-block
 * CRC-9), so no single copy had a valid CRC32 although two copies between them carried every
 * block intact. The receiver keeps the last failed copy and, when the next copy of the same
 * packet fails too, tries the block-wise combinations of the two; the CRC32 is the arbiter. */
static void testStockRetransmitBlockMerge(void)
{
    fresh();
    trxDMRID = 12342;
    playReconCopy(3, 12343, 1, 0, 0x10);                       /* block 1: 0x0C -> 0x1C */
    check(dmrSmsCount(0) == 0 && notifications == 0, "a copy with a bit error in block 1 does not decode");
    playReconCopy(3, 12343, 3, 2, 0x80);                       /* block 3: a padding byte flipped */
    check(dmrSmsCount(0) == 1 && inboxHas("Hello") && notifications == 1,
          "two retransmissions with errors in different blocks merge into the message, shown once");
    playReconCopy(3, 12343, 2, 5, 0x01);                       /* a third damaged copy of the same PDU */
    check(dmrSmsCount(0) == 1 && notifications == 1, "a later damaged copy of the same packet adds nothing");

    fresh();
    trxDMRID = 12342;
    playReconCopy(3, 12343, 1, 0, 0x10);
    playReconCopy(3, 12343, 1, 0, 0x10);
    check(dmrSmsCount(0) == 0 && notifications == 0, "the same error in both copies cannot be repaired");
    playReconCopy(3, 12343, 1, 3, 0x40);
    check(dmrSmsCount(0) == 0 && notifications == 0, "a different error in the same block cannot be repaired either");
    playReconCopy(4, 12343, -1, 0, 0);
    check(dmrSmsCount(0) == 1 && inboxHas("Hello") && notifications == 1, "a clean copy after the damaged ones still decodes once");

    fresh();
    trxDMRID = 12342;
    playReconCopy(3, 12343, 1, 0, 0x10);
    playReconCopy(3, 12399, 3, 2, 0x80);                       /* another sender's damaged packet */
    check(dmrSmsCount(0) == 0 && notifications == 0, "blocks from a different sender's packet are never merged");
    playReconCopy(3, 12343, 3, 2, 0x80);
    check(dmrSmsCount(0) == 0 && notifications == 0, "the other sender's copy replaced the kept one; no merge with the first");
    playReconCopy(3, 12343, 1, 0, 0x10);
    check(dmrSmsCount(0) == 1 && inboxHas("Hello") && notifications == 1, "the next copy of the right packet merges with the kept one");
    trxDMRID = 2345678;
}

static void testStockReconPrivateHello(void)
{
    fresh();
    trxDMRID = 12342;                              /* we are RADIO2, the addressee */
    playReconPrivateHello(0);
    check(dmrSmsCount(0) == 1 && inboxHas("Hello"), "stock r4 capture: private 'Hello' in rate-3/4 confirmed blocks decodes");
    const dmrSmsMessage_t *m = dmrSmsGet(0, 0);
    check(m && (m->peerId == 12343) && !(m->flags & DMR_SMS_FLAG_GROUP), "from 12343, stored as private");

    /* r3: the sender's unacknowledged repeats, N(S) 3,3,3 then 4,4 */
    playReconPrivateHello(3); playReconPrivateHello(3); playReconPrivateHello(3);
    playReconPrivateHello(4); playReconPrivateHello(4);
    check(dmrSmsCount(0) == 1 && notifications == 1, "its five retransmissions (N(S) 3,3,3,4,4) add nothing");

    fresh();
    trxDMRID = 12343;                              /* we are RADIO3, the sender's own ID */
    playReconPrivateHello(0);
    check(dmrSmsCount(0) == 0, "the same PDU heard by a radio that is not the addressee is ignored");
    trxDMRID = 2345678;
}

static void testPrivateSmsToAnotherRadioIsNotOurs(void)
{
    stream_t s, c;
    fresh();
    sendAndCapture("not for you", 1111111, 0, 1, &s);   /* individual, dst != trxDMRID */
    playback(&s, 0);
    check(dmrSmsCount(0) == 0, "unconfirmed private SMS to another ID is not stored");
    toConfirmed(&s, plaintextLen("not for you"), 1111111, &c);
    playback(&c, 0);
    check(dmrSmsCount(0) == 0 && notifications == 0, "confirmed private SMS to another ID is not stored");
}

static void testRetransmissionIsNotASecondMessage(void)
{
    stream_t s, c;
    fresh();
    sendAndCapture("ack me", trxDMRID, 0, 1, &s);
    toConfirmed(&s, plaintextLen("ack me"), trxDMRID, &c);
    playback(&c, 0);
    playback(&c, 0);   /* the stock radio gets no ACK from us and sends the same packet again */
    playback(&c, 0);
    check(dmrSmsCount(0) == 1 && notifications == 1, "a retransmitted packet is stored and shown once");

    /* Bench r3: after three unanswered link-layer repeats the stock radio sends the message
     * once more from the TMS layer, with a new IP identification and TMS sequence; that is
     * still the one message the user sent. */
    sendAndCapture("ack me", trxDMRID, 0, 1, &s);   /* same text, new IP id / TMS seq */
    toConfirmed(&s, plaintextLen("ack me"), trxDMRID, &c);
    playback(&c, 0);
    playback(&c, 0);
    check(dmrSmsCount(0) == 1 && notifications == 1, "the sender's TMS-layer repeat (new IP id / TMS seq, same text) within a minute is not a second message");

    sendAndCapture("ack me", trxDMRID, 0, 1, &s);   /* the same text as a new message, later */
    toConfirmed(&s, plaintextLen("ack me"), trxDMRID, &c);
    clockAdvance(61000);
    playback(&c, 0);
    check(dmrSmsCount(0) == 2 && notifications == 2, "the same text sent again as a new message a minute later is stored");

    sendAndCapture("ack me", trxDMRID, 0, 1, &s);   /* same text right away, from another radio */
    toConfirmed(&s, plaintextLen("ack me"), trxDMRID, &c);
    for (int i = 0; i < c.nhdr; i++)
    {
        if ((c.hdr[i][0] & 0x0F) == 0x03) { c.hdr[i][5] = 0x00; c.hdr[i][6] = 0x30; c.hdr[i][7] = 0x39; }   /* source 12345 */
    }
    playback(&c, 0);
    check(dmrSmsCount(0) == 3, "the same text from a different sender right away is a new message");
}

/* Bench: sms_diag.py --reset (USB 0x94) is used between replays of the same capture; it must
 * also forget the retransmission filter's last message, or the replay is dropped as a repeat
 * and the radio has to be rebooted (reflashed) for a clean run. */
static void testDiagResetForgetsLastMessage(void)
{
    stream_t s, c;
    fresh();
    sendAndCapture("again", trxDMRID, 0, 1, &s);
    toConfirmed(&s, plaintextLen("again"), trxDMRID, &c);
    playback(&c, 0);
    playback(&c, 0);
    check(dmrSmsCount(0) == 1, "the same packet twice is one message");
    dmrSmsRxDiagReset();
    playback(&c, 0);
    check(dmrSmsCount(0) == 2, "after the diag reset the same packet is received as new");
}

/* Bench: the 0x95 last-PDU dump showed keyId=69 (0x45) after a cleartext PDU. The key id is
 * set only by an ENC header and lives in CCM, which is not zeroed at boot; a cleartext PDU
 * must report 0, also right after an encrypted one. */
static void testCleartextPduReportsNoKeyId(void)
{
    stream_t s, c;
    CodeplugChannel_t ch = { 0xFF };
    uint8_t out[16];
    fresh();
    sendAndCapture("enc", trxDMRID, 0, 1, &s);
    toConfirmed(&s, plaintextLen("enc"), trxDMRID, &c);
    playback(&c, 0);
    check(dmrSmsRxLastPdu(out, sizeof out) >= 8 && out[2] == 1, "the encrypted PDU reports key id 1");
    currentChannelData = &ch;
    sendAndCapture("clear", trxDMRID, 0, 0, &s);
    currentChannelData = NULL;
    toConfirmed(&s, plaintextLen("clear"), trxDMRID, &c);
    playback(&c, 0);
    check(dmrSmsCount(0) == 2 && inboxHas("clear"), "the cleartext PDU after it decodes");
    check(dmrSmsRxLastPdu(out, sizeof out) >= 8 && out[2] == 0, "and reports key id 0, not the previous packet's");
}

/* Bench r3: a stock radio keeps retransmitting a confirmed private SMS (five copies, then a
 * TMS-layer repeat) because this fork never acknowledged it. The stock receiver answers in
 * the very next timeslot with a single Response PDU header and no CSBK preamble, so the
 * reassembler builds the header the moment the last block completes and the HR-C6000 ISR
 * takes it with dmrSmsAckTake(). The three ACKs in the recon captures, RADIO2 12342
 * acknowledging RADIO3's N(S) 0, 1 and 4, are the expected bytes, CRC included. */
static const uint8_t STOCK_ACK_NS0[12] = { 0x01,0x40,0x00,0x30,0x37,0x00,0x30,0x36,0x00,0x08,0x67,0x0B };
static const uint8_t STOCK_ACK_NS1[12] = { 0x01,0x40,0x00,0x30,0x37,0x00,0x30,0x36,0x00,0x09,0x77,0x2A };
static const uint8_t STOCK_ACK_NS4[12] = { 0x01,0x40,0x00,0x30,0x37,0x00,0x30,0x36,0x00,0x0C,0x27,0x8F };

static void testConfirmedPrivateSmsIsAcknowledged(void)
{
    uint8_t ack[12];
    fresh();
    trxDMRID = 12342;
    check(dmrSmsAckTake(ack) == 0, "nothing to acknowledge before a packet completes");
    playReconPrivateHello(0);
    check(dmrSmsCount(0) == 1, "the message is stored");
    check((dmrSmsAckTake(ack) == 1) && (memcmp(ack, STOCK_ACK_NS0, 12) == 0), "the Response PDU for N(S)=0 is byte for byte what the stock radio sent");
    check(dmrSmsAckTake(ack) == 0, "the ACK is taken once");

    playReconPrivateHello(1);                       /* the sender's next packet */
    check((dmrSmsAckTake(ack) == 1) && (memcmp(ack, STOCK_ACK_NS1, 12) == 0), "N(S)=1 is echoed in the status field");
    playReconPrivateHello(4);                       /* a repeat of the same text, N(S)=4: not stored, still acknowledged */
    check(dmrSmsCount(0) == 1 && (dmrSmsAckTake(ack) == 1) && (memcmp(ack, STOCK_ACK_NS4, 12) == 0), "a repeat that is not stored again is still acknowledged, with its own N(S)");

    playReconCopy(3, 12343, 1, 0, 0x10);            /* a damaged copy never completes */
    check(dmrSmsAckTake(ack) == 0, "an incomplete PDU is not acknowledged");

    fresh();
    trxDMRID = 12343;                               /* addressed to someone else */
    playReconCopy(0, 12399, -1, 0, 0);
    check(dmrSmsAckTake(ack) == 0, "a PDU for another radio is not acknowledged");
    trxDMRID = 2345678;

    /* encrypted under a key we do not hold: still acknowledged at the link layer */
    stream_t s, c;
    fresh();
    sendAndCapture("secret", trxDMRID, 0, 1, &s);
    toConfirmed(&s, plaintextLen("secret"), trxDMRID, &c);
    dmrAesInit();                                   /* drop the keys */
    playback(&c, 0);
    check(dmrSmsCount(0) == 0 && (dmrSmsAckTake(ack) == 1) && ((ack[0] & 0x0F) == 0x01) && (ack[1] == 0x40) && (ack[8] == 0) && (ack[9] == 0x08),
          "a confirmed PDU we cannot decrypt is still acknowledged");

    /* confirmed but without the response-requested bit: no answer */
    fresh();
    sendAndCapture("quiet", trxDMRID, 0, 1, &s);
    toConfirmed(&s, plaintextLen("quiet"), trxDMRID, &c);
    for (int i = 0; i < c.nhdr; i++) { if ((c.hdr[i][0] & 0x0F) == 0x03) { c.hdr[i][0] &= (uint8_t)~0x40; } }
    playback(&c, 0);
    check(dmrSmsCount(0) == 1 && (dmrSmsAckTake(ack) == 0), "a confirmed PDU that does not ask for a response gets none");

    /* unconfirmed group SMS: no response asked for, none sent */
    fresh();
    sendAndCapture("to all", 9, 1, 1, &s);
    playback(&s, 0);
    check(dmrSmsCount(0) == 1 && (dmrSmsAckTake(ack) == 0), "an unconfirmed group SMS is not acknowledged");
}

/* A network-relayed SMS (BrandMeister through a repeater or hotspot, MOTOTRBO gateways)
 * arrives as Defined Short Data: header DPF 0x0D, SAP 0xA, the text itself in the blocks
 * with no IP/UDP framing, UTF-16BE (DD format 0x14) or 8-bit, bit padding in header byte 9,
 * the packet CRC32 in the last block like any data PDU. Build one and play it. */
static void playDefinedShortData(uint32_t src, uint32_t dst, int group, int respReq, uint8_t ddFormat,
                                 const uint8_t *payload, int payloadLen)
{
    uint8_t pdu[120];
    int total = ((payloadLen + 4 + 11) / 12) * 12;
    int pad = total - 4 - payloadLen;
    int nblk = total / 12;
    uint8_t hdr[12] = { 0 };
    memset(pdu, 0, sizeof pdu);
    memcpy(pdu, payload, (size_t)payloadLen);
    uint32_t crc = crc32_dmr(pdu, total);
    pdu[total - 4] = (uint8_t)(crc >> 24); pdu[total - 3] = (uint8_t)(crc >> 16);
    pdu[total - 2] = (uint8_t)(crc >> 8);  pdu[total - 1] = (uint8_t)crc;
    hdr[0] = (uint8_t)((group ? 0x80 : 0) | (respReq ? 0x40 : 0) | (((nblk >> 4) & 0x03) << 4) | 0x0D);
    hdr[1] = (uint8_t)(0xA0 | (nblk & 0x0F));
    hdr[2] = (uint8_t)(dst >> 16); hdr[3] = (uint8_t)(dst >> 8); hdr[4] = (uint8_t)dst;
    hdr[5] = (uint8_t)(src >> 16); hdr[6] = (uint8_t)(src >> 8); hdr[7] = (uint8_t)src;
    hdr[8] = (uint8_t)((ddFormat << 2) | 0x01);            /* DD format, SARQ 0, F 1 */
    hdr[9] = (uint8_t)(pad * 8);                           /* bit padding */
    dmrSmsRxBurst(6, hdr);
    for (int b = 0; b < nblk; b++) { dmrSmsRxBurst(7, pdu + b * 12); }
    dmrSmsRxReset();
    dmrSmsRxTick();
}

static void testDefinedShortDataSms(void)
{
    static const uint8_t HELLO_BE[] = { 0,'N',0,'e',0,'t',0,' ',0,'h',0,'e',0,'l',0,'l',0,'o' };   /* "Net hello" UTF-16BE */
    static const uint8_t EIGHT_BIT[] = "Latin text";
    uint8_t ack[12];
    const dmrSmsMessage_t *m;

    fresh();
    trxDMRID = 12342;
    playDefinedShortData(2621234, 12342, 0, 1, 0x14, HELLO_BE, sizeof HELLO_BE);
    check(dmrSmsCount(0) == 1 && inboxHas("Net hello"), "a private Defined Short Data message in UTF-16BE decodes into the inbox");
    m = dmrSmsGet(0, 0);
    check(m && (m->peerId == 2621234) && !(m->flags & DMR_SMS_FLAG_GROUP), "stored as private from the network sender");
    check((dmrSmsAckTake(ack) == 1) && ((ack[0] & 0x0F) == 0x01) && (ack[9] == 0x08), "its response request is acknowledged (status 0, no N(S) in a short data header)");

    playDefinedShortData(2621234, 12342, 0, 1, 0x14, HELLO_BE, sizeof HELLO_BE);   /* the network repeats it */
    check(dmrSmsCount(0) == 1 && (dmrSmsAckTake(ack) == 1), "a repeat within the window is acknowledged and not stored again");

    clockAdvance(61000);
    playDefinedShortData(2621234, 9, 1, 0, 0x03, EIGHT_BIT, (int)strlen((const char *)EIGHT_BIT));
    check(dmrSmsCount(0) == 2 && inboxHas("Latin text"), "a group Defined Short Data message in 8-bit text decodes");
    m = dmrSmsGet(0, 0);
    check(m && (m->flags & DMR_SMS_FLAG_GROUP) && (dmrSmsAckTake(ack) == 0), "stored as a group message; no acknowledgement for a group message");

    playDefinedShortData(2621234, 12399, 0, 1, 0x14, HELLO_BE, sizeof HELLO_BE);
    check(dmrSmsCount(0) == 2 && (dmrSmsAckTake(ack) == 0), "a private short data message for another radio is neither stored nor acknowledged");
    trxDMRID = 2345678;
}

/* The stock radio's OTHER text format, "DMR format" (the CPS channel option next to the
 * Motorola IP/UDP/TMS one), live on the bench 2026-10-24 (RADIO3 12343, channel "PD-UE",
 * key 1 selected, HackRF + DSD-FME, the fork's burst trace agreeing byte for byte): no IP/UDP,
 * a 5-byte prefix [sequence 00 00|02 01 01], UTF-16BE text, the header's padding octets, the
 * CRC32. The private messages went as ONE Confirmed rate-3/4 block (header 43 93: individual,
 * response requested, DPF 3, SAP 9, POC 3; then the ENC header 4F 10 51 01), the group one as
 * two Unconfirmed rate-1/2 blocks (82 99 ... : POC 9). All three CRC32s verify. Being 15 bytes
 * or shorter, none of them was actually encrypted under its ENC header (the firmware ECB-encrypts
 * whole 16-byte blocks only), so these are the clear forms; the encrypted form is built below. */
static const uint8_t TYT_PRIV_GM[16] = { 0x03,0x00,0x00,0x01,0x01,0x00,0x67,0x00,0x6D,0x00,0x00,0x00,0x95,0xBF,0xFE,0x7A };
static const uint8_t TYT_PRIV_GT[16] = { 0x04,0x00,0x00,0x01,0x01,0x00,0x67,0x00,0x74,0x00,0x00,0x00,0xBE,0x13,0xAC,0x8D };
static const uint8_t TYT_GROUP_GTW[24] = { 0x05,0x00,0x02,0x01,0x01,0x00,0x67,0x00,0x74,0x00,0x77,0x00,
                                           0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x94,0xBA,0xC0,0x6D };
static const uint8_t TYT_ENC_HDR[12] = { 0x4F,0x10,0x51,0x01,0,0,0,0,0,0,0,0 };

/* One stock DMR-format PDU as the chip delivers it. Private: the Confirmed header (N(S) ns),
 * the ENC header, the 16-byte payloads behind a stand-in DBSN/CRC-9 field. Group: the
 * Unconfirmed header, the ENC header, 12-byte rate-1/2 blocks. */
static void playTytFormat(uint32_t src, uint32_t dst, int group, uint8_t ns, int poc, const uint8_t *pdu, int pduLen)
{
    uint8_t hdr[12] = { 0 };
    int pay = group ? 12 : 16;
    int nblk = pduLen / pay;
    hdr[0] = (uint8_t)(group ? 0x82 : 0x43);
    hdr[1] = (uint8_t)(0x90 | (poc & 0x0F));
    hdr[2] = (uint8_t)(dst >> 16); hdr[3] = (uint8_t)(dst >> 8); hdr[4] = (uint8_t)dst;
    hdr[5] = (uint8_t)(src >> 16); hdr[6] = (uint8_t)(src >> 8); hdr[7] = (uint8_t)src;
    hdr[8] = (uint8_t)(0x80 | (nblk + 1));                   /* F=1, blocks to follow incl. the ENC header */
    hdr[9] = (uint8_t)(group ? 0x00 : ((ns & 0x07) << 4));
    dmrSmsRxBurst(6, hdr);
    dmrSmsRxBurst(6, TYT_ENC_HDR);
    for (int b = 0; b < nblk; b++)
    {
        if (group) { dmrSmsRxBurst(7, pdu + b * 12); }
        else
        {
            uint8_t raw[18] = { (uint8_t)(b << 1), 0x5A };
            memcpy(raw + 2, pdu + b * 16, 16);
            dmrSmsRxBurst(8, raw);
        }
    }
    dmrSmsRxReset();
    dmrSmsRxTick();
}

static void testStockDmrFormatSms(void)
{
    uint8_t ack[12];
    const dmrSmsMessage_t *m;
    fresh();
    trxDMRID = 12341;

    playTytFormat(12343, 12341, 0, 0, 3, TYT_PRIV_GM, 16);
    check(dmrSmsCount(0) == 1 && inboxHas("gm"), "a stock private DMR-format SMS (one rate-3/4 block) decodes into the inbox");
    m = dmrSmsGet(0, 0);
    check(m && (m->peerId == 12343) && !(m->flags & DMR_SMS_FLAG_GROUP), "stored as private from the stock radio");
    check((dmrSmsAckTake(ack) == 1) && (ack[9] == 0x08) && (ack[2] == 0x00) && (ack[3] == 0x30) && (ack[4] == 0x37), "its response request is acknowledged to the sender");

    playTytFormat(12343, 12341, 0, 1, 3, TYT_PRIV_GT, 16);
    check(dmrSmsCount(0) == 2 && inboxHas("gt"), "the next message (sequence 4) is a second message");
    check((dmrSmsAckTake(ack) == 1) && (ack[9] == 0x09), "acknowledged with its N(S)");

    playTytFormat(12343, 12341, 0, 1, 3, TYT_PRIV_GT, 16);   /* the sender's repeat (no ACK heard) */
    check(dmrSmsCount(0) == 2 && (dmrSmsAckTake(ack) == 1), "its repeat (same sequence byte) is acknowledged and not stored again");

    clockAdvance(61000);
    playTytFormat(12343, 12341, 0, 2, 3, TYT_PRIV_GT, 16);   /* same text, same sequence, long after */
    check(dmrSmsCount(0) == 2 && (dmrSmsAckTake(ack) == 1), "the same sequence byte from the same sender is still a repeat after the text window (and acknowledged)");

    playTytFormat(12343, 9661, 1, 0, 9, TYT_GROUP_GTW, 24);
    check(dmrSmsCount(0) == 3 && inboxHas("gtw"), "a stock group DMR-format SMS (two rate-1/2 blocks) decodes");
    m = dmrSmsGet(0, 0);
    check(m && (m->flags & DMR_SMS_FLAG_GROUP) && (m->peerId == 12343) && (dmrSmsAckTake(ack) == 0), "stored as a group message, not acknowledged");

    playTytFormat(12343, 12399, 0, 0, 3, TYT_PRIV_GM, 16);
    check(dmrSmsCount(0) == 3 && (dmrSmsAckTake(ack) == 0), "a DMR-format message for another radio is neither stored nor acknowledged");

    /* under a lost ENC header (its burst failed the chip's CRC) a clear-form message still reads */
    {
        uint8_t hdr[12] = { 0x43,0x93,0x00,0x30,0x35,0x00,0x30,0x37,0x82,0x00,0,0 };
        uint8_t raw[18] = { 0x00, 0x5A };
        memcpy(raw + 2, TYT_PRIV_GM, 16);
        dmrSmsRxBurst(6, hdr);
        dmrSmsRxBurst(8, raw);
        dmrSmsRxReset();
        dmrSmsRxTick();
        check(dmrSmsCount(0) == 4 && inboxHas("gm"), "a DMR-format message whose ENC header was lost still decodes");
    }
    trxDMRID = 2345678;
}

/* A longer DMR-format message is encrypted the way the IP format is: AES-256-ECB over the whole
 * 16-byte blocks of [prefix + text], the tail and the padding clear. Built from the captured
 * prefix (sequence 6, private), 19 characters = 43 bytes -> 2 blocks encrypted, 11 clear, POC 1
 * to fill three rate-3/4 payloads. */
static void testStockDmrFormatEncryptedSms(void)
{
    static const char TEXT[] = "Encrypted DMR text!";   /* 19 chars */
    uint8_t pdu[48];
    int tlen = (int)strlen(TEXT);
    int len = 5 + 2 * tlen;                              /* 43 */
    int total = 48, poc = total - 4 - len;               /* 1 */
    memset(pdu, 0, sizeof pdu);
    pdu[0] = 0x06; pdu[1] = 0x00; pdu[2] = 0x00; pdu[3] = 0x01; pdu[4] = 0x01;
    for (int i = 0; i < tlen; i++) { pdu[5 + 2 * i] = 0x00; pdu[6 + 2 * i] = (uint8_t)TEXT[i]; }
    for (int i = 0; i + 16 <= len; i += 16) { aes256_ecb_encrypt(KEY1, pdu + i); }
    uint32_t crc = crc32_dmr(pdu, total);
    pdu[total - 4] = (uint8_t)(crc >> 24); pdu[total - 3] = (uint8_t)(crc >> 16);
    pdu[total - 2] = (uint8_t)(crc >> 8);  pdu[total - 1] = (uint8_t)crc;

    fresh();
    trxDMRID = 12341;
    playTytFormat(12343, 12341, 0, 0, poc, pdu, total);
    check(dmrSmsCount(0) == 1 && inboxHas(TEXT), "an encrypted DMR-format SMS decrypts under the signalled key");

    uint8_t other[32]; memcpy(other, KEY1, 32); other[5] ^= 0x33;
    fresh();
    trxDMRID = 12341;
    dmrAesInit(); dmrAesSetKeyRam(1, other);
    playTytFormat(12343, 12341, 0, 0, poc, pdu, total);
    check(dmrSmsCount(0) == 0 && notifications == 0, "under the wrong key it is dropped");

    fresh();
    trxDMRID = 12341;
    dmrAesInit(); dmrAesSetKeyRam(3, KEY1);              /* the key sits in another slot: every loaded key is tried */
    playTytFormat(12343, 12341, 0, 0, poc, pdu, total);
    check(dmrSmsCount(0) == 1 && inboxHas(TEXT), "decrypts under a key loaded in another slot");
    trxDMRID = 2345678;
}

static void testEncHeaderWithGroupBit(void)
{
    stream_t s;
    fresh();
    sendAndCapture("group enc", 9, 1, 1, &s);
    for (int i = 0; i < s.nhdr; i++) { if (s.hdr[i][0] == 0x4F) { s.hdr[i][0] = 0xCF; } }
    playback(&s, 0);
    check(dmrSmsCount(0) == 1 && inboxHas("group enc"), "ENC header with the group bit set still decrypts");
}

static void testLostEncHeaderStillDecrypts(void)
{
    stream_t s;
    fresh();
    sendAndCapture("lost header", 9, 1, 1, &s);
    playback(&s, 1);   /* the ENC header burst failed its CRC and never reached us */
    check(dmrSmsCount(0) == 1 && inboxHas("lost header"), "encrypted SMS whose ENC header was lost still decrypts");
}

static void testWrongKeyIsDropped(void)
{
    stream_t s;
    fresh();
    sendAndCapture("secret", 9, 1, 1, &s);
    uint8_t other[32]; memcpy(other, KEY1, 32); other[0] ^= 0x55;
    dmrAesInit(); dmrAesSetKeyRam(1, other);   /* receiver has a different key 1 */
    playback(&s, 0);
    check(dmrSmsCount(0) == 0 && notifications == 0, "encrypted SMS under an unknown key is dropped");
}

int main(void)
{
    testUnconfirmedGroupEncryptedRoundTrip();
    testUnconfirmedClearRoundTrip();
    testConfirmedPrivateEncryptedSms();
    testConfirmedPrivateClearSms();
    testConfirmedPrivateRate34EncryptedSms();
    testConfirmedPrivateRate34ClearSms();
    testStockReconPrivateHello();
    testStockRetransmitBlockMerge();
    testPrivateSmsToAnotherRadioIsNotOurs();
    testRetransmissionIsNotASecondMessage();
    testDiagResetForgetsLastMessage();
    testCleartextPduReportsNoKeyId();
    testConfirmedPrivateSmsIsAcknowledged();
    testDefinedShortDataSms();
    testStockDmrFormatSms();
    testStockDmrFormatEncryptedSms();
    testEncHeaderWithGroupBit();
    testLostEncHeaderStillDecrypts();
    testWrongKeyIsDropped();
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}

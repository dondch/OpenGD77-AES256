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
uint32_t ticksGetMillis(void) { static uint32_t t = 1000; return t += 60; }

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

/* A captured stream, split into what the chip hands the ISR: headers (type 6) and blocks (type 7). */
typedef struct
{
    uint8_t hdr[4][12]; int nhdr;      /* data header [, ENC header] */
    uint8_t blk[32][12]; int nblk;     /* rate-1/2 data blocks */
} stream_t;

static void capture(stream_t *s)
{
    memset(s, 0, sizeof *s);
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
    for (int i = 0; i < s->nblk; i++) { dmrSmsRxBurst(7, s->blk[i]); }
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

/* Rewrite an unconfirmed stream as a stock radio's CONFIRMED delivery to one radio:
 * DPF 3, response requested, and 10-byte block payloads behind a 2-byte DBSN/CRC-9 field. */
static void toConfirmed(const stream_t *in, int plaintextLen, uint32_t dst, stream_t *out)
{
    uint8_t ct[400], pdu[400];
    int ctLen = plaintextLen;             /* ciphertext (or cleartext) length == IP packet length */
    memset(out, 0, sizeof *out);
    for (int i = 0; i < in->nblk; i++) { memcpy(ct + i * 12, in->blk[i], 12); }
    int total = (((ctLen + 4) + 9) / 10) * 10;
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
            out->hdr[i][8] = (uint8_t)(0x80 | ((total / 10) + (in->nhdr > 1 ? 1 : 0)));
            out->hdr[i][9] = 0x00;                       /* S=0, Ns=0, FSN=0 */
        }
    }
    out->nhdr = in->nhdr;
    out->nblk = total / 10;
    for (int b = 0; b < out->nblk; b++)
    {
        uint16_t sn_crc9 = (uint16_t)((b & 0x7F) << 9) | (uint16_t)(0x1A5 ^ b);   /* DBSN + a CRC-9 we don't check */
        out->blk[b][0] = (uint8_t)(sn_crc9 >> 8);
        out->blk[b][1] = (uint8_t)sn_crc9;
        memcpy(out->blk[b] + 2, pdu + b * 10, 10);
    }
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
    sendAndCapture("ack me", trxDMRID, 0, 1, &s);   /* same text, new message (new IP id / TMS seq) */
    toConfirmed(&s, plaintextLen("ack me"), trxDMRID, &c);
    playback(&c, 0);
    check(dmrSmsCount(0) == 2, "the same text sent again as a new message is stored");
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
    testPrivateSmsToAnotherRadioIsNotOurs();
    testRetransmissionIsNotASecondMessage();
    testEncHeaderWithGroupBit();
    testLostEncHeaderStillDecrypts();
    testWrongKeyIsDropped();
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}

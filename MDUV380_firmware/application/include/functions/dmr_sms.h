/*
 * dmr_sms.h — on-radio encrypted DMR short-message (SMS) service for OpenGD77-AES.
 *
 * Builds on the proven pieces:
 *   - crypto/dmr_aes.c  : AES-256-ECB encrypt/decrypt + dmr_sms_rx_decrypt (RE'd + on-air
 *                         validated 2026-06-27; a stock TYT decrypts our TX).
 *   - functions/dmr_data.c : dmrDataTxLoad() data-burst TX harness (keys a data call).
 *   - hardware/HR-C6000.c  : the RX interrupt hands each BPTC-decoded, CRC-checked data
 *                            burst (page 0x02) to dmrSmsRxBurst() for software reassembly.
 *
 * The whole module is behind ENABLE_DMR_DATA, so builds without it are unchanged.
 * Message store lives in the OpenGD77 custom-data MESSAGES block (SPI flash) so it survives a
 * power-cycle; a CCM-RAM working copy is the live store (no net-new main RAM -> AMBE codec
 * buffer addresses don't shift). See dmr_sms.c.
 *
 * LEGAL: encrypted traffic is illegal on amateur bands in most jurisdictions. PMR/commercial use.
 */
#ifndef _OPENGD77_DMR_SMS_H_
#define _OPENGD77_DMR_SMS_H_

#include <stdint.h>

#if defined(ENABLE_DMR_DATA)

#define DMR_SMS_TEXT_MAX    144    /* hard max chars stored/composed per message (buffer size) */
#define DMR_SMS_STORE_DATA  1342   /* packed variable-length message bytes (keeps the MSGS block
                                    * the same 1352 B total as the old fixed 24x56 array) */

/* message record flags */
#define DMR_SMS_FLAG_USED      0x01
#define DMR_SMS_FLAG_UNREAD    0x02
#define DMR_SMS_FLAG_OUTGOING  0x04   /* a Sent message (else Inbox)        */
#define DMR_SMS_FLAG_GROUP     0x08   /* peerId is a talkgroup (else DMR ID) */

typedef struct
{
	uint8_t  flags;
	uint8_t  textLen;
	uint16_t seq;                 /* monotonically increasing -> newest = highest seq */
	uint32_t peerId;              /* inbox: source ID; sent: destination ID/TG         */
	char     text[DMR_SMS_TEXT_MAX];
} dmrSmsMessage_t;

/* ---- lifecycle --------------------------------------------------------- */
/* Load the message store from flash into the CCM working copy. Call once at boot. */
void dmrSmsInit(void);

/* ---- store query (UI) -------------------------------------------------- */
/* Count of messages for a folder: outgoing!=0 -> Sent, ==0 -> Inbox. */
int  dmrSmsCount(int outgoing);
/* idx 0 = newest. Returns NULL if out of range. */
const dmrSmsMessage_t *dmrSmsGet(int outgoing, int idx);
/* Total unread (Inbox) messages — for the idle-screen indicator. */
int  dmrSmsUnreadCount(void);

/* ---- store mutate (UI) ------------------------------------------------- */
void dmrSmsMarkRead(int outgoing, int idx);
void dmrSmsMarkAllRead(void);
void dmrSmsDelete(int outgoing, int idx);
void dmrSmsDeleteAll(int outgoing);     /* outgoing<0 -> both folders */

/* ---- RX (HR-C6000 ISR context) ----------------------------------------- *
 * Called for each CRC-valid data-sync burst: rxDataType is the ETSI slot data type
 * (6 = Data Header, 7 = Rate-1/2 Data, 8 = Rate-3/4 Data), payload = the decoded info
 * bytes read from HR-C6000 page 0x02 (12 for types 6 and 7, 18 for type 8). Accumulates
 * a PDU; on completion raises a flag the main loop drains in dmrSmsRxTick(). Cheap +
 * ISR-safe (no flash, no AES here). A stock TYT sends a private SMS in rate-3/4
 * confirmed blocks and a group SMS in rate-1/2 unconfirmed ones. */
void dmrSmsRxBurst(int rxDataType, const uint8_t *payload12);
/* Drop any partial RX accumulation (call on Terminator / call end). */
void dmrSmsRxReset(void);
/* The acknowledgement a confirmed private SMS asks for: when such a PDU addressed to us has
 * just completed, fills out[12] with the Response PDU data header (DPF 1, ACK, status = the
 * acked N(S)) and returns 1, else 0. ISR context, called by the HR-C6000 right after the
 * burst that completed the packet so the header goes out in the very next timeslot, which
 * is when a stock radio sends it (bench: 0 to 1 ms after the sender's last burst, no CSBK
 * preamble). Taken once. */
int  dmrSmsAckTake(uint8_t out[12]);

/* Diagnostic (bench bring-up). dmrSmsRxDiagBurst() is called for EVERY data-sync-class
 * burst the chip delivers (any type, any CRC). dmrSmsRxDiag() fills out[7]:
 *   [0]=all data bursts, [1]=hdr CRC-ok, [2]=hdr CRC-bad, [3]=blk CRC-ok,
 *   [4]=blk CRC-bad, [5]=PDUs reassembled, [6]=messages decoded. */
void dmrSmsRxDiagBurst(int rxDataType, int crcOk);
void dmrSmsRxDiag(uint32_t out[7]);
/* Zero the counters and trace, and forget the last stored message (the retransmission filter)
 * and any kept copy, so the same capture can be replayed without a reboot. */
void dmrSmsRxDiagReset(void);
/* Dump the last reassembled (still-encrypted) PDU + metadata for offline analysis:
 * out = [pduLen_hi, pduLen_lo, keyId, expBlocks, peer(4 LE), rawPdu...]. Returns length. */
int  dmrSmsRxLastPdu(uint8_t *out, int maxlen);
/* Per-burst trace of the last 8 bursts handed to dmrSmsRxBurst(): out = [writeIndex, then
 * 8 x (type, state, payload[0..3])], state = blocks held before the burst (bits 0..5) |
 * haveHeader << 6 | pduReady << 7. Returns bytes written (49), 0 if maxlen is short. */
int  dmrSmsRxTrace(uint8_t *out, int maxlen);

/* ---- RX (main-loop context) -------------------------------------------- *
 * Decrypt a completed PDU, store it to the Inbox, and pop a notification. Safe to
 * call every main-loop tick; does nothing unless a PDU has been reassembled. */
void dmrSmsRxTick(void);

/* ---- config (quick-text presets + default recipient) ------------------- *
 * Read from the MSGC custom-data block written by the CHIRP module. */
#define DMR_SMS_NUM_PRESETS  10
int  dmrSmsPresetCount(void);              /* number of non-empty presets */
const char *dmrSmsPresetGet(int idx);      /* preset text by index, or NULL */
/* Default compose recipient; *dst==0 if unset (fall back to current channel). */
void dmrSmsDefaultRecipient(uint32_t *dst, int *group);
/* CHIRP-configurable max compose length, clamped to [1, DMR_SMS_TEXT_MAX]; the MSGC
 * maxLen byte (0 or out-of-range -> DMR_SMS_TEXT_MAX). Limits on-radio composing only. */
int  dmrSmsMaxLen(void);
/* CHIRP "Encrypt SMS" master gate: 1 = encrypt per-channel like voice, 0 = always cleartext.
 * Default (unset MSGC byte) = 1, preserving the encrypted behaviour. */
int  dmrSmsEncryptEnabled(void);

/* ---- TX ---------------------------------------------------------------- *
 * Build a stock-TYT-compatible SMS and key a data call. Encrypted (AES-256-ECB + ENC header)
 * or cleartext per the CHIRP master gate and the channel's encrypt byte (voice logic).
 *   text  : ASCII message (encoded UTF-16LE on the wire)
 *   dst   : destination talkgroup (group!=0) or DMR ID (group==0)
 *   keyId : explicit AES key slot 1..15 (0 -> follow the gate + per-channel voice logic)
 * Returns 0 on success, <0 on error (busy / bad args). Also files the message into Sent. */
int  dmrSmsSend(const char *text, uint32_t dst, int group, uint8_t keyId);

/* DIAGNOSTIC (USB 0x97 A/B): make the next dmrSmsSend calls skip the store_add() Sent-folder
 * flash write, to isolate bug #3 (menu sends after the first fail; USB harness sends don't).
 * The menu path never sets this; it is toggled only around the 0x97 handler call. */
void dmrSmsDiagSetSkipStore(int skip);

#if defined(ENABLE_REMOTE_PROVISION)
/* ---- shared with the remote-provisioning sender (dmr_prov.c) ------------- *
 * The 520-byte burst-queue scratch dmrSmsSend builds into (only live inside the building
 * call: dmrDataTxLoad copies it), and the on-air CRCs, so a provisioning PDU is framed
 * exactly like an SMS without a second copy of either. */
uint8_t *dmrSmsBurstQueueScratch(void);
uint32_t dmrSmsCrc32(const uint8_t *pdu, int len);
void     dmrSmsHdrCrc(const uint8_t *h, int len, uint16_t mask, uint8_t out2[2]);
#endif

#endif /* ENABLE_DMR_DATA */
#endif /* _OPENGD77_DMR_SMS_H_ */

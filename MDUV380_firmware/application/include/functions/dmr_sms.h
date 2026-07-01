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
 * The whole module is behind ENABLE_AES + ENABLE_DMR_DATA, so stock builds are byte-identical.
 * Message store lives in the OpenGD77 custom-data MESSAGES block (SPI flash) so it survives a
 * power-cycle; a CCM-RAM working copy is the live store (no net-new main RAM -> AMBE codec
 * buffer addresses don't shift). See dmr_sms.c.
 *
 * LEGAL: encrypted traffic is illegal on amateur bands in most jurisdictions. PMR/commercial use.
 */
#ifndef _OPENGD77_DMR_SMS_H_
#define _OPENGD77_DMR_SMS_H_

#include <stdint.h>

#if defined(ENABLE_DMR_DATA) && defined(ENABLE_AES)

#define DMR_SMS_TEXT_MAX    48     /* max chars stored/composed per message (ASCII) */
#define DMR_SMS_MAX_COUNT   24     /* ring capacity (inbox + sent combined)          */

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
 * (6 = Data Header, 7 = Rate-1/2 Data), payload12 = the 12 BPTC-decoded info bytes
 * read from HR-C6000 page 0x02. Accumulates a PDU; on completion raises a flag the
 * main loop drains in dmrSmsRxTick(). Cheap + ISR-safe (no flash, no AES here). */
void dmrSmsRxBurst(int rxDataType, const uint8_t *payload12);
/* Drop any partial RX accumulation (call on Terminator / call end). */
void dmrSmsRxReset(void);

/* Diagnostic (bench bring-up). dmrSmsRxDiagBurst() is called for EVERY data-sync-class
 * burst the chip delivers (any type, any CRC). dmrSmsRxDiag() fills out[7]:
 *   [0]=all data bursts, [1]=hdr CRC-ok, [2]=hdr CRC-bad, [3]=blk CRC-ok,
 *   [4]=blk CRC-bad, [5]=PDUs reassembled, [6]=messages decoded. */
void dmrSmsRxDiagBurst(int rxDataType, int crcOk);
void dmrSmsRxDiag(uint32_t out[7]);
void dmrSmsRxDiagReset(void);
/* Dump the last reassembled (still-encrypted) PDU + metadata for offline analysis:
 * out = [pduLen_hi, pduLen_lo, keyId, expBlocks, peer(4 LE), rawPdu...]. Returns length. */
int  dmrSmsRxLastPdu(uint8_t *out, int maxlen);

/* Chip "Received Information" RX-RAM capture: HR-C6000.c calls dmrSmsRiCapture() when the
 * chip signals a fully reassembled PDU; dmrSmsRiDump() returns the last dump over USB:
 * out = [riCount(4 LE), reg90, len_hi, len_lo, ramBytes...]. */
void dmrSmsRiCapture(uint8_t reg90, const uint8_t *ram, int len);
int  dmrSmsRiDump(uint8_t *out, int maxlen);

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

/* ---- TX ---------------------------------------------------------------- *
 * Build a stock-TYT-compatible AES-256-ECB encrypted SMS and key a data call.
 *   text  : ASCII message (encoded UTF-16LE on the wire)
 *   dst   : destination talkgroup (group!=0) or DMR ID (group==0)
 *   keyId : AES key slot 1..15 (0 -> use the global TX key)
 * Returns 0 on success, <0 on error (no key / busy / bad args). Also files the
 * message into the Sent folder. */
int  dmrSmsSend(const char *text, uint32_t dst, int group, uint8_t keyId);

#endif /* ENABLE_DMR_DATA && ENABLE_AES */
#endif /* _OPENGD77_DMR_SMS_H_ */

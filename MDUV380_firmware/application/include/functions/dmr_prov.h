/*
 * dmr_prov.h — secure remote provisioning over DMR data (ENABLE_REMOTE_PROVISION).
 *
 * A master radio pushes codeplug writes to slave radios in encrypt-then-MAC PDUs riding the
 * DMR data path of dmr_sms.c / dmr_data.c. The design, PDU layout, key derivation, sequence
 * rules, transaction state machine and threat model are in PROVISIONING.md; this header is
 * the API between the module (dmr_prov.c, host-testable), the firmware glue that writes the
 * codeplug (dmr_prov_apply.c), the SMS data path and the USB dev commands.
 *
 * Needs ENABLE_DMR_DATA. Compiles to nothing without ENABLE_REMOTE_PROVISION.
 */
#ifndef _OPENGD77_DMR_PROV_H_
#define _OPENGD77_DMR_PROV_H_

#include <stdint.h>

#if defined(ENABLE_REMOTE_PROVISION)

/* ---- the PROV custom-data block (SPI flash), PROVISIONING.md §2.1 ------------ */
#define DMR_PROV_BLOCK_LEN      96
#define DMR_PROV_MAX_MASTERS    4
#define DMR_PROV_KEY_BYTES      32
#define DMR_PROV_SEQ_RESERVE    64          /* master: flash write once per this many PDUs */
#define DMR_PROV_SEQ_LIMIT      0xFFFFFF00u /* master refuses to send at or above this */

/* ---- the frame, §3.2 ------------------------------------------------------------ */
#define DMR_PROV_MAGIC          0x50
#define DMR_PROV_VERSION        0x01
#define DMR_PROV_HDR_LEN        10
#define DMR_PROV_TAG_LEN        16
#define DMR_PROV_SAP            9           /* proprietary packet data, no proprietary header */
#define DMR_PROV_BODY_MAX       260         /* op + the largest argument block (zone: 1+16+160) */
#define DMR_PROV_PDU_MAX        320         /* 32 confirmed rate-1/2 blocks x 10 payload bytes */

/* operations (first body byte), §3.3 */
#define DMR_PROV_OP_STATUS        0x01
#define DMR_PROV_OP_WRITE_CHANNEL 0x10
#define DMR_PROV_OP_WRITE_ZONE    0x11
#define DMR_PROV_OP_SET_VFO       0x12
#define DMR_PROV_OP_SET_SETTING   0x13
#define DMR_PROV_OP_STORE_KEY     0x14
#define DMR_PROV_OP_COMMIT        0x20
#define DMR_PROV_OP_ABORT         0x21
#define DMR_PROV_OP_REBOOT        0x30
#define DMR_PROV_OP_STATUS_REPLY  0x81

/* result codes in a STATUS reply */
#define DMR_PROV_RES_OK            0
#define DMR_PROV_RES_BAD_OP        1
#define DMR_PROV_RES_BAD_SEQ       2
#define DMR_PROV_RES_TXN_MISMATCH  3
#define DMR_PROV_RES_CRC_MISMATCH  4
#define DMR_PROV_RES_NO_SPACE      5
#define DMR_PROV_RES_BUSY          6
#define DMR_PROV_RES_WRITE_FAILED  7
#define DMR_PROV_RES_TIMEOUT       8

/* STATUS reply body after the op byte: result(1) txn(1) lastSeq(4 BE) codeplugCrc(4 BE)
 * gitHash(8) battery centivolts(2 BE) staged(1) failedMacs(2 BE) = 23 bytes */
#define DMR_PROV_STATUS_BODY_LEN  (1 + 23)

/* ---- lifecycle (main loop task) ------------------------------------------------- */
/* Boot: load the PROV block, derive the session keys, zero the CCM state. */
void dmrProvInit(void);
/* The block was rewritten over USB: reload it and re-derive (main loop or CPS task, no flash
 * write). */
void dmrProvReload(void);
/* Main loop: slave commits, the slave's deferred STATUS reply, the master's send/ACK/STATUS
 * state machine, transaction and hold-off timeouts. Cheap when idle. */
void dmrProvTick(void);
/* 1 when a pairing key is loaded (either role can work). */
int  dmrProvIsPaired(void);

/* ---- RX side, called from dmr_sms.c --------------------------------------------- */
/* ISR-safe: is id one of the configured master IDs? Gates the link-layer ACK of a
 * provisioning PDU (SAP 9, no ENC header) in the HR-C6000 ISR. */
int  dmrProvIsMasterId(uint32_t id);
/* ISR-safe: 0 while the failed-MAC hold-off is in force (no ACKs then). */
int  dmrProvAckAllowed(void);
/* Main loop: a complete SAP 9 PDU without an ENC header, addressed as (src -> dst), pdu =
 * frame || padding || CRC32 (len bytes, CRC already checked by the caller). Decrypts in
 * place. Returns 1 when the PDU was a provisioning frame (consumed, whatever the verdict),
 * 0 when it is not one (the caller may try other decoders). */
int  dmrProvRxPdu(uint8_t *pdu, int len, uint32_t src, uint32_t dst);
/* ISR: an ETSI Response PDU (data header DPF 1) from src to dst with its status byte
 * (class/type/status, N(S) in the low 3 bits). The master's ACK. */
void dmrProvRxResponse(uint32_t src, uint32_t dst, uint8_t status);

/* ---- master job API (USB dev command 0x9A), §7 ---------------------------------- *
 * job = slaveId(4 LE) txn(1) flags(1) then items, each op(1) len(2 LE) body(len).
 * flags bit0: append an implicit COMMIT(count, crc) after the items; bit1: the items are
 * single-shot (STATUS / REBOOT), no COMMIT. Returns 0 when queued, <0: -1 busy (a job is
 * running), -2 not paired, -3 arena held by another feature, -4 malformed, -5 too big,
 * -6 sequence exhausted (re-key); the USB handler adds -7 (the CPS screen is up: no RF). */
int  dmrProvJobLoad(const uint8_t *job, int len);
/* Snapshot for the host: phase, item index/count, tries, last ACK N(S), last STATUS fields.
 * Returns the bytes written. */
int  dmrProvJobStatus(uint8_t *out, int max);
/* Abandon a running job (the slave times out and discards what was staged). */
void dmrProvJobCancel(void);

/* ---- diagnostics (USB 0x9B) ---------------------------------------------------- */
/* Counters and slave state, never key bytes: see dmr_prov.c dmrProvDiag for the layout. */
int  dmrProvDiag(uint8_t *out, int max);

/* ---- frame / PDU builders (the master's and the host test's) -------------------- *
 * Build the complete on-air PDU for one frame: header-less payload = frame || zero padding
 * || CRC32, sized to whole confirmed rate-1/2 blocks (10 payload bytes). master/slave are
 * the IDs the MAC binds to; reply selects the direction. Returns the PDU length, or <0.
 * Uses this radio's derived keys. */
int  dmrProvBuildPdu(uint32_t master, uint32_t slave, uint32_t seq, uint8_t txn, int reply,
                     const uint8_t *body, int bodyLen, uint8_t *pdu, int pduMax);
/* Turn a PDU into the burst queue (count x [type, 12 bytes]): CSBK preamble, the confirmed
 * data header (SAP 9, A set, N(S) = seq & 7), then the blocks with DBSN and CRC-9. Returns
 * the burst count, or <0. */
int  dmrProvBuildBursts(const uint8_t *pdu, int pduLen, uint32_t dst, uint32_t src, uint32_t seq,
                        uint8_t *q, int qMaxBursts);
/* ETSI TS 102 361-1 B.3.11 CRC-9 of a confirmed rate-1/2 block (10 data bytes, 7-bit DBSN).
 * Exposed for the host test and tools/prov_packet.py. */
uint16_t dmrProvCrc9(const uint8_t *data, int len, uint8_t dbsn);

/* ---- the firmware glue dmr_prov.c calls (dmr_prov_apply.c; the host test stubs it) ---- */
int      dmrProvApplyChannel(uint16_t index, const uint8_t *record56);
int      dmrProvApplyZone(uint8_t zone, const uint8_t *record176);
int      dmrProvApplyVfo(uint8_t vfo, const uint8_t *record56);
int      dmrProvApplySetting(uint8_t field, uint32_t value);     /* staged in RAM; saved by ...SettingsSave */
int      dmrProvApplySettingsSave(void);
int      dmrProvApplyStoreKey(uint8_t slot, const uint8_t *key32);
void     dmrProvApplyReboot(void);                                /* deferred, after the reply went out */
/* Read a committed record back (the codeplug CRC in the STATUS reply); returns bytes, 0 if
 * the op has nothing to read back. */
int      dmrProvApplyReadBack(uint8_t op, const uint8_t *body, int bodyLen, uint8_t *out, int max);
int      dmrProvApplyTxAllowed(void);                             /* not RX-only, not TX inhibited */
void     dmrProvApplyNotify(const char *text);                    /* a UI notification */
uint16_t dmrProvApplyBatteryCentivolts(void);
const char *dmrProvApplyGitHash(void);                            /* 7-8 chars */
/* Block persistence: the PROV block in the custom-data region (flash write: main loop only). */
int      dmrProvApplyBlockRead(uint8_t *blk96);
int      dmrProvApplyBlockWrite(const uint8_t *blk96);
/* The staging / job buffer: the feature arena. NULL when another feature holds it. */
uint8_t *dmrProvApplyArenaClaim(void);
void     dmrProvApplyArenaRelease(void);
#define  DMR_PROV_ARENA_BYTES  11904

#endif /* ENABLE_REMOTE_PROVISION */
#endif /* _OPENGD77_DMR_PROV_H_ */

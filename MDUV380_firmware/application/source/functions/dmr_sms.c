/*
 * dmr_sms.c — on-radio encrypted DMR SMS service. See dmr_sms.h.
 *
 * Compiles to nothing unless built -DENABLE_AES -DENABLE_DMR_DATA (stock = byte-identical).
 *
 * Cipher + framing are the RE'd, on-air-validated stock-TYT scheme (a factory radio decrypts
 * our TX): AES-256-ECB over an IPv4/UDP/TMS plaintext (each 16-byte block independent, no IV),
 * carried in an Unconfirmed data PDU with a Motorola ENC extended header (ALG05 AES256). This
 * is the C port of tools/dmr_enc_sms.py (TX) and the inverse for RX, reusing crypto/dmr_aes.c.
 */
#include "functions/dmr_sms.h"

#if defined(ENABLE_DMR_DATA) && defined(ENABLE_AES)

#include "functions/dmr_data.h"
#include "functions/trx.h"
#include "functions/ticks.h"
#include "functions/codeplug.h"
#include "crypto/dmr_aes.h"
#include "crypto/dmr_aes_hook.h"
#include "user_interface/menuSystem.h"   /* uiNotificationShow + NOTIFICATION_* */
#include <string.h>

/* ETSI slot data types as reported in HR-C6000 reg 0x51 [7:4]. */
#define DT_DATA_HEADER   6
#define DT_RATE12_DATA   7
/* Burst slot-type bytes we write on TX (page 0x04 reg 0x50: type<<4). */
#define DTB_CSBK         0x30
#define DTB_DATA_HEADER  0x60
#define DTB_RATE12_DATA  0x70

#define UDP_SMS_PORT     0x0FA7          /* 4007, src==dst, stock TYT */

/* ============================ message store ============================== */
/* The store struct is the on-flash MESSAGES custom-data block verbatim; a CCM working
 * copy (DMR_AES_CCM -> no net-new main RAM, so AMBE buffers don't shift) is the live store. */
typedef struct
{
	char    magic[4];                       /* "MSGS" */
	uint8_t version;                        /* 1 */
	uint8_t count;                          /* messages in use (0..DMR_SMS_MAX_COUNT) */
	uint16_t nextSeq;                       /* next message ordering id */
	dmrSmsMessage_t msg[DMR_SMS_MAX_COUNT];
} dmrSmsStore_t;

static dmrSmsStore_t s_store DMR_AES_CCM;
static uint8_t       s_loaded DMR_AES_CCM;  /* CCM is not zeroed at boot -> guard init */

/* Zero every CCM runtime/diagnostic variable (CCM is NOT cleared at boot, so without
 * this they hold garbage: bogus rx counters, a stray s_rxReady that processes junk, a
 * stray s_cfgLoaded that skips the config load -> garbage default recipient). Defined
 * at end-of-file where all the statics are in scope; called first thing in dmrSmsInit. */
static void runtimeReset(void);

static void store_blank(void)
{
	memset(&s_store, 0, sizeof s_store);
	memcpy(s_store.magic, "MSGS", 4);
	s_store.version = 1;
	s_store.count = 0;
	s_store.nextSeq = 1;
}

void dmrSmsInit(void)
{
	uint8_t *blk = (uint8_t *)&s_store;
	runtimeReset();     /* clear CCM runtime state (counters, flags) before first use */
	s_loaded = 1;
	dmrAesLoadKeys();   /* ensure the key store is populated for RX decrypt */
	if (codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_MESSAGES, blk) &&
			(memcmp(s_store.magic, "MSGS", 4) == 0) && (s_store.version == 1) &&
			(s_store.count <= DMR_SMS_MAX_COUNT))
	{
		return;   /* valid store loaded from flash */
	}
	store_blank();
}

static void store_ensure(void)
{
	if (!s_loaded) { dmrSmsInit(); }
}

static void store_save(void)
{
	dmrAesEnsureCustomDataRegion();   /* OpenGD77 magic must exist for the block chain */
	codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_MESSAGES,
			(uint8_t *)&s_store, (int)sizeof s_store);
}

/* Find the array slot for the idx-th newest message of a folder (newest = idx 0). */
static int slot_for(int outgoing, int idx)
{
	int want = outgoing ? DMR_SMS_FLAG_OUTGOING : 0;
	int order[DMR_SMS_MAX_COUNT];
	int n = 0;
	for (int i = 0; i < DMR_SMS_MAX_COUNT; i++)
	{
		dmrSmsMessage_t *m = &s_store.msg[i];
		if ((m->flags & DMR_SMS_FLAG_USED) && ((m->flags & DMR_SMS_FLAG_OUTGOING) == want))
		{
			order[n++] = i;
		}
	}
	/* sort matching slots by seq descending (n is small) -> newest first */
	for (int a = 0; a < n; a++)
	{
		for (int b = a + 1; b < n; b++)
		{
			if (s_store.msg[order[b]].seq > s_store.msg[order[a]].seq)
			{
				int t = order[a]; order[a] = order[b]; order[b] = t;
			}
		}
	}
	return (idx >= 0 && idx < n) ? order[idx] : -1;
}

int dmrSmsCount(int outgoing)
{
	store_ensure();
	int want = outgoing ? DMR_SMS_FLAG_OUTGOING : 0;
	int n = 0;
	for (int i = 0; i < DMR_SMS_MAX_COUNT; i++)
	{
		dmrSmsMessage_t *m = &s_store.msg[i];
		if ((m->flags & DMR_SMS_FLAG_USED) && ((m->flags & DMR_SMS_FLAG_OUTGOING) == want)) { n++; }
	}
	return n;
}

const dmrSmsMessage_t *dmrSmsGet(int outgoing, int idx)
{
	store_ensure();
	int slot = slot_for(outgoing, idx);
	return (slot >= 0) ? &s_store.msg[slot] : NULL;
}

int dmrSmsUnreadCount(void)
{
	store_ensure();
	int n = 0;
	for (int i = 0; i < DMR_SMS_MAX_COUNT; i++)
	{
		dmrSmsMessage_t *m = &s_store.msg[i];
		if ((m->flags & DMR_SMS_FLAG_USED) && (m->flags & DMR_SMS_FLAG_UNREAD) &&
				((m->flags & DMR_SMS_FLAG_OUTGOING) == 0)) { n++; }
	}
	return n;
}

void dmrSmsMarkRead(int outgoing, int idx)
{
	store_ensure();
	int slot = slot_for(outgoing, idx);
	if (slot >= 0 && (s_store.msg[slot].flags & DMR_SMS_FLAG_UNREAD))
	{
		s_store.msg[slot].flags &= ~DMR_SMS_FLAG_UNREAD;
		store_save();
	}
}

void dmrSmsMarkAllRead(void)
{
	store_ensure();
	int changed = 0;
	for (int i = 0; i < DMR_SMS_MAX_COUNT; i++)
	{
		if ((s_store.msg[i].flags & DMR_SMS_FLAG_USED) && (s_store.msg[i].flags & DMR_SMS_FLAG_UNREAD))
		{
			s_store.msg[i].flags &= ~DMR_SMS_FLAG_UNREAD; changed = 1;
		}
	}
	if (changed) { store_save(); }
}

void dmrSmsDelete(int outgoing, int idx)
{
	store_ensure();
	int slot = slot_for(outgoing, idx);
	if (slot >= 0)
	{
		memset(&s_store.msg[slot], 0, sizeof s_store.msg[slot]);
		if (s_store.count) { s_store.count--; }
		store_save();
	}
}

void dmrSmsDeleteAll(int outgoing)
{
	store_ensure();
	int changed = 0;
	for (int i = 0; i < DMR_SMS_MAX_COUNT; i++)
	{
		dmrSmsMessage_t *m = &s_store.msg[i];
		if ((m->flags & DMR_SMS_FLAG_USED) &&
				((outgoing < 0) || (((m->flags & DMR_SMS_FLAG_OUTGOING) != 0) == (outgoing != 0))))
		{
			memset(m, 0, sizeof *m);
			if (s_store.count) { s_store.count--; }
			changed = 1;
		}
	}
	if (changed) { store_save(); }
}

/* Insert a new message, evicting the oldest of its folder if the store is full. */
static void store_add(uint8_t flags, uint32_t peerId, const char *text, int textLen)
{
	store_ensure();
	if (textLen > DMR_SMS_TEXT_MAX) { textLen = DMR_SMS_TEXT_MAX; }

	int slot = -1;
	for (int i = 0; i < DMR_SMS_MAX_COUNT; i++)
	{
		if ((s_store.msg[i].flags & DMR_SMS_FLAG_USED) == 0) { slot = i; break; }
	}
	if (slot < 0)
	{
		/* full: evict the globally-oldest message (lowest seq) */
		uint16_t lo = 0xFFFF;
		for (int i = 0; i < DMR_SMS_MAX_COUNT; i++)
		{
			if (s_store.msg[i].seq <= lo) { lo = s_store.msg[i].seq; slot = i; }
		}
	}
	else
	{
		s_store.count++;
	}

	dmrSmsMessage_t *m = &s_store.msg[slot];
	memset(m, 0, sizeof *m);
	m->flags = (uint8_t)(DMR_SMS_FLAG_USED | flags);
	m->textLen = (uint8_t)textLen;
	m->seq = s_store.nextSeq++;
	m->peerId = peerId;
	memcpy(m->text, text, textLen);
	store_save();
}

/* ============================ config (MSGC block) ======================= */
/* Read-only from the firmware's side; written by the CHIRP module. Layout is
 * shared byte-for-byte (opengd77_aes.py MsgConfig). */
#define MSGC_PRESET_LEN  48
typedef struct
{
	char     magic[4];                 /* "MSGC" */
	uint8_t  version;
	uint8_t  numPresets;
	uint8_t  defaultGroup;
	uint8_t  rsvd;
	uint32_t defaultDst;               /* little-endian on the wire == native */
	char     preset[DMR_SMS_NUM_PRESETS][MSGC_PRESET_LEN];
} dmrSmsCfg_t;

static dmrSmsCfg_t s_cfg DMR_AES_CCM;
static uint8_t     s_cfgLoaded DMR_AES_CCM;

static void cfg_load(void)
{
	s_cfgLoaded = 1;
	if (codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_MSG_CONFIG, (uint8_t *)&s_cfg) &&
			(memcmp(s_cfg.magic, "MSGC", 4) == 0))
	{
		return;
	}
	memset(&s_cfg, 0, sizeof s_cfg);
}

static void cfg_ensure(void) { if (!s_cfgLoaded) { cfg_load(); } }

int dmrSmsPresetCount(void)
{
	cfg_ensure();
	int n = 0;
	for (int i = 0; i < DMR_SMS_NUM_PRESETS; i++)
	{
		if (s_cfg.preset[i][0] != 0) { n++; }
	}
	return n;
}

const char *dmrSmsPresetGet(int idx)
{
	cfg_ensure();
	if (idx < 0 || idx >= DMR_SMS_NUM_PRESETS || s_cfg.preset[idx][0] == 0) { return 0; }
	return s_cfg.preset[idx];
}

void dmrSmsDefaultRecipient(uint32_t *dst, int *group)
{
	cfg_ensure();
	if (dst)   { *dst = s_cfg.defaultDst & 0x00FFFFFF; }
	if (group) { *group = s_cfg.defaultGroup ? 1 : 0; }
}

/* ============================ checksums / CRCs =========================== */
static uint16_t ip_cksum(const uint8_t *b, int len)
{
	uint32_t s = 0;
	for (int i = 0; i < len; i += 2)
	{
		s += ((uint32_t)b[i] << 8) | ((i + 1 < len) ? b[i + 1] : 0);
	}
	while (s >> 16) { s = (s & 0xFFFF) + (s >> 16); }
	return (uint16_t)(~s & 0xFFFF);
}

static uint16_t crc16d(const uint8_t *data, int len)   /* CCITT, poly 0x1021, ^0xFFFF */
{
	uint16_t crc = 0;
	for (int i = 0; i < len; i++)
	{
		for (int k = 7; k >= 0; k--)
		{
			int bit = (data[i] >> k) & 1;
			if (((crc >> 15) & 1) ^ bit) { crc = (uint16_t)((crc << 1) ^ 0x1021); }
			else                         { crc = (uint16_t)(crc << 1); }
		}
	}
	return (uint16_t)(crc ^ 0xFFFF);
}

static void hdr_crc(const uint8_t *h, int len, uint16_t mask, uint8_t out2[2])
{
	uint16_t v = (uint16_t)(crc16d(h, len) ^ mask);
	out2[0] = (uint8_t)(v >> 8); out2[1] = (uint8_t)(v & 0xFF);
}

/* DMR data-PDU CRC32: byte-pair swap, poly 0x04C11DB7, over (len*8-32) bits. */
static uint32_t crc32_dmr(const uint8_t *pdu, int len)
{
	uint32_t crc = 0;
	int nbits = len * 8 - 32;
	int bitno = 0;
	for (int i = 0; i + 1 < len; i += 2)
	{
		for (int pass = 0; pass < 2; pass++)
		{
			uint8_t byte = (pass == 0) ? pdu[i + 1] : pdu[i];
			for (int k = 7; k >= 0; k--)
			{
				if (bitno >= nbits) { goto done; }
				int bit = (byte >> k) & 1;
				if (((crc >> 31) & 1) ^ bit) { crc = (crc << 1) ^ 0x04C11DB7; }
				else                         { crc = (crc << 1); }
				bitno++;
			}
		}
	}
done:
	/* byte-reverse to wire order */
	return ((crc & 0xFF) << 24) | ((crc & 0xFF00) << 8) | ((crc >> 8) & 0xFF00) | ((crc >> 24) & 0xFF);
}

/* ============================ plaintext builder ========================== */
/* Build the IPv4/UDP/TMS plaintext into out (>=160 B). Returns length. */
static int build_plaintext(const char *text, int tlen, uint32_t src, uint32_t dst,
                           uint8_t seq, uint16_t ipid, uint8_t *out)
{
	uint8_t tms[16 + 2 * DMR_SMS_TEXT_MAX];
	int L = tlen * 2;            /* UTF-16LE byte count */
	int ti = 0;
	tms[ti++] = 0x00; tms[ti++] = (uint8_t)(8 + L);
	tms[ti++] = 0xA0; tms[ti++] = 0x00; tms[ti++] = seq; tms[ti++] = 0x04;
	tms[ti++] = (uint8_t)(L + 3); tms[ti++] = 0x00;
	tms[ti++] = (uint8_t)L; tms[ti++] = 0x00;
	for (int i = 0; i < tlen; i++) { tms[ti++] = (uint8_t)text[i]; tms[ti++] = 0x00; }

	int udpLen = 8 + ti;
	uint8_t srcIp[4] = { 0x0C, 0x00, (uint8_t)(src >> 8), (uint8_t)src };
	uint8_t dstIp[4] = { 0xE1, 0x00, (uint8_t)(dst >> 8), (uint8_t)dst };

	uint8_t udp[8 + 16 + 2 * DMR_SMS_TEXT_MAX];
	udp[0] = UDP_SMS_PORT >> 8; udp[1] = UDP_SMS_PORT & 0xFF;
	udp[2] = UDP_SMS_PORT >> 8; udp[3] = UDP_SMS_PORT & 0xFF;
	udp[4] = (uint8_t)(udpLen >> 8); udp[5] = (uint8_t)udpLen; udp[6] = 0; udp[7] = 0;
	memcpy(udp + 8, tms, ti);

	/* UDP checksum (pseudo-header) */
	{
		uint8_t pseudo[12 + 8 + 16 + 2 * DMR_SMS_TEXT_MAX];
		int p = 0;
		memcpy(pseudo + p, srcIp, 4); p += 4;
		memcpy(pseudo + p, dstIp, 4); p += 4;
		pseudo[p++] = 0; pseudo[p++] = 0x11;
		pseudo[p++] = (uint8_t)(udpLen >> 8); pseudo[p++] = (uint8_t)udpLen;
		memcpy(pseudo + p, udp, udpLen); p += udpLen;
		if (p & 1) { pseudo[p++] = 0; }
		uint16_t uc = ip_cksum(pseudo, p);
		if (uc == 0) { uc = 0xFFFF; }
		udp[6] = (uint8_t)(uc >> 8); udp[7] = (uint8_t)uc;
	}

	int totLen = 20 + udpLen;
	uint8_t *ip = out;
	ip[0] = 0x45; ip[1] = 0x00; ip[2] = (uint8_t)(totLen >> 8); ip[3] = (uint8_t)totLen;
	ip[4] = (uint8_t)(ipid >> 8); ip[5] = (uint8_t)ipid; ip[6] = 0; ip[7] = 0;
	ip[8] = 0x40; ip[9] = 0x11; ip[10] = 0; ip[11] = 0;
	memcpy(ip + 12, srcIp, 4); memcpy(ip + 16, dstIp, 4);
	uint16_t ic = ip_cksum(ip, 20);
	ip[10] = (uint8_t)(ic >> 8); ip[11] = (uint8_t)ic;
	memcpy(out + 20, udp, udpLen);
	return totLen;
}

/* ============================ TX ======================================== */
/* Burst queue for the data-TX harness: count*(1 type byte + 12 payload). */
static int append_burst(uint8_t *q, int n, uint8_t typeByte, const uint8_t *p12)
{
	q[n * 13 + 0] = typeByte;
	memcpy(q + n * 13 + 1, p12, 12);
	return n + 1;
}

int dmrSmsSend(const char *text, uint32_t dst, int group, uint8_t keyId)
{
	store_ensure();
	if (text == NULL) { return -1; }
	int tlen = (int)strlen(text);
	if (tlen == 0) { return -1; }
	if (tlen > DMR_SMS_TEXT_MAX) { tlen = DMR_SMS_TEXT_MAX; }
	if (dmrDataTxActive()) { return -2; }   /* a data call is already keyed */

	/* Resolve the AES key: caller-supplied id, else the global TX selector, else any
	 * loaded key (so per-channel-encryption radios with no global TX key still send —
	 * e.g. KEY1 in slot 1). The ENC ext header signals this key id to the receiver. */
	if (keyId == 0) { keyId = dmrAesTxKeyId(); }
	if (dmr_aes_key_ptr(keyId) == NULL)
	{
		int fk = dmr_aes_first_keyid();
		if (fk > 0) { keyId = (uint8_t)fk; }
	}
	const uint8_t *key = dmr_aes_key_ptr(keyId);
	if (key == NULL) { return -3; }          /* no usable key loaded */

	uint32_t src = trxDMRID;

	/* 1) plaintext -> pad to AES block -> ECB encrypt */
	uint8_t pt[160];
	int ptLen = build_plaintext(text, tlen, src, dst, 0x00, 0x0001, pt);
	while (ptLen & 15) { pt[ptLen++] = 0x00; }
	for (int i = 0; i + 16 <= ptLen; i += 16) { aes256_ecb_encrypt(key, pt + i); }
	int ctLen = ptLen;                       /* multiple of 16 */

	/* 2) pdu = ct + pad(poc) + crc32, padded so (ct+4) fills whole 12-byte blocks */
	int totalData = (((ctLen + 4) + 11) / 12) * 12;
	int poc = totalData - ctLen - 4;
	uint8_t pdu[384];
	if (totalData > (int)sizeof pdu) { return -4; }
	memcpy(pdu, pt, ctLen);
	memset(pdu + ctLen, 0, poc);
	uint32_t crc = crc32_dmr(pdu, totalData) ;  /* placeholder bytes already zero */
	pdu[totalData - 4] = (uint8_t)(crc >> 24); pdu[totalData - 3] = (uint8_t)(crc >> 16);
	pdu[totalData - 2] = (uint8_t)(crc >> 8);  pdu[totalData - 1] = (uint8_t)crc;
	int nDataBlocks = totalData / 12;
	int nblocks = 1 + nDataBlocks;           /* +1 ENC ext header block */

	/* 3) build the burst queue: CSBK preamble + 2 headers + rate-1/2 blocks */
	static uint8_t q[DMR_DATA_MAX_BURSTS * 13];
	int n = 0;
	int preamble = 6;
	int tail = 2 + nDataBlocks;              /* headers + data blocks after the CSBKs */
	uint8_t g = group ? 0x80 : 0x00;
	uint8_t gc = group ? 0xC0 : 0x80;
	for (int i = 0; i < preamble; i++)
	{
		uint8_t body[10];
		body[0] = 0xBD; body[1] = 0x00; body[2] = gc; body[3] = (uint8_t)((preamble - 1 - i) + tail);
		body[4] = (uint8_t)(dst >> 16); body[5] = (uint8_t)(dst >> 8); body[6] = (uint8_t)dst;
		body[7] = (uint8_t)(src >> 16); body[8] = (uint8_t)(src >> 8); body[9] = (uint8_t)src;
		uint8_t p12[12]; memcpy(p12, body, 10); hdr_crc(body, 10, 0xA5A5, p12 + 10);
		if (n >= DMR_DATA_MAX_BURSTS) { return -5; }
		n = append_burst(q, n, DTB_CSBK, p12);
	}
	/* Unconfirmed data header (SAP09 EXTD) */
	{
		uint8_t h[10];
		h[0] = (uint8_t)(g | 0x02); h[1] = (uint8_t)((9 << 4) | (poc & 0x0F));
		h[2] = (uint8_t)(dst >> 16); h[3] = (uint8_t)(dst >> 8); h[4] = (uint8_t)dst;
		h[5] = (uint8_t)(src >> 16); h[6] = (uint8_t)(src >> 8); h[7] = (uint8_t)src;
		h[8] = (uint8_t)(0x80 | (nblocks & 0x7F)); h[9] = 0x00;
		uint8_t p12[12]; memcpy(p12, h, 10); hdr_crc(h, 10, 0xCCCC, p12 + 10);
		if (n >= DMR_DATA_MAX_BURSTS) { return -5; }
		n = append_burst(q, n, DTB_DATA_HEADER, p12);
	}
	/* ENC extended header (SAP04 IP, MFID Moto, ALG05 AES256, key id, MI=0) */
	{
		uint8_t e[10] = { 0x4F, 0x10, 0x51, keyId, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		uint8_t p12[12]; memcpy(p12, e, 10); hdr_crc(e, 10, 0xCCCC, p12 + 10);
		if (n >= DMR_DATA_MAX_BURSTS) { return -5; }
		n = append_burst(q, n, DTB_DATA_HEADER, p12);
	}
	/* rate-1/2 data blocks */
	for (int b = 0; b < nDataBlocks; b++)
	{
		if (n >= DMR_DATA_MAX_BURSTS) { return -5; }
		n = append_burst(q, n, DTB_RATE12_DATA, pdu + b * 12);
	}

	dmrDataTxLoad(q, (uint8_t)n);
	store_add(DMR_SMS_FLAG_OUTGOING | (group ? DMR_SMS_FLAG_GROUP : 0), dst, text, tlen);
	return 0;
}

/* ============================ RX ======================================== */
/* ISR-side reassembly state (CCM, not zeroed at boot -> reset in dmrSmsRxReset). */
static volatile uint8_t  s_rxHaveHeader DMR_AES_CCM;
static volatile uint8_t  s_rxHaveEnc DMR_AES_CCM;
static volatile uint8_t  s_rxExpBlocks DMR_AES_CCM;   /* rate-1/2 blocks expected (nblocks-1) */
static volatile uint8_t  s_rxCount DMR_AES_CCM;
static volatile uint8_t  s_rxGroup DMR_AES_CCM;
static volatile uint8_t  s_rxKeyId DMR_AES_CCM;
static volatile uint32_t s_rxSrc DMR_AES_CCM;
static volatile uint32_t s_rxDst DMR_AES_CCM;
static uint8_t  s_rxBlocks[32][12] DMR_AES_CCM;
/* hand-off to main loop */
static volatile uint8_t  s_rxReady DMR_AES_CCM;       /* a complete PDU is waiting */
static uint8_t  s_rxPdu[384] DMR_AES_CCM;
static volatile uint16_t s_rxPduLen DMR_AES_CCM;
static volatile uint32_t s_rxPeer DMR_AES_CCM;
static volatile uint8_t  s_rxPeerGroup DMR_AES_CCM;
static volatile uint8_t  s_rxPeerKeyId DMR_AES_CCM;
/* diagnostic counters (visible on the Messages home screen) to localise RX failures */
static volatile uint32_t s_diagData   DMR_AES_CCM; /* ALL data-sync-class bursts the chip delivered */
static volatile uint32_t s_diagHdrOk  DMR_AES_CCM; /* type-6 data-header, CRC OK   */
static volatile uint32_t s_diagHdrBad DMR_AES_CCM; /* type-6 data-header, CRC bad  */
static volatile uint32_t s_diagBlkOk  DMR_AES_CCM; /* type-7 rate-1/2,   CRC OK    */
static volatile uint32_t s_diagBlkBad DMR_AES_CCM; /* type-7 rate-1/2,   CRC bad   */
static volatile uint32_t s_diagPdu    DMR_AES_CCM; /* completed PDUs handed to main loop */
static volatile uint32_t s_diagMsg    DMR_AES_CCM; /* successfully decrypted + stored    */
/* snapshot of the last reassembled (encrypted) PDU, for offline inspection over USB */
static uint8_t  s_diagLastPdu[120] DMR_AES_CCM;
static volatile uint16_t s_diagLastPduLen DMR_AES_CCM;
static volatile uint8_t  s_diagLastKeyId DMR_AES_CCM;
static volatile uint8_t  s_diagLastExp DMR_AES_CCM;
static volatile uint32_t s_diagLastPeer DMR_AES_CCM;

/* ---- chip "Received Information" RX-RAM capture (de-permute experiment) ---- *
 * When the HR-C6000 reports a fully reassembled+verified data PDU, HR-C6000.c reads a
 * chunk of the chip's RX data RAM and hands it here. We dump it over USB to see whether
 * the chip's own reassembly is in the correct (de-interleaved) order. */
static volatile uint32_t s_riCount DMR_AES_CCM;   /* Received-Information ints seen */
static volatile uint8_t  s_riReg90 DMR_AES_CCM;   /* last reg 0x90 sub-status       */
static uint8_t  s_riBuf[160] DMR_AES_CCM;         /* last RX-RAM dump               */
static volatile uint16_t s_riLen DMR_AES_CCM;

void dmrSmsRiCapture(uint8_t reg90, const uint8_t *ram, int len)
{
	s_riCount++;
	s_riReg90 = reg90;
	if (len > (int)sizeof s_riBuf) { len = (int)sizeof s_riBuf; }
	for (int i = 0; i < len; i++) { s_riBuf[i] = ram[i]; }
	s_riLen = (uint16_t)len;
}

/* Fill out with [riCount(4 LE), reg90, len_hi, len_lo, ramBytes...]. Returns bytes. */
int dmrSmsRiDump(uint8_t *out, int maxlen)
{
	int n = s_riLen;
	if (n > (int)sizeof s_riBuf) { n = (int)sizeof s_riBuf; }
	if (maxlen < 7 + n) { n = maxlen - 7; if (n < 0) n = 0; }
	out[0] = (uint8_t)(s_riCount);
	out[1] = (uint8_t)(s_riCount >> 8);
	out[2] = (uint8_t)(s_riCount >> 16);
	out[3] = (uint8_t)(s_riCount >> 24);
	out[4] = s_riReg90;
	out[5] = (uint8_t)(s_riLen >> 8);
	out[6] = (uint8_t)(s_riLen);
	for (int i = 0; i < n; i++) { out[7 + i] = s_riBuf[i]; }
	return 7 + n;
}

/* Fill out with [pduLen_hi,pduLen_lo, keyId, expBlocks, peer(4 LE), rawPdu...]. Returns bytes. */
int dmrSmsRxLastPdu(uint8_t *out, int maxlen)
{
	int n = s_diagLastPduLen;
	if (n > 384) { n = 384; }
	if (maxlen < 8 + n) { n = maxlen - 8; if (n < 0) n = 0; }
	out[0] = (uint8_t)(s_diagLastPduLen >> 8);
	out[1] = (uint8_t)(s_diagLastPduLen);
	out[2] = s_diagLastKeyId;
	out[3] = s_diagLastExp;
	out[4] = (uint8_t)(s_diagLastPeer);
	out[5] = (uint8_t)(s_diagLastPeer >> 8);
	out[6] = (uint8_t)(s_diagLastPeer >> 16);
	out[7] = (uint8_t)(s_diagLastPeer >> 24);
	for (int i = 0; i < n; i++) { out[8 + i] = s_diagLastPdu[i]; }
	return 8 + n;
}

/* Called for EVERY data-sync-class burst (any type, any CRC) so we can see exactly what
 * the HR-C6000 delivers during a stock SMS transmission. ISR context: counters only. */
void dmrSmsRxDiagBurst(int rxDataType, int crcOk)
{
	s_diagData++;
	if (rxDataType == DT_DATA_HEADER) { if (crcOk) s_diagHdrOk++; else s_diagHdrBad++; }
	else if (rxDataType == DT_RATE12_DATA) { if (crcOk) s_diagBlkOk++; else s_diagBlkBad++; }
}

void dmrSmsRxDiagReset(void)
{
	s_diagData = s_diagHdrOk = s_diagHdrBad = s_diagBlkOk = s_diagBlkBad = 0;
	s_diagPdu = s_diagMsg = 0;
	s_diagLastPduLen = 0;
}

void dmrSmsRxDiag(uint32_t out[7])
{
	out[0] = s_diagData;
	out[1] = s_diagHdrOk; out[2] = s_diagHdrBad;
	out[3] = s_diagBlkOk; out[4] = s_diagBlkBad;
	out[5] = s_diagPdu;   out[6] = s_diagMsg;
}

void dmrSmsRxReset(void)
{
	s_rxHaveHeader = 0; s_rxHaveEnc = 0; s_rxExpBlocks = 0; s_rxCount = 0;
}

void dmrSmsRxBurst(int rxDataType, const uint8_t *p)
{
	if (rxDataType == DT_DATA_HEADER)
	{
		if (p[0] == 0x4F && p[1] == 0x10 && (p[2] & 0x3F) == (0x51 & 0x3F))
		{
			/* Motorola ENC extended header: ALG/key/MI. It immediately precedes this
			 * message's data blocks, so restart block accumulation here — this prevents
			 * mixing leftover blocks from a previous (missed-terminator) retransmit. */
			s_rxKeyId = p[3];
			s_rxHaveEnc = 1;
			s_rxCount = 0;
		}
		else
		{
			/* Unconfirmed/UDT data header: start a fresh PDU */
			s_rxGroup = (p[0] & 0x80) ? 1 : 0;
			s_rxDst = ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
			s_rxSrc = ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
			uint8_t nblocks = (uint8_t)(p[8] & 0x7F);
			s_rxExpBlocks = (uint8_t)((nblocks > 0) ? (nblocks - 1) : 0);  /* minus ENC header */
			s_rxCount = 0;
			s_rxHaveHeader = 1;
			s_rxHaveEnc = 0;
		}
		return;
	}

	if (rxDataType == DT_RATE12_DATA)
	{
		if (!s_rxHaveHeader || s_rxCount >= 32) { return; }
		memcpy(s_rxBlocks[s_rxCount], p, 12);
		s_rxCount++;

		/* Complete when the accumulated blocks form a CRC32-valid data PDU. This is
		 * self-terminating and does NOT trust the header's block count (which can be
		 * stale when a header is missed) — block mixing or truncation simply won't
		 * produce a valid CRC32, so only a correct, complete PDU is accepted. The
		 * smallest SMS is 5 rate-1/2 blocks (60 B), so don't bother checking below that. */
		if (s_rxHaveEnc && (s_rxCount >= 5) && !s_rxReady)
		{
			int total = s_rxCount * 12;
			if (total > (int)sizeof s_rxPdu) { return; }
			for (int i = 0; i < s_rxCount; i++)
			{
				memcpy(s_rxPdu + i * 12, s_rxBlocks[i], 12);
			}
			uint32_t want = ((uint32_t)s_rxPdu[total - 4] << 24) | ((uint32_t)s_rxPdu[total - 3] << 16) |
					((uint32_t)s_rxPdu[total - 2] << 8) | (uint32_t)s_rxPdu[total - 1];
			if (crc32_dmr(s_rxPdu, total) != want)
			{
				return;   /* not a complete/clean PDU yet — keep accumulating */
			}
			s_rxPduLen = (uint16_t)total;
			s_rxPeer = s_rxSrc;
			s_rxPeerGroup = s_rxGroup;
			s_rxPeerKeyId = s_rxKeyId;
			s_rxReady = 1;          /* main loop will decrypt + store */
			s_diagPdu++;
			/* snapshot raw (still-encrypted) PDU for USB inspection */
			s_diagLastPduLen = (uint16_t)total;
			s_diagLastKeyId = s_rxKeyId;
			s_diagLastExp = s_rxExpBlocks;
			s_diagLastPeer = s_rxSrc;
			for (int i = 0; i < total && i < (int)sizeof s_diagLastPdu; i++) { s_diagLastPdu[i] = s_rxPdu[i]; }
			dmrSmsRxReset();
		}
	}
}

void dmrSmsRxTick(void)
{
	if (!s_rxReady) { return; }

	/* snapshot the FULL pdu (incl. pad+crc32), then release the ISR buffer */
	uint8_t pdu[384];
	int pduLen = s_rxPduLen;
	uint32_t peer = s_rxPeer;
	uint8_t  group = s_rxPeerGroup;
	uint8_t  keyId = s_rxPeerKeyId;
	if (pduLen > (int)sizeof pdu) { pduLen = (int)sizeof pdu; }
	memcpy(pdu, s_rxPdu, pduLen);
	s_rxReady = 0;

	int ctLen = (pduLen / 16) * 16;         /* ciphertext = whole AES blocks (drops pad+crc32) */
	if (ctLen < 16 || pduLen < 8) { return; }

	/* Validate the data-PDU CRC32 over [ct+pad] before trusting the bytes. This rejects
	 * reassemblies that mixed blocks across retransmits (a missed burst/header) — without it
	 * a corrupted PDU decrypts to garbage and, worse, can store a junk message. */
	{
		uint32_t want = ((uint32_t)pdu[pduLen - 4] << 24) | ((uint32_t)pdu[pduLen - 3] << 16) |
				((uint32_t)pdu[pduLen - 2] << 8) | (uint32_t)pdu[pduLen - 1];
		if (crc32_dmr(pdu, pduLen) != want) { return; }   /* corrupted reassembly -> drop */
	}

	/* Try the signalled key id first, then every loaded key (decrypt is destructive,
	 * so each attempt works on a fresh copy of the ciphertext). */
	char text[DMR_SMS_TEXT_MAX + 1];
	uint8_t tmp[384];
	int got = -1;

	for (int attempt = 0; attempt <= DMR_AES_MAX_KEYS && got < 0; attempt++)
	{
		uint8_t k = (attempt == 0) ? keyId : (uint8_t)attempt;
		if (k == 0 || k >= DMR_AES_MAX_KEYS) { continue; }
		memcpy(tmp, pdu, ctLen);
		int r = dmr_aes_sms_decrypt(k, tmp, ctLen, text, sizeof text);
		if (r > 0) { got = r; }
	}
	if (got <= 0) { return; }   /* wrong/no key, or not an SMS */

	store_add(DMR_SMS_FLAG_UNREAD | (group ? DMR_SMS_FLAG_GROUP : 0), peer, text, got);
	s_diagMsg++;

	/* notify the user */
	char note[DMR_SMS_TEXT_MAX + 12];
	snprintf(note, sizeof note, "SMS: %s", text);
	uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 4000, note, true);
}

/* Clear all CCM runtime state to known values (see the forward decl up top). */
static void runtimeReset(void)
{
	s_cfgLoaded = 0;                 /* force the MSGC config to (re)load */
	s_diagData = s_diagHdrOk = s_diagHdrBad = s_diagBlkOk = s_diagBlkBad = 0;
	s_diagPdu = s_diagMsg = 0;
	s_rxReady = 0;                   /* don't process stray garbage as a PDU */
	dmrSmsRxReset();                 /* clear the burst accumulator */
}

#endif /* ENABLE_DMR_DATA && ENABLE_AES */

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
#include "functions/settings.h"   /* currentChannelData (per-channel encrypt byte, voice logic) */
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
/* Variable-length packed store: the MSGS custom-data block holds a small header plus a
 * byte area of back-to-back entries, so a message uses only the space its text needs
 * (stock-like: many short messages OR a few long ones share the same fixed block). A CCM
 * working copy (DMR_AES_CCM -> no net-new main RAM) is the live store. Block stays 1352 B
 * (== the old fixed 24x56 array) so it drops into an existing codeplug slot.
 * Entry layout in data[]:  [0]=flags [1]=textLen [2..3]=seq(LE) [4..7]=peerId(LE) [8..]=text */
#define SMS_ENTRY_HDR  8
typedef struct
{
	char     magic[4];                 /* "MSGV" (v2 variable-length; old "MSGS" is ignored) */
	uint8_t  version;                  /* 2 */
	uint8_t  rsvd;
	uint16_t used;                     /* bytes in use in data[] */
	uint16_t nextSeq;                  /* next message ordering id */
	uint8_t  data[DMR_SMS_STORE_DATA];
} dmrSmsStore_t;

static dmrSmsStore_t   s_store   DMR_AES_CCM;
static uint8_t         s_loaded  DMR_AES_CCM;  /* CCM is not zeroed at boot -> guard init */
static dmrSmsMessage_t s_scratch;                /* copy-out buffer for dmrSmsGet (main RAM, not CCM) */

static void runtimeReset(void);

static int entry_size(int off) { return SMS_ENTRY_HDR + s_store.data[off + 1]; }
static uint16_t entry_seq(int off) { return (uint16_t)(s_store.data[off + 2] | (s_store.data[off + 3] << 8)); }
static int entry_matches(int off, int outgoing)
{
	int want = outgoing ? DMR_SMS_FLAG_OUTGOING : 0;
	return (s_store.data[off] & DMR_SMS_FLAG_OUTGOING) == want;
}

static void store_blank(void)
{
	memset(&s_store, 0, sizeof s_store);
	memcpy(s_store.magic, "MSGV", 4);
	s_store.version = 2;
	s_store.used = 0;
	s_store.nextSeq = 1;
}

/* Walk the packed entry chain: every entry must lie fully inside used[] with a sane
 * textLen, and the chain must tile data[] exactly up to used. Rejects a corrupted
 * flash block whose lengths would otherwise index garbage entry boundaries. */
static int store_chain_valid(void)
{
	int o = 0;
	while (o < (int)s_store.used)
	{
		if ((o + SMS_ENTRY_HDR > (int)s_store.used) ||
				(s_store.data[o + 1] > DMR_SMS_TEXT_MAX) ||
				(o + entry_size(o) > (int)s_store.used))
		{
			return 0;
		}
		o += entry_size(o);
	}
	return 1;
}

void dmrSmsInit(void)
{
	uint8_t *blk = (uint8_t *)&s_store;
	runtimeReset();     /* clear CCM runtime state (counters, flags) before first use */
	s_loaded = 1;
	dmrAesLoadKeys();   /* ensure the key store is populated for RX decrypt */
	if (codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_MESSAGES, blk) &&
			(memcmp(s_store.magic, "MSGV", 4) == 0) && (s_store.version == 2) &&
			(s_store.used <= DMR_SMS_STORE_DATA) && store_chain_valid())
	{
		return;   /* valid variable-length store loaded from flash */
	}
	store_blank();      /* fresh, corrupted, or an old "MSGS" v1 block -> start empty */
}

static void store_ensure(void) { if (!s_loaded) { dmrSmsInit(); } }

static void store_save(void)
{
	dmrAesEnsureCustomDataRegion();   /* OpenGD77 magic must exist for the block chain */
	codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_MESSAGES,
			(uint8_t *)&s_store, (int)sizeof s_store);
}

/* Byte offset of the idx-th newest entry of a folder (newest = idx 0), or -1. Selection
 * scan (no large stack array): rank 0 = highest seq, each next rank = highest seq below it. */
static int off_for(int outgoing, int idx)
{
	uint32_t prevSeq = 0x10000;   /* above any 16-bit seq */
	int chosen = -1;
	for (int rank = 0; rank <= idx; rank++)
	{
		int bestOff = -1; uint32_t bestSeq = 0;
		for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
		{
			if (!entry_matches(o, outgoing)) { continue; }
			uint32_t sq = entry_seq(o);
			if (sq < prevSeq && (bestOff < 0 || sq > bestSeq)) { bestSeq = sq; bestOff = o; }
		}
		if (bestOff < 0) { return -1; }
		chosen = bestOff; prevSeq = bestSeq;
	}
	return chosen;
}

int dmrSmsCount(int outgoing)
{
	store_ensure();
	int n = 0;
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		if (entry_matches(o, outgoing)) { n++; }
	}
	return n;
}

const dmrSmsMessage_t *dmrSmsGet(int outgoing, int idx)
{
	store_ensure();
	int o = off_for(outgoing, idx);
	if (o < 0) { return NULL; }
	memset(&s_scratch, 0, sizeof s_scratch);
	s_scratch.flags   = (uint8_t)(s_store.data[o] | DMR_SMS_FLAG_USED);
	s_scratch.textLen = s_store.data[o + 1];
	s_scratch.seq     = entry_seq(o);
	s_scratch.peerId  = (uint32_t)s_store.data[o + 4] | ((uint32_t)s_store.data[o + 5] << 8) |
	                    ((uint32_t)s_store.data[o + 6] << 16) | ((uint32_t)s_store.data[o + 7] << 24);
	int tl = s_scratch.textLen; if (tl > DMR_SMS_TEXT_MAX) { tl = DMR_SMS_TEXT_MAX; }
	memcpy(s_scratch.text, &s_store.data[o + SMS_ENTRY_HDR], tl);
	return &s_scratch;
}

int dmrSmsUnreadCount(void)
{
	store_ensure();
	int n = 0;
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		if (((s_store.data[o] & DMR_SMS_FLAG_OUTGOING) == 0) && (s_store.data[o] & DMR_SMS_FLAG_UNREAD)) { n++; }
	}
	return n;
}

void dmrSmsMarkRead(int outgoing, int idx)
{
	store_ensure();
	int o = off_for(outgoing, idx);
	if (o >= 0 && (s_store.data[o] & DMR_SMS_FLAG_UNREAD))
	{
		s_store.data[o] &= (uint8_t)~DMR_SMS_FLAG_UNREAD;
		store_save();
	}
}

void dmrSmsMarkAllRead(void)
{
	store_ensure();
	int changed = 0;
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		if (((s_store.data[o] & DMR_SMS_FLAG_OUTGOING) == 0) && (s_store.data[o] & DMR_SMS_FLAG_UNREAD))
		{
			s_store.data[o] &= (uint8_t)~DMR_SMS_FLAG_UNREAD; changed = 1;
		}
	}
	if (changed) { store_save(); }
}

/* Remove the entry at byte offset o, compacting the block down. */
static void entry_remove(int o)
{
	int sz = entry_size(o);
	int tail = (int)s_store.used - (o + sz);
	if (tail > 0) { memmove(&s_store.data[o], &s_store.data[o + sz], (size_t)tail); }
	s_store.used = (uint16_t)(s_store.used - sz);
}

void dmrSmsDelete(int outgoing, int idx)
{
	store_ensure();
	int o = off_for(outgoing, idx);
	if (o >= 0) { entry_remove(o); store_save(); }
}

void dmrSmsDeleteAll(int outgoing)
{
	store_ensure();
	int changed = 0;
	int o = 0;
	while (o + SMS_ENTRY_HDR <= (int)s_store.used)
	{
		int match = (outgoing < 0) ||
				(((s_store.data[o] & DMR_SMS_FLAG_OUTGOING) != 0) == (outgoing != 0));
		if (match) { entry_remove(o); changed = 1; }   /* next entry shifted into o; don't advance */
		else { o += entry_size(o); }
	}
	if (changed) { store_save(); }
}

/* Renumber every entry's seq to 1..N preserving age order (oldest = 1). Called when the
 * 16-bit nextSeq wraps, so "newest = highest seq" ordering and lowest-seq eviction stay
 * correct across the wrap. Flag bit 0x80 is a transient "renumbered" marker (cleared
 * before returning, never persisted set). */
#define SMS_FLAG_TMP_MARK  0x80
static void seq_renumber(void)
{
	int count = 0;
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		s_store.data[o] &= (uint8_t)~SMS_FLAG_TMP_MARK;
		count++;
	}
	for (int newSeq = count; newSeq >= 1; newSeq--)
	{
		int bestOff = -1; uint16_t bestSq = 0;
		for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
		{
			if (s_store.data[o] & SMS_FLAG_TMP_MARK) { continue; }
			uint16_t sq = entry_seq(o);
			if (bestOff < 0 || sq >= bestSq) { bestSq = sq; bestOff = o; }
		}
		if (bestOff < 0) { break; }
		s_store.data[bestOff + 2] = (uint8_t)(newSeq & 0xFF);
		s_store.data[bestOff + 3] = (uint8_t)(newSeq >> 8);
		s_store.data[bestOff] |= SMS_FLAG_TMP_MARK;
	}
	for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
	{
		s_store.data[o] &= (uint8_t)~SMS_FLAG_TMP_MARK;
	}
	s_store.nextSeq = (uint16_t)(count + 1);
}

/* Insert a new message, evicting the globally-oldest (lowest seq) until it fits. */
static void store_add(uint8_t flags, uint32_t peerId, const char *text, int textLen)
{
	store_ensure();
	if (textLen > DMR_SMS_TEXT_MAX) { textLen = DMR_SMS_TEXT_MAX; }
	if (textLen < 0) { textLen = 0; }
	int need = SMS_ENTRY_HDR + textLen;
	if (need > DMR_SMS_STORE_DATA) { return; }   /* can't ever fit */

	while ((int)s_store.used + need > DMR_SMS_STORE_DATA)
	{
		int oldest = -1; uint16_t lo = 0xFFFF;
		for (int o = 0; o + SMS_ENTRY_HDR <= (int)s_store.used; o += entry_size(o))
		{
			uint16_t sq = entry_seq(o);
			if (sq <= lo) { lo = sq; oldest = o; }
		}
		if (oldest < 0) { break; }
		entry_remove(oldest);
	}

	int o = s_store.used;
	if (s_store.nextSeq == 0) { seq_renumber(); }   /* 16-bit seq wrapped -> renumber by age */
	uint16_t seq = s_store.nextSeq++;
	s_store.data[o + 0] = (uint8_t)(DMR_SMS_FLAG_USED | flags);
	s_store.data[o + 1] = (uint8_t)textLen;
	s_store.data[o + 2] = (uint8_t)(seq & 0xFF);
	s_store.data[o + 3] = (uint8_t)(seq >> 8);
	s_store.data[o + 4] = (uint8_t)(peerId & 0xFF);
	s_store.data[o + 5] = (uint8_t)((peerId >> 8) & 0xFF);
	s_store.data[o + 6] = (uint8_t)((peerId >> 16) & 0xFF);
	s_store.data[o + 7] = (uint8_t)((peerId >> 24) & 0xFF);
	memcpy(&s_store.data[o + SMS_ENTRY_HDR], text, (size_t)textLen);
	s_store.used = (uint16_t)(o + need);
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
	uint8_t  maxLen;                 /* CHIRP-set max compose length (0 = default 144) */
	uint32_t defaultDst;               /* little-endian on the wire == native */
	char     preset[DMR_SMS_NUM_PRESETS][MSGC_PRESET_LEN];
	uint8_t  smsEncrypt;             /* SMS-encrypt master gate: 0 = default (on), 1 = off/clear, 2 = on.
	                                  * Appended last so an old (shorter) MSGC block reads as 0 = default. */
} dmrSmsCfg_t;

static dmrSmsCfg_t s_cfg DMR_AES_CCM;
static uint8_t     s_cfgLoaded DMR_AES_CCM;

static void cfg_load(void)
{
	s_cfgLoaded = 1;
	memset(&s_cfg, 0, sizeof s_cfg);   /* default any field a shorter/absent block omits (smsEncrypt=0) */
	/* Bounded read: dataLength comes from flash — a corrupt/oversized block must not overrun s_cfg. */
	if (codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_MSG_CONFIG, (uint8_t *)&s_cfg, (int)sizeof s_cfg) &&
			(memcmp(s_cfg.magic, "MSGC", 4) == 0))
	{
		/* Force-terminate every preset row: dmrSmsPresetGet() returns these as C strings
		 * and the compose copy reads up to dmrSmsMaxLen() (144) chars, so an unterminated
		 * 48-byte row written by CHIRP must not run into the next row / off the struct. */
		for (int i = 0; i < DMR_SMS_NUM_PRESETS; i++) { s_cfg.preset[i][MSGC_PRESET_LEN - 1] = 0; }
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

int dmrSmsMaxLen(void)
{
	cfg_ensure();
	int m = s_cfg.maxLen;
	if (m <= 0 || m > DMR_SMS_TEXT_MAX) { m = DMR_SMS_TEXT_MAX; }
	return m;
}

/* SMS-encrypt master gate (CHIRP "Encrypt SMS"): 1 = encrypt-per-channel-like-voice,
 * 0 = always cleartext. Default (unset MSGC byte) = 1 to preserve the encrypted behaviour. */
int dmrSmsEncryptEnabled(void)
{
	cfg_ensure();
	return (s_cfg.smsEncrypt == 1) ? 0 : 1;   /* 1 = force clear; 0(default)/2 = encrypt */
}

/* AES TX key for the current channel — a mirror of hrc6000ResolveAesTxKeyId (HR-C6000.c) so
 * SMS encryption follows the exact same per-channel logic as voice: the global TX selector,
 * overridden by the channel encrypt byte (0xFF -> clear, 1..15 -> key slot, 0 -> inherit).
 * Byte 41 is shared with optional-DMR-ID, which wins (then the channel isn't an encrypt slot). */
static uint8_t smsResolveTxKeyId(void)
{
	uint8_t keyId = dmrAesTxKeyId();
	if ((currentChannelData != NULL) &&
			(codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_OPTIONAL_DMRID) == 0))
	{
		uint8_t chEnc = currentChannelData->encrypt;
		if (chEnc == 0xFF) { keyId = 0; }
		else if ((chEnc >= 1) && (chEnc < DMR_AES_MAX_KEYS)) { keyId = chEnc; }
	}
	return keyId;
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
	tms[ti++] = (uint8_t)((8 + L) >> 8); tms[ti++] = (uint8_t)(8 + L);   /* 2-byte TMS length */
	tms[ti++] = 0xA0; tms[ti++] = 0x00; tms[ti++] = seq; tms[ti++] = 0x04;
	tms[ti++] = 0x0D; tms[ti++] = 0x00;   /* fixed CRLF header (stock uses 0d/0a here, NOT L+3/L) */
	tms[ti++] = 0x0A; tms[ti++] = 0x00;
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

	/* Decide encrypt-or-clear, following the VOICE key-selection logic gated by the CHIRP
	 * "Encrypt SMS" master switch: gate OFF -> always cleartext (even on an encrypted channel);
	 * ON -> the per-channel encrypt byte exactly like voice (0xFF or no loaded key -> clear,
	 * 1..15 -> that key slot, 0 -> the global TX selector). An explicit caller keyId (CPS/bench)
	 * overrides the gate. When encrypting, the ENC ext header signals the key id to the receiver. */
	if (keyId == 0 && dmrSmsEncryptEnabled()) { keyId = smsResolveTxKeyId(); }
	const uint8_t *key = (keyId != 0) ? dmr_aes_key_ptr(keyId) : NULL;
	int encrypt = (key != NULL);             /* resolved to a loaded key -> encrypt; else cleartext */

	uint32_t src = trxDMRID;

	/* 1) plaintext. When encrypting, ECB-encrypt the WHOLE 16-byte blocks only; the trailing
	 *    partial block (< 16 B) stays CLEAR, exactly like a stock TYT. Cleartext SMS skips the
	 *    encryption (same IPv4/UDP/TMS structure, and no ENC header emitted below). Do NOT pad. */
	uint8_t pt[360];   /* IPv4(20)+UDP(8)+TMS(10)+2*144 = 326 B max */
	/* TMS type/seq byte = 0x90 (stock "text message" flag). With 0x00 a stock TYT starts
	 * reading the text 4 bytes early and prepends the L+3 length field as a stray char. */
	int ptLen = build_plaintext(text, tlen, src, dst, 0x90, 0x0001, pt);
	if (encrypt) { for (int i = 0; i + 16 <= ptLen; i += 16) { aes256_ecb_encrypt(key, pt + i); } }
	int ctLen = ptLen;                       /* ct = whole enc blocks + clear tail (or all clear) */

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
	int nblocks = (encrypt ? 1 : 0) + nDataBlocks;  /* +1 ENC ext header block only when encrypting */

	/* 3) build the burst queue: CSBK preamble + 2 headers + rate-1/2 blocks */
	static uint8_t q[DMR_DATA_MAX_BURSTS * 13];
	int n = 0;
	int preamble = 6;
	int tail = (encrypt ? 2 : 1) + nDataBlocks;  /* headers (Unconfirmed [+ ENC]) + data blocks after CSBKs */
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
	/* Unconfirmed data header. SAP 09 [EXTD HDR] when encrypting (the ENC ext header is
	 * the extended header that follows); SAP 04 [IP Based] for cleartext (no extended
	 * header) — a stock TYT rejects SAP09-with-no-extended-header, decoded from a real
	 * stock cleartext SMS capture (2026-07-04). */
	{
		uint8_t h[10];
		h[0] = (uint8_t)(g | 0x02); h[1] = (uint8_t)(((encrypt ? 9 : 4) << 4) | (poc & 0x0F));
		h[2] = (uint8_t)(dst >> 16); h[3] = (uint8_t)(dst >> 8); h[4] = (uint8_t)dst;
		h[5] = (uint8_t)(src >> 16); h[6] = (uint8_t)(src >> 8); h[7] = (uint8_t)src;
		h[8] = (uint8_t)(0x80 | (nblocks & 0x7F)); h[9] = 0x00;
		uint8_t p12[12]; memcpy(p12, h, 10); hdr_crc(h, 10, 0xCCCC, p12 + 10);
		if (n >= DMR_DATA_MAX_BURSTS) { return -5; }
		n = append_burst(q, n, DTB_DATA_HEADER, p12);
	}
	/* ENC extended header (SAP04 IP, MFID Moto, ALG05 AES256, key id, MI=0) — encrypted SMS only */
	if (encrypt)
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

	/* File the message to the Sent folder BEFORE keying the data call. store_add() does a
	 * blocking flash sector erase+write (~100s of ms); dmrDataTxLoad() defers the keyup by
	 * ~100 ms and then feeds rate-1/2 bursts to the HR-C6000 over SPI in real time. If the
	 * flash write ran after (concurrently with) the keyup, it starved/disrupted the burst
	 * emission and corrupted the on-air data — a stock radio synced the carrier (green LED)
	 * but the rate-1/2 FEC/CRC failed, so nothing landed in its inbox (HW-diagnosed: host TX
	 * with no flash write always decoded; menu TX with this write did not). Doing the flash
	 * write first keeps it entirely off the TX-keying window. */
	store_add(DMR_SMS_FLAG_OUTGOING | (group ? DMR_SMS_FLAG_GROUP : 0), dst, text, tlen);
	dmrDataTxLoad(q, (uint8_t)n);
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
static volatile uint8_t  s_rxPeerEnc DMR_AES_CCM;   /* 1 = PDU carried the ENC header (decrypt); 0 = cleartext */
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

/* Fill out with [pduLen_hi,pduLen_lo, keyId, expBlocks, peer(4 LE), rawPdu...]. Returns bytes.
 * pduLen (and the raw bytes) are clamped to the snapshot buffer size: a PDU longer than
 * sizeof s_diagLastPdu is stored truncated, so only that many bytes exist to dump. */
int dmrSmsRxLastPdu(uint8_t *out, int maxlen)
{
	int n = s_diagLastPduLen;
	if (n > (int)sizeof s_diagLastPdu) { n = (int)sizeof s_diagLastPdu; }
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
		 * produce a valid CRC32, so only a correct, complete PDU is accepted.
		 * Minimum is 2 blocks: a stock 1-char SMS is BLOCKS 05 = 4 rate-1/2 data blocks
		 * (a hardcoded >=5 gate here silently dropped every short message — 5+ char msgs
		 * have 5+ data blocks and worked, 1-char have 4 and never completed). CRC32 gates
		 * correctness, so checking from 2 up is safe. Gate on s_rxHaveHeader (not s_rxHaveEnc)
		 * so a CLEARTEXT SMS (no ENC header) also completes; the IPv4/UDP check in the tick
		 * rejects non-SMS data PDUs, and s_rxHaveEnc is carried through to pick decrypt vs clear. */
		if (s_rxHaveHeader && (s_rxCount >= 2) && !s_rxReady)
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
			s_rxPeerEnc = s_rxHaveEnc;   /* decrypt if the ENC header was seen, else read cleartext */
			s_rxReady = 1;          /* main loop will decrypt (or read cleartext) + store */
			s_diagPdu++;
			/* snapshot raw (still-encrypted) PDU for USB inspection (clamped to the
			 * snapshot buffer — the reported length must never exceed the bytes stored) */
			s_diagLastPduLen = (uint16_t)((total > (int)sizeof s_diagLastPdu) ? (int)sizeof s_diagLastPdu : total);
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
	uint8_t  enc = s_rxPeerEnc;
	if (pduLen > (int)sizeof pdu) { pduLen = (int)sizeof pdu; }
	memcpy(pdu, s_rxPdu, pduLen);
	s_rxReady = 0;

	if (pduLen < 32) { return; }            /* whole PDU is passed; decrypt derives the enc len */

	/* Validate the data-PDU CRC32 over [ct+pad] before trusting the bytes. This rejects
	 * reassemblies that mixed blocks across retransmits (a missed burst/header) — without it
	 * a corrupted PDU decrypts to garbage and, worse, can store a junk message. */
	{
		uint32_t want = ((uint32_t)pdu[pduLen - 4] << 24) | ((uint32_t)pdu[pduLen - 3] << 16) |
				((uint32_t)pdu[pduLen - 2] << 8) | (uint32_t)pdu[pduLen - 1];
		if (crc32_dmr(pdu, pduLen) != want) { return; }   /* corrupted reassembly -> drop */
	}

	char text[DMR_SMS_TEXT_MAX + 1];
	int got = -1;

	if (enc)
	{
		/* Encrypted: try the signalled key id first, then every loaded key (decrypt is
		 * destructive, so each attempt works on a fresh copy of the ciphertext). */
		uint8_t tmp[384];
		for (int attempt = 0; attempt <= DMR_AES_MAX_KEYS && got < 0; attempt++)
		{
			uint8_t k = (attempt == 0) ? keyId : (uint8_t)attempt;
			if (k == 0 || k >= DMR_AES_MAX_KEYS) { continue; }
			memcpy(tmp, pdu, pduLen);
			int r = dmr_aes_sms_decrypt(k, tmp, pduLen, text, sizeof text);
			if (r > 0) { got = r; }
		}
	}
	else
	{
		/* Cleartext SMS: the reassembled PDU IS the plaintext IPv4/UDP/TMS packet. */
		got = dmr_sms_text_from_plaintext(pdu, pduLen, text, sizeof text);
	}
	if (got <= 0) { return; }   /* wrong/no key, not IPv4/UDP, or not an SMS */

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

/* Glue between OpenGD77 HR-C6000 DMR path and the DMRA AES-256 module.
 * No-ops unless built with -DENABLE_AES, so stock builds are unaffected. */
#ifndef DMR_AES_HOOK_H
#define DMR_AES_HOOK_H
#include <stdint.h>
#ifdef ENABLE_AES
void dmrAesInit(void);   /* zero AES state at boot (.ccmram is not startup-initialized) */
void dmrAesSetKeyRam(uint8_t keyId, const uint8_t *key32); /* RAM-only key load (bench) */
void dmrAesLoadKeys(void);
int  dmrAesStoreKey(uint8_t keyId, const uint8_t *key32);
/* TX key selection: 0 = encrypted TX disabled, otherwise the keyId to transmit with. */
uint8_t dmrAesTxKeyId(void);
int     dmrAesSetTxKeyId(uint8_t keyId);
/* RX. Decryption is applied in the codec at the 49-bit decoded-AMBE layer
 * (dmrAesRxCodecFrame), NOT on the 27 raw FEC octets. The HR-C6000 ISR calls
 * dmrAesRxBurst(seq) per voice burst to advance the superframe/keystream offset;
 * codecDecode calls dmrAesRxCodecFrame per decoded AMBE frame. */
void dmrAesRxPI(const uint8_t *pi, int len);
void dmrAesRxBurst(int seq);                       /* per voice burst (seq 1..6) */
void dmrAesRxLateEntry(int seq, const uint8_t *ambe27); /* read Late-Entry MI bits from a burst */
void dmrAesRxCodecFrame(uint16_t *b49, int idxInBurst); /* per decoded AMBE frame (idx 0..2) */
void dmrAesRxEnd(void);
/* TX */
void dmrAesTxStart(uint8_t keyId, uint32_t miSeed);
int  dmrAesTxBuildPI(uint8_t *piOut7);
void dmrAesTxVoice(uint8_t *ambe, int seq);
void dmrAesTxCodecFrame(uint16_t *b49);            /* encrypt 49 AMBE params during encode */
void dmrAesTxStuffMI(uint16_t *b72);               /* stuff Late-Entry MI into post-ECC codeword */
int  dmrAesTxActive(void);
void dmrAesTxEnd(void);
int  dmrAesTxEmbSb(uint8_t out4[4]);               /* burst-F EMB Late-Entry Single Block (alg/key) */
#else
static inline void dmrAesInit(void){ }
static inline void dmrAesSetKeyRam(uint8_t k, const uint8_t *p){ (void)k; (void)p; }
static inline void dmrAesLoadKeys(void){ }
static inline int  dmrAesStoreKey(uint8_t k, const uint8_t *p){ (void)k; (void)p; return 0; }
static inline uint8_t dmrAesTxKeyId(void){ return 0; }
static inline int  dmrAesSetTxKeyId(uint8_t k){ (void)k; return 0; }
static inline void dmrAesRxPI(const uint8_t *p, int n){ (void)p; (void)n; }
static inline void dmrAesRxBurst(int s){ (void)s; }
static inline void dmrAesRxLateEntry(int s, const uint8_t *a){ (void)s; (void)a; }
static inline void dmrAesRxCodecFrame(uint16_t *b, int i){ (void)b; (void)i; }
static inline void dmrAesRxEnd(void){ }
static inline void dmrAesTxStart(uint8_t k, uint32_t m){ (void)k; (void)m; }
static inline int  dmrAesTxBuildPI(uint8_t *p){ (void)p; return 0; }
static inline void dmrAesTxVoice(uint8_t *a, int s){ (void)a; (void)s; }
static inline void dmrAesTxCodecFrame(uint16_t *b){ (void)b; }
static inline void dmrAesTxStuffMI(uint16_t *b){ (void)b; }
static inline int  dmrAesTxActive(void){ return 0; }
static inline void dmrAesTxEnd(void){ }
static inline int  dmrAesTxEmbSb(uint8_t *o){ (void)o; return 0; }
#endif
#endif

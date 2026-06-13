/* Glue between OpenGD77 HR-C6000 DMR path and the DMRA AES-256 module.
 * No-ops unless built with -DENABLE_AES, so stock builds are unaffected. */
#ifndef DMR_AES_HOOK_H
#define DMR_AES_HOOK_H
#include <stdint.h>
#ifdef ENABLE_AES
void dmrAesLoadKeys(void);
int  dmrAesStoreKey(uint8_t keyId, const uint8_t *key32);
/* RX */
void dmrAesRxPI(const uint8_t *pi, int len);
void dmrAesRxVoice(uint8_t *ambe, int seq);
void dmrAesRxEnd(void);
/* TX */
void dmrAesTxStart(uint8_t keyId, uint32_t miSeed);
int  dmrAesTxBuildPI(uint8_t *piOut7);
void dmrAesTxVoice(uint8_t *ambe, int seq);
int  dmrAesTxActive(void);
void dmrAesTxEnd(void);
#else
static inline void dmrAesLoadKeys(void){ }
static inline int  dmrAesStoreKey(uint8_t k, const uint8_t *p){ (void)k; (void)p; return 0; }
static inline void dmrAesRxPI(const uint8_t *p, int n){ (void)p; (void)n; }
static inline void dmrAesRxVoice(uint8_t *a, int s){ (void)a; (void)s; }
static inline void dmrAesRxEnd(void){ }
static inline void dmrAesTxStart(uint8_t k, uint32_t m){ (void)k; (void)m; }
static inline int  dmrAesTxBuildPI(uint8_t *p){ (void)p; return 0; }
static inline void dmrAesTxVoice(uint8_t *a, int s){ (void)a; (void)s; }
static inline int  dmrAesTxActive(void){ return 0; }
static inline void dmrAesTxEnd(void){ }
#endif
#endif

/* RX glue between OpenGD77 HR-C6000 DMR path and the DMRA AES-256 module.
 * No-ops unless built with -DENABLE_AES, so stock builds are unaffected. */
#ifndef DMR_AES_HOOK_H
#define DMR_AES_HOOK_H
#include <stdint.h>
#ifdef ENABLE_AES
void dmrAesLoadKeys(void);                     /* (re)load AES keys from the codeplug custom-data store */
void dmrAesRxPI(const uint8_t *pi, int len);   /* feed a candidate PI/LC burst */
void dmrAesRxVoice(uint8_t *ambe, int seq);    /* decrypt one 27-byte AMBE burst (seq=Voice frame A..F) */
void dmrAesRxEnd(void);                        /* call ended */
#else
static inline void dmrAesLoadKeys(void){ }
static inline void dmrAesRxPI(const uint8_t *pi, int len){ (void)pi; (void)len; }
static inline void dmrAesRxVoice(uint8_t *ambe, int seq){ (void)ambe; (void)seq; }
static inline void dmrAesRxEnd(void){ }
#endif
#endif

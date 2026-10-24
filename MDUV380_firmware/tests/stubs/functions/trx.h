/* Host-test stand-in for the firmware trx.h: only what dmr_sms.c uses. */
#ifndef TEST_STUB_TRX_H
#define TEST_STUB_TRX_H
#include <stdbool.h>
#include <stdint.h>
extern volatile bool trxTransmissionEnabled;
extern volatile bool trxIsTransmitting;
extern uint32_t trxDMRID;
#endif

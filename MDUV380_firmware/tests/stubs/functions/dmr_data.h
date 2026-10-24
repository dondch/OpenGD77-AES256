/* Host-test stand-in for the firmware dmr_data.h: the data-TX harness API dmr_sms.c calls. */
#ifndef TEST_STUB_DMR_DATA_H
#define TEST_STUB_DMR_DATA_H
#include <stdint.h>
#define DMR_DATA_MAX_BURSTS  40
#define DMR_DATA_BURST_LEN   12
void dmrDataTxLoad(const uint8_t *bursts, uint8_t count);
int  dmrDataTxActive(void);
int  dmrDataTxNextBurst(uint8_t *dataTypeOut, uint8_t *payload12Out);
void dmrDataTxEnd(void);
#endif

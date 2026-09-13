/* Host-test stand-in for the firmware codeplug.h: only what dmr_aes_hook.c uses. */
#ifndef TEST_STUB_CODEPLUG_H
#define TEST_STUB_CODEPLUG_H
#include <stdbool.h>
#include <stdint.h>

#define FLASH_ADDRESS_OFFSET (128 * 1024)

typedef enum { CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS = 3 } CodeplugCustomDataType_t;

bool codeplugGetOpenGD77CustomDataBounded(CodeplugCustomDataType_t dataType, uint8_t *dataBuf, int maxLen);
bool codeplugSetOpenGD77CustomData(CodeplugCustomDataType_t dataType, uint8_t *dataBuf, int len);
#endif

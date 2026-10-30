/* Host-test stand-in for the firmware codeplug.h: only what dmr_aes_hook.c and dmr_sms.c use. */
#ifndef TEST_STUB_CODEPLUG_H
#define TEST_STUB_CODEPLUG_H
#include <stdbool.h>
#include <stdint.h>

#define FLASH_ADDRESS_OFFSET (128 * 1024)

typedef enum
{
    CODEPLUG_CUSTOM_DATA_TYPE_EMPTY = 0xFFFFFFFF,
    CODEPLUG_CUSTOM_DATA_TYPE_IMAGE = 1,
    CODEPLUG_CUSTOM_DATA_TYPE_BEEP,
    CODEPLUG_CUSTOM_DATA_TYPE_SATELLITE_TLE,
    CODEPLUG_CUSTOM_DATA_TYPE_THEME_DAY,
    CODEPLUG_CUSTOM_DATA_TYPE_THEME_NIGHT,
    CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS,
    CODEPLUG_CUSTOM_DATA_TYPE_MESSAGES,
    CODEPLUG_CUSTOM_DATA_TYPE_MSG_CONFIG,
    CODEPLUG_CUSTOM_DATA_TYPE_PROVISION,
} CodeplugCustomDataType_t;

/* Only the member the AES/SMS code reads (the per-channel encrypt byte). */
typedef struct
{
    uint8_t encrypt;
} CodeplugChannel_t;

typedef enum { CHANNEL_FLAG_OPTIONAL_DMRID = 0 } ChannelFlag_t;

uint8_t codeplugChannelGetFlag(CodeplugChannel_t *channelBuf, ChannelFlag_t flag);
bool codeplugGetOpenGD77CustomData(CodeplugCustomDataType_t dataType, uint8_t *dataBuf);
bool codeplugGetOpenGD77CustomDataBounded(CodeplugCustomDataType_t dataType, uint8_t *dataBuf, int maxLen);
bool codeplugSetOpenGD77CustomData(CodeplugCustomDataType_t dataType, uint8_t *dataBuf, int len);
#endif

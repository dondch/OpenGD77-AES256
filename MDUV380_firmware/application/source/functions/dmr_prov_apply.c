/*
 * dmr_prov_apply.c — the firmware side of remote provisioning: everything dmr_prov.c needs
 * from the radio (codeplug and settings writers, the key store, the feature arena, the UI,
 * battery, git hash, reboot). The host test replaces this file with stubs that record what
 * would have been written.
 *
 * Every write here runs in the main loop task (dmrProvTick / dmrSmsRxTick), never in an
 * ISR and never inside the CPS critical section: EEPROM_Write and SPI_Flash_write poll with
 * osDelay() (see the 0x80/0x82/0x98 handlers in usb_com.c for what happens otherwise).
 */
#include "functions/dmr_prov.h"

#if defined(ENABLE_REMOTE_PROVISION)

#include "main.h"
#include "utils.h"
#include "functions/codeplug.h"
#include "functions/settings.h"
#include "functions/featureArena.h"
#include "functions/ticks.h"
#include "functions/trx.h"
#include "crypto/dmr_aes.h"
#include "crypto/dmr_aes_hook.h"
#include "hardware/EEPROM.h"
#include "hardware/SPI_Flash.h"
#include "functions/dmr_sms.h"
#include "user_interface/uiGlobals.h"
#include "interfaces/batteryAndPowerManagement.h"
#include "user_interface/menuSystem.h"
#if defined(ENABLE_SIMPLE_MODE)
#include "user_interface/uiSimpleMode.h"
#endif
#include <string.h>
#include <stddef.h>

int dmrProvApplyChannel(uint16_t index, const uint8_t *record56)
{
    if ((index < CODEPLUG_CHANNELS_MIN) || (index > CODEPLUG_CHANNELS_MAX)) { return 0; }
    if (!codeplugChannelSaveRawDataForIndex(index, record56)) { return 0; }
    if (!codeplugAllChannelsIndexIsInUse(index)) { codeplugAllChannelsIndexSetUsed(index); }
    return 1;
}

int dmrProvApplyZone(uint8_t zone, const uint8_t *record176)
{
    return codeplugZoneSaveRawDataForIndex(zone, record176) ? 1 : 0;
}

int dmrProvApplyVfo(uint8_t vfo, const uint8_t *record56)
{
    if (vfo > 1) { return 0; }
    if (!EEPROM_Write(CODEPLUG_ADDR_VFO_A_CHANNEL + (CODEPLUG_CHANNEL_DATA_STRUCT_SIZE * (int)vfo), (uint8_t *)record56, CODEPLUG_CHANNEL_DATA_STRUCT_SIZE)) { return 0; }
    /* refresh the live copy (converted form), as a boot would */
    codeplugGetVFO_ChannelData(&settingsVFOChannel[vfo], (vfo == 0) ? CHANNEL_VFO_A : CHANNEL_VFO_B);
    return 1;
}

/* The settings fields a master may set, by index (PROVISIONING.md §6; tools/provision.py
 * mirrors the names). Offsets come from the real struct, so a layout change cannot break it.
 * width 0 is a single option bit: off holds the bitfieldOptions_t value, the setting is 0 or 1
 * (a bit must not travel as a whole bank: the write would clobber every other option). */
typedef struct { uint32_t off; uint8_t width; } provField_t;
#define F(name, w) { (uint32_t)offsetof(settingsStruct_t, name), (w) }
#define B(bit)     { (uint32_t)(bit), 0 }
static const provField_t s_fields[] =
{
    F(txPowerLevel, 1), F(backlightMode, 1), F(backLightTimeout, 1), F(displayContrast, 1),
    F(scanDelay, 1), F(scanModePause, 1), F(scanStepTime, 1), F(dmrDestinationFilter, 1),
    F(dmrCcTsFilter, 1), F(privateCalls, 1), F(txTimeoutBeepX5Secs, 1), F(micGainDMR, 1),
    F(micGainFM, 1), F(ecoLevel, 1), F(apo, 1), F(autolockTimer, 1), F(currentZone, 2),
    F(beepVolumeDivider, 1), F(bitfieldOptions[0], 4), F(bitfieldOptions[1], 4),
#if defined(ENABLE_SIMPLE_MODE)
    B(BIT_SIMPLE_MODE), F(simpleHomeZone, 2), F(simpleHomeChannel, 2), F(simpleIdleMinutes, 1),
#endif
};
#undef F
#undef B
#define PROV_NUM_FIELDS ((int)(sizeof s_fields / sizeof s_fields[0]))

int dmrProvApplySetting(uint8_t field, uint32_t value)
{
    if (field >= PROV_NUM_FIELDS) { return 0; }
    uint8_t *p = (uint8_t *)&nonVolatileSettings + s_fields[field].off;
    switch (s_fields[field].width)
    {
        case 0: settingsSetOptionBit((bitfieldOptions_t)s_fields[field].off, value != 0); break;
        case 1: { uint8_t v = (uint8_t)value; memcpy(p, &v, 1); break; }
        case 2: { uint16_t v = (uint16_t)value; memcpy(p, &v, 2); break; }
        default: { memcpy(p, &value, 4); break; }
    }
#if defined(ENABLE_SIMPLE_MODE)
    nonVolatileSettings.simpleValid = SIMPLE_SETTINGS_VALID;
#endif
    return 1;
}

int dmrProvApplySettingsSave(void)
{
#if defined(ENABLE_SIMPLE_MODE)
    simpleModeSettingsChanged();   /* a new home or a freshly set bit applies on the next main-loop tick */
#endif
    return settingsSaveSettings(true) ? 1 : 0;
}

int dmrProvApplyStoreKey(uint8_t slot, const uint8_t *key32)
{
    if ((slot < 1) || (slot >= DMR_AES_MAX_KEYS)) { return 0; }
    return dmrAesStoreKey(slot, key32);
}

int dmrProvApplyContact(uint16_t index, const uint8_t *record24)
{
    if ((index < CODEPLUG_CONTACTS_MIN) || (index > CODEPLUG_CONTACTS_MAX)) { return 0; }
    CodeplugContact_t c;
    memcpy(&c, record24, CODEPLUG_CONTACT_DATA_SIZE);
    /* the record is in codeplug form; the saver takes the converted id and converts it back,
     * then updates the contacts cache itself (a name starting with 0xFF removes the contact) */
    c.tgNumber = bcd2int(__builtin_bswap32(c.tgNumber));
    c.NOT_IN_CODEPLUGDATA_indexNumber = index;
    return codeplugContactSaveDataForIndex(index, &c) ? 1 : 0;
}

int dmrProvApplyIdentity(uint32_t dmrId, const uint8_t *callsign8)
{
    if ((dmrId == 0) || (dmrId > 0x00FFFFFFu)) { return 0; }
    uint32_t bcd = __builtin_bswap32(int2bcd(dmrId));   /* as codeplugSetUserDMRID stores it */
    if (!EEPROM_Write(CODEPLUG_ADDR_USER_DMRID, (uint8_t *)&bcd, 4)) { return 0; }
    return EEPROM_Write(CODEPLUG_ADDR_USER_CALLSIGN, (uint8_t *)callsign8, DMR_PROV_CALLSIGN_LEN) ? 1 : 0;
}

void dmrProvApplyIdentityActivate(void)
{
    trxDMRID = uiDataGlobal.userDMRId = codeplugGetUserDMRID();   /* what settingsLoadSettings does at boot */
}

int dmrProvApplyMsgPresets(uint8_t first, uint8_t count, const uint8_t *rows)
{
    return dmrSmsPresetsWrite(first, count, rows);
}

static void provRebootCallback(void)
{
    settingsSaveSettings(true);
    NVIC_SystemReset();
}

void dmrProvApplyReboot(void)
{
    addTimerCallback(provRebootCallback, 1500, MENU_ANY, false);
}

int dmrProvApplyReadBack(uint8_t op, const uint8_t *body, int bodyLen, uint8_t *out, int max)
{
    (void)bodyLen;
    switch (op)
    {
        case DMR_PROV_OP_WRITE_CHANNEL:
        {
            CodeplugChannel_t ch;
            uint16_t index = (uint16_t)((body[0] << 8) | body[1]);
            if ((index < CODEPLUG_CHANNELS_MIN) || (index > CODEPLUG_CHANNELS_MAX) || (max < CODEPLUG_CHANNEL_DATA_STRUCT_SIZE)) { return 0; }
            codeplugChannelGetDataWithOffsetAndLengthForIndex(index, &ch, 0, CODEPLUG_CHANNEL_DATA_STRUCT_SIZE);   /* raw bytes */
            memcpy(out, &ch, CODEPLUG_CHANNEL_DATA_STRUCT_SIZE);
            return CODEPLUG_CHANNEL_DATA_STRUCT_SIZE;
        }
        case DMR_PROV_OP_WRITE_ZONE:
            return codeplugZoneGetRawDataForIndex(body[0], out, max);
        case DMR_PROV_OP_SET_VFO:
            if ((body[0] > 1) || (max < CODEPLUG_CHANNEL_DATA_STRUCT_SIZE)) { return 0; }
            EEPROM_Read(CODEPLUG_ADDR_VFO_A_CHANNEL + (CODEPLUG_CHANNEL_DATA_STRUCT_SIZE * (int)body[0]), out, CODEPLUG_CHANNEL_DATA_STRUCT_SIZE);
            return CODEPLUG_CHANNEL_DATA_STRUCT_SIZE;
        case DMR_PROV_OP_SET_SETTING:
            if ((body[0] >= PROV_NUM_FIELDS) || (max < 4)) { return 0; }
            if (s_fields[body[0]].width == 0) { out[0] = settingsIsOptionBitSet((bitfieldOptions_t)s_fields[body[0]].off) ? 1 : 0; return 1; }
            memcpy(out, (uint8_t *)&nonVolatileSettings + s_fields[body[0]].off, s_fields[body[0]].width);
            return s_fields[body[0]].width;
        case DMR_PROV_OP_WRITE_CONTACT:
        {
            uint16_t index = (uint16_t)((body[0] << 8) | body[1]);
            if ((index < CODEPLUG_CONTACTS_MIN) || (index > CODEPLUG_CONTACTS_MAX) || (max < CODEPLUG_CONTACT_DATA_SIZE)) { return 0; }
            SPI_Flash_read(FLASH_ADDRESS_OFFSET + CODEPLUG_ADDR_CONTACTS + (index - 1) * CODEPLUG_CONTACT_DATA_SIZE, out, CODEPLUG_CONTACT_DATA_SIZE);
            return CODEPLUG_CONTACT_DATA_SIZE;
        }
        case DMR_PROV_OP_SET_IDENTITY:
        {
            if (max < 4 + DMR_PROV_CALLSIGN_LEN) { return 0; }
            uint32_t id = codeplugGetUserDMRID();
            out[0] = (uint8_t)(id >> 24); out[1] = (uint8_t)(id >> 16); out[2] = (uint8_t)(id >> 8); out[3] = (uint8_t)id;
            EEPROM_Read(CODEPLUG_ADDR_USER_CALLSIGN, out + 4, DMR_PROV_CALLSIGN_LEN);
            return 4 + DMR_PROV_CALLSIGN_LEN;
        }
        case DMR_PROV_OP_WRITE_MSG_PRESETS:
            if (max < body[1] * DMR_PROV_PRESET_LEN) { return 0; }
            return dmrSmsPresetsRead(body[0], body[1], out);
        default:
            return 0;   /* keys are never read back */
    }
}

int dmrProvApplyTxAllowed(void)
{
    if ((currentChannelData != NULL) && (codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_RX_ONLY) != 0)) { return 0; }
    if (settingsIsOptionBitSet(BIT_TX_INHIBIT)) { return 0; }
    return 1;
}

void dmrProvApplyNotify(const char *text)
{
    uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 4000, text, true);
}

uint16_t dmrProvApplyBatteryCentivolts(void)
{
    float v = averageBatteryVoltage * 10.0f;   /* averageBatteryVoltage is in decivolts */
    if (v < 0) { v = 0; }
    if (v > 65535.0f) { v = 65535.0f; }
    return (uint16_t)v;
}

const char *dmrProvApplyGitHash(void)
{
    return XSTRINGIFY(GITVERSION);
}

int dmrProvApplyBlockRead(uint8_t *blk96)
{
    return codeplugGetOpenGD77CustomDataBounded(CODEPLUG_CUSTOM_DATA_TYPE_PROVISION, blk96, DMR_PROV_BLOCK_LEN) ? 1 : 0;
}

int dmrProvApplyBlockWrite(const uint8_t *blk96)
{
    dmrAesEnsureCustomDataRegion();
    return codeplugSetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_PROVISION, (uint8_t *)blk96, DMR_PROV_BLOCK_LEN) ? 1 : 0;
}

uint8_t *dmrProvApplyArenaClaim(void)
{
    return (uint8_t *)featureArenaClaim(FEATURE_ARENA_PROVISION, DMR_PROV_ARENA_BYTES);
}

void dmrProvApplyArenaRelease(void)
{
    featureArenaRelease(FEATURE_ARENA_PROVISION);
}

#endif /* ENABLE_REMOTE_PROVISION */

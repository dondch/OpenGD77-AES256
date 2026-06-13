#include "crypto/dmr_aes_hook.h"
#ifdef ENABLE_AES
#include "crypto/dmr_aes.h"
#include "functions/codeplug.h"
#include <stddef.h>
#include <string.h>

/* DMR voice burst = 27 bytes (AMBE_AUDIO_LENGTH) = 3x 9-byte AMBE frames. */
#define DMR_AMBE_BURST 27

/* AES key store: OpenGD77 custom-data block type CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS.
 * Stored safely in the firmware-managed custom-data region (no codeplug-offset guessing).
 * Block layout:
 *   [0..3]  magic "AESK"
 *   [4]     version (1)
 *   [5]     key count
 *   [6..7]  reserved
 *   then <count> entries, each 36 bytes: [0]=keyId  [1..3]=reserved  [4..35]=32-byte AES-256 key
 */
#define AESK_HDR_LEN   8
#define AESK_ENTRY_LEN 36

static dmr_aes_ctx_t s_rx;
static size_t        s_rxOff;
static int           s_rxActive;
static int           s_keysLoaded;

void dmrAesLoadKeys(void)
{
    static uint8_t buf[AESK_HDR_LEN + DMR_AES_MAX_KEYS * AESK_ENTRY_LEN];
    s_keysLoaded = 1;
    dmr_aes_clear_keys();
    if (!codeplugGetOpenGD77CustomData(CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS, buf)) { return; }
    if (memcmp(buf, "AESK", 4) != 0) { return; }
    uint8_t count = buf[5];
    for (uint8_t i = 0; (i < count) && (i < DMR_AES_MAX_KEYS); i++)
    {
        uint8_t *e = buf + AESK_HDR_LEN + (size_t)i * AESK_ENTRY_LEN;
        dmr_aes_set_key(e[0], e + 4);   /* e[0] = keyId, e+4 = 32-byte key */
    }
}

/* Called on each decoded LC burst; loads keys on first use, acts only on a DMRA PI header. */
void dmrAesRxPI(const uint8_t *pi, int len)
{
    dmr_pi_t p;
    if (!s_keysLoaded) { dmrAesLoadKeys(); }
    if (dmr_pi_parse(pi, (size_t)len, &p) && p.valid)
    {
        s_rxActive = (dmr_aes_rx_init(&s_rx, &p) == 0);
        s_rxOff = 0;
        if (s_rxActive) { dmr_aes_superframe(&s_rx); }
    }
}

void dmrAesRxVoice(uint8_t *ambe, int seq)
{
    if (!s_rxActive) { return; }
    if (seq == 0) { dmr_aes_superframe(&s_rx); s_rxOff = 0; }
    s_rxOff = dmr_aes_crypt_frame(&s_rx, ambe, DMR_AMBE_BURST, s_rxOff);
}

void dmrAesRxEnd(void) { s_rxActive = 0; }
#endif

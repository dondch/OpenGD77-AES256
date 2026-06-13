#include "crypto/dmr_aes_hook.h"
#ifdef ENABLE_AES
#include "crypto/dmr_aes.h"
#include <stddef.h>
#define DMR_AMBE_BURST 27   /* AMBE_AUDIO_LENGTH: 3x 9-byte AMBE frames per burst */
static dmr_aes_ctx_t s_rx;
static size_t        s_rxOff;
static int           s_rxActive;
/* TODO(OTA): confirm exactly which burst carries ALG/KEYID/MI on TYT. */
void dmrAesRxPI(const uint8_t *pi, int len)
{
    dmr_pi_t p;
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

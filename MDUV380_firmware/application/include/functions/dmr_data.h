/*
 * dmr_data.h — DMR data-path harness (Phase 2).
 *
 * Self-test / development harness for DMR short-data (SMS) and GPS-over-DMR work:
 *  - button-free firmware flashing (reboot into the STM32 ROM DFU bootloader over USB),
 *  - (later) host-parameterised DMR data-frame TX, so a connected PC + HackRF can
 *    trigger and verify transmissions without a second radio.
 *
 * Everything here is behind -DENABLE_DMR_DATA; the file compiles to nothing in stock
 * builds, so they stay byte-identical. Does not touch the AES paths.
 */
#ifndef _OPENGD77_DMR_DATA_H_
#define _OPENGD77_DMR_DATA_H_

#include <stdint.h>

#if defined(ENABLE_DMR_DATA)

/*
 * Schedule a reboot into the STM32F405 ROM system bootloader (USB DFU, 0483:DF11),
 * ~500 ms later so the triggering command's USB ACK is sent first. Lets the firmware
 * loader flash over USB with no PTT+power-on button dance. Recoverable: a normal
 * power-cycle returns to the app (nothing is erased by the jump itself).
 */
void dmrDataTriggerReboot(void);

/* ---- host-parameterised DMR data-frame TX (bring-up harness) -----------------
 * The PC supplies a sequence of DMR data BURSTS; the firmware keys a DMR TX and
 * drives the HR-C6000 to emit each burst (the chip does the BPTC/Trellis FEC), so
 * the SMS / GPS framing is iterated on the PC without reflashing. Each burst is
 * 1 data-type byte (written to HR-C6000 page 0x04 reg 0x50: slot-type<<4 | data |
 * header-bit | LCSS — e.g. 0x60 data header, 0x70 rate-1/2 data, 0x20 terminator)
 * followed by 12 payload bytes (the 96 info bits the chip FEC-encodes), matching
 * the proven AES PI-header burst path (page 0x02 reg 0x00 + reg 0x50). */
#define DMR_DATA_MAX_BURSTS  24
#define DMR_DATA_BURST_LEN   12

/* Load the burst queue (bursts = count * (1 type byte + DMR_DATA_BURST_LEN payload))
 * and key a data call (deferred to the main loop). Called from the CPS handler. */
void dmrDataTxLoad(const uint8_t *bursts, uint8_t count);

/* HR-C6000 TX state-machine hooks (called from the ISR): is a data call queued, and
 * fetch the next burst (1 + fills dataType/payload; 0 when the queue is drained). */
int  dmrDataTxActive(void);
int  dmrDataTxNextBurst(uint8_t *dataTypeOut, uint8_t *payload12Out);
void dmrDataTxEnd(void);   /* clear the data-call state (queue drained / TX ended) */

#endif // ENABLE_DMR_DATA
#endif // _OPENGD77_DMR_DATA_H_

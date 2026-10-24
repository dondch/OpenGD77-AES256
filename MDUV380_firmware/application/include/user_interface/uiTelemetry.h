/*
 * Copyright (C) 2026 TA2TWC
 *
 * Based on the OpenGD77 firmware
 * Copyright (C) 2019-2025 Roger Clark, VK3KYY / G4KYF
 *                         Daniel Caujolle-Bert, F1RMB
 *
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _OPENGD77_UITELEMETRY_H_
#define _OPENGD77_UITELEMETRY_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Needs the whole 160x128 display (which is not the case of the DM-1701)
#if defined(ENABLE_TA2TWC_SCREENS) && (defined(PLATFORM_MDUV380) || defined(PLATFORM_RT84_DM1701))
#define HAS_TELEMETRY                   1
#endif

// Vertical positions, they are using the areas left free by the Channel and VFO screens on the 160x128 displays
#if defined(PLATFORM_VARIANT_DM1701) // 120 rows: what is at the bottom goes 8 pixels up
#define TELEMETRY_Y_POS_RX_CHANNEL      77
#define TELEMETRY_Y_POS_RX_VFO          44
#define TELEMETRY_Y_POS_TX              109
#define TELEMETRY_Y_POS_TX_TOT_BAR      54  // Under the Tx timer
#define TELEMETRY_Y_POS_TX_DETAILS      80  // Between the contact and the channel name (or the Tx frequency)
#define TELEMETRY_Y_POS_DMR             108 // Received TS, CC and DMR ID, while the data of the caller is displayed
#else
#define TELEMETRY_Y_POS_RX_CHANNEL      83
#define TELEMETRY_Y_POS_RX_VFO          46
#define TELEMETRY_Y_POS_TX              117
#define TELEMETRY_Y_POS_TX_TOT_BAR      58  // Under the Tx timer
#define TELEMETRY_Y_POS_TX_DETAILS      86  // Between the contact and the channel name (or the Tx frequency)
#define TELEMETRY_Y_POS_DMR             116 // Received TS, CC and DMR ID, while the data of the caller is displayed
#endif
#define TELEMETRY_RX_HEIGHT             13  // S-meter line + bar graph
#define TELEMETRY_RX_DETAILS_Y_OFFSET   17  // Noise floor, battery voltage and temperature line
#define TELEMETRY_LINE_HEIGHT           8

// Signal strength history graph, one sample per column
#if defined(PLATFORM_VARIANT_DM1701) // 120 rows: under the contact and over the channel name, under the Tx frequency
#define TELEMETRY_HISTORY_Y_POS_CHANNEL     41
#define TELEMETRY_HISTORY_HEIGHT_CHANNEL    14
#define TELEMETRY_HISTORY_Y_POS_VFO         113
#define TELEMETRY_HISTORY_HEIGHT_VFO        7
#else
#define TELEMETRY_HISTORY_Y_POS_CHANNEL     43
#define TELEMETRY_HISTORY_HEIGHT_CHANNEL    18
#define TELEMETRY_HISTORY_Y_POS_VFO         114
#define TELEMETRY_HISTORY_HEIGHT_VFO        13
#endif
#define TELEMETRY_HISTORY_SAMPLE_PERIOD_MS  200U // 160 samples: 32 seconds
#define TELEMETRY_HISTORY_FIRST_ROW(y)      ((y) / 8)
#define TELEMETRY_HISTORY_LAST_ROW(y, h)    (((y) + (h) + 7) / 8)

// Display rows (8 pixels each) covering the Rx parts, for displayRenderRows()
#define TELEMETRY_RX_FIRST_ROW(y)       ((y) / 8)
#define TELEMETRY_RX_LAST_ROW(y)        ((((y) + TELEMETRY_RX_DETAILS_Y_OFFSET + TELEMETRY_LINE_HEIGHT) / 8) + 1)

typedef struct
{
	int16_t     rssidBm;
	uint8_t     noiseRaw;   // noise level of the RF chip, in its own unit (the squelch works on it): low when the signal is clean
	uint16_t    batteryDeciVolts;        // 78 is 7.8V
	int16_t     temperatureDeciCelsius;  // 315 is 31.5 degrees
	const char *powerLevel;              // "5", "750", "+W-"...
	const char *powerUnit;               // "W", "mW" or ""
} uiTelemetryData_t;

// These functions are only drawing in the screen buffer, clearing their own area first.
uint8_t uiTelemetryNoiseToQuality(uint8_t noiseRaw); // 0..100 %
void uiTelemetryDrawRx(const uiTelemetryData_t *data, int16_t y, bool withDetails);
void uiTelemetryDrawTx(const uiTelemetryData_t *data);
// Tx frequency (in 10Hz units, not displayed if 0) and mode related text (TS and CC, CTCSS tone...), with the remaining time before the timeout
// as a bar (not displayed if totTotalSecs is 0).
void uiTelemetryDrawTxDetails(uint32_t txFrequency, const char *modeText, int16_t totRemainingSecs, int16_t totTotalSecs);
// "S0" to "S9", then "S9+10" to "S9+60"
void uiTelemetryBuildSMeterText(char *buffer, size_t bufferLen, int16_t dBm);
// timeSlot is 1 or 2. matchesTxSettings is false when replying wouldn't be done on the TS and CC of the caller.
void uiTelemetryDrawDmr(uint8_t timeSlot, uint8_t colourCode, uint32_t sourceId, bool matchesTxSettings);

void uiTelemetryHistoryClear(void);
void uiTelemetryHistoryAddGap(void); // the sampling has been interrupted
void uiTelemetryHistoryAdd(int16_t dBm);
void uiTelemetryHistoryDraw(int16_t y, int16_t height);

#endif // _OPENGD77_UITELEMETRY_H_

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

#ifndef _OPENGD77_UIHOMESCREEN_H_
#define _OPENGD77_UIHOMESCREEN_H_

#include <stdint.h>
#include <stdbool.h>
#include "user_interface/uiTelemetry.h"

/*
 * Redesigned main screen (Channel mode), for the 160x128 colour displays.
 *
 * This module only draws, from the data it is given: it reads nothing from the radio (see uiUtilityHomeScreenDraw()).
 */

// Layout (y positions)
#define HOME_STATUS_BAR_HEIGHT          14 // the area cleared when the header has to be redrawn (see applicationMain.c)
#if defined(PLATFORM_VARIANT_DM1701) // 120 rows: the lines are closer, the history is thinner
#define HOME_Y_POS_NAME                 13
#define HOME_Y_POS_FREQUENCY            28
#define HOME_Y_POS_INFO                 61
#define HOME_Y_POS_CONTACT              70
#define HOME_Y_POS_TELEMETRY            87
#define HOME_Y_POS_HISTORY              113
#define HOME_HISTORY_HEIGHT             7
#define HOME_Y_POS_MIC_BAR              95 // while transmitting
#else
#define HOME_Y_POS_NAME                 14
#define HOME_Y_POS_FREQUENCY            30
#define HOME_Y_POS_INFO                 63
#define HOME_Y_POS_CONTACT              73
#define HOME_Y_POS_TELEMETRY            91
#define HOME_Y_POS_HISTORY              117
#define HOME_HISTORY_HEIGHT             11
#define HOME_Y_POS_MIC_BAR              100 // while transmitting
#endif
#define HOME_MIC_BAR_HEIGHT             4
#define HOME_ZONE_MAX_LEN               12
#define HOME_INFO_MAX_LEN               26

typedef struct
{
	bool               isDigital;
	bool               isTransmitting;
	bool               isScanning;
	uint8_t            timeSlot;         // 1 or 2 (DMR)
	uint8_t            colourCode;       // DMR
	bool               bandwidthIs25kHz; // FM
	const char        *channelName;
	const char        *zoneText;         // zone name and channel number
	const char        *contactText;      // TG / PC name (DMR), or CSS tone (FM)
	const char        *sideText;         // small text on the right of the contact line, could be NULL
	const char        *statusFlags;      // some letters for the status bar (R: reverse repeater, T: talkaround...), could be NULL
	uint32_t           rxFrequency;      // in 10Hz units
	uint32_t           txFrequency;
	int8_t             hours;            // clock, negative to hide it
	int8_t             minutes;
	uint8_t            batteryPercentage;
	uint16_t           txTimeSecs;       // while transmitting
	uint16_t           totTotalSecs;     // 0: no timeout
	uiTelemetryData_t  telemetry;

	// A DMR call is received: the caller replaces the channel name and the frequency
	bool               isReceivingCall;
	bool               callIsPrivate;
	bool               callMatchesTxSettings; // false when replying wouldn't be done on the TG, TS and CC of the caller
	const char        *callTalkgroupText;     // "TG 286", or the "Private call" text of the current language
	const char        *callerCallsign;
	const char        *callerText;            // name, location... could be NULL
	uint32_t           callerId;
} uiHomeScreenData_t;

// Draws the whole screen in the screen buffer
void uiHomeScreenDraw(const uiHomeScreenData_t *data);
// Only redraws the status bar (mode, TS/CC, clock, power, battery)
void uiHomeScreenDrawStatusBar(const uiHomeScreenData_t *data);

#endif // _OPENGD77_UIHOMESCREEN_H_

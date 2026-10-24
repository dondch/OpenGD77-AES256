/*
 * Copyright (C) 2023-2025 Roger Clark, VK3KYY / G4KYF
 *                         Daniel Caujolle-Bert, F1RMB
 *
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

#include "user_interface/uiGlobals.h"
#include "user_interface/menuSystem.h"
#include "user_interface/uiUtilities.h"
#include "user_interface/uiDualScreen.h"
#include "user_interface/uiLocalisation.h"
#include "functions/trx.h"
#include "functions/sound.h"
#include "functions/rxPowerSaving.h"

#if defined(HAS_DUAL_WATCH_OPTIONS)

// "Dual screen": like the Anytone radios, two rows, A on top and B below. Each row is independently a VFO
// (row A = VFO A, row B = VFO B) or a channel of the current zone. One row is active: the radio is tuned to
// it and the normal VFO / channel screen of that row is the one running; the other row is only drawn.
// Up/Down select the other row, the red key switches the active row between VFO and channel (the rotary,
// as always, changes the frequency or the channel).
//
// State: nonVolatileSettings.dualWatchOptions bits DUALWATCH_LINE_A_CHANNEL / _LINE_B_CHANNEL / _ACTIVE_B.
// Each channel row has its own zone and index in that zone, kept in RAM (the active row is always the
// current zone's last used channel, the other row keeps what it had).

static int16_t slotIndex[2] = { -1, -1 };
static int16_t slotZone[2] = { -1, -1 };
static bool slotsRestored = false;

static uint16_t opts(void)
{
	return (uint16_t)nonVolatileSettings.dualWatchOptions;
}

static void setOpts(uint16_t value)
{
	if (value != opts())
	{
		settingsSet(nonVolatileSettings.dualWatchOptions, (int16_t)value);
	}
}

static int activeLine(void)
{
	return ((opts() & DUALWATCH_ACTIVE_B) ? 1 : 0);
}

static void setActiveLine(int line)
{
	setOpts(line ? (opts() | DUALWATCH_ACTIVE_B) : (opts() & ~DUALWATCH_ACTIVE_B));
}

static bool lineIsChannel(int line)
{
	return ((opts() & (line ? DUALWATCH_LINE_B_CHANNEL : DUALWATCH_LINE_A_CHANNEL)) != 0U);
}

static void setLineType(int line, bool channel)
{
	uint16_t mask = (line ? DUALWATCH_LINE_B_CHANNEL : DUALWATCH_LINE_A_CHANNEL);

	setOpts(channel ? (opts() | mask) : (opts() & ~mask));
}

static bool zoneUsable(void)
{
	return (currentZone.NOT_IN_CODEPLUGDATA_numChannelsInZone > 0);
}

// Index in zone 'z' to use for a channel row (0 / first in use when the stored one is not valid any more)
static int16_t validIndexInZone(const CodeplugZone_t *z, int16_t idx)
{
	if (idx < 0)
	{
		idx = codeplugGetLastUsedChannelInZone(z->NOT_IN_CODEPLUGDATA_indexNumber); // row not set yet: what the zone had
	}

	if (CODEPLUG_ZONE_IS_ALLCHANNELS(*z))
	{
		if ((idx >= 1) && codeplugAllChannelsIndexIsInUse(idx))
		{
			return idx;
		}

		for (int16_t i = 1; i <= z->NOT_IN_CODEPLUGDATA_highestIndex; i++)
		{
			if (codeplugAllChannelsIndexIsInUse(i))
			{
				return i;
			}
		}
		return 1;
	}

	return (((idx >= 0) && (idx < z->NOT_IN_CODEPLUGDATA_numChannelsInZone)) ? idx : 0);
}

static int16_t channelNumberForIndex(const CodeplugZone_t *z, int16_t idx)
{
	return (CODEPLUG_ZONE_IS_ALLCHANNELS(*z) ? idx : z->channels[idx]);
}

// The zone a channel row belongs to (the current one until the row has its own)
static int16_t zoneOfLine(int line)
{
	return ((slotZone[line] >= 0) ? slotZone[line] : nonVolatileSettings.currentZone);
}

// Remember where the active channel row is (zone changes and the rotary move it)
static void rememberActiveChannelRow(int line)
{
	if (zoneUsable())
	{
		slotZone[line] = nonVolatileSettings.currentZone;
		slotIndex[line] = codeplugGetLastUsedChannelInCurrentZone();
	}
}

// A signal is being received when the radio unmutes its speaker amplifier
static bool receiving(void)
{
	return ((audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF) != 0);
}

// ---- Row watch ---------------------------------------------------------------------------------------------
// With one receiver the other row can only be heard by tuning to it for a moment. Every WATCH_PEEK_INTERVAL_MS the radio
// looks at the other row for WATCH_PEEK_DWELL_MS: no carrier, and it goes straight back. With a carrier it waits for audio
// (it can be a carrier with the wrong tone) and then listens until the audio has been gone for the Scan delay, and returns.
// The active row never changes, so PTT always goes where the user selected. Analog rows only for now (a DMR row needs its
// colour code, slot and talkgroup set up to be heard): nothing is done while the active row is DMR, and DMR rows are skipped.
#define WATCH_PEEK_INTERVAL_MS   1500U
#define WATCH_QUIET_MS           3000U // no peek until the active row has been quiet this long (pauses in speech, squelch tail)
#define WATCH_PEEK_DWELL_MS      80U
#define WATCH_VERIFY_MS          500U
#define WATCH_BACKOFF_MS         10000U // a carrier that gave no audio: leave that row alone for a while

typedef enum
{
	WATCH_IDLE = 0,
	WATCH_PEEK,
	WATCH_VERIFY,
	WATCH_LISTEN
} watchState_t;

static watchState_t watchState = WATCH_IDLE;
static uint32_t watchTime;         // when the current state started
static uint32_t watchBackoffUntil;
static uint32_t watchLastBusy;     // last time the active row had audio or a carrier
static uint32_t savedRx;
static uint32_t savedTx;
static int savedMode;
static int savedDmrMode;
static bool savedBw25;
static uint16_t savedRxTone;
static bool lastListening;

static bool receivingNow(void)
{
	return ((audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF) != 0);
}

static void watchRestoreRadio(void)
{
	trxSetSquelchOverride(false, 0, 0);
	trxSetFrequency(savedRx, savedTx, savedDmrMode);
	trxSetModeAndBandwidth(savedMode, savedBw25);
	trxSetRxCSS(RADIO_DEVICE_PRIMARY, savedRxTone);
}

static void watchGoIdle(uint32_t now)
{
	trxSetSquelchOverride(false, 0, 0);
	watchState = WATCH_IDLE;
	watchTime = now;
}

// Channel data of the row that is not active, false when it can not be used
static bool otherRowData(CodeplugChannel_t *out)
{
	const int other = (1 - activeLine());
	CodeplugZone_t zone;

	if (lineIsChannel(other) == false)
	{
		memcpy(out, &settingsVFOChannel[other], sizeof(CodeplugChannel_t));
		return (out->rxFreq != 0U);
	}

	if ((codeplugZoneGetDataForNumber(zoneOfLine(other), &zone) == false) || (zone.NOT_IN_CODEPLUGDATA_numChannelsInZone <= 0))
	{
		return false;
	}

	int16_t chNumber = channelNumberForIndex(&zone, validIndexInZone(&zone, slotIndex[other]));

	if (chNumber < 1)
	{
		return false;
	}

	codeplugChannelGetDataForIndex(chNumber, out);
	return (out->rxFreq != 0U);
}

static bool watchAllowed(void)
{
	int menu = menuSystemGetCurrentMenuNumber();

	return (uiDualScreenIsEnabled() && ((opts() & DUALWATCH_WATCH) != 0U) &&
			((menu == UI_VFO_MODE) || (menu == UI_CHANNEL_MODE)) &&
			(uiDataGlobal.Scan.active == false) && (trxTransmissionEnabled == false) && (trxIsTransmitting == false) &&
			(uiDataGlobal.FreqEnter.index == 0) && (uiDataGlobal.displayChannelSettings == false) &&
			(trxGetMode() == RADIO_MODE_ANALOG) && rxPowerSavingIsRxOn() && (aprsBeaconingIsTransmitting() == false));
}

bool uiDualScreenWatchIsTunedAway(void)
{
	return (watchState != WATCH_IDLE);
}

bool uiDualScreenWatchIsPeeking(void)
{
	return (watchState == WATCH_PEEK);
}

void uiDualScreenWatchAbort(void)
{
	if (watchState != WATCH_IDLE)
	{
		watchRestoreRadio();
	}
	watchGoIdle(ticksGetMillis());
}

void uiDualScreenWatchTick(void)
{
	const uint32_t now = ticksGetMillis();

	if (watchAllowed() == false)
	{
		if (watchState != WATCH_IDLE)
		{
			uiDualScreenWatchAbort();
		}
		return;
	}

	switch (watchState)
	{
		case WATCH_IDLE:
			if (receivingNow())
			{
				watchLastBusy = now;
			}
			else if (((now - watchTime) >= WATCH_PEEK_INTERVAL_MS) && (trxCarrierDetected(RADIO_DEVICE_PRIMARY)))
			{
				watchLastBusy = now;
			}

			if (((now - watchTime) >= WATCH_PEEK_INTERVAL_MS) && ((now - watchLastBusy) >= WATCH_QUIET_MS) &&
					(((int32_t)(now - watchBackoffUntil)) >= 0))
			{
				CodeplugChannel_t other;

				watchTime = now;

				if (otherRowData(&other) && (other.chMode == RADIO_MODE_ANALOG))
				{
					// What the radio is doing now, to put it back
					savedRx = currentRadioDevice->currentRxFrequency;
					savedTx = currentRadioDevice->currentTxFrequency;
					savedMode = trxGetMode();
					savedBw25 = trxGetBandwidthIs25kHz();
					savedDmrMode = currentRadioDevice->trxDMRModeRx;
					savedRxTone = currentChannelData->rxTone;

					rxPowerSavingSetState(ECOPHASE_POWERSAVE_INACTIVE); // keep the receiver on while looking
					trxSetSquelchOverride(true, other.sql, other.rxTone); // squelch and tone of the other row, not the active one's
					trxSetFrequency(other.rxFreq, other.txFreq, DMR_MODE_AUTO);
					trxSetModeAndBandwidth(RADIO_MODE_ANALOG, (codeplugChannelGetFlag(&other, CHANNEL_FLAG_BW_25K) != 0));
					trxSetRxCSS(RADIO_DEVICE_PRIMARY, other.rxTone);
					watchState = WATCH_PEEK;
				}
			}
			break;

		case WATCH_PEEK:
			if ((now - watchTime) >= WATCH_PEEK_DWELL_MS)
			{
				if (trxCarrierDetected(RADIO_DEVICE_PRIMARY))
				{
					watchState = WATCH_VERIFY;
					watchTime = now;
				}
				else
				{
					watchRestoreRadio();
					watchGoIdle(now);
				}
			}
			break;

		case WATCH_VERIFY:
			if (receivingNow())
			{
				watchState = WATCH_LISTEN;
				watchTime = now;
			}
			else if ((now - watchTime) >= WATCH_VERIFY_MS)
			{
				watchRestoreRadio();
				watchBackoffUntil = (now + WATCH_BACKOFF_MS);
				watchGoIdle(now);
			}
			break;

		case WATCH_LISTEN:
			if (receivingNow())
			{
				watchTime = now; // still there
			}
			else
			{
				uint32_t hang = ((uint32_t)nonVolatileSettings.scanDelay * 1000U);

				if ((now - watchTime) >= ((hang > 500U) ? hang : 500U))
				{
					watchRestoreRadio();
					watchGoIdle(now);
				}
			}
			break;
	}
}

bool uiDualScreenRxStateChanged(void)
{
	static bool lastReceiving = false;
	bool now = (receiving() && (watchState == WATCH_IDLE)); // the watch's own audio is not a signal on the active row
	bool listening = (watchState == WATCH_LISTEN);
	bool changed = (((now != lastReceiving) || (listening != lastListening)) && uiDualScreenIsEnabled());

	lastReceiving = now;
	lastListening = listening;
	return changed;
}

bool uiDualScreenIsEnabled(void)
{
	return ((opts() & DUALWATCH_DUAL_SCREEN) != 0U);
}

bool uiDualScreenCanDraw(void)
{
	// An active channel row needs the channel screen's data
	if (lineIsChannel(activeLine()) && (channelScreenChannelData.rxFreq == 0U))
	{
		return false;
	}

	return (uiDualScreenIsEnabled() && (trxTransmissionEnabled == false) && (trxIsTransmitting == false) && (uiDataGlobal.displayChannelSettings == false));
}

// Fixed palettes (RGB888). Each row has small coloured badges on its first line (channel number or "VFO A", and DMR / ANA),
// then the name (or the frequency of a VFO) and the zone. The active row is drawn with vivid colours, the other one muted.
// Two palettes so that everything is readable on a light theme background (the default) as well as on a dark one.
typedef struct
{
	uint32_t name;
	uint32_t zone;
	uint32_t numBg, numFg;
	uint32_t dmrBg, dmrFg;
	uint32_t anaBg, anaFg;
	uint32_t vfoBg, vfoFg;
} palette_t;

static const palette_t paletteLightActive   = { 0x000000U, 0x0D47A1U, 0x455A64U, 0xFFFFFFU, 0x00802BU, 0xFFFFFFU, 0xD35400U, 0xFFFFFFU, 0x00838FU, 0xFFFFFFU };
static const palette_t paletteLightInactive = { 0x8E979FU, 0xA7B8C9U, 0xC3CCD2U, 0xFFFFFFU, 0xA6CDB6U, 0xFFFFFFU, 0xEBC9ABU, 0xFFFFFFU, 0xB2D3D8U, 0xFFFFFFU };
static const palette_t paletteDarkActive    = { 0xFFFFFFU, 0x6FC3FFU, 0x546E7AU, 0xFFFFFFU, 0x1FA85AU, 0xFFFFFFU, 0xE08A00U, 0x1A1000U, 0x1B97A6U, 0xFFFFFFU };
static const palette_t paletteDarkInactive  = { 0x7C8894U, 0x4F7391U, 0x2E3A42U, 0x8A98A4U, 0x1D4D35U, 0x8FBFA3U, 0x4D3A1CU, 0xB89C74U, 0x1E474DU, 0x8FB7BDU };

#define COL_RX_BG           0x1DD65CU // "RX" badge on the active row while a signal is received
#define COL_RX_FG           0x000000U
#define BADGE_HEIGHT        10
#define BADGE_GAP           3

#if defined(HAS_COLOURS)
// Print 'text' with colour 'fg' on the card colour 'card', 'x' is the left edge
static void printOnCard(int16_t x, int16_t y, const char *text, ucFont_t font, uint32_t fg, uint16_t card)
{
	displaySetForegroundAndBackgroundColours(displayConvertRGB888ToNative(fg), card);
	displayPrintCore(x, y, text, font, TEXT_ALIGN_LEFT, false);
}

// Centered on the full width, characters are 8 pixels wide in both fonts used here
static void printCentered(int16_t y, const char *text, ucFont_t font, uint32_t fg, uint16_t card)
{
	printOnCard((int16_t)((DISPLAY_SIZE_X - (int)(strlen(text) * 8U)) / 2), y, text, font, fg, card);
}

// Small badge: rounded coloured rectangle with the text in it, returns its width
static int16_t drawBadge(int16_t x, int16_t y, const char *text, uint32_t fg, uint32_t bg, uint16_t card)
{
	int16_t w = (int16_t)((strlen(text) * 8U) + 4U);
	uint16_t bgNative = displayConvertRGB888ToNative(bg);

	displaySetForegroundAndBackgroundColours(bgNative, card);
	displayFillRoundRect(x, y, w, BADGE_HEIGHT, 2, true);
	printOnCard((int16_t)(x + 2), (int16_t)(y + 1), text, FONT_SIZE_2, fg, bgNative);
	return w;
}
#endif

static void drawRow(int line, int16_t y, bool active)
{
	char name[SCREEN_LINE_BUFFER_SIZE];
	char l1[SCREEN_LINE_BUFFER_SIZE + 8];
	char number[8];
	char mode[8];
	char l3[SCREEN_LINE_BUFFER_SIZE + 8];
	CodeplugChannel_t tmp;
	CodeplugZone_t zoneTmp;
	CodeplugChannel_t *ch = NULL;
	bool digital = false;
	bool isChannelRow = lineIsChannel(line);

	l1[0] = 0;
	number[0] = 0;
	mode[0] = 0;
	l3[0] = 0;

	if (isChannelRow == false)
	{
		ch = &settingsVFOChannel[line];
		digital = (ch->chMode == RADIO_MODE_DIGITAL);
		snprintf(l1, sizeof(l1), "%d.%05d", (int)(ch->rxFreq / 100000), (int)(ch->rxFreq % 100000));
		snprintf(l3, sizeof(l3), "VFO %c", (line ? 'B' : 'A'));
	}
	else
	{
		const CodeplugZone_t *zone = NULL;
		int num = 0; // position in the zone, 1 based (the channel number itself in the All Channels zone)

		if (active)
		{
			if (zoneUsable())
			{
				ch = &channelScreenChannelData;
				zone = &currentZone;
				num = codeplugGetLastUsedChannelInCurrentZone();
			}
		}
		else if (codeplugZoneGetDataForNumber(zoneOfLine(line), &zoneTmp) && (zoneTmp.NOT_IN_CODEPLUGDATA_numChannelsInZone > 0))
		{
			int16_t idx = validIndexInZone(&zoneTmp, slotIndex[line]);
			int16_t chNumber = channelNumberForIndex(&zoneTmp, idx);

			if (chNumber >= 1) // 0 is an empty slot of the zone
			{
				codeplugChannelGetDataForIndex(chNumber, &tmp);
				ch = &tmp;
				zone = &zoneTmp;
				num = idx;
			}
		}

		if (zone != NULL)
		{
			num += (CODEPLUG_ZONE_IS_ALLCHANNELS(*zone) ? 0 : 1);
		}

		if ((ch != NULL) && (ch->rxFreq != 0U) && (zone != NULL))
		{
			digital = (ch->chMode == RADIO_MODE_DIGITAL);
			codeplugUtilConvertBufToString((char *)ch->name, name, 16);
			snprintf(l1, sizeof(l1), "%s", name);
			snprintf(number, sizeof(number), "C%03d", num);

			if (CODEPLUG_ZONE_IS_ALLCHANNELS(*zone))
			{
				snprintf(l3, sizeof(l3), "%s", currentLanguage->all_channels);
			}
			else
			{
				codeplugUtilConvertBufToString((char *)zone->name, name, 16);
				snprintf(l3, sizeof(l3), "%s", name);
			}
		}
		else
		{
			snprintf(l1, sizeof(l1), "%s", "--");
			snprintf(l3, sizeof(l3), "%s", "no channel");
			ch = NULL;
		}
	}

	if (ch != NULL)
	{
		snprintf(mode, sizeof(mode), "%s", (digital ? "DMR" : "ANA"));
	}
	else
	{
		snprintf(mode, sizeof(mode), "%s", "");
	}

#if defined(HAS_COLOURS)
	{
		uint16_t fgDummy;
		uint16_t card; // the theme background, everything is printed on it
		bool darkBackground;
		const palette_t *pal;
		int16_t x = (DISPLAY_X_POS_MENU_OFFSET + 2);

		displayThemeApply(THEME_ITEM_FG_DEFAULT, THEME_ITEM_BG);
		displayGetForegroundAndBackgroundColours(&fgDummy, &card);
		// Brightness from the green channel (6 bits, same position in RGB565 and BGR565), the colours are stored byte swapped
		darkBackground = ((((uint16_t)((card >> 8) | (card << 8)) >> 5) & 0x3FU) < 0x20U);

		if (active)
		{
			pal = (darkBackground ? &paletteDarkActive : &paletteLightActive);
		}
		else
		{
			pal = (darkBackground ? &paletteDarkInactive : &paletteLightInactive);
		}

		// First line, at the start of the row: channel number (or "VFO A") and the mode
		if (isChannelRow == false)
		{
			x += (drawBadge(x, y, l3, pal->vfoFg, pal->vfoBg, card) + BADGE_GAP);
		}
		else if (number[0] != 0)
		{
			x += (drawBadge(x, y, number, pal->numFg, pal->numBg, card) + BADGE_GAP);
		}

		if (mode[0] != 0)
		{
			drawBadge(x, y, mode, (digital ? pal->dmrFg : pal->anaFg), (digital ? pal->dmrBg : pal->anaBg), card);
		}

		// Signal received on the active row
		// the signal is on the active row, or on the other one while the row watch listens to it
		if (active ? (receiving() && (watchState == WATCH_IDLE)) : (watchState == WATCH_LISTEN))
		{
			drawBadge((int16_t)(DISPLAY_SIZE_X - (DISPLAY_X_POS_MENU_OFFSET + 2) - ((2 * 8) + 4)), y, "RX", COL_RX_FG, COL_RX_BG, card);
		}

		// Name (or the frequency of a VFO), then the zone
		printCentered((y + BADGE_HEIGHT + 3), l1, FONT_SIZE_3, pal->name, card);

		if (isChannelRow)
		{
			printCentered((y + BADGE_HEIGHT + 3 + FONT_SIZE_3_HEIGHT + 1), l3, FONT_SIZE_2, pal->zone, card);
		}

		displayThemeResetToDefault();
	}
#else
	{
		char l2[SCREEN_LINE_BUFFER_SIZE + 8];

		snprintf(l2, sizeof(l2), "%s%s%s", number, (number[0] ? " " : ""), mode);

		if (active)
		{
			displayThemeApply(THEME_ITEM_BG_MENU_ITEM_SELECTED, THEME_ITEM_BG);
			displayFillRoundRect(DISPLAY_X_POS_MENU_OFFSET, y, (DISPLAY_SIZE_X - (DISPLAY_X_POS_MENU_OFFSET * 2)), ((FONT_SIZE_3_HEIGHT * 2) + FONT_SIZE_2_HEIGHT + 6), 2, true);
		}

		displayThemeApply(THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_BG);
		displayPrintCore(0, (y + 2), l1, FONT_SIZE_3, TEXT_ALIGN_CENTER, active);
		displayPrintCore(0, (y + 2 + FONT_SIZE_3_HEIGHT), l2, FONT_SIZE_3, TEXT_ALIGN_CENTER, active);
		displayPrintCore(0, (y + 2 + (FONT_SIZE_3_HEIGHT * 2)), l3, FONT_SIZE_2, TEXT_ALIGN_CENTER, active);
		displayThemeResetToDefault();
	}
#endif
}

void uiDualScreenDraw(void)
{
	drawRow(0, 22, (activeLine() == 0));
	drawRow(1, 70, (activeLine() == 1));

	// Bottom left label of the green key (opens the menu). 0xFC is u-umlaut in the firmware font (Windows-1252)
	displayThemeApply(THEME_ITEM_FG_DEFAULT, THEME_ITEM_BG);
	displayPrintCore((DISPLAY_X_POS_MENU_OFFSET + 2), (DISPLAY_SIZE_Y - FONT_SIZE_2_HEIGHT - 1), "Men\xFC", FONT_SIZE_2, TEXT_ALIGN_LEFT, false);
	displayThemeResetToDefault();
}

// The row that is not active is the only one whose channel has to be saved (the active one is the radio's own
// zone / channel). It is packed in dualScreenSlot (16 bits) + one bit of dualWatchOptions: zone (7) | index (10).
static void persistPassiveRow(void)
{
	int p = 1 - activeLine();
	uint16_t o = (opts() & ~(DUALWATCH_SLOT_BIT16 | DUALWATCH_SLOT_VALID));

	if (lineIsChannel(p) && (slotZone[p] >= 0) && (slotZone[p] < 128) && (slotIndex[p] >= 0) && (slotIndex[p] < 1024))
	{
		uint32_t packed = (((uint32_t)slotZone[p] << 10) | (uint32_t)slotIndex[p]);
		int16_t low = (int16_t)(packed & 0xFFFFU);

		o |= (DUALWATCH_SLOT_VALID | (((packed >> 16) & 1U) ? DUALWATCH_SLOT_BIT16 : 0U));

		if (nonVolatileSettings.dualScreenSlot != low)
		{
			settingsSet(nonVolatileSettings.dualScreenSlot, low);
		}
	}

	setOpts(o);
}

// After a power cycle: get the non active channel row back
static void restorePassiveRow(void)
{
	int p = 1 - activeLine();

	if ((opts() & DUALWATCH_SLOT_VALID) && lineIsChannel(p) && (slotZone[p] < 0))
	{
		uint32_t packed = (((opts() & DUALWATCH_SLOT_BIT16) ? 0x10000UL : 0UL) | (uint16_t)nonVolatileSettings.dualScreenSlot);

		slotZone[p] = (int16_t)(packed >> 10);
		slotIndex[p] = (int16_t)(packed & 0x3FFU);
	}
}

void uiDualScreenScreenEntered(bool channelScreen)
{
	watchGoIdle(ticksGetMillis()); // entering a screen has just tuned the radio: nothing to put back

	if (uiDualScreenIsEnabled() == false)
	{
		return;
	}

	if (slotsRestored == false)
	{
		slotsRestored = true;
		restorePassiveRow();
	}

	// A channel row that has no zone yet gets the current one (so a later zone change only moves the active row)
	for (int l = 0; l < 2; l++)
	{
		if (lineIsChannel(l) && (slotZone[l] < 0) && zoneUsable())
		{
			slotZone[l] = nonVolatileSettings.currentZone;
			slotIndex[l] = codeplugGetLastUsedChannelInCurrentZone();
		}
	}

	// Whatever way the screen has been entered, the active row follows it
	if (channelScreen)
	{
		int a = activeLine();

		setLineType(a, true);
		rememberActiveChannelRow(a);
	}
	else
	{
		int v = nonVolatileSettings.currentVFONumber;

		setActiveLine(v);
		setLineType(v, false);
	}

	persistPassiveRow();
}

// Select what the radio has to show / tune to for a row, by running the matching screen
static void goToLine(int line)
{
	if (lineIsChannel(line))
	{
		int16_t zoneNum = zoneOfLine(line);

		if ((zoneNum < 0) || (zoneNum >= codeplugZonesGetCount()))
		{
			slotZone[line] = -1; // the zone has gone away, stay in the current one
			slotIndex[line] = -1;
			zoneNum = nonVolatileSettings.currentZone;
		}

		if ((zoneNum != nonVolatileSettings.currentZone) || (zoneUsable() == false))
		{
			if (zoneNum != nonVolatileSettings.currentZone)
			{
				// Same things as choosing the zone in the zone list
				settingsSet(nonVolatileSettings.overrideTG, 0);
				settingsSet(nonVolatileSettings.currentZone, zoneNum);
				settingsSet(nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_CHANNEL_MODE], 0);
			}
			uiChannelInitializeCurrentZone();
		}

		if (zoneUsable() == false)
		{
			// No channel to go to: stay a VFO
			setLineType(line, false);
			goToLine(line);
			return;
		}

		slotZone[line] = nonVolatileSettings.currentZone;
		codeplugSetLastUsedChannelInCurrentZone(validIndexInZone(&currentZone, slotIndex[line]));
		channelScreenChannelData.rxFreq = 0; // tells the channel screen its data is not valid and has to be reloaded
		menuSystemSetCurrentMenu(UI_CHANNEL_MODE);
	}
	else
	{
		settingsSet(nonVolatileSettings.currentVFONumber, (uint8_t)line);
		currentChannelData = &settingsVFOChannel[line];
		menuSystemSetCurrentMenu(UI_VFO_MODE);
	}
}

bool uiDualScreenHandleKey(uiEvent_t *ev, bool channelScreen, bool busy)
{
	if ((uiDualScreenIsEnabled() == false) || busy || uiDataGlobal.Scan.active || ((ev->events & KEY_EVENT) == 0) ||
			BUTTONCHECK_DOWN(ev, BUTTON_SK1) || BUTTONCHECK_DOWN(ev, BUTTON_SK2) || trxTransmissionEnabled || trxIsTransmitting)
	{
		return false;
	}

	if (KEYCHECK_SHORTUP(ev->keys, KEY_FRONT_UP) || KEYCHECK_SHORTUP(ev->keys, KEY_FRONT_DOWN))
	{
		// The other row
		int a = activeLine();

		if (channelScreen && lineIsChannel(a))
		{
			rememberActiveChannelRow(a); // where the rotary / the zone list left it
		}

		setActiveLine(1 - a);
		goToLine(1 - a);
		return true;
	}

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		// VFO <-> channel for the active row
		int a = activeLine();

		if (channelScreen && lineIsChannel(a))
		{
			rememberActiveChannelRow(a);
		}

		setLineType(a, (lineIsChannel(a) == false));
		goToLine(a);
		return true;
	}

	return false;
}

#endif // HAS_DUAL_WATCH_OPTIONS

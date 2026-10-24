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

#include <stdio.h>
#include <string.h>
#include "hardware/HX8353E.h"
#include "user_interface/uiGlobals.h"
#include "user_interface/uiTelemetry.h"
#include "user_interface/uiCustomStrings.h"

#if defined(HAS_TELEMETRY)

#define BAR_SEGMENTS             15 // S1 to S9, then S9+10 to S9+60
#define BAR_S9_SEGMENTS          9
#define BAR_SEGMENT_PITCH        10
#define BAR_SEGMENT_WIDTH        8
#define BAR_HEIGHT               4
#define BAR_X                    ((DISPLAY_SIZE_X - ((BAR_SEGMENTS * BAR_SEGMENT_PITCH) - (BAR_SEGMENT_PITCH - BAR_SEGMENT_WIDTH))) / 2)
#define BAR_Y_OFFSET             9
#define TEXT_X_MARGIN            2
#define CHAR_WIDTH               6
#define NOISE_RAW_NO_SIGNAL      64 // noise level read without any signal

#define HISTORY_SAMPLES          DISPLAY_SIZE_X
#define HISTORY_GAP_SAMPLES      2
#define HISTORY_RANGE_DB         (SMETER_S9_40 - SMETER_S0) // full height of the graph

// Colours of the signal strength history, from the noise (dark blue) to the strongest signals (red, then white): the
// palette of the fork's spectrum module, which this tree does not use (its sweep has its own waterfall).
#define SPECTRUM_LEVELS 16
static const uint32_t HISTORY_PALETTE_RGB888[SPECTRUM_LEVELS] =
{
		0x040A18U, 0x0A2A5EU, 0x0B3F8FU, 0x0A5FBFU, 0x0088D8U, 0x00B4E0U, 0x00D8C8U, 0x00E890U,
		0x40F050U, 0x90F020U, 0xD8F000U, 0xFFD800U, 0xFFA800U, 0xFF7000U, 0xFF3820U, 0xFFFFFFU
};

static uint16_t historyColour(uint8_t level)
{
	return displayConvertRGB888ToNative(HISTORY_PALETTE_RGB888[((level < SPECTRUM_LEVELS) ? level : (SPECTRUM_LEVELS - 1))]);
}
#define HISTORY_LOWEST_LEVEL     3 // the darker colours of the palette are not readable enough

// Signal strength above S0 (in dB, 0 when there is no sample yet), as a circular buffer.
// The RAM is almost full, this is living in the CCM, which is not initialised.
static __attribute__((section(".ccm_noinit"))) uint8_t history[HISTORY_SAMPLES];
static uint8_t historyNext = 0; // also the oldest sample
static bool historyIsInitialised = false;

// Lower limit, in dBm, of each segment of the bar graph
static const int16_t SEGMENT_LEVELS[BAR_SEGMENTS + 1] =
{
		SMETER_S0, SMETER_S1, SMETER_S2, SMETER_S3, SMETER_S4, SMETER_S5, SMETER_S6, SMETER_S7, SMETER_S8,
		SMETER_S9, SMETER_S9_10, SMETER_S9_20, SMETER_S9_30, SMETER_S9_40, SMETER_S9_50, SMETER_S9_60
};

// Note: displayFillRect() is drawing with the foreground colour when isInverted is false, the other shapes when it's true.
static void clearArea(int16_t y, int16_t height)
{
	displayThemeResetToDefault();
	displayFillRect(0, y, DISPLAY_SIZE_X, height, true);
}

static void printRight(int16_t xRight, int16_t y, const char *text, ucFont_t font)
{
	displayPrintCore((xRight - (strlen(text) * CHAR_WIDTH)), y, text, font, TEXT_ALIGN_LEFT, false);
}

void uiTelemetryBuildSMeterText(char *buffer, size_t bufferLen, int16_t dBm)
{
	if (dBm >= SMETER_S9_10)
	{
		int over = (((dBm - SMETER_S9) / 10) * 10);

		snprintf(buffer, bufferLen, "S9+%d", ((over > 60) ? 60 : over));
	}
	else
	{
		int unit = 0;

		while ((unit < BAR_S9_SEGMENTS) && (dBm >= SEGMENT_LEVELS[unit + 1]))
		{
			unit++;
		}

		snprintf(buffer, bufferLen, "S%d", unit);
	}
}

static void drawSMeterBar(int16_t y, int16_t dBm)
{
	for (int16_t segment = 0; segment < BAR_SEGMENTS; segment++)
	{
		int16_t x = BAR_X + (segment * BAR_SEGMENT_PITCH);
		int16_t low = SEGMENT_LEVELS[segment];
		int16_t high = SEGMENT_LEVELS[segment + 1];
		int16_t filled = 0;

		if (dBm >= high)
		{
			filled = BAR_SEGMENT_WIDTH;
		}
		else if (dBm > low)
		{
			filled = (((dBm - low) * BAR_SEGMENT_WIDTH) / (high - low));
		}

		// Scale: a baseline under each segment, taller on S9
		displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
		displayFillRect(x, (y + BAR_HEIGHT - 1), BAR_SEGMENT_WIDTH, 1, false);
		if (segment == (BAR_S9_SEGMENTS - 1))
		{
			displayFillRect((x + BAR_SEGMENT_WIDTH), y, 1, BAR_HEIGHT, false);
		}

		if (filled > 0)
		{
			displayThemeApply(((segment < BAR_S9_SEGMENTS) ? THEME_ITEM_FG_RSSI_BAR : THEME_ITEM_FG_RSSI_BAR_S9P), THEME_ITEM_BG);
			displayFillRect(x, y, filled, BAR_HEIGHT, false);
		}
	}
}

// The RF chip gives its noise level in its own unit (not in dBm): about NOISE_RAW_NO_SIGNAL without any signal, going down to 0 on a clean one.
uint8_t uiTelemetryNoiseToQuality(uint8_t noiseRaw)
{
	if (noiseRaw >= NOISE_RAW_NO_SIGNAL)
	{
		return 0;
	}

	return (uint8_t)(((NOISE_RAW_NO_SIGNAL - noiseRaw) * 100) / NOISE_RAW_NO_SIGNAL);
}

void uiTelemetryDrawRx(const uiTelemetryData_t *data, int16_t y, bool withDetails)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];

	clearArea(y, TELEMETRY_RX_HEIGHT);

	// S-meter value
	uiTelemetryBuildSMeterText(buffer, sizeof(buffer), data->rssidBm);
	displayThemeApply(((data->rssidBm >= SMETER_S9_10) ? THEME_ITEM_FG_RSSI_BAR_S9P : THEME_ITEM_FG_RSSI_BAR), THEME_ITEM_BG);
	displayPrintCore(TEXT_X_MARGIN, y, buffer, FONT_SIZE_1_BOLD, TEXT_ALIGN_LEFT, false);

	// Signal quality
	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
	displayPrintCore(58, y, "Q", FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
	snprintf(buffer, sizeof(buffer), "%u%%", uiTelemetryNoiseToQuality(data->noiseRaw));
	displayThemeApply(THEME_ITEM_FG_CSS_SQL_VALUES, THEME_ITEM_BG);
	displayPrintCore((58 + (2 * CHAR_WIDTH)), y, buffer, FONT_SIZE_1, TEXT_ALIGN_LEFT, false);

	// Signal strength
	snprintf(buffer, sizeof(buffer), "%ddBm", data->rssidBm);
	displayThemeApply(THEME_ITEM_FG_RSSI_BAR, THEME_ITEM_BG);
	printRight((DISPLAY_SIZE_X - TEXT_X_MARGIN), y, buffer, FONT_SIZE_1_BOLD);

	drawSMeterBar((y + BAR_Y_OFFSET), data->rssidBm);

	if (withDetails)
	{
		y += TELEMETRY_RX_DETAILS_Y_OFFSET;

		clearArea(y, TELEMETRY_LINE_HEIGHT);

		displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
		displayPrintCore(TEXT_X_MARGIN, y, uiCustomStringGet(CUSTOM_STRING_NOISE), FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
		snprintf(buffer, sizeof(buffer), "%u", data->noiseRaw);
		displayThemeApply(THEME_ITEM_FG_ZONE_NAME, THEME_ITEM_BG);
		displayPrintCore((TEXT_X_MARGIN + (2 * CHAR_WIDTH) - 3), y, buffer, FONT_SIZE_1, TEXT_ALIGN_LEFT, false);

		snprintf(buffer, sizeof(buffer), "%u.%uV", (data->batteryDeciVolts / 10), (data->batteryDeciVolts % 10));
		displayPrintCore(0, y, buffer, FONT_SIZE_1, TEXT_ALIGN_CENTER, false);

		snprintf(buffer, sizeof(buffer), "%d°" "C", (data->temperatureDeciCelsius / 10));
		printRight((DISPLAY_SIZE_X - TEXT_X_MARGIN), y, buffer, FONT_SIZE_1);
	}

	displayThemeResetToDefault();
}

void uiTelemetryDrawTx(const uiTelemetryData_t *data)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];
	const int16_t y = TELEMETRY_Y_POS_TX;

	clearArea(y, TELEMETRY_LINE_HEIGHT);

	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
	displayPrintCore(TEXT_X_MARGIN, y, uiCustomStringGet(CUSTOM_STRING_POWER), FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
	snprintf(buffer, sizeof(buffer), "%s%s", data->powerLevel, data->powerUnit);
	displayThemeApply(THEME_ITEM_FG_TX_COUNTER, THEME_ITEM_BG);
	displayPrintCore((TEXT_X_MARGIN + (4 * CHAR_WIDTH)), y, buffer, FONT_SIZE_1_BOLD, TEXT_ALIGN_LEFT, false);

	// The battery voltage under load and the temperature are the things to look at while transmitting
	displayThemeApply(THEME_ITEM_FG_ZONE_NAME, THEME_ITEM_BG);
	snprintf(buffer, sizeof(buffer), "%u.%uV", (data->batteryDeciVolts / 10), (data->batteryDeciVolts % 10));
	displayPrintCore(0, y, buffer, FONT_SIZE_1, TEXT_ALIGN_CENTER, false);

	snprintf(buffer, sizeof(buffer), "%d°" "C", (data->temperatureDeciCelsius / 10));
	printRight((DISPLAY_SIZE_X - TEXT_X_MARGIN), y, buffer, FONT_SIZE_1);

	displayThemeResetToDefault();
}

#define TOT_BAR_X                10
#define TOT_BAR_WIDTH            (DISPLAY_SIZE_X - (TOT_BAR_X * 2))
#define TOT_BAR_HEIGHT           4
#define TOT_WARNING_SECS         10

void uiTelemetryDrawTxDetails(uint32_t txFrequency, const char *modeText, int16_t totRemainingSecs, int16_t totTotalSecs)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];
	const int16_t y = TELEMETRY_Y_POS_TX_DETAILS;

	// Remaining time before the timeout
	if (totTotalSecs > 0)
	{
		int16_t remaining = ((totRemainingSecs < 0) ? 0 : ((totRemainingSecs > totTotalSecs) ? totTotalSecs : totRemainingSecs));
		int16_t width = (int16_t)(((int32_t)remaining * TOT_BAR_WIDTH) / totTotalSecs);

		clearArea(TELEMETRY_Y_POS_TX_TOT_BAR, TOT_BAR_HEIGHT);

		displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
		displayFillRect(TOT_BAR_X, (TELEMETRY_Y_POS_TX_TOT_BAR + TOT_BAR_HEIGHT - 1), TOT_BAR_WIDTH, 1, false);

		if (width > 0)
		{
			displayThemeApply(((remaining <= TOT_WARNING_SECS) ? THEME_ITEM_FG_TX_COUNTER : THEME_ITEM_FG_RSSI_BAR), THEME_ITEM_BG);
			displayFillRect(TOT_BAR_X, TELEMETRY_Y_POS_TX_TOT_BAR, width, TOT_BAR_HEIGHT, false);
		}
	}

	clearArea(y, TELEMETRY_LINE_HEIGHT);

	if (txFrequency != 0U)
	{
		snprintf(buffer, sizeof(buffer), "%u.%05u", (unsigned int)(txFrequency / 100000U), (unsigned int)(txFrequency % 100000U));
		displayThemeApply(THEME_ITEM_FG_TX_FREQ, THEME_ITEM_BG);
		displayPrintCore(TEXT_X_MARGIN, y, buffer, FONT_SIZE_1_BOLD, TEXT_ALIGN_LEFT, false);

		displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
		displayPrintCore((TEXT_X_MARGIN + (strlen(buffer) * CHAR_WIDTH) + 2), y, "MHz", FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
	}

	if ((modeText != NULL) && (modeText[0] != 0))
	{
		displayThemeApply(THEME_ITEM_FG_CSS_SQL_VALUES, THEME_ITEM_BG);

		if (txFrequency != 0U)
		{
			printRight((DISPLAY_SIZE_X - TEXT_X_MARGIN), y, modeText, FONT_SIZE_1);
		}
		else
		{
			displayPrintCore(0, y, modeText, FONT_SIZE_1, TEXT_ALIGN_CENTER, false);
		}
	}

	displayThemeResetToDefault();
}

void uiTelemetryDrawDmr(uint8_t timeSlot, uint8_t colourCode, uint32_t sourceId, bool matchesTxSettings)
{
	char buffer[28];
	const int16_t y = TELEMETRY_Y_POS_DMR;

	clearArea(y, TELEMETRY_LINE_HEIGHT);

	// TS and CC are the usual acronyms, in every language
	snprintf(buffer, sizeof(buffer), "TS%u CC%u", timeSlot, colourCode);
	displayThemeApply((matchesTxSettings ? THEME_ITEM_FG_CHANNEL_CONTACT_INFO : THEME_ITEM_FG_TX_COUNTER), THEME_ITEM_BG);
	displayPrintCore(TEXT_X_MARGIN, y, buffer, FONT_SIZE_1_BOLD, TEXT_ALIGN_LEFT, false);

	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
	snprintf(buffer, sizeof(buffer), "%u", (unsigned int)sourceId);
	printRight((DISPLAY_SIZE_X - TEXT_X_MARGIN - (strlen(buffer) * CHAR_WIDTH) - (CHAR_WIDTH / 2)), y, "ID", FONT_SIZE_1);
	displayThemeApply(THEME_ITEM_FG_CHANNEL_CONTACT_INFO, THEME_ITEM_BG);
	printRight((DISPLAY_SIZE_X - TEXT_X_MARGIN), y, buffer, FONT_SIZE_1);

	displayThemeResetToDefault();
}

void uiTelemetryHistoryClear(void)
{
	memset(history, 0x00, sizeof(history));
	historyNext = 0;
	historyIsInitialised = true;
}

void uiTelemetryHistoryAdd(int16_t dBm)
{
	int16_t value = (dBm - SMETER_S0);

	if (historyIsInitialised == false)
	{
		uiTelemetryHistoryClear();
	}

	// 0 means no sample
	history[historyNext] = (uint8_t)((value < 1) ? 1 : ((value > HISTORY_RANGE_DB) ? HISTORY_RANGE_DB : value));
	historyNext = ((historyNext + 1) % HISTORY_SAMPLES);
}

// Leaves an empty space in the graph: what follows is not the continuation of what is before
void uiTelemetryHistoryAddGap(void)
{
	if (historyIsInitialised == false)
	{
		uiTelemetryHistoryClear();
	}

	for (uint8_t i = 0; i < HISTORY_GAP_SAMPLES; i++)
	{
		history[historyNext] = 0;
		historyNext = ((historyNext + 1) % HISTORY_SAMPLES);
	}
}

// The newest sample is on the right
void uiTelemetryHistoryDraw(int16_t y, int16_t height)
{
	uint16_t fgColour, bgColour;

	if (historyIsInitialised == false)
	{
		uiTelemetryHistoryClear();
	}

	clearArea(y, height);

	// Baseline
	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
	displayFillRect(0, (y + height - 1), DISPLAY_SIZE_X, 1, false);
	displayGetForegroundAndBackgroundColours(&fgColour, &bgColour);

	for (int16_t x = 0; x < HISTORY_SAMPLES; x++)
	{
		uint8_t value = history[(historyNext + x) % HISTORY_SAMPLES];

		if (value != 0)
		{
			int16_t columnHeight = 1 + ((value * (height - 2)) / HISTORY_RANGE_DB);
			uint8_t level = ((value * (SPECTRUM_LEVELS - 2)) / HISTORY_RANGE_DB); // not the white, a full block of it is too bright

			displaySetForegroundAndBackgroundColours(historyColour((level < HISTORY_LOWEST_LEVEL) ? HISTORY_LOWEST_LEVEL : level), bgColour);
			displayFillRect(x, ((y + height - 1) - columnHeight), 1, columnHeight, false);
		}
	}

	displayThemeResetToDefault();
}

#endif // HAS_TELEMETRY

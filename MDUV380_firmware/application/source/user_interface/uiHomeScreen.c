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
#include "user_interface/uiHomeScreen.h"
#include "user_interface/uiCustomStrings.h"

#if defined(HAS_TELEMETRY)

#define CHAR_WIDTH               6  // FONT_SIZE_1
#define CHAR_WIDTH_MEDIUM        8  // FONT_SIZE_3
#define CHAR_WIDTH_BIG           16 // FONT_SIZE_4
#define X_MARGIN                 3
#define STATUS_TEXT_Y            3
#define BADGE_X                  2
#define BADGE_Y                  2
#define BADGE_HEIGHT             10
#define BADGE_PADDING            3
#define BATTERY_WIDTH            13
#define BATTERY_HEIGHT           7
#define BATTERY_X                (DISPLAY_SIZE_X - X_MARGIN - BATTERY_WIDTH - 2)
#define BATTERY_Y                3
#define BATTERY_LOW_PERCENTAGE   15
#define BATTERY_MEDIUM_PERCENTAGE 35
#define FREQUENCY_X              4
#if defined(PLATFORM_VARIANT_DM1701)
#define TOT_BAR_Y                89
#define TX_VALUES_Y              101
#else
#define TOT_BAR_Y                94
#define TX_VALUES_Y              106
#endif
#define TOT_BAR_HEIGHT           5
#define TOT_WARNING_SECS         10
// The callsign uses the biggest font, reduced to 3/4 (12x24 pixels): there is no font between 8x16 and 16x32
#define CALLSIGN_CHAR_WIDTH      ((CHAR_WIDTH_BIG * 3) / 4)
#define CALLSIGN_CHAR_HEIGHT     ((FONT_SIZE_4_HEIGHT * 3) / 4)
#define CALLSIGN_MAX_LEN         12 // the last character still needs the room of a full size one to be drawn
#define CALLSIGN_Y               (HOME_Y_POS_FREQUENCY + ((FONT_SIZE_4_HEIGHT - CALLSIGN_CHAR_HEIGHT) / 2))
#define INFO_MAX_LEN             HOME_INFO_MAX_LEN
#define STATUS_CLOCK_LEN          5
#define STATUS_FLAGS_MAX_LEN      6

// Note: displayFillRect() is drawing with the foreground colour when isInverted is false, the other shapes when it's true.
// The texts are only drawing the pixels of the characters: they can be printed over a filled area.

static void printRight(int16_t xRight, int16_t y, const char *text, ucFont_t font, int16_t charWidth)
{
	displayPrintCore((xRight - (strlen(text) * charWidth)), y, text, font, TEXT_ALIGN_LEFT, false);
}

static void drawSeparator(int16_t y)
{
	displayThemeApply(THEME_ITEM_BG_MENU_NAME, THEME_ITEM_BG);
	displayFillRect(X_MARGIN, y, (DISPLAY_SIZE_X - (X_MARGIN * 2)), 1, false);
}

static themeItem_t batteryColour(uint8_t percentage)
{
	return ((percentage <= BATTERY_LOW_PERCENTAGE) ? THEME_ITEM_FG_TX_COUNTER :
			((percentage <= BATTERY_MEDIUM_PERCENTAGE) ? THEME_ITEM_FG_CSS_SQL_VALUES : THEME_ITEM_FG_CHANNEL_CONTACT_INFO));
}

static void drawBattery(uint8_t percentage)
{
	int16_t fill = (((BATTERY_WIDTH - 4) * ((percentage > 100) ? 100 : percentage)) / 100);

	displayThemeApply(THEME_ITEM_FG_HEADER_TEXT, THEME_ITEM_BG);
	displayDrawRect(BATTERY_X, BATTERY_Y, BATTERY_WIDTH, BATTERY_HEIGHT, true);
	displayFillRect((BATTERY_X + BATTERY_WIDTH), (BATTERY_Y + 2), 2, (BATTERY_HEIGHT - 4), false); // positive terminal

	if (fill > 0)
	{
		displayThemeApply(batteryColour(percentage), THEME_ITEM_BG);
		displayFillRect((BATTERY_X + 2), (BATTERY_Y + 2), fill, (BATTERY_HEIGHT - 4), false);
	}
}

static void drawStatusBar(const uiHomeScreenData_t *data)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];
	const char *badge = (data->isTransmitting ? "TX" : (data->isScanning ? uiCustomStringGet(CUSTOM_STRING_SCAN) : (data->isDigital ? "DMR" : "FM")));
	int16_t badgeWidth = ((strlen(badge) * CHAR_WIDTH) + (BADGE_PADDING * 2));
	bool hasClock = ((data->hours >= 0) && (data->minutes >= 0));
	uint8_t batteryPercentage = ((data->batteryPercentage > 100) ? 100 : data->batteryPercentage);
	int16_t x, xRight;

	// Background of the bar
	displayThemeApply(THEME_ITEM_BG_MENU_NAME, THEME_ITEM_COLOUR_NONE);
	displayFillRect(0, 0, DISPLAY_SIZE_X, HOME_STATUS_BAR_HEIGHT, false);

	// Mode, as a badge: the text has the colour of the bar
	displayThemeApply((data->isTransmitting ? THEME_ITEM_FG_TX_COUNTER : (data->isDigital ? THEME_ITEM_FG_RSSI_BAR : THEME_ITEM_FG_CHANNEL_CONTACT_INFO)), THEME_ITEM_BG_MENU_NAME);
	displayFillRect(BADGE_X, BADGE_Y, badgeWidth, BADGE_HEIGHT, false);
	displayPrintCore((BADGE_X + BADGE_PADDING), STATUS_TEXT_Y, badge, FONT_SIZE_1, TEXT_ALIGN_LEFT, true);

	// Mode settings
	if (data->isDigital)
	{
		snprintf(buffer, sizeof(buffer), "TS%u CC%u", data->timeSlot, data->colourCode);
	}
	else
	{
		snprintf(buffer, sizeof(buffer), "%s", (data->bandwidthIs25kHz ? "25kHz" : "12.5kHz"));
	}

	x = (BADGE_X + badgeWidth + 5);
	displayThemeApply(((data->isReceivingCall && (data->callMatchesTxSettings == false)) ? THEME_ITEM_FG_TX_COUNTER : THEME_ITEM_FG_HEADER_TEXT), THEME_ITEM_BG);
	displayPrintCore(x, STATUS_TEXT_Y, buffer, FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
	displayThemeApply(THEME_ITEM_FG_HEADER_TEXT, THEME_ITEM_BG);

	x += ((8 * CHAR_WIDTH) + 4);

	// Battery, from the right: the percentage has the colour of the level. There is no room for the icon when the clock is displayed.
	xRight = (DISPLAY_SIZE_X - X_MARGIN);

	if (hasClock == false)
	{
		drawBattery(batteryPercentage);
		xRight = (BATTERY_X - 3);
	}

	snprintf(buffer, sizeof(buffer), "%u%%", batteryPercentage);
	displayThemeApply(batteryColour(batteryPercentage), THEME_ITEM_BG);
	printRight(xRight, STATUS_TEXT_Y, buffer, FONT_SIZE_1, CHAR_WIDTH);
	xRight -= ((strlen(buffer) * CHAR_WIDTH) + 4);

	// Power level
	snprintf(buffer, sizeof(buffer), "%s%s", data->telemetry.powerLevel, data->telemetry.powerUnit);
	displayThemeApply(THEME_ITEM_FG_HEADER_TEXT, THEME_ITEM_BG);
	printRight(xRight, STATUS_TEXT_Y, buffer, FONT_SIZE_1, CHAR_WIDTH);
	xRight -= ((strlen(buffer) * CHAR_WIDTH) + 3);

	// Clock, then the status letters, in the remaining room
	if (hasClock && ((x + (STATUS_CLOCK_LEN * CHAR_WIDTH)) <= xRight))
	{
		snprintf(buffer, sizeof(buffer), "%02d:%02d", data->hours, data->minutes);
		displayPrintCore(x, STATUS_TEXT_Y, buffer, FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
		x += ((STATUS_CLOCK_LEN * CHAR_WIDTH) + 3);
	}

	if ((data->statusFlags != NULL) && (data->statusFlags[0] != 0) && (xRight >= (x + CHAR_WIDTH)))
	{
		size_t maxLen = ((xRight - x) / CHAR_WIDTH);

		if (maxLen > STATUS_FLAGS_MAX_LEN)
		{
			maxLen = STATUS_FLAGS_MAX_LEN;
		}

		snprintf(buffer, (maxLen + 1), "%s", data->statusFlags);
		displayThemeApply(THEME_ITEM_FG_CSS_SQL_VALUES, THEME_ITEM_BG);
		displayPrintCore(x, STATUS_TEXT_Y, buffer, FONT_SIZE_1_BOLD, TEXT_ALIGN_LEFT, false);
		displayThemeApply(THEME_ITEM_FG_HEADER_TEXT, THEME_ITEM_BG);
	}
}

// 439.450 in big digits, the two last digits and the unit in small ones
static void drawFrequency(uint32_t frequency, themeItem_t colour)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];
	const int16_t y = HOME_Y_POS_FREQUENCY;
	const int16_t xSmall = (FREQUENCY_X + (7 * CHAR_WIDTH_BIG) + 3);

	displayThemeApply(colour, THEME_ITEM_BG);

	snprintf(buffer, sizeof(buffer), "%3u.%03u", (unsigned int)(frequency / 100000U), (unsigned int)((frequency % 100000U) / 100U));
	displayPrintCore(FREQUENCY_X, y, buffer, FONT_SIZE_4, TEXT_ALIGN_LEFT, false);

	snprintf(buffer, sizeof(buffer), "%02u", (unsigned int)(frequency % 100U));
	displayPrintCore(xSmall, (y + FONT_SIZE_4_HEIGHT - FONT_SIZE_3_HEIGHT - 2), buffer, FONT_SIZE_3, TEXT_ALIGN_LEFT, false);

	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
	displayPrintCore(xSmall, (y + 3), "MHz", FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
}

static void drawInfoLine(const uiHomeScreenData_t *data)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];
	const int16_t y = HOME_Y_POS_INFO;

	snprintf(buffer, (HOME_ZONE_MAX_LEN + 1), "%s", data->zoneText);
	displayThemeApply(THEME_ITEM_FG_ZONE_NAME, THEME_ITEM_BG);
	displayPrintCore(X_MARGIN, y, buffer, FONT_SIZE_1, TEXT_ALIGN_LEFT, false);

	// The other frequency of the channel
	if (data->txFrequency != data->rxFrequency)
	{
		uint32_t frequency = (data->isTransmitting ? data->rxFrequency : data->txFrequency);

		snprintf(buffer, sizeof(buffer), "%u.%03u", (unsigned int)(frequency / 100000U), (unsigned int)((frequency % 100000U) / 100U));
		displayThemeApply((data->isTransmitting ? THEME_ITEM_FG_RX_FREQ : THEME_ITEM_FG_TX_FREQ), THEME_ITEM_BG);
		printRight((DISPLAY_SIZE_X - X_MARGIN), y, buffer, FONT_SIZE_1, CHAR_WIDTH);

		displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
		printRight((DISPLAY_SIZE_X - X_MARGIN - (strlen(buffer) * CHAR_WIDTH) - 3), y, (data->isTransmitting ? "RX" : "TX"), FONT_SIZE_1, CHAR_WIDTH);
	}
}

static void drawContactLine(const uiHomeScreenData_t *data)
{
	const int16_t y = HOME_Y_POS_CONTACT;

	displayThemeApply(THEME_ITEM_FG_CHANNEL_CONTACT, THEME_ITEM_BG);
	displayPrintCore(X_MARGIN, y, data->contactText, FONT_SIZE_3, TEXT_ALIGN_LEFT, false);

	if (data->isTransmitting)
	{
		char buffer[SCREEN_LINE_BUFFER_SIZE];

		// Tx timer (it is the remaining time when the channel has a timeout)
		snprintf(buffer, sizeof(buffer), "%u:%02u", (data->txTimeSecs / 60U), (data->txTimeSecs % 60U));
		displayThemeApply(THEME_ITEM_FG_TX_COUNTER, THEME_ITEM_BG);
		printRight((DISPLAY_SIZE_X - X_MARGIN), y, buffer, FONT_SIZE_3, CHAR_WIDTH_MEDIUM);
	}
	else if ((data->sideText != NULL) && (data->sideText[0] != 0))
	{
		displayThemeApply(THEME_ITEM_FG_CSS_SQL_VALUES, THEME_ITEM_BG);
		printRight((DISPLAY_SIZE_X - X_MARGIN), (y + 4), data->sideText, FONT_SIZE_1, CHAR_WIDTH);
	}
}

static void drawTxValues(const uiHomeScreenData_t *data)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];
	const int16_t barWidth = (DISPLAY_SIZE_X - (X_MARGIN * 2));

	// Remaining time before the timeout
	if (data->totTotalSecs > 0)
	{
		uint16_t remaining = ((data->txTimeSecs > data->totTotalSecs) ? data->totTotalSecs : data->txTimeSecs);
		int16_t width = (int16_t)(((int32_t)remaining * barWidth) / data->totTotalSecs);

		displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
		displayFillRect(X_MARGIN, (TOT_BAR_Y + TOT_BAR_HEIGHT - 1), barWidth, 1, false);

		if (width > 0)
		{
			displayThemeApply(((remaining <= TOT_WARNING_SECS) ? THEME_ITEM_FG_TX_COUNTER : THEME_ITEM_FG_RSSI_BAR), THEME_ITEM_BG);
			displayFillRect(X_MARGIN, TOT_BAR_Y, width, TOT_BAR_HEIGHT, false);
		}
	}

	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
	displayPrintCore(X_MARGIN, TX_VALUES_Y, uiCustomStringGet(CUSTOM_STRING_POWER), FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
	snprintf(buffer, sizeof(buffer), "%s%s", data->telemetry.powerLevel, data->telemetry.powerUnit);
	displayThemeApply(THEME_ITEM_FG_TX_COUNTER, THEME_ITEM_BG);
	displayPrintCore((X_MARGIN + (4 * CHAR_WIDTH)), TX_VALUES_Y, buffer, FONT_SIZE_1_BOLD, TEXT_ALIGN_LEFT, false);

	displayThemeApply(THEME_ITEM_FG_ZONE_NAME, THEME_ITEM_BG);
	snprintf(buffer, sizeof(buffer), "%u.%uV", (data->telemetry.batteryDeciVolts / 10), (data->telemetry.batteryDeciVolts % 10));
	displayPrintCore(0, TX_VALUES_Y, buffer, FONT_SIZE_1, TEXT_ALIGN_CENTER, false);

	snprintf(buffer, sizeof(buffer), "%d\xB0" "C", (data->telemetry.temperatureDeciCelsius / 10));
	printRight((DISPLAY_SIZE_X - X_MARGIN), TX_VALUES_Y, buffer, FONT_SIZE_1, CHAR_WIDTH);
}

// Prints one character of the biggest font at 3/4 of its size.
// It is drawn at full size in the screen buffer, then reduced in place (3 pixels out of 4, the 4th one being merged with the
// previous one to keep the thin strokes), which needs a free area of 16x32 pixels at (x, y).
static void printReducedChar(int16_t x, int16_t y, char c)
{
	uint16_t fgColour, bgColour;
	uint16_t *origin = (displayGetScreenBuffer() + (y * DISPLAY_SIZE_X) + x);
	char text[2] = { c, 0 };

	if ((x < 0) || ((x + CHAR_WIDTH_BIG) > DISPLAY_SIZE_X) || (y < 0) || ((y + FONT_SIZE_4_HEIGHT) > DISPLAY_SIZE_Y))
	{
		return;
	}

	displayGetForegroundAndBackgroundColours(&fgColour, &bgColour);

	displayFillRect(x, y, CHAR_WIDTH_BIG, FONT_SIZE_4_HEIGHT, true);
	displayPrintCore(x, y, text, FONT_SIZE_4, TEXT_ALIGN_LEFT, false);

	// The source pixels are always after the destination ones: nothing is overwritten before being read
	for (int16_t dy = 0; dy < CALLSIGN_CHAR_HEIGHT; dy++)
	{
		int16_t sy = ((dy * 4) / 3);
		int16_t sy2 = (((dy % 3) == 2) ? (sy + 1) : sy);

		for (int16_t dx = 0; dx < CALLSIGN_CHAR_WIDTH; dx++)
		{
			int16_t sx = ((dx * 4) / 3);
			int16_t sx2 = (((dx % 3) == 2) ? (sx + 1) : sx);
			bool isSet = ((origin[(sy * DISPLAY_SIZE_X) + sx] == fgColour) || (origin[(sy * DISPLAY_SIZE_X) + sx2] == fgColour) ||
					(origin[(sy2 * DISPLAY_SIZE_X) + sx] == fgColour) || (origin[(sy2 * DISPLAY_SIZE_X) + sx2] == fgColour));

			origin[(dy * DISPLAY_SIZE_X) + dx] = (isSet ? fgColour : bgColour);
		}
	}

	// Remains of the full size character
	displayFillRect((x + CALLSIGN_CHAR_WIDTH), y, (CHAR_WIDTH_BIG - CALLSIGN_CHAR_WIDTH), FONT_SIZE_4_HEIGHT, true);
	displayFillRect(x, (y + CALLSIGN_CHAR_HEIGHT), CALLSIGN_CHAR_WIDTH, (FONT_SIZE_4_HEIGHT - CALLSIGN_CHAR_HEIGHT), true);
}

// Centered text, in the reduced big font. It has to be drawn before what is under it (see printReducedChar()).
static void printReducedCentered(int16_t y, const char *text)
{
	size_t len = strlen(text);
	int16_t x;

	if (len > CALLSIGN_MAX_LEN)
	{
		len = CALLSIGN_MAX_LEN;
	}

	x = ((DISPLAY_SIZE_X - (int16_t)(len * CALLSIGN_CHAR_WIDTH)) / 2);

	for (size_t i = 0; i < len; i++)
	{
		if (text[i] != ' ')
		{
			printReducedChar((x + (int16_t)(i * CALLSIGN_CHAR_WIDTH)), y, text[i]);
		}
	}
}

// The caller of the received DMR call, using the lines of the channel name, the frequency, the zone and the contact
static void drawCall(const uiHomeScreenData_t *data)
{
	char buffer[INFO_MAX_LEN + 1];
	// Talkgroup, highlighted when the PTT wouldn't reply to it
	if (data->callIsPrivate || (data->callMatchesTxSettings == false))
	{
		displayThemeApply((data->callIsPrivate ? THEME_ITEM_FG_CSS_SQL_VALUES : THEME_ITEM_FG_TX_COUNTER), THEME_ITEM_BG);
		displayFillRect(0, HOME_Y_POS_NAME, DISPLAY_SIZE_X, FONT_SIZE_3_HEIGHT, false);
		displayPrintCore(0, HOME_Y_POS_NAME, data->callTalkgroupText, FONT_SIZE_3, TEXT_ALIGN_CENTER, true);
	}
	else
	{
		displayThemeApply(THEME_ITEM_FG_CHANNEL_CONTACT, THEME_ITEM_BG);
		displayPrintCore(0, HOME_Y_POS_NAME, data->callTalkgroupText, FONT_SIZE_3, TEXT_ALIGN_CENTER, false);
	}

	// Callsign: the same size whatever its length is
	displayThemeApply(THEME_ITEM_FG_CHANNEL_CONTACT_INFO, THEME_ITEM_BG);
	printReducedCentered(CALLSIGN_Y, data->callerCallsign);

	if ((data->callerText != NULL) && (data->callerText[0] != 0))
	{
		snprintf(buffer, sizeof(buffer), "%s", data->callerText);
		displayPrintCore(0, HOME_Y_POS_INFO, buffer, FONT_SIZE_1, TEXT_ALIGN_CENTER, false);
	}

	// DMR ID, and the frequency which is not displayed anymore in the big font
	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
	displayPrintCore(X_MARGIN, (HOME_Y_POS_CONTACT + 4), "ID", FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
	snprintf(buffer, sizeof(buffer), "%u", (unsigned int)data->callerId);
	displayThemeApply(THEME_ITEM_FG_CHANNEL_CONTACT, THEME_ITEM_BG);
	displayPrintCore((X_MARGIN + (2 * CHAR_WIDTH) + 4), HOME_Y_POS_CONTACT, buffer, FONT_SIZE_3, TEXT_ALIGN_LEFT, false);

	snprintf(buffer, sizeof(buffer), "%u.%03u", (unsigned int)(data->rxFrequency / 100000U), (unsigned int)((data->rxFrequency % 100000U) / 100U));
	displayThemeApply(THEME_ITEM_FG_RX_FREQ, THEME_ITEM_BG);
	printRight((DISPLAY_SIZE_X - X_MARGIN), (HOME_Y_POS_CONTACT + 4), buffer, FONT_SIZE_1, CHAR_WIDTH);
}

void uiHomeScreenDrawStatusBar(const uiHomeScreenData_t *data)
{
	drawStatusBar(data);
	displayThemeResetToDefault();
}

void uiHomeScreenDraw(const uiHomeScreenData_t *data)
{
	displayThemeResetToDefault();
	displayClearBuf();

	drawStatusBar(data);

	if (data->isReceivingCall && (data->isTransmitting == false))
	{
		drawCall(data);
	}
	else
	{
		displayThemeApply(THEME_ITEM_FG_CHANNEL_NAME, THEME_ITEM_BG);
		displayPrintCore(0, HOME_Y_POS_NAME, data->channelName, FONT_SIZE_3, TEXT_ALIGN_CENTER, false);

		drawFrequency((data->isTransmitting ? data->txFrequency : data->rxFrequency), (data->isTransmitting ? THEME_ITEM_FG_TX_FREQ : THEME_ITEM_FG_RX_FREQ));
		drawInfoLine(data);
		drawContactLine(data);
	}

	drawSeparator(HOME_Y_POS_CONTACT - 2);
	drawSeparator(HOME_Y_POS_TELEMETRY - 2);

	if (data->isTransmitting)
	{
		drawTxValues(data);
	}
	else
	{
		uiTelemetryDrawRx(&data->telemetry, HOME_Y_POS_TELEMETRY, true);
		uiTelemetryHistoryDraw(HOME_Y_POS_HISTORY, HOME_HISTORY_HEIGHT);
	}

	displayThemeResetToDefault();
}

#endif // HAS_TELEMETRY

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
#include <math.h>
#include "hardware/HX8353E.h"
#include "user_interface/uiGlobals.h"
#include "user_interface/uiAnalogMeter.h"
#include "user_interface/uiTelemetry.h"
#include "user_interface/uiCustomStrings.h"

#if defined(HAS_ANALOG_METER)

// Geometry: the pivot of the needle is at the bottom of the screen, the angles are in degrees, 0 is the vertical
#define CENTRE_X                 (DISPLAY_SIZE_X / 2)
#define CENTRE_Y                 (DISPLAY_SIZE_Y - 8)
#define RADIUS                   78
#define ANGLE_S0                 -50.0f
#define ANGLE_S9                 10.0f   // S0 to S9 uses 60% of the scale
#define ANGLE_S9_60              50.0f
#define ARC_ANGLE_STEP           0.5f
#define HUB_RADIUS               6
#define TICK_LENGTH              5
#define TICK_LENGTH_MAJOR        9
#define LABEL_RADIUS             (RADIUS + 9)
#define NEEDLE_START_RADIUS      (HUB_RADIUS + 3)
#define NEEDLE_END_RADIUS        (RADIUS - 3)
#define PEAK_START_RADIUS        (RADIUS - 14)
#define PEAK_END_RADIUS          (RADIUS - 10)
#define CHAR_WIDTH               6
#define INFO_Y_OFFSET            3
#define VALUES_Y_POS             (DISPLAY_SIZE_Y - FONT_SIZE_3_HEIGHT - 3)
#define TEXT_X_MARGIN            3

// Note: displayFillRect() is drawing with the foreground colour when isInverted is false, the other shapes when it's true.

static float dBmX10ToAngle(int32_t dBmX10)
{
	if (dBmX10 <= (SMETER_S0 * 10))
	{
		return ANGLE_S0;
	}

	if (dBmX10 >= (SMETER_S9_60 * 10))
	{
		return ANGLE_S9_60;
	}

	if (dBmX10 <= (SMETER_S9 * 10))
	{
		return (ANGLE_S0 + (((float)(dBmX10 - (SMETER_S0 * 10)) * (ANGLE_S9 - ANGLE_S0)) / (float)((SMETER_S9 - SMETER_S0) * 10)));
	}

	return (ANGLE_S9 + (((float)(dBmX10 - (SMETER_S9 * 10)) * (ANGLE_S9_60 - ANGLE_S9)) / (float)((SMETER_S9_60 - SMETER_S9) * 10)));
}

static void polarToXY(float angle, int16_t radius, int16_t *x, int16_t *y)
{
	float radians = (angle * DEG_TO_RAD);

	*x = (CENTRE_X + (int16_t)lroundf(sinf(radians) * (float)radius));
	*y = (CENTRE_Y - (int16_t)lroundf(cosf(radians) * (float)radius));
}

static void drawRadialLine(float angle, int16_t radiusFrom, int16_t radiusTo, bool thick)
{
	int16_t x0, y0, x1, y1;

	polarToXY(angle, radiusFrom, &x0, &y0);
	polarToXY(angle, radiusTo, &x1, &y1);

	displayDrawLine(x0, y0, x1, y1, true);
	if (thick)
	{
		displayDrawLine((x0 + 1), y0, (x1 + 1), y1, true);
	}
}

static void drawArc(float angleFrom, float angleTo)
{
	int16_t x, y;

	for (float angle = angleFrom; angle <= angleTo; angle += ARC_ANGLE_STEP)
	{
		polarToXY(angle, RADIUS, &x, &y);
		displaySetPixel(x, y, true);
		displaySetPixel(x, (y + 1), true);
	}
}

static void drawLabel(float angle, const char *text)
{
	int16_t x, y;

	polarToXY(angle, LABEL_RADIUS, &x, &y);
	displayPrintCore((x - ((strlen(text) * CHAR_WIDTH) / 2)), (y - (FONT_SIZE_1_HEIGHT / 2)), text, FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
}

static void drawScale(void)
{
	static const int16_t S_LEVELS[9] = { SMETER_S1, SMETER_S2, SMETER_S3, SMETER_S4, SMETER_S5, SMETER_S6, SMETER_S7, SMETER_S8, SMETER_S9 };
	static const int16_t PLUS_LEVELS[6] = { SMETER_S9_10, SMETER_S9_20, SMETER_S9_30, SMETER_S9_40, SMETER_S9_50, SMETER_S9_60 };
	char buffer[4];

	// S1 to S9
	displayThemeApply(THEME_ITEM_FG_RSSI_BAR, THEME_ITEM_BG);
	drawArc(ANGLE_S0, ANGLE_S9);

	for (uint8_t i = 0; i < 9; i++)
	{
		float angle = dBmX10ToAngle(S_LEVELS[i] * 10);
		bool major = ((i % 2) == 0); // S1, S3, S5, S7 and S9

		drawRadialLine(angle, (RADIUS - (major ? TICK_LENGTH_MAJOR : TICK_LENGTH)), RADIUS, false);

		if (major)
		{
			snprintf(buffer, sizeof(buffer), "%d", (i + 1));
			drawLabel(angle, buffer);
		}
	}

	// S9+10 to S9+60
	displayThemeApply(THEME_ITEM_FG_RSSI_BAR_S9P, THEME_ITEM_BG);
	drawArc(ANGLE_S9, ANGLE_S9_60);

	for (uint8_t i = 0; i < 6; i++)
	{
		float angle = dBmX10ToAngle(PLUS_LEVELS[i] * 10);
		bool major = ((i % 2) == 1); // +20, +40 and +60

		drawRadialLine(angle, (RADIUS - (major ? TICK_LENGTH_MAJOR : TICK_LENGTH)), RADIUS, false);

		if (major)
		{
			snprintf(buffer, sizeof(buffer), "+%d", ((i + 1) * 10));
			drawLabel(angle, buffer);
		}
	}
}

static void buildSMeterText(char *buffer, size_t bufferLen, int16_t dBm)
{
	if (dBm >= SMETER_S9_10)
	{
		int over = (((dBm - SMETER_S9) / 10) * 10);

		snprintf(buffer, bufferLen, "S9+%d", ((over > 60) ? 60 : over));
	}
	else
	{
		int unit = ((dBm - SMETER_S0) / (SMETER_S1 - SMETER_S0));

		snprintf(buffer, bufferLen, "S%d", ((unit < 0) ? 0 : ((unit > 9) ? 9 : unit)));
	}
}

void uiAnalogMeterDraw(const uiAnalogMeterData_t *data)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];

	displayThemeResetToDefault();
	displayFillRect(0, ANALOG_METER_Y_POS, DISPLAY_SIZE_X, (DISPLAY_SIZE_Y - ANALOG_METER_Y_POS), true);

	// Information line
	if (data->infoText != NULL)
	{
		displayThemeApply(THEME_ITEM_FG_CSS_SQL_VALUES, THEME_ITEM_BG);
		displayPrintCore(0, (ANALOG_METER_Y_POS + INFO_Y_OFFSET), data->infoText, FONT_SIZE_1, TEXT_ALIGN_CENTER, false);
	}
	else
	{
		displayThemeApply(THEME_ITEM_FG_ZONE_NAME, THEME_ITEM_BG);
		snprintf(buffer, sizeof(buffer), "%s %u", uiCustomStringGet(CUSTOM_STRING_NOISE), data->noiseRaw);
		displayPrintCore(TEXT_X_MARGIN, (ANALOG_METER_Y_POS + INFO_Y_OFFSET), buffer, FONT_SIZE_1, TEXT_ALIGN_LEFT, false);

		snprintf(buffer, sizeof(buffer), "Q %u%%", uiTelemetryNoiseToQuality(data->noiseRaw));
		displayPrintCore((DISPLAY_SIZE_X - TEXT_X_MARGIN - (strlen(buffer) * CHAR_WIDTH)), (ANALOG_METER_Y_POS + INFO_Y_OFFSET), buffer, FONT_SIZE_1, TEXT_ALIGN_LEFT, false);
	}

	drawScale();

	// Peak marker
	displayThemeApply(THEME_ITEM_FG_CSS_SQL_VALUES, THEME_ITEM_BG);
	drawRadialLine(dBmX10ToAngle(data->peakdBmX10), PEAK_START_RADIUS, PEAK_END_RADIUS, true);

	// Values, in the bottom corners
	buildSMeterText(buffer, sizeof(buffer), data->rssidBm);
	displayThemeApply(((data->rssidBm >= SMETER_S9_10) ? THEME_ITEM_FG_RSSI_BAR_S9P : THEME_ITEM_FG_RSSI_BAR), THEME_ITEM_BG);
	displayPrintCore(TEXT_X_MARGIN, VALUES_Y_POS, buffer, FONT_SIZE_3, TEXT_ALIGN_LEFT, false);

	snprintf(buffer, sizeof(buffer), "%ddBm", data->rssidBm);
	displayThemeApply(THEME_ITEM_FG_CHANNEL_NAME, THEME_ITEM_BG);
	displayPrintCore((DISPLAY_SIZE_X - TEXT_X_MARGIN - (strlen(buffer) * 8)), VALUES_Y_POS, buffer, FONT_SIZE_3, TEXT_ALIGN_LEFT, false);

	// Needle and its hub
	displayThemeApply(THEME_ITEM_FG_CHANNEL_NAME, THEME_ITEM_BG);
	drawRadialLine(dBmX10ToAngle(data->needledBmX10), NEEDLE_START_RADIUS, NEEDLE_END_RADIUS, true);

	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
	displayFillCircle(CENTRE_X, CENTRE_Y, HUB_RADIUS, true);

	displayThemeResetToDefault();
}

#endif // HAS_ANALOG_METER

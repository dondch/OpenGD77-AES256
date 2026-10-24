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
#include "user_interface/uiBootAnimation.h"
#include "user_interface/uiCustomStrings.h"

#if defined(HAS_COLOURS)

#define COLOUR_BACKGROUND      0x03070EU
#define COLOUR_GRID            0x0C2A3AU
#define COLOUR_FRAME           0x1A7C96U
#define COLOUR_ACCENT          0x00E5FFU
#define COLOUR_ACCENT_DIM      0x0B6F86U
#define COLOUR_PEAK            0xFFFFFFU
#define COLOUR_CALLSIGN        0xF2FBFFU
#define COLOUR_CALLSIGN_GLOW   0x0A6F8CU
#define COLOUR_LABEL           0x5FA8BCU
#define COLOUR_READY           0x39FF7AU
#define COLOUR_WARM            0xFFB000U

#define FRAME_MARGIN           2
#define FRAME_CORNER_LENGTH    9

#if defined(PLATFORM_VARIANT_DM1701)
#define MODEL_NAME             "DM-1701"
#elif defined(PLATFORM_RT84_DM1701)
#define MODEL_NAME             "RT-84"
#else
#define MODEL_NAME             "MD-UV390"
#endif

#define SPECTRUM_X             8
#if defined(PLATFORM_VARIANT_DM1701) // 120 rows
#define SPECTRUM_Y             19
#define CALLSIGN_Y             52
#define SUBTITLE_Y             86
#define PROGRESS_Y             98
#define STATUS_Y               108
#else
#define SPECTRUM_Y             21
#define CALLSIGN_Y             56
#define SUBTITLE_Y             92
#define PROGRESS_Y             105
#define STATUS_Y               115
#endif
#define SPECTRUM_HEIGHT        30
#define SPECTRUM_BARS          36
#define SPECTRUM_BAR_PITCH     4
#define SPECTRUM_BAR_WIDTH     3

#define CALLSIGN_CHAR_WIDTH    16

#define PROGRESS_X             8
#define PROGRESS_WIDTH         (DISPLAY_SIZE_X - (PROGRESS_X * 2))
#define PROGRESS_HEIGHT        6

// Timeline (milliseconds since the splash screen has been displayed)
#define TIME_SPECTRUM_RISE     450U
#define TIME_CALLSIGN_START    250U
#define TIME_CALLSIGN_PER_CHAR 110U
#define TIME_PROGRESS_START    150U
#define TIME_PROGRESS_END      1700U
#define TIME_CURSOR_BLINK      300U
#define SHUTDOWN_SPECTRUM_TIME 5000U // the spectrum of the shutdown is the one of a running radio

// Quarter of a sine wave, 0..255
static const uint8_t SINE_QUARTER[17] = { 0, 25, 50, 74, 98, 120, 142, 162, 180, 197, 212, 225, 236, 244, 250, 254, 255 };

static uint16_t backgroundNative;

// Note: displayFillRect() is drawing with the foreground colour when isInverted is false, the other shapes when it's true.
static void setColour(uint32_t rgb888)
{
	displaySetForegroundAndBackgroundColours(displayConvertRGB888ToNative(rgb888), backgroundNative);
}

// phase: 0..63 is a full period, returns 0..255
static uint32_t wave(uint32_t phase)
{
	int32_t value;

	phase &= 0x3F;

	if (phase < 16)
	{
		value = SINE_QUARTER[phase];
	}
	else if (phase < 32)
	{
		value = SINE_QUARTER[32 - phase];
	}
	else if (phase < 48)
	{
		value = -SINE_QUARTER[phase - 32];
	}
	else
	{
		value = -SINE_QUARTER[64 - phase];
	}

	return (uint32_t)((value + 255) / 2);
}

static uint32_t hash(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7FEB352DU;
	x ^= x >> 15;
	x *= 0x846CA68BU;
	x ^= x >> 16;

	return x;
}

// Noise for a bar, linearly interpolated between two steps, returns 0..255
static uint32_t noise(uint32_t bar, uint32_t timeMs, uint32_t stepMs)
{
	uint32_t step = timeMs / stepMs;
	uint32_t fraction = ((timeMs % stepMs) * 256U) / stepMs;
	uint32_t a = hash((bar * 7919U) + step) & 0xFF;
	uint32_t b = hash((bar * 7919U) + step + 1U) & 0xFF;

	return ((a * (256U - fraction)) + (b * fraction)) >> 8;
}

// A fake carrier with a bell shape, returns 0..255
static uint32_t carrier(int32_t bar, int32_t centre, int32_t width)
{
	int32_t distance = bar - centre;

	if (distance < 0)
	{
		distance = -distance;
	}

	if (distance >= width)
	{
		return 0;
	}

	distance = ((width - distance) * 255) / width;

	return (uint32_t)((distance * distance) / 255);
}

static void drawFrame(void)
{
	const int16_t left = FRAME_MARGIN;
	const int16_t top = FRAME_MARGIN;
	const int16_t right = DISPLAY_SIZE_X - 1 - FRAME_MARGIN;
	const int16_t bottom = DISPLAY_SIZE_Y - 1 - FRAME_MARGIN;

	setColour(COLOUR_FRAME);

	displayDrawFastHLine(left, top, FRAME_CORNER_LENGTH, true);
	displayDrawFastVLine(left, top, FRAME_CORNER_LENGTH, true);
	displayDrawFastHLine(right - FRAME_CORNER_LENGTH + 1, top, FRAME_CORNER_LENGTH, true);
	displayDrawFastVLine(right, top, FRAME_CORNER_LENGTH, true);
	displayDrawFastHLine(left, bottom, FRAME_CORNER_LENGTH, true);
	displayDrawFastVLine(left, bottom - FRAME_CORNER_LENGTH + 1, FRAME_CORNER_LENGTH, true);
	displayDrawFastHLine(right - FRAME_CORNER_LENGTH + 1, bottom, FRAME_CORNER_LENGTH, true);
	displayDrawFastVLine(right, bottom - FRAME_CORNER_LENGTH + 1, FRAME_CORNER_LENGTH, true);

	setColour(COLOUR_LABEL);
	displayPrintAt(SPECTRUM_X, 6, MODEL_NAME, FONT_SIZE_1);
	displayPrintAt(DISPLAY_SIZE_X - SPECTRUM_X - (7 * 6), 6, "VHF/UHF", FONT_SIZE_1);

	setColour(COLOUR_GRID);
	displayDrawFastHLine(SPECTRUM_X, 16, DISPLAY_SIZE_X - (SPECTRUM_X * 2), true);
}

// rise: height of the bars, 0..256
static void drawSpectrum(uint32_t timeMs, uint32_t rise)
{
	const int16_t baseline = SPECTRUM_Y + SPECTRUM_HEIGHT;
	// The two main carriers are slowly drifting
	int32_t carrierA = 6 + (int32_t)((wave(timeMs / 90U) * 9U) / 255U);
	int32_t carrierB = 29 - (int32_t)((wave((timeMs / 140U) + 20U) * 8U) / 255U);

	// Graticule
	setColour(COLOUR_GRID);
	for (int16_t y = SPECTRUM_Y; y < baseline; y += 10)
	{
		for (int16_t x = SPECTRUM_X; x < (DISPLAY_SIZE_X - SPECTRUM_X); x += 4)
		{
			displaySetPixel(x, y, true);
		}
	}
	displayDrawFastHLine(SPECTRUM_X, baseline, (SPECTRUM_BARS * SPECTRUM_BAR_PITCH) - 1, true);

	for (int32_t bar = 0; bar < SPECTRUM_BARS; bar++)
	{
		uint32_t level = 20U + (noise(bar, timeMs, 120U) / 5U);

		level += (carrier(bar, carrierA, 6) * 170U) / 255U;
		level += (carrier(bar, carrierB, 4) * 120U) / 255U;
		level += (carrier(bar, 18, 3) * ((wave(timeMs / 25U) * 90U) / 255U)) / 255U; // pulsing one

		if (level > 255U)
		{
			level = 255U;
		}

		int16_t height = (int16_t)((((level * SPECTRUM_HEIGHT) / 255U) * rise) >> 8);

		if (height < 1)
		{
			height = 1;
		}

		int16_t x = SPECTRUM_X + (bar * SPECTRUM_BAR_PITCH);

		setColour((height > ((SPECTRUM_HEIGHT * 2) / 3)) ? COLOUR_ACCENT : COLOUR_ACCENT_DIM);
		displayFillRect(x, baseline - height, SPECTRUM_BAR_WIDTH, height, false);

		// Brighter cap on the top of each bar
		setColour((height > (SPECTRUM_HEIGHT / 2)) ? COLOUR_PEAK : COLOUR_ACCENT);
		displayDrawFastHLine(x, baseline - height, SPECTRUM_BAR_WIDTH, true);
	}
}

static char bootCallsign[BOOT_ANIMATION_CALLSIGN_MAX_LEN + 1] = BOOT_ANIMATION_DEFAULT_CALLSIGN;

void uiBootAnimationSetCallsign(const char *callsign)
{
	if ((callsign != NULL) && (callsign[0] != 0))
	{
		snprintf(bootCallsign, sizeof(bootCallsign), "%s", callsign);
	}
}

const char *uiBootAnimationGetCallsign(void)
{
	return bootCallsign;
}

static void drawCallsign(uint32_t timeMs, bool withCursor)
{
	char buffer[sizeof(bootCallsign)];
	const uint32_t length = strlen(bootCallsign);
	uint32_t visible = 0;
	int16_t x = (DISPLAY_SIZE_X - (int16_t)(length * CALLSIGN_CHAR_WIDTH)) / 2;

	if (timeMs > TIME_CALLSIGN_START)
	{
		visible = 1U + ((timeMs - TIME_CALLSIGN_START) / TIME_CALLSIGN_PER_CHAR);
		if (visible > length)
		{
			visible = length;
		}
	}

	memcpy(buffer, bootCallsign, visible);
	buffer[visible] = 0;

	// The glow is the same text, shifted
	setColour(COLOUR_CALLSIGN_GLOW);
	displayPrintAt(x + 1, CALLSIGN_Y + 1, buffer, FONT_SIZE_4);
	displayPrintAt(x - 1, CALLSIGN_Y + 1, buffer, FONT_SIZE_4);
	setColour(COLOUR_CALLSIGN);
	displayPrintAt(x, CALLSIGN_Y, buffer, FONT_SIZE_4);

	// Typing cursor, blinking once the whole callsign is displayed
	if (withCursor && ((visible < length) || (((timeMs / TIME_CURSOR_BLINK) & 0x01) == 0)))
	{
		setColour(COLOUR_ACCENT);
		displayFillRect(x + (int16_t)(visible * CALLSIGN_CHAR_WIDTH) + 2, CALLSIGN_Y + 26, 10, 3, false);
	}
}

static void drawProgress(uint32_t timeMs)
{
	uint32_t percent = 0;

	if (timeMs >= TIME_PROGRESS_END)
	{
		percent = 100;
	}
	else if (timeMs > TIME_PROGRESS_START)
	{
		percent = ((timeMs - TIME_PROGRESS_START) * 100U) / (TIME_PROGRESS_END - TIME_PROGRESS_START);
	}

	setColour(COLOUR_FRAME);
	displayDrawRect(PROGRESS_X, PROGRESS_Y, PROGRESS_WIDTH, PROGRESS_HEIGHT, true);

	// Segmented filling
	int16_t fill = (int16_t)(((PROGRESS_WIDTH - 4) * percent) / 100U);

	setColour((percent >= 100U) ? COLOUR_READY : COLOUR_ACCENT);
	for (int16_t x = 0; x < fill; x += 4)
	{
		int16_t width = ((fill - x) < 3) ? (fill - x) : 3;

		displayFillRect(PROGRESS_X + 2 + x, PROGRESS_Y + 2, width, PROGRESS_HEIGHT - 4, false);
	}

	if (percent >= 100U)
	{
		displayPrintAt(PROGRESS_X, STATUS_Y, uiCustomStringGet(CUSTOM_STRING_READY), FONT_SIZE_1);
		setColour(COLOUR_WARM);
		displayPrintAt(DISPLAY_SIZE_X - PROGRESS_X - (2 * 6), STATUS_Y, "73", FONT_SIZE_1);
	}
	else
	{
		char text[20];

		snprintf(text, sizeof(text), "%s %02u%%", uiCustomStringGet(CUSTOM_STRING_BOOT), (unsigned int)percent);

		setColour(COLOUR_LABEL);
		displayPrintAt(PROGRESS_X, STATUS_Y, text, FONT_SIZE_1);
	}
}

void uiBootAnimationDraw(const char *subtitle, uint32_t elapsedMilliseconds)
{
	backgroundNative = displayConvertRGB888ToNative(COLOUR_BACKGROUND);
	setColour(COLOUR_BACKGROUND);
	displayClearBuf();

	drawFrame();
	drawSpectrum(elapsedMilliseconds, ((elapsedMilliseconds >= TIME_SPECTRUM_RISE) ? 256U : ((elapsedMilliseconds * 256U) / TIME_SPECTRUM_RISE)));
	drawCallsign(elapsedMilliseconds, true);

	if ((subtitle != NULL) && (subtitle[0] != 0))
	{
		setColour(COLOUR_LABEL);
		displayPrintCore(0, SUBTITLE_Y, subtitle, FONT_SIZE_1, TEXT_ALIGN_CENTER, false);
	}

	drawProgress(elapsedMilliseconds);

	displayThemeResetToDefault();
}

// The reverse of the boot: the spectrum is fading out and the bar is emptying, with the "73" of the end of a contact
void uiBootAnimationDrawShutdown(uint32_t elapsedMilliseconds, uint32_t durationMilliseconds)
{
	uint32_t remaining = ((elapsedMilliseconds >= durationMilliseconds) ? 0U : (((durationMilliseconds - elapsedMilliseconds) * 256U) / durationMilliseconds)); // 256..0
	int16_t fill = (int16_t)(((PROGRESS_WIDTH - 4) * remaining) >> 8);

	backgroundNative = displayConvertRGB888ToNative(COLOUR_BACKGROUND);
	setColour(COLOUR_BACKGROUND);
	displayClearBuf();

	drawFrame();
	drawSpectrum((SHUTDOWN_SPECTRUM_TIME + elapsedMilliseconds), remaining);
	drawCallsign(SHUTDOWN_SPECTRUM_TIME, false);

	setColour(COLOUR_WARM);
	displayPrintCore(0, (SUBTITLE_Y - 3), "73", FONT_SIZE_3, TEXT_ALIGN_CENTER, false);

	setColour(COLOUR_FRAME);
	displayDrawRect(PROGRESS_X, PROGRESS_Y, PROGRESS_WIDTH, PROGRESS_HEIGHT, true);

	setColour(COLOUR_WARM);
	for (int16_t x = 0; x < fill; x += 4)
	{
		int16_t width = ((fill - x) < 3) ? (fill - x) : 3;

		displayFillRect(PROGRESS_X + 2 + x, PROGRESS_Y + 2, width, PROGRESS_HEIGHT - 4, false);
	}

	setColour(COLOUR_LABEL);
	displayPrintAt(PROGRESS_X, STATUS_Y, uiCustomStringGet(CUSTOM_STRING_SHUTDOWN), FONT_SIZE_1);

	displayThemeResetToDefault();
}

#endif // HAS_COLOURS

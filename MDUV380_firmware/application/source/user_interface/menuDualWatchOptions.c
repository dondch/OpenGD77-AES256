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
#include "user_interface/uiLocalisation.h"

#if defined(HAS_DUAL_WATCH_OPTIONS)

enum
{
	DW_OPT_DUAL_SCREEN = 0,
	DW_OPT_WATCH,
	DW_OPT_SIDE_B,
	DW_OPT_HOME,
	DW_OPT_SPEED,
	DW_OPT_ON_RX,
	NUM_DW_OPTIONS
};

static void updateScreen(bool isFirstRun);
static void handleEvent(uiEvent_t *ev);

static menuStatus_t menuDualWatchExitCode = MENU_STATUS_SUCCESS;

static const char *const homeNames[4] = { "Current", "VFO A", "VFO B", "Channel" };
static const char *const speedNames[DUALWATCH_NUM_SPEEDS] = { "Default", "90ms", "200ms", "400ms" };

static uint16_t options(void)
{
	return (uint16_t)nonVolatileSettings.dualWatchOptions;
}

static void setOptions(uint16_t value)
{
	settingsSet(nonVolatileSettings.dualWatchOptions, (int16_t)value);
}

menuStatus_t menuDualWatchOptions(uiEvent_t *ev, bool isFirstRun)
{
	if (isFirstRun)
	{
		menuDataGlobal.numItems = NUM_DW_OPTIONS;
		menuDataGlobal.currentItemIndex = 0;

		voicePromptsInit();
		voicePromptsAppendPrompt(PROMPT_SILENCE);
		voicePromptsAppendLanguageString(currentLanguage->dual_watch);
		voicePromptsAppendPrompt(PROMPT_SILENCE);
		promptsPlayNotAfterTx();

		updateScreen(true);
		return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
	}

	menuDualWatchExitCode = MENU_STATUS_SUCCESS;

	if (ev->hasEvent)
	{
		handleEvent(ev);
	}

	return menuDualWatchExitCode;
}

// Setting names (full words, a value follows after the colon), and what the selected setting does for the bottom row
static const char *const optionNames[NUM_DW_OPTIONS] = { "Dual screen", "Row watch", "2nd side", "Home", "Speed", "On signal" };

static const char *optionHelp(int option)
{
	switch (option)
	{
		case DW_OPT_DUAL_SCREEN:
			return "A/B rows on home";

		case DW_OPT_WATCH:
			return "Peek at other row";

		case DW_OPT_SIDE_B:
			return "VFO B or a channel";

		case DW_OPT_HOME:
			return "Side kept in Stay";

		case DW_OPT_SPEED:
			return "Time spent per side";

		default:
			return ((options() & DUALWATCH_STAY) ? "Beep, stay on home" : "Goes to a signal");
	}
}

static const char *optionValue(int option)
{
	const uint16_t o = options();

	switch (option)
	{
		case DW_OPT_DUAL_SCREEN:
			return ((o & DUALWATCH_DUAL_SCREEN) ? currentLanguage->on : currentLanguage->off);

		case DW_OPT_WATCH:
			return ((o & DUALWATCH_WATCH) ? currentLanguage->on : currentLanguage->off);

		case DW_OPT_SIDE_B:
			return ((o & DUALWATCH_CHANNEL_B) ? "Channel" : "VFO B");

		case DW_OPT_HOME:
			return homeNames[(o & DUALWATCH_HOME_MASK) >> DUALWATCH_HOME_SHIFT];

		case DW_OPT_SPEED:
			return speedNames[(o & DUALWATCH_SPEED_MASK) >> DUALWATCH_SPEED_SHIFT];

		default:
			return ((o & DUALWATCH_STAY) ? "Stay" : "Switch");
	}
}

// A plain list (no wrap around, so no empty row) with the value in the options colour, like the other options menus.
// The last row of the screen says what the selected setting does.
static void updateScreen(bool isFirstRun)
{
	char buf[SCREEN_LINE_BUFFER_SIZE];
	const int rows = (MENU_END_ITERATION_VALUE - MENU_START_ITERATION_VALUE + 1);
	const int visible = (rows - 1);
	int first = (menuDataGlobal.currentItemIndex - (visible - 1));

	(void)isFirstRun;

	if (first > (NUM_DW_OPTIONS - visible))
	{
		first = (NUM_DW_OPTIONS - visible);
	}
	if (first < 0)
	{
		first = 0;
	}

	displayClearBuf();
	menuDisplayTitle("DW Options");

	for (int r = 0; (r < visible) && ((first + r) < NUM_DW_OPTIONS); r++)
	{
		const int item = (first + r);

		snprintf(buf, sizeof(buf), "%s:%s", optionNames[item], optionValue(item));
		menuDisplayEntry((MENU_START_ITERATION_VALUE + r), item, buf, (strlen(optionNames[item]) + 1), THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}

	displayThemeApply(THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	displayPrintCore(0, (DISPLAY_Y_POS_MENU_ENTRY_HIGHLIGHT + ((MENU_START_ITERATION_VALUE + visible) * MENU_ENTRY_HEIGHT) + 4),
			optionHelp(menuDataGlobal.currentItemIndex), FONT_SIZE_2, TEXT_ALIGN_CENTER, false);
	displayThemeResetToDefault();

	displayRender();
}

// Step a value of 'count' states, wrapping around
static uint16_t cycle(uint16_t value, uint16_t count, bool up)
{
	return (up ? ((value + 1U) % count) : ((value + count - 1U) % count));
}

static void changeOption(bool up)
{
	uint16_t o = options();

	switch (menuDataGlobal.currentItemIndex)
	{
		case DW_OPT_DUAL_SCREEN:
			o ^= DUALWATCH_DUAL_SCREEN;
			break;

		case DW_OPT_WATCH:
			o ^= DUALWATCH_WATCH;
			break;

		case DW_OPT_SIDE_B:
			o ^= DUALWATCH_CHANNEL_B;
			break;

		case DW_OPT_HOME:
			o = ((o & ~DUALWATCH_HOME_MASK) | (cycle(((o & DUALWATCH_HOME_MASK) >> DUALWATCH_HOME_SHIFT), 4U, up) << DUALWATCH_HOME_SHIFT));
			break;

		case DW_OPT_SPEED:
			o = ((o & ~DUALWATCH_SPEED_MASK) | (cycle(((o & DUALWATCH_SPEED_MASK) >> DUALWATCH_SPEED_SHIFT), DUALWATCH_NUM_SPEEDS, up) << DUALWATCH_SPEED_SHIFT));
			break;

		default:
			o ^= DUALWATCH_STAY;
			break;
	}

	setOptions(o);
}

static void handleEvent(uiEvent_t *ev)
{
	if ((ev->events & FUNCTION_EVENT) && (ev->function == FUNC_REDRAW))
	{
		updateScreen(false);
		return;
	}

	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, NUM_DW_OPTIONS);
		updateScreen(false);
		menuDualWatchExitCode |= MENU_STATUS_LIST_TYPE;
	}
	else if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, NUM_DW_OPTIONS);
		updateScreen(false);
		menuDualWatchExitCode |= MENU_STATUS_LIST_TYPE;
	}
	else if (KEYCHECK_PRESS(ev->keys, KEY_RIGHT))
	{
		changeOption(true);
		updateScreen(false);
	}
	else if (KEYCHECK_PRESS(ev->keys, KEY_LEFT))
	{
		changeOption(false);
		updateScreen(false);
	}
	else if (KEYCHECK_SHORTUP(ev->keys, KEY_RED) || KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		menuSystemPopPreviousMenu();
	}
}

#endif // HAS_DUAL_WATCH_OPTIONS

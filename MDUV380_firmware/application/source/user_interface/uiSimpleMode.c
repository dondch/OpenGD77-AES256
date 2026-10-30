/*
 * uiSimpleMode.c — the screen side of the "dumb radio" mode. See uiSimpleMode.h.
 */
#include "user_interface/uiSimpleMode.h"

#if defined(ENABLE_SIMPLE_MODE)

#include "functions/settings.h"
#include "functions/codeplug.h"
#include "functions/ticks.h"
#include "functions/trx.h"
#include "functions/sound.h"
#include "user_interface/uiGlobals.h"
#include "user_interface/uiUtilities.h"
#include "user_interface/uiLocalisation.h"
#include "io/keyboard.h"
#include "io/buttons.h"
#include "hardware/HR-C6000.h"   /* PC_CALL_FLAG */
#include <stdio.h>
#include <string.h>

static simpleTimers_t s_timers;
static bool    s_unlocked;                 /* the PIN was entered; re-armed when the channel screen is back with nothing pending */
static int     s_pendingMenu = MENU_EMPTY; /* the menu the PIN was asked for */
static int32_t s_pin;
static bool    s_goHomePending;

bool simpleModeIsOn(void)
{
	return settingsIsOptionBitSet(BIT_SIMPLE_MODE);
}

void simpleModeSettingsSanitize(void)
{
	if (nonVolatileSettings.simpleValid != SIMPLE_SETTINGS_VALID)
	{
		nonVolatileSettings.simpleHomeZone = 0;
		nonVolatileSettings.simpleHomeChannel = 0;
		nonVolatileSettings.simpleIdleMinutes = 0;
		nonVolatileSettings.simpleValid = SIMPLE_SETTINGS_VALID;
	}
	if (nonVolatileSettings.simpleIdleMinutes > SIMPLE_IDLE_MINUTES_MAX)
	{
		nonVolatileSettings.simpleIdleMinutes = 0;
	}
}

/* The home zone number as it exists in this codeplug (the last "zone" of the count is the
 * virtual All Channels one, which is no home). */
static int homeZone(void)
{
	int realZones = codeplugZonesGetCount() - 1;
	int zone = nonVolatileSettings.simpleHomeZone;
	return ((realZones > 0) && (zone < realZones)) ? zone : 0;
}

void simpleModeGoHome(void)
{
	CodeplugZone_t zone;
	int z = homeZone();

	settingsSet(nonVolatileSettings.currentZone, (int16_t)z);
	codeplugZoneGetDataForNumber(z, &zone);
	if (!CODEPLUG_ZONE_IS_ALLCHANNELS(zone))
	{
		codeplugSetLastUsedChannelInZone(z, (int16_t)simpleHomePosition(zone.channels, zone.NOT_IN_CODEPLUGDATA_numChannelsInZone, nonVolatileSettings.simpleHomeChannel));
	}
	settingsSet(nonVolatileSettings.overrideTG, 0);
	menuPrivateCallClear();
	uiChannelModeColdStart();
	simpleTimersReset(&s_timers, ticksGetMillis());
}

static bool atHome(void)
{
	if (nonVolatileSettings.currentZone != homeZone())
	{
		return false;
	}
	if (nonVolatileSettings.simpleHomeChannel == 0)
	{
		return codeplugGetLastUsedChannelInCurrentZone() == 0;
	}
	return codeplugGetLastUsedChannelNumberInCurrentZone() == (int16_t)nonVolatileSettings.simpleHomeChannel;
}

/* ---- the PIN gate --------------------------------------------------------------------- */
/* Screens a fleet handheld may show without the PIN. */
static bool menuIsOpen(int m)
{
	switch (m)
	{
		case UI_CHANNEL_MODE: case UI_TX_SCREEN: case UI_LOCK_SCREEN: case UI_PRIVATE_CALL: case UI_MESSAGE_BOX:
		case UI_SPLASH_SCREEN: case UI_POWER_OFF: case UI_CPS: case UI_HOTSPOT_MODE:
		case MENU_CONTACT_QUICKLIST:
#if defined(ENABLE_DMR_DATA)
		case MENU_MESSAGES:
#endif
			return true;
		default:
			return false;
	}
}

static bool pinValidator(void)
{
	if (uiDataGlobal.MessageBox.keyPressed == KEY_GREEN)
	{
		if (freqEnterRead(0, uiDataGlobal.MessageBox.pinLength, false) == s_pin)
		{
			s_unlocked = true;
			return true;
		}
		soundSetMelody(MELODY_ERROR_BEEP);
		return false;   /* the box clears the digits and stays */
	}
	/* RED: give up */
	s_pendingMenu = MENU_EMPTY;
	freqEnterReset();
	return true;
}

int simpleModeMenuGate(int menuNumber)
{
	if (!simpleModeIsOn())
	{
		return menuNumber;
	}
	if (menuNumber == UI_VFO_MODE)
	{
		return MENU_EMPTY;
	}
	if (menuIsOpen(menuNumber) || s_unlocked)
	{
		return menuNumber;
	}
	int pinLength = codeplugGetPasswordPin(&s_pin);
	if (pinLength == 0)
	{
		return menuNumber;   /* no PIN provisioned: nothing to ask for */
	}
	s_pendingMenu = menuNumber;
	snprintf(uiDataGlobal.MessageBox.message, MESSAGEBOX_MESSAGE_LEN_MAX, "%s", currentLanguage->pin_code);
	uiDataGlobal.MessageBox.type = MESSAGEBOX_TYPE_PIN_CODE;
	uiDataGlobal.MessageBox.buttons = MESSAGEBOX_BUTTONS_NONE;
	uiDataGlobal.MessageBox.decoration = MESSAGEBOX_DECORATION_NONE;
	uiDataGlobal.MessageBox.pinLength = (uint8_t)pinLength;
	uiDataGlobal.MessageBox.validatorCallback = pinValidator;
	freqEnterReset();
	return UI_MESSAGE_BOX;
}

int simpleModeRootGate(int menuNumber)
{
	return (simpleModeIsOn() && (menuNumber == UI_VFO_MODE)) ? UI_CHANNEL_MODE : menuNumber;
}

/* ---- the channel screen's keys --------------------------------------------------------- */
simpleAction_t simpleModeRouteUiEvent(uiEvent_t *ev)
{
	if (!simpleModeIsOn())
	{
		return SM_PASS;
	}
	simpleKeyEvent_t k = { SM_KEY_NONE, 0, BUTTONCHECK_DOWN(ev, BUTTON_SK1) ? 1 : 0, BUTTONCHECK_DOWN(ev, BUTTON_SK2) ? 1 : 0 };

	if (ev->events & FUNCTION_EVENT)
	{
		return (ev->function == FUNC_REDRAW) ? SM_PASS : SM_BLOCK;   /* quick keys, scan on boot */
	}
	if (ev->events & BUTTON_EVENT)
	{
		if (BUTTONCHECK_SHORTUP(ev, BUTTON_SK1) || BUTTONCHECK_LONGDOWN(ev, BUTTON_SK1))
		{
			k.key = SM_KEY_SK1; k.isLong = BUTTONCHECK_LONGDOWN(ev, BUTTON_SK1) ? 1 : 0; k.sk1Held = 0;
			return simpleModeRouteKey(&k);
		}
		if (BUTTONCHECK_SHORTUP(ev, BUTTON_ORANGE) || BUTTONCHECK_LONGDOWN(ev, BUTTON_ORANGE))
		{
			k.key = SM_KEY_ORANGE;
			return simpleModeRouteKey(&k);
		}
		if (k.sk1Held && ((ev->events & KEY_EVENT) == 0))
		{
			return SM_BLOCK;   /* SK1 going down: not the channel-settings peek */
		}
	}
	if (ev->events & KEY_EVENT)
	{
		if (ev->keys.key == 0)
		{
			return SM_PASS;
		}
		if (KEYCHECK_SHORTUP(ev->keys, ev->keys.key))       { k.isLong = 0; }
		else if (KEYCHECK_LONGDOWN(ev->keys, ev->keys.key)) { k.isLong = 1; }
		else                                                { return SM_BLOCK; }   /* key down, repeats */
		switch (ev->keys.key)
		{
			case KEY_RED:              k.key = SM_KEY_RED;   break;
			case KEY_GREEN:            k.key = SM_KEY_GREEN; break;
			case KEY_ROTARY_INCREMENT: case KEY_FRONT_UP:   k.key = SM_KEY_UP;   break;
			case KEY_ROTARY_DECREMENT: case KEY_FRONT_DOWN: k.key = SM_KEY_DOWN; break;
			case KEY_LEFT:             k.key = SM_KEY_LEFT;  break;
			case KEY_RIGHT:            k.key = SM_KEY_RIGHT; break;
			case KEY_STAR:             k.key = SM_KEY_STAR;  break;
			case KEY_HASH:             k.key = SM_KEY_HASH;  break;
			default:                   k.key = ((ev->keys.key >= '0') && (ev->keys.key <= '9')) ? SM_KEY_DIGIT : SM_KEY_OTHER; break;
		}
		return simpleModeRouteKey(&k);
	}
	return SM_PASS;
}

/* ---- the main loop ---------------------------------------------------------------------- */
/* Screens the idle return and the private-call expiry leave alone. */
static bool screenIsBusy(int m)
{
	switch (m)
	{
		case UI_TX_SCREEN: case UI_LOCK_SCREEN: case UI_MESSAGE_BOX: case UI_PRIVATE_CALL:
		case UI_SPLASH_SCREEN: case UI_POWER_OFF: case UI_CPS: case UI_HOTSPOT_MODE:
			return true;
		default:
			return false;
	}
}

static void showChannelScreen(void)
{
	menuSystemPopAllAndDisplaySpecificRootMenu(UI_CHANNEL_MODE, true);
}

void simpleModeSettingsChanged(void)
{
	s_goHomePending = true;
}

void simpleModeTick(bool activity)
{
	if (!simpleModeIsOn())
	{
		s_unlocked = false; s_pendingMenu = MENU_EMPTY; s_goHomePending = false;
		return;
	}
	uint32_t now = ticksGetMillis();
	int currentMenu = menuSystemGetCurrentMenuNumber();

	if ((s_pendingMenu != MENU_EMPTY) && s_unlocked && (currentMenu != UI_MESSAGE_BOX))
	{
		int m = s_pendingMenu;
		s_pendingMenu = MENU_EMPTY;
		menuSystemPushNewMenu(m);
		return;
	}
	if ((currentMenu == UI_CHANNEL_MODE) && (s_pendingMenu == MENU_EMPTY))
	{
		s_unlocked = false;   /* back on the channel screen: the next menu asks again */
	}

	bool busy = trxIsTransmitting || ((audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF) != 0);
	bool pcActive = (nonVolatileSettings.overrideTG != 0) && ((nonVolatileSettings.overrideTG >> 24) == PC_CALL_FLAG);
	bool home = atHome() && (nonVolatileSettings.overrideTG == 0);

	if (s_goHomePending && !screenIsBusy(currentMenu) && !busy)
	{
		s_goHomePending = false;
		simpleModeGoHome();
		showChannelScreen();
		return;
	}
	if (simplePrivateCallExpired(&s_timers, now, pcActive, activity, busy) && !screenIsBusy(currentMenu))
	{
		/* the call is over: back to the group the private call replaced (SK2 + RED does the same by hand) */
		settingsSet(nonVolatileSettings.overrideTG, uiDataGlobal.tgBeforePcMode);
		menuPrivateCallClear();
		if (currentMenu == UI_CHANNEL_MODE)
		{
			showChannelScreen();   /* reloads trxTalkGroupOrPcId from the override and redraws */
		}
		return;
	}
	if (simpleIdleReturnDue(&s_timers, now, activity, busy, home, nonVolatileSettings.simpleIdleMinutes) && !screenIsBusy(currentMenu))
	{
		simpleModeGoHome();
		showChannelScreen();
	}
}

#endif /* ENABLE_SIMPLE_MODE */

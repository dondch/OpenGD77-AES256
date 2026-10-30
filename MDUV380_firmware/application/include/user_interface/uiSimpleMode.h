/*
 * uiSimpleMode.h — the screen side of the "dumb radio" mode (ENABLE_SIMPLE_MODE).
 *
 * Decisions come from functions/simpleMode.h; this is what acts on them: the home channel,
 * the PIN gate in front of every menu that could change the radio, the key routing of the
 * channel screen, the idle return and the end of a private call. Everything is a no-op while
 * the settings bit BIT_SIMPLE_MODE is clear, which is how the master radios run.
 */
#ifndef _OPENGD77_UI_SIMPLE_MODE_H_
#define _OPENGD77_UI_SIMPLE_MODE_H_

#include <stdbool.h>
#include "user_interface/menuSystem.h"
#include "functions/simpleMode.h"

#if defined(ENABLE_SIMPLE_MODE)

#define SIMPLE_SETTINGS_VALID  0x5A   /* nonVolatileSettings.simpleValid when the simple-mode fields have been written */

bool simpleModeIsOn(void);
/* Select the home zone and channel, drop any talkgroup or private-call override, and make the
 * channel screen reload on its next first run. */
void simpleModeGoHome(void);
/* The menu number to push instead of menuNumber: the same, UI_MESSAGE_BOX asking for the PIN
 * (menuNumber is pushed by simpleModeTick once the PIN is right), or MENU_EMPTY for a screen
 * that does not exist in simple mode (the VFO). */
int  simpleModeMenuGate(int menuNumber);
/* A root screen that would be the VFO becomes the channel screen. */
int  simpleModeRootGate(int menuNumber);
/* The channel screen's keys: what the event means in simple mode (SM_PASS = not ours). */
simpleAction_t simpleModeRouteUiEvent(uiEvent_t *ev);
/* Main loop, once per iteration: the pending menu after a PIN, the idle return, the end of a
 * private call, a home change pushed over the air. activity = a key or button this tick. */
void simpleModeTick(bool activity);
/* Remote provisioning rewrote the settings, a channel or a zone: go to the (possibly new) home
 * channel on the next tick, which also reloads the channel screen's copy of the record. */
void simpleModeSettingsChanged(void);
/* Keep the three simple-mode settings fields sane (a radio upgraded from a build without the
 * flag reads whatever its EEPROM held there). Called from settingsLoadSettings. */
void simpleModeSettingsSanitize(void);

#endif /* ENABLE_SIMPLE_MODE */
#endif /* _OPENGD77_UI_SIMPLE_MODE_H_ */

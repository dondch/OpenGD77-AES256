/*
 * simpleMode.h — the "dumb radio" rules (ENABLE_SIMPLE_MODE), the host-testable part.
 *
 * A fleet handheld boots on a home channel, steps through the home zone only, makes private
 * calls from a short contact list, sends preset messages, and goes back to the home channel
 * after N idle minutes; everything else is behind the PIN. This header holds the decisions
 * that need nothing from the radio (key routing, the idle-return and private-call hang timers,
 * the home channel lookup) so tests/test_simple_mode.c can run them with plain gcc. The screen
 * glue that acts on them is user_interface/uiSimpleMode.c and the hooks in the channel screen,
 * the menu system and the main loop. A build with the flag and the settings bit clear (the
 * master radios) runs none of it.
 */
#ifndef _OPENGD77_SIMPLE_MODE_H_
#define _OPENGD77_SIMPLE_MODE_H_

#include <stdint.h>

#define SIMPLE_PC_HANG_MS        60000u  /* a private-call target drops back to the group after this long without TX or RX */
#define SIMPLE_IDLE_MINUTES_MAX  240     /* simpleIdleMinutes above this reads as "never" (an unwritten EEPROM byte is 255) */

/* The keys the channel screen distinguishes in simple mode. UP and DOWN are the channel knob
 * or the front arrows; DIGIT is 0..9; SK1 and ORANGE are the side buttons. */
typedef enum
{
	SM_KEY_NONE = 0, SM_KEY_RED, SM_KEY_GREEN, SM_KEY_UP, SM_KEY_DOWN, SM_KEY_LEFT, SM_KEY_RIGHT,
	SM_KEY_STAR, SM_KEY_HASH, SM_KEY_DIGIT, SM_KEY_SK1, SM_KEY_ORANGE, SM_KEY_OTHER
} simpleKey_t;

typedef struct
{
	simpleKey_t key;
	uint8_t     isLong;    /* long press (the key is still down) rather than a short release */
	uint8_t     sk1Held;
	uint8_t     sk2Held;
} simpleKeyEvent_t;

typedef enum
{
	SM_PASS = 0,              /* not simple mode's business: the stock handler runs */
	SM_BLOCK,                 /* swallow it */
	SM_CHANNEL_UP,            /* next channel in the home zone */
	SM_CHANNEL_DOWN,
	SM_CONTACTS,              /* the private-call contact list */
	SM_PIN_MENU,              /* the main menu, behind the PIN */
	SM_PRESETS,               /* the message preset list */
	SM_INBOX,                 /* received messages */
	SM_CANCEL_PRIVATE_CALL    /* back to the channel's group right away */
} simpleAction_t;

simpleAction_t simpleModeRouteKey(const simpleKeyEvent_t *ev);

/* The two timers the main loop drives once per tick. */
typedef struct
{
	uint32_t lastActivityMs;   /* idle return: last key, PTT, TX or RX, or the last tick spent on the home channel */
	uint32_t pcQuietSinceMs;   /* private-call hang: last activity while a private-call target was selected */
} simpleTimers_t;

void simpleTimersReset(simpleTimers_t *t, uint32_t nowMs);
/* 1 when the radio must return to the home channel now: idleMinutes (1..240) without activity
 * while away from it. activity = a key or button this tick; busy = transmitting or receiving
 * audio; atHome = already on the home zone and channel. 0 minutes = never. */
int  simpleIdleReturnDue(simpleTimers_t *t, uint32_t nowMs, int activity, int busy, int atHome, uint32_t idleMinutes);
/* 1 when a selected private-call target (pcActive) has seen no activity or audio for
 * SIMPLE_PC_HANG_MS: the call is over, the target returns to the channel's group. */
int  simplePrivateCallExpired(simpleTimers_t *t, uint32_t nowMs, int pcActive, int activity, int busy);
/* Position of codeplug channel homeChannel in a zone's channel list; 0 when it is not there
 * (or homeChannel is 0): the first channel of the zone is the home then. */
int  simpleHomePosition(const uint16_t *zoneChannels, int numInZone, uint16_t homeChannel);

#endif /* _OPENGD77_SIMPLE_MODE_H_ */

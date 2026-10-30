/* Host test of the simple-mode decisions (functions/simpleMode.c): the key routing table of the
 * channel screen, the idle-return timer, the private-call hang timer and the home channel
 * lookup. The screen glue (uiSimpleMode.c) is bench work; this is what it decides on.
 *
 * Build: gcc -O2 -Wall -DENABLE_SIMPLE_MODE -Iapplication/include -o /tmp/test_simple_mode \
 *          tests/test_simple_mode.c application/source/functions/simpleMode.c && /tmp/test_simple_mode
 */
#include <stdio.h>
#include <string.h>
#include "functions/simpleMode.h"

static int fails = 0;
static void check(int ok, const char *what)
{
	printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) { fails++; }
}

static simpleAction_t route(simpleKey_t key, int isLong, int sk1, int sk2)
{
	simpleKeyEvent_t ev = { key, (uint8_t)isLong, (uint8_t)sk1, (uint8_t)sk2 };
	return simpleModeRouteKey(&ev);
}

static void testKeyRouting(void)
{
	check(route(SM_KEY_NONE, 0, 0, 0) == SM_PASS, "no key: the stock handler runs");
	check(route(SM_KEY_GREEN, 0, 0, 0) == SM_CONTACTS, "GREEN opens the contact list");
	check(route(SM_KEY_GREEN, 1, 0, 0) == SM_PIN_MENU, "GREEN held opens the menu behind the PIN");
	check(route(SM_KEY_GREEN, 0, 0, 1) == SM_BLOCK, "SK2 + GREEN (channel details) is blocked");
	check(route(SM_KEY_SK1, 0, 0, 0) == SM_INBOX, "SK1 opens the inbox");
	check(route(SM_KEY_SK1, 1, 0, 0) == SM_PRESETS, "SK1 held opens the send list");
	check(route(SM_KEY_SK1, 0, 0, 1) == SM_BLOCK, "SK2 + SK1 is blocked");
	check(route(SM_KEY_RED, 0, 0, 0) == SM_CANCEL_PRIVATE_CALL, "RED never enters the VFO: it only ends a private-call target");
	check(route(SM_KEY_RED, 1, 0, 0) == SM_BLOCK && route(SM_KEY_RED, 0, 1, 0) == SM_BLOCK && route(SM_KEY_RED, 0, 0, 1) == SM_BLOCK,
	      "RED held, SK1 + RED (theme) and SK2 + RED are blocked");
	check(route(SM_KEY_UP, 0, 0, 0) == SM_CHANNEL_UP && route(SM_KEY_DOWN, 0, 0, 0) == SM_CHANNEL_DOWN, "knob / arrows step the channel");
	check(route(SM_KEY_UP, 0, 0, 1) == SM_BLOCK && route(SM_KEY_DOWN, 0, 0, 1) == SM_BLOCK, "SK2 + knob (zone change) is blocked");
	check(route(SM_KEY_UP, 1, 0, 0) == SM_BLOCK, "UP held (scan start) is blocked");
	check(route(SM_KEY_RIGHT, 0, 0, 0) == SM_CHANNEL_UP && route(SM_KEY_LEFT, 0, 0, 0) == SM_CHANNEL_DOWN, "the keypad arrows (LEFT / RIGHT on this platform) step the channel too");
	check(route(SM_KEY_LEFT, 0, 0, 1) == SM_BLOCK && route(SM_KEY_RIGHT, 0, 0, 1) == SM_BLOCK && route(SM_KEY_RIGHT, 1, 0, 0) == SM_BLOCK, "SK2 + arrows (power) and a held arrow are blocked");
	check(route(SM_KEY_STAR, 0, 0, 0) == SM_BLOCK && route(SM_KEY_STAR, 0, 0, 1) == SM_BLOCK && route(SM_KEY_STAR, 1, 0, 0) == SM_BLOCK,
	      "STAR (timeslot, mode, timeslot reset) is blocked");
	check(route(SM_KEY_HASH, 0, 0, 0) == SM_BLOCK && route(SM_KEY_HASH, 0, 0, 1) == SM_BLOCK && route(SM_KEY_HASH, 1, 0, 0) == SM_BLOCK,
	      "HASH (numeric entry, quick list, reverse repeater) is blocked");
	check(route(SM_KEY_DIGIT, 0, 0, 0) == SM_BLOCK && route(SM_KEY_DIGIT, 0, 0, 1) == SM_BLOCK && route(SM_KEY_DIGIT, 0, 1, 0) == SM_BLOCK && route(SM_KEY_DIGIT, 1, 0, 0) == SM_BLOCK,
	      "digits (direct channel, quick keys, APRS, mute) are blocked");
	check(route(SM_KEY_ORANGE, 0, 0, 0) == SM_BLOCK && route(SM_KEY_OTHER, 0, 0, 0) == SM_BLOCK, "the orange button (quick menu) and anything else are blocked");
}

static void testIdleReturn(void)
{
	simpleTimers_t t;
	uint32_t now = 1000;
	simpleTimersReset(&t, now);
	int fired = 0;
	/* away from home, nothing pressed: due after 5 minutes */
	for (int s = 0; s < 299; s++) { now += 1000; fired += simpleIdleReturnDue(&t, now, 0, 0, 0, 5); }
	check(fired == 0, "5 minute idle return: nothing at 4:59");
	now += 1000;
	check(simpleIdleReturnDue(&t, now, 0, 0, 0, 5) == 1, "... due at 5:00");
	check(simpleIdleReturnDue(&t, now + 1000, 0, 0, 0, 5) == 0, "... and fires once");
	/* a key press restarts the countdown */
	simpleTimersReset(&t, now = 0);
	for (int s = 0; s < 240; s++) { now += 1000; simpleIdleReturnDue(&t, now, 0, 0, 0, 5); }
	simpleIdleReturnDue(&t, now, 1, 0, 0, 5);
	fired = 0;
	for (int s = 0; s < 299; s++) { now += 1000; fired += simpleIdleReturnDue(&t, now, 0, 0, 0, 5); }
	check(fired == 0 && simpleIdleReturnDue(&t, now + 1000, 0, 0, 0, 5) == 1, "a key press at 4:00 moves the return to 9:00");
	/* TX or RX audio holds it */
	simpleTimersReset(&t, now = 0);
	for (int s = 0; s < 600; s++) { now += 1000; fired = simpleIdleReturnDue(&t, now, 0, 1, 0, 5); }
	check(fired == 0, "busy (TX or RX audio) for 10 minutes: no return");
	for (int s = 0; s < 299; s++) { now += 1000; fired += simpleIdleReturnDue(&t, now, 0, 0, 0, 5); }
	check(fired == 0 && simpleIdleReturnDue(&t, now + 1000, 0, 0, 0, 5) == 1, "... then 5 quiet minutes bring it home");
	/* on the home channel the timer idles */
	simpleTimersReset(&t, now = 0);
	for (int s = 0; s < 3600; s++) { now += 1000; fired = simpleIdleReturnDue(&t, now, 0, 0, 1, 5); }
	check(fired == 0, "an hour on the home channel: nothing to do");
	for (int s = 0; s < 299; s++) { now += 1000; fired += simpleIdleReturnDue(&t, now, 0, 0, 0, 5); }
	check(fired == 0 && simpleIdleReturnDue(&t, now + 1000, 0, 0, 0, 5) == 1, "... leaving home starts the 5 minutes");
	/* 0 = never, 255 (unwritten EEPROM) = never, 1 minute works */
	simpleTimersReset(&t, now = 0);
	for (int s = 0; s < 7200; s++) { now += 1000; fired = simpleIdleReturnDue(&t, now, 0, 0, 0, 0); }
	check(fired == 0, "0 minutes: never");
	simpleTimersReset(&t, now = 0);
	for (int s = 0; s < 7200; s++) { now += 1000; fired = simpleIdleReturnDue(&t, now, 0, 0, 0, 255); }
	check(fired == 0, "255 minutes (an unwritten byte): never");
	simpleTimersReset(&t, now = 0);
	for (int s = 0; s < 59; s++) { now += 1000; fired += simpleIdleReturnDue(&t, now, 0, 0, 0, 1); }
	check(fired == 0 && simpleIdleReturnDue(&t, now + 1000, 0, 0, 0, 1) == 1, "1 minute: due at 1:00");
	/* millisecond wrap-around */
	simpleTimersReset(&t, now = 0xFFFFFFFFu - 30000u);
	for (int s = 0; s < 59; s++) { now += 1000; fired += simpleIdleReturnDue(&t, now, 0, 0, 0, 1); }
	check(fired == 0 && simpleIdleReturnDue(&t, now + 1000, 0, 0, 0, 1) == 1, "the countdown survives the 32-bit millisecond wrap");
}

static void testPrivateCallHang(void)
{
	simpleTimers_t t;
	uint32_t now = 5000;
	simpleTimersReset(&t, now);
	int fired = 0;
	for (int s = 0; s < 600; s++) { now += 1000; fired = simplePrivateCallExpired(&t, now, 0, 0, 0); }
	check(fired == 0, "no private-call target: nothing expires");
	/* target selected, a call: TX, then the reply, then silence */
	for (int s = 0; s < 10; s++) { now += 1000; fired += simplePrivateCallExpired(&t, now, 1, 0, 1); }
	for (int s = 0; s < 59; s++) { now += 1000; fired += simplePrivateCallExpired(&t, now, 1, 0, 0); }
	check(fired == 0, "59 s of silence after the last over: the target stays");
	now += 1000;
	check(simplePrivateCallExpired(&t, now, 1, 0, 0) == 1, "60 s of silence: the target drops back to the group");
	check(simplePrivateCallExpired(&t, now + 1000, 1, 0, 0) == 0, "... reported once");
	/* a key press (PTT counts as one) restarts it */
	simpleTimersReset(&t, now = 0);
	for (int s = 0; s < 50; s++) { now += 1000; simplePrivateCallExpired(&t, now, 1, 0, 0); }
	simplePrivateCallExpired(&t, now, 1, 1, 0);
	fired = 0;
	for (int s = 0; s < 59; s++) { now += 1000; fired += simplePrivateCallExpired(&t, now, 1, 0, 0); }
	check(fired == 0 && simplePrivateCallExpired(&t, now + 1000, 1, 0, 0) == 1, "activity at 50 s moves the expiry to 110 s");
}

static void testHomePosition(void)
{
	uint16_t zone[80] = { 7, 3, 12, 9, 0 };
	check(simpleHomePosition(zone, 4, 12) == 2, "home channel 12 is the third of the zone");
	check(simpleHomePosition(zone, 4, 7) == 0, "home channel 7 is the first");
	check(simpleHomePosition(zone, 4, 9) == 3, "home channel 9 is the last");
	check(simpleHomePosition(zone, 4, 5) == 0, "a home channel not in the zone falls back to the first");
	check(simpleHomePosition(zone, 4, 0) == 0, "home channel 0 means the first");
	check(simpleHomePosition(zone, 0, 7) == 0, "an empty zone: position 0");
}

int main(void)
{
	testKeyRouting();
	testIdleReturn();
	testPrivateCallHang();
	testHomePosition();
	printf("%s\n", fails ? "FAILED" : "ALL PASS");
	return fails ? 1 : 0;
}

/*
 * simpleMode.c — the decisions of the "dumb radio" mode (see simpleMode.h). No radio state is
 * read here: the caller passes what matters and acts on the answer. Compiles to nothing without
 * ENABLE_SIMPLE_MODE.
 */
#include "functions/simpleMode.h"

#if defined(ENABLE_SIMPLE_MODE)

simpleAction_t simpleModeRouteKey(const simpleKeyEvent_t *ev)
{
	switch (ev->key)
	{
		case SM_KEY_NONE:
			return SM_PASS;
		case SM_KEY_SK1:      /* short: the inbox; long: send a message (the user's choice on the bench) */
			return ev->sk2Held ? SM_BLOCK : (ev->isLong ? SM_PRESETS : SM_INBOX);
		case SM_KEY_GREEN:    /* short: who to call; long: the menu (PIN) */
			return ev->sk2Held ? SM_BLOCK : (ev->isLong ? SM_PIN_MENU : SM_CONTACTS);
		case SM_KEY_RED:      /* the VFO key: here it only ends a private call target */
			return (ev->isLong || ev->sk1Held || ev->sk2Held) ? SM_BLOCK : SM_CANCEL_PRIVATE_CALL;
		case SM_KEY_UP:       /* with SK2 that would be a zone change, held it would start a scan */
			return (ev->isLong || ev->sk2Held) ? SM_BLOCK : SM_CHANNEL_UP;
		case SM_KEY_DOWN:
			return (ev->isLong || ev->sk2Held) ? SM_BLOCK : SM_CHANNEL_DOWN;
		case SM_KEY_RIGHT:    /* the keypad arrows reach the channel screen as LEFT / RIGHT (talkgroup or squelch
		                       * steps there, and power with SK2): here they step the channel like the knob */
			return (ev->isLong || ev->sk2Held) ? SM_BLOCK : SM_CHANNEL_UP;
		case SM_KEY_LEFT:
			return (ev->isLong || ev->sk2Held) ? SM_BLOCK : SM_CHANNEL_DOWN;
		default:              /* timeslot, mode, numeric entry, quick menu, APRS, mute ... */
			return SM_BLOCK;
	}
}

void simpleTimersReset(simpleTimers_t *t, uint32_t nowMs)
{
	t->lastActivityMs = nowMs;
	t->pcQuietSinceMs = nowMs;
}

int simpleIdleReturnDue(simpleTimers_t *t, uint32_t nowMs, int activity, int busy, int atHome, uint32_t idleMinutes)
{
	if (activity || busy || atHome)
	{
		t->lastActivityMs = nowMs;
		return 0;
	}
	if ((idleMinutes == 0) || (idleMinutes > SIMPLE_IDLE_MINUTES_MAX))
	{
		return 0;
	}
	if ((uint32_t)(nowMs - t->lastActivityMs) < idleMinutes * 60000u)
	{
		return 0;
	}
	t->lastActivityMs = nowMs;
	return 1;
}

int simplePrivateCallExpired(simpleTimers_t *t, uint32_t nowMs, int pcActive, int activity, int busy)
{
	if (!pcActive || activity || busy)
	{
		t->pcQuietSinceMs = nowMs;
		return 0;
	}
	if ((uint32_t)(nowMs - t->pcQuietSinceMs) < SIMPLE_PC_HANG_MS)
	{
		return 0;
	}
	t->pcQuietSinceMs = nowMs;
	return 1;
}

int simpleHomePosition(const uint16_t *zoneChannels, int numInZone, uint16_t homeChannel)
{
	if (homeChannel != 0)
	{
		for (int i = 0; i < numInZone; i++)
		{
			if (zoneChannels[i] == homeChannel)
			{
				return i;
			}
		}
	}
	return 0;
}

#endif /* ENABLE_SIMPLE_MODE */

/*
 * featureArena.c — see featureArena.h.
 */
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "functions/featureArena.h"

static uint8_t s_arena[FEATURE_ARENA_SIZE] __attribute__((aligned(8)));
static featureArenaOwner_t s_owner = FEATURE_ARENA_FREE;

void *featureArenaClaim(featureArenaOwner_t owner, size_t bytes)
{
	bool granted = false;
	bool alreadyHeld = false;

	if ((owner == FEATURE_ARENA_FREE) || (bytes > FEATURE_ARENA_SIZE))
	{
		return NULL;
	}

	taskENTER_CRITICAL();
	if (s_owner == owner)
	{
		granted = alreadyHeld = true;
	}
	else if (s_owner == FEATURE_ARENA_FREE)
	{
		s_owner = owner;
		granted = true;
	}
	taskEXIT_CRITICAL();

	if (!granted)
	{
		return NULL;
	}

	if (!alreadyHeld)
	{
		memset(s_arena, 0x00, sizeof(s_arena));
	}

	return s_arena;
}

void featureArenaRelease(featureArenaOwner_t owner)
{
	taskENTER_CRITICAL();
	if (s_owner == owner)
	{
		s_owner = FEATURE_ARENA_FREE;
	}
	taskEXIT_CRITICAL();
}

bool featureArenaIsHeldBy(featureArenaOwner_t owner)
{
	return (s_owner == owner);
}

featureArenaOwner_t featureArenaGetOwner(void)
{
	return s_owner;
}

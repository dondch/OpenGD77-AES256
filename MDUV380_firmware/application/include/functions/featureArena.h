/*
 * featureArena.h — one static block of SRAM shared by features that are never live at
 * the same time.
 *
 * The STM32F405 target has about 4 KB of SRAM left above static data, and the FreeRTOS
 * heap (20 KB) already carries the task stacks, so a feature that needs 12 KB cannot
 * allocate it at run time. The arena is the upstream pattern (sound.c's
 * audioAndHotspotDataBuffer) made explicit: a feature claims the arena when its screen
 * or command starts, releases it when that ends, and an owner tag catches a second
 * claimant on the bench instead of letting it corrupt the first.
 *
 * Owners, and why they never overlap:
 *   SATELLITE       the satellite screen (Keps table + predictions). Claimed on the
 *                   screen's first run, released by its exit callback. Keps reload on
 *                   every entry; a load delay on entering a feature is accepted.
 *   VFO_SWEEP       the VFO sweep's strip staging buffer, max-hold and average traces
 *                   (ENABLE_FAST_SCAN). The sweep is a VFO screen operation mode; the
 *                   satellite screen is a menu reached by stopping it.
 *   SPECTRUM_PROBE  the USB settle-probe sample buffer (ENABLE_SPECTRUM). Lives for one
 *                   CPS command, run from the main task; refused while the arena is held.
 *
 * Rules: claim and release from the main task only (nothing here is touched from the
 * HR-C6000 task, the beep task or an interrupt); a claim zeroes the block, so nothing
 * survives a release; the arena is in SRAM, not CCM, because the strip buffer is a DMA
 * source and the STM32F4 DMA cannot read CCM.
 */
#ifndef _OPENGD77_FEATURE_ARENA_H_
#define _OPENGD77_FEATURE_ARENA_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "user_interface/uiGlobals.h"   /* VFO_SWEEP_NUM_SAMPLES */
#include "hardware/HX8353E.h"           /* DISPLAY_SIZE_Y */

/* Sized for the satellite table (25 x satelliteData_t, 11,900 bytes), rounded to 8.
 * satellite.c static-asserts that it fits. */
#define FEATURE_ARENA_SIZE  11904U

typedef enum
{
	FEATURE_ARENA_FREE = 0,
	FEATURE_ARENA_SATELLITE,
	FEATURE_ARENA_VFO_SWEEP,
	FEATURE_ARENA_SPECTRUM_PROBE
} featureArenaOwner_t;

/* Layout of the VFO sweep's share. 4 columns is what the sweep needs (a sample column plus
 * a two-column cursor, worst case four wide). */
#define FEATURE_ARENA_STRIP_MAX_COLS  4
typedef struct
{
	uint16_t strip[FEATURE_ARENA_STRIP_MAX_COLS * DISPLAY_SIZE_Y];
	uint8_t  hold[VFO_SWEEP_NUM_SAMPLES];
	uint8_t  avg[VFO_SWEEP_NUM_SAMPLES];
} featureArenaSweep_t;

/* Returns the zeroed block, or NULL when another owner holds it or bytes > FEATURE_ARENA_SIZE.
 * A second claim by the current owner returns the block untouched (not zeroed). */
void *featureArenaClaim(featureArenaOwner_t owner, size_t bytes);
/* No-op unless owner matches. */
void featureArenaRelease(featureArenaOwner_t owner);
bool featureArenaIsHeldBy(featureArenaOwner_t owner);
featureArenaOwner_t featureArenaGetOwner(void);

#endif /* _OPENGD77_FEATURE_ARENA_H_ */

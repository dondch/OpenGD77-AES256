/*
 * scanreject.c — see scanreject.h.
 *
 * Deliberately pure arithmetic: no I2C, no timing, no hardware. The caller supplies the
 * RSSI reading it has already taken, which is what lets this compile under either
 * ENABLE_FAST_SCAN (the shipped feature) or ENABLE_SPECTRUM (the dev tooling that
 * measured it) without dragging the spectrum module in behind it.
 *
 * Compiles to nothing with neither flag, so a stock build stays byte-identical.
 */
#include "functions/scanreject.h"

#if defined(ENABLE_FAST_SCAN) || defined(ENABLE_SPECTRUM)

uint16_t scanRejectTicks = SCAN_REJECT_DEFAULT_TICKS;
uint8_t  scanRejectMargin = SCAN_REJECT_DEFAULT_MARGIN;
uint32_t scanRejectStepsTotal = 0;
uint32_t scanRejectStepsRejected = 0;

/* The floor is carried in 1/256 of a count.
 *
 * ★ A plain integer IIR on these numbers does not converge at all: with
 * `floor += (sample - floor) >> 3` and readings in the 40s, every step whose sample is
 * within 8 counts of the estimate shifts to zero and the floor sticks wherever it was
 * seeded. The fractional part is not a refinement, it is the difference between an
 * estimator and a constant. */
static int32_t s_floorQ8 = 0;
static bool s_floorSeeded = false;
static uint16_t s_detectRun = 0;

/* IIR rate: floor += (sample - floor) >> SHIFT. 8 steps of memory is fast enough to
 * follow band tilt across a scan and slow enough not to chase a single spur. */
#define SCAN_REJECT_FLOOR_SHIFT  3

/* If the floor is ever learned too low -- a seed taken on an unusually quiet sample, or
 * a step onto a much quieter part of the band -- every sample reads as lifted, and since
 * lifted samples are excluded from the estimate the floor could never recover. That is a
 * detector that latches on and stays on, i.e. a scanner that has silently stopped
 * rejecting anything. After this many consecutive detections, let the floor move anyway. */
#define SCAN_REJECT_RUN_MAX  64

void scanRejectReset(void)
{
	s_floorSeeded = false;
	s_detectRun = 0;
	scanRejectStepsTotal = 0;
	scanRejectStepsRejected = 0;
}

uint8_t scanRejectFloor(void)
{
	return (uint8_t)(s_floorSeeded ? ((s_floorQ8 >> 8) & 0xFF) : 0);
}

bool scanRejectFloorTest(uint8_t rssi, uint8_t margin)
{
	int32_t sampleQ8 = ((int32_t)rssi) << 8;
	bool lifted;

	if (s_floorSeeded == false)
	{
		s_floorQ8 = sampleQ8;
		s_floorSeeded = true;
	}

	lifted = (((int32_t)rssi) >= ((s_floorQ8 >> 8) + (int32_t)margin));

	if ((lifted == false) || (s_detectRun >= SCAN_REJECT_RUN_MAX))
	{
		s_floorQ8 += ((sampleQ8 - s_floorQ8) >> SCAN_REJECT_FLOOR_SHIFT);
	}

	s_detectRun = lifted ? (s_detectRun + 1) : 0;

	return lifted;
}

void scanRejectCountStep(void)
{
	scanRejectStepsTotal++;
}

bool scanRejectStep(uint8_t rssi)
{
	bool lifted;

	if (scanRejectTicks == 0)
	{
		return false;
	}

	lifted = scanRejectFloorTest(rssi, scanRejectMargin);

	if (lifted == false)
	{
		scanRejectStepsRejected++;
	}

	return (lifted == false);
}

#endif /* ENABLE_FAST_SCAN || ENABLE_SPECTRUM */

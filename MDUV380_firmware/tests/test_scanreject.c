/* Host test of functions/scanreject.c, the fast-reject floor estimator the analog VFO scan
 * uses under ENABLE_FAST_SCAN (and the bench tooling under ENABLE_SPECTRUM).
 *
 * The scanner samples RSSI a few ms into each step and throws the step away when the level
 * has not lifted above a running estimate of the noise floor. A wrong reject costs
 * sensitivity, so the estimator has to: seed from the first sample, follow a slowly tilting
 * floor, NOT be dragged up by the carriers it detects, and recover if it is ever learned too
 * low (a latched-on detector is a scanner that silently stopped rejecting anything).
 *
 * Build: gcc -O2 -DENABLE_FAST_SCAN -Iapplication/include -o /tmp/test_scanreject \
 *          tests/test_scanreject.c application/source/functions/scanreject.c && /tmp/test_scanreject
 */
#include <stdio.h>
#include <stdlib.h>
#include "functions/scanreject.h"

static int fails = 0;
static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { fails++; }
}

static uint32_t rng = 0x1234567u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
/* noise around `centre`, +/- spread counts */
static uint8_t noisy(int centre, int spread) { return (uint8_t)(centre - spread + (int)(rnd() % (unsigned)(2 * spread + 1))); }

static void testDisabledNeverRejects(void)
{
    scanRejectReset();
    scanRejectTicks = 0;
    int rejected = 0;
    for (int i = 0; i < 100; i++) { if (scanRejectStep(40)) { rejected++; } }
    check(rejected == 0 && scanRejectStepsRejected == 0, "rejectTicks 0: the reject is off");
    scanRejectTicks = SCAN_REJECT_DEFAULT_TICKS;
}

static void testSeedsAndRejectsFlatFloor(void)
{
    scanRejectReset();
    scanRejectMargin = SCAN_REJECT_DEFAULT_MARGIN;
    int rejected = 0;
    for (int i = 0; i < 200; i++) { if (scanRejectStep(noisy(40, 2))) { rejected++; } }
    check(rejected == 200, "flat noise (+/-2 counts, margin 8): every step rejected");
    check(abs((int)scanRejectFloor() - 40) <= 1, "floor converges on the noise level");
}

static void testCarrierIsKept(void)
{
    scanRejectReset();
    for (int i = 0; i < 50; i++) { scanRejectStep(noisy(40, 2)); }
    uint8_t floorBefore = scanRejectFloor();
    int kept = 0;
    for (int i = 0; i < 20; i++) { if (!scanRejectStep(noisy(62, 2))) { kept++; } }   /* 22 counts up */
    check(kept == 20, "a carrier 22 counts above the floor is never rejected");
    check(abs((int)scanRejectFloor() - (int)floorBefore) <= 1, "20 detected steps do not drag the floor up");
    int rejectedAfter = 0;
    for (int i = 0; i < 20; i++) { if (scanRejectStep(noisy(40, 2))) { rejectedAfter++; } }
    check(rejectedAfter == 20, "back on noise, rejection resumes at once");
}

static void testFollowsBandTilt(void)
{
    scanRejectReset();
    int rejected = 0;
    for (int i = 0; i < 400; i++)
    {
        int centre = 33 + (i * 13) / 400;    /* 33 -> 46 counts across the scan, as measured on the radio */
        if (scanRejectStep(noisy(centre, 1))) { rejected++; }
    }
    check(rejected >= 395, "a 13-count tilt across 400 steps is followed, not detected");
}

static void testRecoversFromAFloorLearnedTooLow(void)
{
    scanRejectReset();
    scanRejectStep(20);                       /* seeded on one freak quiet sample */
    int rejected = 0;
    for (int i = 0; i < 200; i++) { if (scanRejectStep(noisy(40, 2))) { rejected++; } }
    check(rejected > 0, "a floor seeded 20 counts low eventually lets the estimate move and rejects again");
    check(abs((int)scanRejectFloor() - 40) <= 2, "and the floor ends up on the real noise");
}

static void testCountersAndReset(void)
{
    scanRejectReset();
    for (int i = 0; i < 10; i++) { scanRejectCountStep(); scanRejectStep(noisy(40, 1)); }
    check(scanRejectStepsTotal == 10 && scanRejectStepsRejected <= 10, "step and reject counters count");
    scanRejectReset();
    check(scanRejectStepsTotal == 0 && scanRejectStepsRejected == 0 && scanRejectFloor() == 0, "reset forgets floor and counters");
}

int main(void)
{
    testDisabledNeverRejects();
    testSeedsAndRejectsFlatFloor();
    testCarrierIsKept();
    testFollowsBandTilt();
    testRecoversFromAFloorLearnedTooLow();
    testCountersAndReset();
    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}

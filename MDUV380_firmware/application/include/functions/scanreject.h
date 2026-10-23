/*
 * scanreject.h — abandon an empty analog scan step early, on RSSI.
 *
 * The scanner stops on `trxRxNoise < squelch`. That byte is the right one to decide on --
 * it is SNR-like and frequency-flat, where RSSI is an absolute level that moves with band
 * tilt, front-end response and the chip's own spur comb -- but it is slow: MEASURED, the
 * AT1846S resets it to 127 on the receiver restart that every retune costs, and it takes
 * ~10-15 ms to slew down far enough to mean anything. RSSI is usable at ~2.5 ms.
 *
 * Detection cannot move to RSSI. That was built and measured and it false-alarms: RSSI's
 * step-to-step spread across a scan is the size of a moderate carrier's lift, so any
 * margin that suppresses false stops is already deaf.
 *
 * But detection was never the expensive part. Proving a step EMPTY is, and most steps are
 * empty. So RSSI is used to throw steps away, never to keep them:
 *
 *     sample RSSI a few ms after the retune; if the level has not lifted above a running
 *     floor, end the step now. Otherwise let the dwell run and let the stock rule decide,
 *     untouched.
 *
 * ★ The asymmetry is the whole design. A wrong reject would cost sensitivity; a wrong
 * KEEP costs one wasted dwell and nothing else. So the margin is set low and deliberately
 * trigger-happy, the spread that killed the detector becomes harmless, and because the
 * arbiter is untouched, per-visit sensitivity is identical to stock by construction.
 *
 * ★ The floor compares like with like, and that is what makes it work at 3 ms. Every step
 * samples at the same delay after its own retune, so a running average of recent steps is
 * the floor *at that point on the settle curve*. Being early lowers the reference exactly
 * as much as it lowers the sample, so a carrier's relative lift survives even where its
 * absolute lift has not arrived yet. Comparing against a settled floor instead would
 * reject weak carriers, and predicting that it must is a mistake already made once.
 *
 * MEASURED on air (2026-07-28), 30 ms dwell, carrier at 433.5025, stock arbiter:
 *   speed     27.7 -> 114.4 steps/s, 36.0 -> 8.7 ms/step, 4.12x, 87% of steps discarded
 *   sensitivity  no setting loses a detection stock makes, at any level down to where
 *                stock itself fails; at the marginal level the reject is BETTER (4/4 vs
 *                2/4), because it revisits each frequency ~4x as often per second. Per
 *                visit unchanged, per second improved.
 */
#ifndef _OPENGD77_SCANREJECT_H_
#define _OPENGD77_SCANREJECT_H_

#include <stdint.h>
#include <stdbool.h>

#if defined(ENABLE_FAST_SCAN) || defined(ENABLE_SPECTRUM)

/* Main-loop ticks (~1 ms) after a step begins at which to take the reject sample.
 * 0 disables the whole thing and scanning is exactly stock. 3 is the measured default:
 * RSSI is usable by ~2.5 ms and every tick spent waiting is a tick not saved. */
#define SCAN_REJECT_DEFAULT_TICKS   3

/* Counts above the running floor that count as "something might be here". Low on
 * purpose -- see the asymmetry note above. */
#define SCAN_REJECT_DEFAULT_MARGIN  8

/* AT1846S rssi_ct_u (0x5A[11:9]) to hold while an analog scan is running: the RSSI
 * tracking bandwidth is 1145 / 2^ct Hz, so the stock 3 is 144 Hz and this is 286 Hz.
 *
 * ★ This exists because the reject's whole decision is ONE RSSI reading taken ~3 ms after
 * a retune, and at the stock bandwidth the reading has barely arrived by then. MEASURED
 * at the reject's own sample point, carrier against empty, 50 steps per cell:
 *
 *   rssi_ct_u   no carrier      carrier      lift   lift/sd
 *   3 (stock)   36.8 +-1.69   55.6 +-0.80   18.8      11.1
 *   2           40.3 +-1.66   67.3 +-0.98   27.0      16.3   <- best separation
 *   1           38.2 +-2.26   71.6 +-2.42   33.4      14.8
 *   0           36.9 +-2.58   71.7 +-3.08   34.8      13.5
 *
 * The figure of merit is separation in units of the no-carrier spread, not the raw lift:
 * a faster filter raises both, and below ct=2 the spread grows faster than the signal.
 *
 * ★★ It buys SAFETY, not speed. The reject's only sensitivity risk is throwing away a
 * step that had a weak carrier in it; a 44% bigger lift at the instant of the decision is
 * exactly what makes that less likely, and the margin of 8 still clears the no-carrier
 * spread by 4.8 sd. Nothing here changes the arbiter, so per-visit sensitivity remains
 * identical to stock by construction, as it was before. */
#define SCAN_REJECT_RSSI_COUNT  2

extern uint16_t scanRejectTicks;
extern uint8_t  scanRejectMargin;

/* Steps taken and steps thrown away early. The reject fraction is the speed win and
 * leaves no trace outside the firmware, so it is counted here. */
extern uint32_t scanRejectStepsTotal;
extern uint32_t scanRejectStepsRejected;

/* Update the running floor with `rssi` and say whether it lifted by `margin`. Detected
 * samples do not teach the floor -- otherwise a carrier drags the estimate up behind it
 * and the radio goes deaf on the frequency it just found -- with a consecutive-detection
 * escape hatch, since a floor learned too low would latch on and never recover. */
bool scanRejectFloorTest(uint8_t rssi, uint8_t margin);

/* One call per scan step at the reject point. True = this step is empty, abandon it. */
bool scanRejectStep(uint8_t rssi);

/* Call at each step boundary, so the fraction is not measured against its own
 * denominator. */
void scanRejectCountStep(void);

uint8_t scanRejectFloor(void);
void scanRejectReset(void);

#endif /* ENABLE_FAST_SCAN || ENABLE_SPECTRUM */

#endif /* _OPENGD77_SCANREJECT_H_ */

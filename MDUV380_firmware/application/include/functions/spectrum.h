/*
 * spectrum.h — dev-only swept-RSSI receiver ("poor man's spectrum analyser").
 *
 * NOT an SDR. The RF front end is an AT1846S: an integrated narrowband FM transceiver
 * with no IQ output. All it exposes is an 8-bit RSSI and an 8-bit noise level in
 * register 0x1B, measured through its own IF filter. So the only instrument that can be
 * built here is a SWEPT receiver: retune, wait for the reading to settle, read RSSI,
 * repeat. Resolution bandwidth is whatever the AT1846S IF filter is (nominally 12.5 or
 * 25 kHz), the sweep rate is set by the settle time, and the amplitude scale is the
 * chip's own RSSI curve -- not a calibrated dBm until it has been measured against a
 * known source.
 *
 * All frequencies are in OpenGD77's usual units of 10 Hz (446.00625 MHz = 44600625).
 *
 * Everything here is behind ENABLE_SPECTRUM, a DEV-ONLY build flag that must never be
 * set for a release build. It is deliberately independent of ENABLE_AES, ENABLE_DMR_DATA
 * and ENABLE_KEY_INJECTION; stock builds stay byte-identical.
 *
 * Driven from the host over USB CPS 'C' subcommands 0xA0 (sweep) and 0xA1 (settle
 * probe); see usb_com.c and tools/sweep.py.
 */
#ifndef _OPENGD77_SPECTRUM_H_
#define _OPENGD77_SPECTRUM_H_

#include <stdint.h>
#include <stdbool.h>
#include "hardware/HX8353E.h"   /* DISPLAY_SIZE_X/Y: the AM capture borrows the framebuffer */

#if defined(ENABLE_SPECTRUM)

/* ---- mode byte, shared by the probe and the sweep ---- */

/* How to retune between points.
 *
 * ★ MEASURED 2026-07-27 against a real HackRF carrier: FAST DOES NOT RETUNE THE
 * RECEIVER. Writing the channel-word registers alone leaves the chip receiving on the
 * old frequency -- a sweep built on it returns the anchor frequency's reading over and
 * over, which looks like a plausible flat trace and is entirely fake. What actually
 * latches the new frequency is the RX-off/RX-on bracket on register 0x30. FAST is kept
 * only as the negative control for that experiment; never use it to measure anything.
 *
 * The cost of latching is a receiver restart, and that is what sets the sweep rate:
 * ~4.6 ms to come within +/-4 counts of the final reading, ~15 ms for +/-1 count. The
 * LATCH/POKE variants below exist to find out whether a cheaper latch is possible --
 * that question matters well beyond this module, because the same restart is what makes
 * the normal channel scanner slow. */
#define SPECTRUM_RETUNE_FAST       0  /* PLL regs only -- PROVEN not to retune */
#define SPECTRUM_RETUNE_RADIO      1  /* radioSetFrequency(): the known-good path */
#define SPECTRUM_RETUNE_TRX        2  /* full trxSetFrequency() */
#define SPECTRUM_RETUNE_LATCH      3  /* PLL regs + the minimal RX off/on bracket only */
#define SPECTRUM_RETUNE_POKE30     4  /* PLL regs + rewrite 0x30 unchanged (no mute) */
#define SPECTRUM_RETUNE_POKE05     5  /* PLL regs + rewrite 0x05 (freq mode select) */
#define SPECTRUM_RETUNE_LATE30     6  /* RX off, PLL regs, RX on -- mute as short as
                                       * possible around only the frequency writes */

/* Candidates 7..10, added 2026-07-28. All four ask the same question from different
 * angles: the AT1846S guide's entire "setting frequency" procedure is *write 29H and
 * 2AH* -- no latch, no strobe, no restart is documented anywhere -- yet writing those
 * two registers alone provably does not move the receiver, and the only trigger found so
 * far is the RX off->on edge on 30H[5], which costs a full receiver restart (~4.4 ms).
 * That restart is therefore undocumented behaviour rather than a datasheet requirement,
 * so a cheaper trigger may exist. Each of these writes the PLL registers and then pokes
 * one thing that plausibly forces the synthesiser to reload without restarting RX. */
#define SPECTRUM_RETUNE_BAND0F     7  /* PLL regs + rewrite 0x0F (band select) unchanged.
                                       * 0x0F[1:0] selects 400-520 / 200-260 / 134-174
                                       * MHz and is absent from the vendor guide entirely
                                       * (it comes from OpenRTX's community map), so a
                                       * write to it is the least explored thing on the
                                       * chip that could plausibly reload the VCO. The
                                       * value is the CURRENT one, read once per
                                       * measurement: an in-band sweep never changes band,
                                       * and writing a different value would retune the
                                       * front end rather than the synthesiser. */
#define SPECTRUM_RETUNE_SQTOGGLE   8  /* PLL regs + toggle sq_on (0x30[3]) and put it
                                       * back. Same register as the known-good latch but
                                       * a different bit, and one that does not gate the
                                       * receiver -- so if the latch is "any write to
                                       * 0x30 that changes a bit" rather than "restart
                                       * RX", this finds out and costs nothing. */
#define SPECTRUM_RETUNE_XTAL       9  /* PLL regs + rewrite 0x2B/0x2C (xtal_freq /
                                       * adclk_freq) unchanged. These feed the
                                       * synthesiser arithmetic, so a write may force the
                                       * dividers to be recomputed from the new channel
                                       * word. Values are the current ones, cached once
                                       * per measurement. */
#define SPECTRUM_RETUNE_HILAST    10  /* The PLL registers alone, low word FIRST. If the
                                       * high word is what commits the pair -- which is
                                       * how a great many synthesisers work and would
                                       * explain the whole mystery -- then this retunes
                                       * with zero extra bus traffic and the settle
                                       * question is answered outright. */

/* The retune index is 4 bits, split so that the low 3 stay exactly where they were.
 * ★ Do NOT widen the mask over 0x08/0x10 instead: spectrum_char.py shipped with
 * MODE_FORCE_FM/MODE_WIDE set to 0x04/0x08, i.e. one bit adrift of this header, which
 * silently sent every one of its measurements as SPECTRUM_RETUNE_POKE30 (a no-op retune)
 * with FM force off. A mode-bit layout change that an out-of-date host cannot detect
 * produces plausible numbers, not an error. Bit 5 was free in every host that exists. */
#define SPECTRUM_MODE_RETUNE_MASK  0x07
#define SPECTRUM_MODE_RETUNE_HI    0x20  /* bit 3 of the retune index */

#define SPECTRUM_MODE_FORCE_FM     0x08  /* switch to analog FM for the measurement */
#define SPECTRUM_MODE_WIDE         0x10  /* 25 kHz IF instead of 12.5 kHz (with FORCE_FM) */

/* ---- register overrides ----
 * Up to this many AT1846S registers are re-applied immediately after every retune, with
 * raw I2C writes that bypass the driver's register cache. Two reasons: radioSetFrequency()
 * rewrites 0x30/0x05/0x29/0x2A/0x49 on every call and would otherwise undo them, and the
 * cache swallows a write whose value has not changed, which is exactly what the POKE
 * experiments need to do. This is what makes the settle-time search possible without a
 * flash cycle per candidate: the host sets the table over USB and re-runs the probe. */
#define SPECTRUM_MAX_OVERRIDES  8

void spectrumSetOverrides(int count, const uint8_t *regValTriplets);
int spectrumGetOverrideCount(void);
bool spectrumReadReg(uint8_t reg, uint8_t *hi, uint8_t *lo);

/* One raw register write, straight onto the bus (CPS 0xAB). The override table only
 * applies during a retune, which is no help when the question is "what does the chip do
 * right now" -- e.g. finding which status bit follows the hardware squelch (30H[3] sq_on,
 * thresholds 48H/49H), which is a matter of setting a bit and then reading candidate
 * registers with the carrier on and off.
 *
 * Bypasses the driver's value cache, so afterwards the cache disagrees with the chip and
 * a later driver write of the cached value will be swallowed. Dev-only, and the next
 * AT1846sInit() or bandwidth change puts it right. */
bool spectrumWriteRegRaw(uint8_t reg, uint8_t hi, uint8_t lo);

/* ---- split-dwell scan experiment ----
 * scanStart() gives every channel the same dwell: settingsGetScanStepTimeMilliseconds(),
 * whose floor is TIMESLOT_DURATION (30 ms). For a DIGITAL channel that floor is real --
 * you have to sit through a DMR timeslot to catch a burst, which is why the digital
 * branch already applies its own larger minimum. For an ANALOG channel nothing requires
 * it: measured on this radio, a latching retune needs ~2.6 ms before a carrier is
 * detectable and ~4.7 ms before the level is accurate.
 *
 * Setting this to a non-zero value makes the analog branch of scanStart() use it instead
 * of the 30 ms floor. It is deliberately a runtime knob rather than a compile-time
 * constant so stock and modified behaviour can be compared on one firmware image --
 * flashing between A and B would change far more than the dwell. 0 = stock behaviour. */
extern uint16_t spectrumScanAnalogDwellMs;

/* ---- built-in VFO sweep step time ----
 * uiVFOMode's spectrum display (long-press # in VFO mode) uses a hardcoded 25 ms per
 * sample. With 160 samples that is a 4.0 s sweep. The measured settle requirement is
 * ~4.4 ms, so 25 ms is roughly 5x more than the hardware needs.
 *
 * Runtime override so the two can be compared live on one firmware image instead of
 * across two flashes -- a rebuild between conditions would change far more than the
 * dwell. 0 = the stock 25 ms. Same floor applies as for the scanner: do not go below
 * 6 ms. Note the sweep reads RSSI directly rather than through the squelch comparison,
 * so it may tolerate a shorter dwell than the scanner does -- worth measuring, not
 * assuming. */
extern uint16_t spectrumSweepStepTimeMs;

/* ---- scan settling interval ----
 * SCAN_FREQ_CHANGE_SETTLING_INTERVAL is 1, i.e. the scanner starts testing the squelch
 * one main-loop tick (~1 ms) after the retune. That was harmless while a full screen
 * redraw sat between the retune and the countdown, handing the receiver ~15.8 ms of
 * settle for free. With that redraw rate-limited (ENABLE_FAST_SCAN) the free settle is
 * gone, and the first squelch tests now land inside the ~4.4 ms window in which the
 * AT1846S noise reading has not recovered -- which is the suspected mechanism behind the
 * measured sub-4 ms detection cliff.
 *
 * Spending the first few ticks of the dwell not testing should be strictly better than
 * testing a receiver that has not settled. How many ticks is a bench question, so it is
 * a runtime knob: 0 = the stock 1 tick. Set over USB (CPS 0xAA) and re-measure the
 * detection threshold against a level-controlled carrier -- NOT against RSSI contrast,
 * which is not what the scanner decides on. */
extern uint16_t spectrumScanSettleTicks;

/* ---- what the scanner decides on ----
 * trxCarrierDetected() stops the scan on `trxRxNoise < squelch`: the LOW byte of 0x1B,
 * which is the chip's filtered noise/SNR figure. If the ~4.4 ms settle turns out to be
 * that filter rather than the PLL, then the fix is not to make the receiver settle
 * faster but to stop asking the slow question -- and the only way to know what that
 * costs in sensitivity is to run the real scanner against a level-controlled carrier
 * with the rule swapped.
 *
 * A runtime knob (CPS 0xAD) rather than a build flag, for the usual reason: reflashing
 * between A and B changes far more than the decision rule, and the threshold search
 * needs tens of runs. 0 = stock, and stock is what a release build compiles to anyway
 * since all of this is behind ENABLE_SPECTRUM.
 *
 *   0  stock: noise < squelch
 *   1  RSSI:  trxRxSignal >= spectrumScanRssiThreshold
 *   2  the chip's own squelch/status bit: (reg spectrumScanSqReg & mask) != 0,
 *      or == 0 if spectrumScanSqInvert. Generic because which register carries that
 *      flag is not documented anywhere -- it has to be found by diffing the register
 *      file with the carrier on and off (`settle.py regs --diff`), and pinning it at
 *      build time would cost a flash cycle to correct.
 *
 * Mode 2 costs an extra ~140 us I2C read per test. That is fine for a measurement and
 * is why this is not a candidate for shipping as-is. */
#define SPECTRUM_DETECT_STOCK      0
#define SPECTRUM_DETECT_RSSI       1
#define SPECTRUM_DETECT_CHIPSQ     2
#define SPECTRUM_DETECT_RSSI_AUTO  3

extern uint8_t  spectrumScanDetectMode;
extern uint8_t  spectrumScanRssiThreshold;
extern uint8_t  spectrumScanSqReg;
extern uint16_t spectrumScanSqMask;
extern bool     spectrumScanSqInvert;

/* ---- mode 3: RSSI against a running noise floor ----
 * Mode 1 needs an absolute threshold, and there is no absolute threshold to be had: the
 * floor was measured at 33-46 counts across 430-433.6 MHz alone, so a number that works
 * at one end of a scan is deaf or permanently open at the other. The floor has to be
 * learned, which is what makes mode 3 rather than mode 1 the shippable shape.
 *
 * ★ The estimator compares like with like, and that is the whole trick. Every scan step
 * samples at the same delay after its retune, so every sample carries the same settle
 * history -- a running average of recent steps IS the floor *at that point on the settle
 * curve*, and the transient cancels out of the comparison instead of having to be waited
 * out. This is why the sub-4 ms cliff does not apply to this rule: it never needs the
 * reading to be correct in absolute terms, only comparable.
 *
 * That only holds if there is exactly ONE sample per step. scanning() calls
 * trxCarrierDetected() on every main-loop tick of the test window, and RSSI swings wildly
 * across a settle (12 to 37 counts on a bare floor), so mixing several ticks of one step
 * into the estimate would put that swing straight into the noise of the comparison. The
 * decision is therefore taken once, on the first test after the frequency changes, and
 * held for the rest of the step. Where in the settle that lands is set by
 * spectrumScanSettleTicks (CPS 0xAA) -- which is exactly the knob the bench wants.
 *
 * margin  counts above the floor that count as a carrier. The IIR rate is fixed in
 *         scanreject.c, which owns the estimator both this and the reject share. */
extern uint8_t spectrumScanRssiMargin;

/* The running floor, in whole counts, for the host to watch converge (CPS 0xAC). */
uint8_t spectrumScanFloor(void);

/* Forget the floor. Called when the detection mode is set, so a measurement never starts
 * against an estimate learned on another band or another carrier. */
void spectrumScanDetectReset(void);

/* True if a carrier is present under the currently selected rule. Only called from
 * trxCarrierDetected(), and only when the mode is not stock. */
bool spectrumScanCarrierDetected(uint8_t rssi, uint8_t noise, uint8_t squelch);

/* ---- the fast reject: MOVED ----
 * It now lives in scanreject.{c,h}, which compiles under ENABLE_FAST_SCAN as well,
 * because it turned out to be a shipped scanner feature rather than dev tooling. The
 * runtime knobs stay reachable from here (CPS 0xAE) for the bench.
 *
 * The rationale, kept below for the reader who arrives at mode 3 first and wonders why
 * detection was not simply moved to RSSI:
 * Mode 3 above was built, measured and does not work: RSSI's step-to-step spread across
 * a scan is the size of a moderate carrier's lift, so any margin that suppresses false
 * stops is already deaf. RSSI is an absolute level; the noise byte is SNR-like and
 * frequency-flat. Detection has to stay on the noise byte.
 *
 * But the expensive thing a scanner does is prove a step EMPTY, and that is what costs
 * the full ~10-15 ms of noise settle. RSSI is usable at ~2.5 ms. So use it to throw
 * steps away, not to keep them:
 *
 *   sample RSSI rejectTicks after the step began; if it has not lifted above the running
 *   floor by rejectMargin, end the step immediately. Otherwise let the dwell run and let
 *   `trxRxNoise < squelch` decide exactly as it does now.
 *
 * ★ The asymmetry is the whole point. A wrong reject would cost sensitivity, but a wrong
 * KEEP costs only one wasted dwell -- so the margin is set low and deliberately
 * trigger-happy, the 16-25 count spread that killed the detector becomes harmless, and
 * the arbiter is untouched, which makes sensitivity identical to stock by construction
 * rather than by measurement.
 *
 * rejectTicks 0 disables it entirely and the scan is stock. Runtime (CPS 0xAE) because
 * the reject fraction and the sensitivity check both want sweeping.
 *
 * Shares the floor estimator with mode 3 above, so do not run both at once: leave the
 * detection mode at stock, which is the configuration this is designed for anyway. */
/* Margin used by mode 3 only; the reject has its own (scanRejectMargin). */
extern uint8_t spectrumScanRssiMargin;

/* ---- Stage 0: settle probe ----
 * Park on fA, retune to fB, then sample a register either as fast as the I2C bus allows
 * (intervalUs == 0) or on a fixed grid, timestamping every sample. Plotting the reading
 * against time gives the real retune -> settle latency, which is what sets the
 * achievable sweep rate. Returns the number of samples captured.
 *
 * `reg` is the AT1846S register to sample; 0 means 0x1B, whose two bytes are the RSSI and
 * the noise/SNR level. Being able to point it somewhere else is what separates the two
 * halves of the settle: VK3KYY describes the wall as "PLL lock time AND a low pass filter
 * on the RSSI and also S/N values", and those have completely different fixes. 0x1B gives
 * both filtered readings at once with a single timestamp, so their curves can be compared
 * without any cross-run alignment; pointing the probe at a status register instead times
 * the chip's own hardware squelch against them.
 *
 * Deliberately reuses the sample's two data bytes rather than adding a third: the sample
 * array is static and 200 entries long, and a fifth byte per sample would cost 200 bytes
 * of a dev build that has a few hundred spare. Two probe runs under identical conditions
 * are cheaper than that, and the retune under test is repeatable by construction. */
#define SPECTRUM_PROBE_MAX_SAMPLES  200

typedef struct
{
	uint16_t tUs;      /* microseconds since the retune write sequence started */
	uint8_t  rssi;     /* sampled register, high byte (reg 0x1B: RSSI)        */
	uint8_t  noise;    /* sampled register, low byte  (reg 0x1B: noise level) */
} spectrumSample_t;

typedef struct
{
	uint16_t retuneUs;   /* how long the retune register writes themselves took */
	uint16_t readUs;     /* how long one RSSI register read takes */
	uint8_t  count;      /* samples actually captured */
} spectrumProbeInfo_t;

int spectrumSettleProbe(uint32_t fA, uint32_t fB, uint8_t mode, uint16_t intervalUs,
		uint8_t nSamples, uint8_t reg, spectrumSample_t *out, spectrumProbeInfo_t *info);

/* ---- Stage 3: AM envelope capture ----
 * Park on freq and record rssi_db as fast as I2C allows, straight into the display
 * framebuffer, for long enough to LISTEN to. The host reads the samples back out with
 * the CPS 'R' area 6 read that already exists for fbmirror.py.
 *
 * The framebuffer is the only buffer on this radio big enough: main RAM has ~1.8 kB of
 * headroom above _ebss, while the framebuffer is 40960 B = ~5.8 s at ~7 kHz. The UI
 * repaints it afterwards, so nothing is permanently disturbed -- but the host must read
 * the samples out BEFORE anything redraws the screen.
 *
 * rssiCt 0..7 overrides 0x5A[11:9] rssi_ct_u for the capture and restores it after;
 * anything else leaves it alone. Measured: f-3dB ~ 1145 / 2^rssi_ct_u Hz, so 0 is the
 * only setting with enough bandwidth for speech.
 *
 * ⚠ This deliberately ignores SPECTRUM_MAX_BUSY_US -- a 250 ms cap is a quarter second
 * of audio, which is not a listening test. The main loop is blocked for the whole
 * capture and the caller chooses how long that is. */
#define SPECTRUM_AM_MAX_SAMPLES  (DISPLAY_SIZE_X * DISPLAY_SIZE_Y * 2)

int spectrumAmCapture(uint32_t freq, uint16_t nSamples, uint8_t rssiCt,
		uint16_t *rateHzOut, uint32_t *elapsedUsOut);

/* ---- Stage 1: sweep ----
 * nPoints readings starting at fStart, stepping by stepHz (also 10 Hz units), dwelling
 * dwellUs after each retune before reading. Writes 2 bytes per point (rssi, noise) into
 * out, which must hold 2*nPoints bytes. Returns the points actually measured, which is
 * fewer than asked for if the call hit its busy-time budget -- the host then continues
 * with a second call. */
#define SPECTRUM_SWEEP_MAX_POINTS  480

int spectrumSweep(uint32_t fStart, uint32_t stepHz, uint16_t nPoints, uint16_t dwellUs,
		uint8_t mode, uint8_t *out, uint16_t *elapsedMs);

/* ---- sweep sessions ----
 * MEASURED (2026-07-27): anything that restarts the receiver -- switching to FM,
 * or radioSetFrequency()'s RX-off/RX-on bracket -- costs about 15 ms before RSSI is
 * stable and ~30 ms before the noise reading is, so a self-contained sweep call spends
 * its first tens of milliseconds reading a settling receiver rather than the spectrum.
 * A session pays that once: begin, then any number of sweeps that only move the PLL,
 * then end. This is what makes a live waterfall possible at all.
 *
 * A session leaves the radio tuned away from its channel, so it self-terminates if the
 * host goes quiet -- spectrumTick() is called from the main loop and ends a session that
 * has seen no sweep for SPECTRUM_SESSION_TIMEOUT_MS. */
#define SPECTRUM_SESSION_TIMEOUT_MS  3000U

void spectrumSessionBegin(uint32_t anchorFreq, uint8_t mode);
void spectrumSessionEnd(void);
bool spectrumSessionIsActive(void);
void spectrumTick(void);

/* ---- Stage 2: scan-step profiler ----
 * MEASURED (2026-07-27): an analog scan step costs dwell + 18 ms, and the 18 ms is
 * constant across every dwell from 6 to 480 ms. Constant means it is NOT per-main-loop
 * work (that would scale with the dwell, which counts main-loop iterations) -- it is a
 * one-shot cost paid in the branch that actually moves to the next frequency. This
 * profiler exists to say which part, instead of guessing.
 *
 * Each slot accumulates count/last/min/max/sum of DWT cycle deltas. Two ways to feed it:
 * SCANPROF_START/SCANPROF_END bracket a region, SCANPROF_PERIOD records the interval
 * since the previous call on the same slot (for periods rather than durations).
 *
 * Everything is a no-op without ENABLE_SCAN_PROFILER, so the instrumented call sites can
 * live in stock files (uiVFOMode.c, applicationMain.c, HX8353E_display.c) without a #if
 * around each one and without changing the stock build by a single byte.
 *
 * ★ Its own flag, separate from ENABLE_SPECTRUM, because the slot table is 544 bytes of
 * RAM -- on a target with 360 bytes spare in the full dev build, that is the difference
 * between having room for a feature and not. It has already answered the question it was
 * built for, so the sweep tooling is usually worth keeping when the profiler is not.
 * Requires ENABLE_SPECTRUM: the implementation lives in spectrum.c. */
#if defined(ENABLE_SCAN_PROFILER)
#define SCANPROF_STEP_PERIOD   0   /* wall time between consecutive scan steps       */
#define SCANPROF_STEP_TOTAL    1   /* the whole "dwell expired" branch of scanning() */
#define SCANPROF_HANDLEUP      2   /* handleUpKey() as called by the scan step       */
#define SCANPROF_STEPFREQ      3   /* stepFrequency(): trxSetFrequency + bookkeeping  */
#define SCANPROF_UPDSCREEN     4   /* uiVFOModeUpdateScreen() from inside handleUpKey */
#define SCANPROF_LOOP_PERIOD   5   /* main loop iteration period (nominally 1 ms)     */
#define SCANPROF_LOOP_BODY     6   /* main loop body, excluding the 1 ms pad          */
#define SCANPROF_MENUTICK      7   /* menuSystemCallCurrentMenuTick()                 */
#define SCANPROF_DISPRENDER    8   /* displayRenderRows(): pin setup + DMA to the LCD */
#define SCANPROF_SCANNING      9   /* the whole scanning() call                       */
#define SCANPROF_TRXSETFREQ   10   /* trxSetFrequency() alone, inside stepFrequency() */
/* Inside trxSetFrequency(). Instrumented at the function definitions rather than at the
 * call sites, because the interesting question is how many times each runs per retune:
 * the analog path reaches radioSetFrequency(), radioSetIF() and trxUpdateC6000Calibration()
 * twice each -- once directly and once again via trxSetRX() -> trxActivateRx(). The
 * per-slot count is what shows that, so do not "tidy" these onto call sites. */
#define SCANPROF_RADIOSETFREQ 11   /* radioSetFrequency(): the PLL writes + RX off/on   */
#define SCANPROF_RADIOSETIF   12   /* radioSetIF() -> radioSetBandwidth()               */
#define SCANPROF_RADIOSETRX   13   /* radioSetRx(): LNA/PA GPIO, RX on, HRC6000SetFMRx  */
#define SCANPROF_C6000CAL     14   /* trxUpdateC6000Calibration(): 3 SPI page writes    */
#define SCANPROF_RADIOCAL     15   /* trxUpdateRadioCalibration(): calibration lookups  */
#define SCANPROF_HRC6000FMRX  16   /* HRC6000SetFMRx(), from inside radioSetRx()        */
#define SCANPROF_SLOTS        17

typedef struct
{
	uint32_t count;
	uint32_t lastCycles;
	uint32_t minCycles;
	uint32_t maxCycles;
	uint64_t sumCycles;
	uint32_t markCycles;   /* previous timestamp, for SCANPROF_PERIOD */
	bool     marked;
} scanProfSlot_t;

extern scanProfSlot_t scanProfSlots[SCANPROF_SLOTS];

uint32_t scanProfNow(void);                            /* DWT cycles, self-initialising */
void scanProfAdd(uint8_t slot, uint32_t startCycles);
void scanProfMarkPeriod(uint8_t slot);
void scanProfReset(void);
uint32_t scanProfCyclesPerUs(void);
#endif /* ENABLE_SCAN_PROFILER */

#endif /* ENABLE_SPECTRUM */

#if defined(ENABLE_SCAN_PROFILER)
#define SCANPROF_START(v)     uint32_t v = scanProfNow()
#define SCANPROF_END(s, v)    scanProfAdd((s), (v))
#define SCANPROF_PERIOD(s)    scanProfMarkPeriod(s)
#else
#define SCANPROF_START(v)     do { } while (0)
#define SCANPROF_END(s, v)    do { } while (0)
#define SCANPROF_PERIOD(s)    do { } while (0)
#endif

#endif /* _OPENGD77_SPECTRUM_H_ */

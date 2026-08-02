/*
 * Copyright (C) 2019-2025 Roger Clark, VK3KYY / G4KYF
 *                         Daniel Caujolle-Bert, F1RMB
 *
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include "user_interface/uiGlobals.h"
#include "hardware/HR-C6000.h"
#if defined(PLATFORM_MD9600) || defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
#include "hardware/radioHardwareInterface.h"
#endif
#include "functions/trx.h"
#include "functions/rxPowerSaving.h"
#include "functions/spectrum.h"   /* SCANPROF_*: dev-only scan-step profiler, no-ops otherwise */
#include "functions/scanreject.h" /* the fast reject; compiles to nothing in a stock build */
#include "user_interface/menuSystem.h"
#include "user_interface/uiUtilities.h"
#include "user_interface/uiLocalisation.h"
#include "utils.h"

typedef enum
{
	VFO_SELECTED_FREQUENCY_INPUT_RX,
	VFO_SELECTED_FREQUENCY_INPUT_TX
} vfoSelectedFrequencyInput_t;

typedef enum
{
	VFO_SCREEN_OPERATION_NORMAL,
	VFO_SCREEN_OPERATION_SCAN,
	VFO_SCREEN_OPERATION_DUAL_SCAN,
	VFO_SCREEN_OPERATION_SWEEP
} vfoScreenOperationMode_t;

typedef enum
{
	SWEEP_SETTING_STEP = 0,
	SWEEP_SETTING_RSSI,
	SWEEP_SETTING_GAIN
} sweepSetting_t;

// internal prototypes
static void handleEvent(uiEvent_t *ev);
static void handleQuickMenuEvent(uiEvent_t *ev);
static void updateQuickMenuScreen(bool isFirstRun);
static void updateFrequency(int frequency, bool announceImmediately);
static void stepFrequency(int increment);
static void toneScan(void);
static void scanning(void);
static void scanInit(void);
static void sweepScanInit(void);
static void sweepScanStep(void);
static void updateTrxID(void );
static void setCurrentFreqToScanLimits(void);
static void handleUpKey(uiEvent_t *ev);
static void handleDownKey(uiEvent_t *ev);
static void vfoSweepUpdateSamples(int offset, bool forceRedraw, int bandwidthRescale);
static void setSweepIncDecSetting(sweepSetting_t type, bool increment);
static void vfoSweepDrawSample(int offset);
static void clearNuisance(void);
#if defined(ENABLE_FAST_SCAN)
static void vfoSweepStopListening(void);
static void vfoSweepDrawPeakMarker(void);
#endif

static vfoSelectedFrequencyInput_t selectedFreq = VFO_SELECTED_FREQUENCY_INPUT_RX;

static const int SCAN_TONE_INTERVAL = 200;//time between each tone for lowest tone. (higher tones take less time.)
static uint8_t scanToneIndex = 0;
static CodeplugCSSTypes_t toneScanType = CSS_TYPE_CTCSS;
static CodeplugCSSTypes_t toneScanCSS = CSS_TYPE_NONE; // Here, CSS_NONE means *ALL* CSS types
static uint16_t prevCSSTone = (CODEPLUG_CSS_TONE_NONE - 1);

static vfoScreenOperationMode_t screenOperationMode[2] = { VFO_SCREEN_OPERATION_NORMAL, VFO_SCREEN_OPERATION_NORMAL };// For VFO A and B

static menuStatus_t menuVFOExitStatus = MENU_STATUS_SUCCESS;
static menuStatus_t menuQuickVFOExitStatus = MENU_STATUS_SUCCESS;

static bool quickmenuNewChannelHandled = false; // Quickmenu new channel confirmation window

static const int VFO_SWEEP_STEP_TIME  = 25;// 25ms

#if defined(ENABLE_FAST_SCAN)
// Per-measurement time when the wide span is selected. The stock 25 ms is chosen for a
// 160-measurement pass; the wide span takes 1600, and 25 ms would make that a 40 second
// pass, which is not a display any more.
//
// 6 ms is the measured floor that still works: the column-strip blit took the DISPLAY cost
// to ~2.9 ms/sample (6 ms renders correctly, where it used to break up), and the sweep now
// holds rssi_ct_u at 1, which is what stops a short dwell reading a half-settled level --
// MEASURED, carrier height 22.2 counts at stock vs 34.9 at ct=1 for a 2.9 ms sample.
// 1600 x 6 ms = ~9.6 s per 20 MHz pass.
#define VFO_SWEEP_WIDE_STEP_TIME  6
static int vfoSweepStepTime(void);
#define VFO_SWEEP_STEP_TIME_BASE  vfoSweepStepTime()
#else
#define VFO_SWEEP_STEP_TIME_BASE  VFO_SWEEP_STEP_TIME
#endif

#if defined(ENABLE_SPECTRUM)
// DEV: let the sweep step time be overridden at runtime so 25 ms and a faster value can
// be compared on one image. 160 samples x 25 ms = a 4.0 s sweep; the measured settle
// requirement is ~4.4 ms. See spectrum.h (included unconditionally at the top of this
// file, for the SCANPROF_* profiler macros).
#define VFO_SWEEP_STEP_TIME_ACTIVE \
	((spectrumSweepStepTimeMs != 0) ? (int)spectrumSweepStepTimeMs : VFO_SWEEP_STEP_TIME_BASE)
#else
#define VFO_SWEEP_STEP_TIME_ACTIVE  VFO_SWEEP_STEP_TIME_BASE
#endif

#if defined(ENABLE_SPECTRUM)
// DEV: sweep the settling interval at runtime instead of reflashing per candidate.
// See spectrum.h. 0 = the stock SCAN_FREQ_CHANGE_SETTLING_INTERVAL.
#define SCAN_SETTLING_INTERVAL_ACTIVE \
	((spectrumScanSettleTicks != 0) ? (int)spectrumScanSettleTicks : SCAN_FREQ_CHANGE_SETTLING_INTERVAL)
#else
#define SCAN_SETTLING_INTERVAL_ACTIVE  SCAN_FREQ_CHANGE_SETTLING_INTERVAL
#endif

#if defined(ENABLE_FAST_SCAN)
// MEASURED on an MD-UV390 (DWT cycle counter, CPS 0xA9): one full VFO screen redraw costs
// ~15.8 ms -- about 3.3 ms drawing into the frame buffer and ~12.5 ms blitting all 40 KB
// of it to the LCD over the FSMC. Doing that once per frequency-scan step is what made a
// scan step cost `dwell + 18 ms` regardless of the dwell (the other ~2.2 ms is
// trxSetFrequency), so at the 6 ms dwell the display was two thirds of the scan.
//
// It is also far more often than anyone can read. During a frequency scan the readout is
// therefore refreshed on a wall clock instead of once per step. 100 ms is 10 fps, which
// still reads as continuous, and cuts the redraw to roughly every twelfth step at 6 ms.
//
// Scan.refreshOnEveryStep (set by scanInit() when the scan range is one step wide) still
// forces a redraw every step: there the frequency is not moving, so the readout is the
// only thing showing that anything is happening at all.
#define VFO_SCAN_DISPLAY_INTERVAL_MS  100
static ticksTimer_t vfoScanDisplayTimer = { 0, 0 };
#endif

#if defined(PLATFORM_RD5R)
#define VFO_SWEEP_GRAPH_START_Y     8
#define VFO_SWEEP_GRAPH_HEIGHT_Y   30
#elif defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
#define VFO_SWEEP_GRAPH_START_Y    10
#define VFO_SWEEP_GRAPH_HEIGHT_Y   78
#else
#define VFO_SWEEP_GRAPH_START_Y    10
#define VFO_SWEEP_GRAPH_HEIGHT_Y   38
#endif


static uint8_t vfoSweepSamples[VFO_SWEEP_NUM_SAMPLES];

#if defined(ENABLE_FAST_SCAN)
// Peak tracking. The sweep could show a spike but never say what frequency it was on --
// you had to eyeball its column, leave the sweep, and tune around until you found it.
// fagci's UV-K5 mod marks the peak and prints its frequency, and that is most of why its
// spectrum screen is usable and ours was not.
//
// The peak is latched at the end of each pass rather than tracked live, so the marker and
// the label stay put while the trace redraws under them instead of chasing every sample.
static uint8_t vfoSweepPeakLevel = 0;        // running max within the pass in progress
static int16_t vfoSweepPeakIndex = -1;
static uint8_t vfoSweepShownPeakLevel = 0;   // latched at the end of the last pass
static int16_t vfoSweepShownPeakIndex = -1;
static bool    vfoSweepPassComplete = false; // set when a pass wraps; drives the full blit

// How far above the noise floor a sample has to be before it is called a peak rather
// than the loudest noise. In the same counts as the samples and the floor setting.
#define VFO_SWEEP_PEAK_MARGIN  6

// A band across the top of the graph that the trace never draws into, reserved for the
// peak marker and its label.
//
// It has to be reserved rather than just drawn over. vfoSweepDrawSample() clears the full
// column height above each sample it draws, so anything painted into the graph is wiped
// as the sweep passes under it -- the marker appeared at the end of a pass and was eaten
// column by column during the next one. Redrawing it per sample instead would mean a
// full-width blit per sample, which is exactly the cost the strip blit just removed.
//
// Costs 8 of 78 pixels of vertical range. Worth it: an unlabelled spike tells you
// something is there but not what frequency it is on, which was the whole complaint.
#define VFO_SWEEP_LABEL_BAND_H   8

// ---- listen on peak ----
// fagci's UV-K5 mod does not just show you a signal, it parks on it and lets you hear it,
// which is what turns a spectrum display into a scanner you would actually use. Same here:
// at the end of a pass, if the peak is strong enough, tune to it and open the audio.
//
// The mechanism is the existing pause state, not a new one. sweepScanStep() already
// returns early unless Scan.state == SCAN_STATE_SCANNING, and -- the part that makes this
// cheap -- trxCheckAnalogSquelch() bails out early on uiVFOModeSweepScanning(false), which
// tests for exactly that same SCANNING state. So moving to SCAN_STATE_PAUSED both stops
// the sweep and re-enables the normal squelch and audio path, with no change to either.
//
// Trigger margin is higher than the marker's: a signal worth interrupting the sweep for is
// a stronger claim than one worth pointing at.
#define VFO_SWEEP_LISTEN_MARGIN    10
#define VFO_SWEEP_LISTEN_MIN_MS  1200   // park at least this long, even if nothing opens
#define VFO_SWEEP_LISTEN_HANG_MS  800   // and this long after the audio last closed

static bool          vfoSweepListening = false;
static ticksTimer_t  vfoSweepListenTimer = { 0, 0 };
static uint32_t      vfoSweepListenFreq = 0;

// ---- max hold ----
// A single pass only shows what was on the air during the ~13 ms this bin was actually
// being listened to. Anything intermittent -- most of what is worth finding -- is invisible
// unless you happen to be looking at the right column at the right moment. The hold keeps
// the highest level each bin has reached and draws it as a thin line above the live trace,
// so a burst that happened three passes ago is still on screen.
//
// It fades rather than holding forever. A true hold fills up with every transient the
// receiver has ever seen and stops meaning anything within a few minutes, and clearing it
// needs a control this keypad does not have to spare. One count per pass is slow enough
// that a genuine intermittent signal stays visible for tens of passes. Set to 0 for a
// true hold.
#define VFO_SWEEP_HOLD_DECAY  1
// Pixels the hold must stand above the live trace before it is drawn at all.
//
// Held noise is not a small effect to be nudged away: max-hold over many passes converges
// on the noise *envelope*, which sits well above the instantaneous floor, so the natural
// result is a speckle band across every bin. Tried 3 and it was still dirty wherever the
// floor was low. 6 leaves the band suppressed while anything worth finding -- which is
// normally tens of counts up -- still shows.
//
// This is the knob that trades readability against catching a marginal held signal. It is
// the first thing to change if the hold feels either too noisy or too deaf.
#define VFO_SWEEP_HOLD_MIN_LIFT  6
static uint8_t vfoSweepHold[VFO_SWEEP_NUM_SAMPLES];

// ---- automatic noise floor and gain ----
// The two manual knobs were the last thing standing between this and a display you can
// just look at. Worse, their defaults do not fit the hardware: vfoSweepRssiNoiseFloor is
// capped at VFO_SWEEP_RSSI_NOISE_FLOOR_MAX (24) while the real noise floor on this radio
// measures around 36-43 counts, so the floor setting cannot reach the noise however it is
// adjusted. The noise therefore always draws as a solid band ~20 pixels up, and the
// useful range above it gets a fraction of the screen.
//
// That same mismatch quietly broke the peak and listen thresholds: both compare against
// `floor + margin`, and with floor pinned below the noise those tests were true for every
// sample the receiver has ever produced. Scaling off the measured floor is what makes
// them mean anything.
//
// The auto values are kept separate from the manual ones rather than written back into
// them, because they do not fit: vfoSweepSettings stores the floor in 5 bits and the gain
// in 7, and the persisted format is shared with CHIRP and the stock firmware.
#define VFO_SWEEP_AUTO_FLOOR_MARGIN  4    // leave this much noise visible, not clipped flat
#define VFO_SWEEP_AUTO_MIN_SPAN     20    // never magnify a flat band into a wall of noise
#define VFO_SWEEP_AUTO_HEADROOM_PCT 85    // put the strongest sample at this % of height
#define VFO_SWEEP_AUTO_SLEW          4    // IIR weight; higher = steadier, slower

static bool    vfoSweepAutoScale = true;
static bool    vfoSweepAutoPrimed = false;
static uint8_t vfoSweepAutoFloor = VFO_SWEEP_RSSI_NOISE_FLOOR_DEFAULT;
static uint8_t vfoSweepAutoGain = VFO_SWEEP_GAIN_DEFAULT;

// AT1846S rssi_ct_u (0x5A[11:9]) to hold while the sweep is running. The tracking
// bandwidth is 1145 / 2^ct Hz, so the stock 3 is 144 Hz and this is 572 Hz.
//
// ★ The sweep has been paying for a filter it cannot afford. Since the column-strip blit
// its floor is ~2.9 ms/sample, while RSSI takes ~5.7 ms to settle to +-2 counts at the
// stock bandwidth -- so every sample is read part-way up the step response and the trace
// is compressed towards the noise floor. MEASURED on the real sweep path (CPS 0xA0, 8
// traces per cell, against the bench harmonic), carrier height above the floor:
//
//   rssi_ct_u   @2.9 ms/sample   @6.0 ms/sample
//   3 (stock)        22.2             33.9
//   2                28.6             35.9
//   1                34.9             37.6      <- 2.9 ms now as good as stock at 6 ms
//   0                32.4             38.0
//
// So this is contrast for free rather than speed for jitter: the sweep keeps its current
// rate and recovers the ~35% of trace height it was losing to the filter. ct=0 is worse
// than ct=1 -- past that point the added jitter costs more than the settle gains.
//
// A different value from the scanner's SCAN_REJECT_RSSI_COUNT on purpose: the reject
// wants the best SEPARATION for one early sample, the sweep wants the best absolute
// height at a fixed sample time. They optimise different things and measured differently.
#define VFO_SWEEP_RSSI_COUNT  1

// ---- wide span: more measurements than columns ----
//
// The sweep has always been capped at 160 columns x 12.5 kHz = 2 MHz, and the reason given
// was the IF bandwidth. Half of that is right and half was an assumption.
//
// RIGHT: the receiver cannot be made to listen wider, so a COARSER STEP leaves holes.
// Documented, AT1846S Programming Guide 1.4: 30H[13] filter_band_sel and 30H[12] band_mode
// are single bits (25 kHz or 12.5 kHz, nothing else), and 15H[12:9] tuning_bit<3:0> --
// "Tuning IF filter center frequency and bw" -- is a 4-bit field the 25 kHz table already
// programs to its maximum of 15. MEASURED passband: flat to +-6.25 kHz at 12.5 kHz IF and
// +-7.5 kHz at 25 kHz. So a 25 kHz step would leave a ~10 kHz hole in every column, ~19 dB
// deep. That route is closed by the hardware.
//
// ASSUMPTION: that a column must be one measurement. Nothing requires it. VFO_SWEEP_NUM_SAMPLES
// is used as both the array size and the pixel column throughout this file, and that coupling
// was mistaken for a constraint.
//
// So: take N measurements per column at the SAME 12.5 kHz spacing and keep the loudest.
// Every 12.5 kHz of the span is genuinely listened to, so the span grows with NO gaps and no
// sensitivity cost -- and since only the column maximum is kept, it costs no extra RAM at all.
// This is what a spectrum analyser does when the span exceeds its display points, and peak
// detect is the right detector for finding narrow signals.
//
// (fagci's UV-K5 mod maps FEWER points onto more pixels -- `i = x >> stepsCount` -- and buys
// its 12.8 MHz with a 100 kHz step on a comparable narrowband part, i.e. with the holes.
// Studying it suggested the wrong direction.)
//
// The only cost is time, exactly linear: N times the retunes per pass.
#define VFO_SWEEP_WIDE_FLAG        0x8000  // spare bit 15 of vfoSweepSettings (3+5+7 = 15 used)
#define VFO_SWEEP_WIDE_SUBSAMPLES  10      // 160 columns x 10 x 12.5 kHz = 20 MHz
#define VFO_SWEEP_WIDE_SUBSTEP_IDX 6       // the table entry whose per-sample step IS 12.5 kHz

static bool    vfoSweepWide = false;
static uint8_t vfoSweepSubIndex = 0;       // which measurement within the current column
static uint8_t vfoSweepColumnPeak = 0;     // running max over that column's measurements

// ---- auto scroll ----
// A 20 MHz window still only covers a sixth of UHF. Advancing the centre by exactly one span
// at the end of every pass tiles the whole band without overlap or omission, which turns the
// sweep from "look at this window" into "survey everything the radio can hear".
//
// Deliberately NOT persisted: it is an action rather than a preference, and a radio that
// silently starts marching across the band the next time you open the sweep would be a
// surprise. vfoSweepWide IS persisted, because that one is a preference.
static bool vfoSweepAutoScroll = false;
static bool vfoSweepScrollPending = false;   // pass ended parked; advance once it resumes

/* ---- dwell: several passes per window before moving on ----
 *
 * ★ MEASURED limitation this exists to fix. A survey that advances after ONE pass finds
 * continuous carriers and little else: at the 20 MHz span each 12.5 kHz slice gets ~6 ms
 * of listening per pass, so anything intermittent has to be transmitting during its own
 * 6 ms to be seen. Surveying this bench that way found every internal clock product and
 * exactly nothing else -- both real signals present (146.0250 and 147.0375 MHz) were
 * missed by the survey and found by spot-checking, despite being STRONGER than several
 * things it did report.
 *
 * Staying N passes multiplies the chance of overlapping a burst by N, and the max hold
 * already carries the result across passes -- it just used to be thrown away by the
 * advance before it could accumulate. 4 passes at the 20 MHz span is ~38 s per window,
 * ~5.8 minutes for a full UHF cycle, which is a survey rather than a display.
 *
 * The hold decay is suppressed while dwelling (see the end-of-pass block): one count per
 * pass is right for a live display, where a stale peak should fade, but wrong for a
 * survey, where a burst three passes ago is exactly what is being looked for. */
#define VFO_SWEEP_DWELL_PASSES  4
static uint8_t vfoSweepDwellPasses = 1;      // 1 = advance every pass
static uint8_t vfoSweepPassesDone = 0;

/* Longest the auto scroll will let a single park last.
 *
 * ★ MEASURED, not guessed: without this the survey stops permanently. The listen hold is
 * "as long as the audio is open, plus a hang" -- deliberately, so a transmission is never
 * cut off mid-sentence -- but a carrier that simply stays up holds the audio open for
 * ever. On the bench the sweep parked at 490 MHz and was still sitting there 190 seconds
 * later, having surveyed nothing.
 *
 * A plain sweep should keep that behaviour: you asked it to listen. A SURVEY must not,
 * because the whole point is to get all the way round the band. Long enough to identify
 * what you have found, short enough that one open repeater cannot eat the run. */
#define VFO_SWEEP_SCROLL_MAX_PARK_MS  6000
static ticksTimer_t vfoSweepParkLimitTimer = { 0, 0 };

static int vfoSweepStepTime(void)
{
	return (vfoSweepWide ? VFO_SWEEP_WIDE_STEP_TIME : VFO_SWEEP_STEP_TIME);
}

// Rebuild the stored word without ever losing bit 15. Two places rebuild it from the three
// named fields, and either would drop the wide flag silently -- the failure would be a mode
// that switches itself off on the next gain adjustment, which nobody would trace back here.
#define VFO_SWEEP_SETTINGS_WORD(stepIdx, floor, gain) \
	((uint16_t)((((stepIdx) & 0x7) << 12) | (((floor) & 0x1F) << 7) | ((gain) & 0x7F) | \
			(vfoSweepWide ? VFO_SWEEP_WIDE_FLAG : 0)))

// Measurements per display column, and the frequency step between them.
static uint8_t vfoSweepSubSamples(void)
{
	return (vfoSweepWide ? VFO_SWEEP_WIDE_SUBSAMPLES : 1);
}

static int vfoSweepSubStep(void)
{
	return VFO_SWEEP_SCAN_RANGE_SAMPLE_STEP_TABLE[vfoSweepWide
			? VFO_SWEEP_WIDE_SUBSTEP_IDX : uiDataGlobal.Scan.sweepStepSizeIndex];
}

// Total width of the display, in OpenGD77 10 Hz units.
static uint32_t vfoSweepSpan(void)
{
	return ((uint32_t)vfoSweepSubStep() * VFO_SWEEP_NUM_SAMPLES * vfoSweepSubSamples())
			/ VFO_SWEEP_PIXELS_PER_STEP;
}

#define VFO_SWEEP_FLOOR_ACTIVE  (vfoSweepAutoScale ? vfoSweepAutoFloor : vfoSweepRssiNoiseFloor)
// MAX(1, ...) because this is a divisor and VFO_SWEEP_GAIN_MIN is 0.
#define VFO_SWEEP_GAIN_ACTIVE   MAX(1, (vfoSweepAutoScale ? vfoSweepAutoGain : vfoSweepGain))
#else
#define VFO_SWEEP_FLOOR_ACTIVE  vfoSweepRssiNoiseFloor
#define VFO_SWEEP_GAIN_ACTIVE   vfoSweepGain
// Stock builds have no wide flag to preserve, so this is the plain packing it always was.
#define VFO_SWEEP_SETTINGS_WORD(stepIdx, floor, gain) \
	((uint16_t)((((stepIdx) & 0x7) << 12) | (((floor) & 0x1F) << 7) | ((gain) & 0x7F)))
#endif

#if defined(ENABLE_FAST_SCAN)
#define VFO_SWEEP_TRACE_START_Y   (VFO_SWEEP_GRAPH_START_Y + VFO_SWEEP_LABEL_BAND_H)
#define VFO_SWEEP_TRACE_HEIGHT_Y  (VFO_SWEEP_GRAPH_HEIGHT_Y - VFO_SWEEP_LABEL_BAND_H)
#else
#define VFO_SWEEP_TRACE_START_Y   VFO_SWEEP_GRAPH_START_Y
#define VFO_SWEEP_TRACE_HEIGHT_Y  VFO_SWEEP_GRAPH_HEIGHT_Y
#endif
static uint8_t vfoSweepRssiNoiseFloor = VFO_SWEEP_RSSI_NOISE_FLOOR_DEFAULT;
static uint8_t vfoSweepGain = VFO_SWEEP_GAIN_DEFAULT;
static bool vfoSweepSavedBandwidth;
const int VFO_SWEEP_SCAN_FREQ_STEP_TABLE[7] 		= {125,250,500,1000,2500,5000,10000};
static uint8_t previousVFONumber = 0xFF; // Keep track of the currently loaded channel data


// Public interface
menuStatus_t uiVFOMode(uiEvent_t *ev, bool isFirstRun)
{
	static uint32_t m = 0, curm = 0;

	if (isFirstRun)
	{
#if ! defined(PLATFORM_GD77S)
		// We're coming back from the lock screen (hence no channel init is needed, at all).
		bool isLockMenu = (menuSystemGetPreviouslyPushedMenuNumber(true) == UI_LOCK_SCREEN);
		if (isLockMenu || lockscreenIsRearming)
		{
			if (isLockMenu)
			{
				menuSystemGetPreviouslyPushedMenuNumber(false); // Clear the previous lock screen trace
			}

			uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			uiVFOModeUpdateScreen(0);

			if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP)
			{
				for(int i = 0; i < VFO_SWEEP_NUM_SAMPLES; i++)
				{
					vfoSweepDrawSample(i);
				}

				displayDrawFastVLine((uiDataGlobal.Scan.sweepSampleIndex) % VFO_SWEEP_NUM_SAMPLES, VFO_SWEEP_TRACE_START_Y, VFO_SWEEP_TRACE_HEIGHT_Y, true);// draw solid line in the next location
				displayDrawFastVLine((uiDataGlobal.Scan.sweepSampleIndex + uiDataGlobal.Scan.sweepSampleIndexIncrement) % VFO_SWEEP_NUM_SAMPLES, VFO_SWEEP_TRACE_START_Y, VFO_SWEEP_TRACE_HEIGHT_Y, true);// draw solid line in the next location

				displayRenderRows(1, ((8 + VFO_SWEEP_GRAPH_HEIGHT_Y) / 8) + 1);
			}

			return MENU_STATUS_SUCCESS;
		}
#endif

		uiDataGlobal.FreqEnter.index = 0;

		uiDataGlobal.isDisplayingQSOData = false;
		uiDataGlobal.reverseRepeaterVFO = false;
		settingsSet(nonVolatileSettings.initialMenuNumber, (uint8_t) UI_VFO_MODE);
		uiDataGlobal.displayQSOStatePrev = QSO_DISPLAY_IDLE;
		currentChannelData = &settingsVFOChannel[nonVolatileSettings.currentVFONumber];
		currentChannelData->libreDMR_Power = 0x00;// Force channel to the Master power

		uiDataGlobal.currentSelectedChannelNumber = CH_DETAILS_VFO_CHANNEL;// This is not a regular channel. Its the special VFO channel!
		uiDataGlobal.displayChannelSettings = false;
		uiDataGlobal.talkaround = false;

#if defined(PLATFORM_MD9600)
		// This could happen if a MK22 codeplug containing 220MHz channel(s) has been flashed.
		if ((trxGetBandFromFrequency(currentChannelData->rxFreq) == FREQUENCY_OUT_OF_BAND) || (trxGetBandFromFrequency(currentChannelData->txFreq) == FREQUENCY_OUT_OF_BAND))
		{
			currentChannelData->rxFreq = OUT_OF_BAND_FALLBACK_FREQUENCY;
			currentChannelData->txFreq = OUT_OF_BAND_FALLBACK_FREQUENCY;
		}
#endif

		radioSetTRxDevice(RADIO_DEVICE_PRIMARY);
		trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));

#if defined(PLATFORM_MD2017)
		if (true)
		{
			radioSetTRxDevice(RADIO_DEVICE_SECONDARY);
			CodeplugChannel_t *secondaryChannel = &settingsVFOChannel[1 - nonVolatileSettings.currentVFONumber];
			radioSetFrequency(secondaryChannel->rxFreq, false);
			trxSetModeAndBandwidth(secondaryChannel->chMode, (codeplugChannelGetFlag(secondaryChannel, CHANNEL_FLAG_BW_25K) != 0));
			radioSetTRxDevice(RADIO_DEVICE_PRIMARY);
		}
#endif

		//Need to load the Rx group if specified even if TG is currently overridden as we may need it later when the left or right button is pressed
		if (currentChannelData->rxGroupList != 0)
		{
			if (currentChannelData->rxGroupList != lastLoadedRxGroup)
			{
				if (codeplugRxGroupGetDataForIndex(currentChannelData->rxGroupList, &currentRxGroupData))
				{
					lastLoadedRxGroup = currentChannelData->rxGroupList;
				}
				else
				{
					lastLoadedRxGroup = -1;
				}
			}
		}
		else
		{
			memset(&currentRxGroupData, 0xFF, sizeof(CodeplugRxGroup_t));// If the VFO doesnt have an Rx Group ( TG List) the global var needs to be cleared, otherwise it contains the data from the previous screen e.g. Channel screen
			lastLoadedRxGroup = -1;
		}

		uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;

		lastHeardClearLastID();

		int nextMenu = menuSystemGetPreviouslyPushedMenuNumber(false); // used to determine if this screen has just been loaded after Tx ended (in loadChannelData()))

		uiVFOModeLoadChannelData((((nextMenu == UI_TX_SCREEN) || (nextMenu == UI_LOCK_SCREEN) || (nextMenu == UI_PRIVATE_CALL)) ? false : true));

		if ((uiDataGlobal.displayQSOState == QSO_DISPLAY_CALLER_DATA) && (trxGetMode() == RADIO_MODE_ANALOG))
		{
			uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
		}

		freqEnterReset();
		uiVFOModeUpdateScreen(0);
		settingsSetVFODirty();

		if ((uiDataGlobal.VoicePrompts.inhibitInitial == false) &&
				((uiDataGlobal.Scan.active == false) ||
						(uiDataGlobal.Scan.active && ((uiDataGlobal.Scan.state == SCAN_STATE_SHORT_PAUSED) || (uiDataGlobal.Scan.state == SCAN_STATE_PAUSED)))))
		{
			announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_AND_CONTACT_OR_VFO_FREQ_AND_MODE,
					((nextMenu == UI_TX_SCREEN) || (nextMenu == UI_PRIVATE_CALL)) ? PROMPT_THRESHOLD_NEVER_PLAY_IMMEDIATELY : PROMPT_THRESHOLD_2);
		}

		if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN)
		{
			// Refresh on every step if scan boundaries is equal to one frequency step.
			uiDataGlobal.Scan.refreshOnEveryStep = ((nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber] - nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber]) <= VFO_FREQ_STEP_TABLE[(currentChannelData->VFOflag5 >> 4)]);
		}

		// Need to do this last, as other things in the screen init, need to know whether the main screen has just changed
		if (uiDataGlobal.VoicePrompts.inhibitInitial)
		{
			uiDataGlobal.VoicePrompts.inhibitInitial = false;
		}

		menuVFOExitStatus = MENU_STATUS_SUCCESS;
	}
	else
	{
		menuVFOExitStatus = MENU_STATUS_SUCCESS;

#if ! defined(PLATFORM_GD77S)
		// Don't go any further while APRS is beaconing
		if (aprsBeaconingForcedManualBeaconingTriggered() && aprsBeaconingIsTransmitting())
		{
			return menuVFOExitStatus;
		}
#endif

		if (ev->events == NO_EVENT)
		{
			bool updateLHDisplay = ((nonVolatileSettings.lastTalkerOnScreenTimer > 0U) &&
					(uiDataGlobal.lastHeardCount > 0) &&
					(uiDataGlobal.Scan.active == false) &&
					ticksTimerIsEnabled(&uiDataGlobal.DMRLastTalkerOnScreen.timer) &&
					ticksTimerHasExpired(&uiDataGlobal.DMRLastTalkerOnScreen.timer) &&
					((audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF) == 0));

			// We are entering digits, so update the screen as we have a cursor to blink
			if ((uiDataGlobal.FreqEnter.index > 0) && ((ev->time - curm) > 300))
			{
				curm = ev->time;
				uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN; // Redraw will happen just below
			}


			// is there an incoming DMR signal
			if ((uiDataGlobal.displayQSOState != QSO_DISPLAY_IDLE) || updateLHDisplay)
			{
				if (updateLHDisplay)
				{
					uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
				}

				uiVFOModeUpdateScreen(0);

				// Force full redraw, due to Notification hidding while sweep scanning
				if (uiVFOModeSweepScanning(true))
				{
					vfoSweepUpdateSamples(0, true, 0);
					displayRender();
				}
			}
			else
			{
				if ((ev->time - m) > RSSI_UPDATE_COUNTER_RELOAD)
				{
					if (rxPowerSavingIsRxOn())
					{
						bool doRendering = true;

						if (uiDataGlobal.Scan.active && (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP) && (uiDataGlobal.Scan.state == SCAN_STATE_PAUSED))
						{
#if defined(PLATFORM_RD5R)
							displayClearRows(0, 1, false);
#else
							displayClearRows(0, 2, false);
#endif
							uiUtilityRenderHeader(false, false, ((ev->buttons & BUTTON_SK1) == BUTTON_SK1));
						}
						else
						{
							if (uiVFOModeDualWatchIsScanning())
							{
								// Header needs to be updated, if Dual Watch is scanning
								uiUtilityRedrawHeaderOnly(true, false, ((ev->buttons & BUTTON_SK1) == BUTTON_SK1));
								doRendering = false;
							}
							else if (uiVFOModeSweepScanning(true) == false)
							{
								 uiUtilityDrawRSSIBarGraph();
							}
						}

						// Only render the second row which contains the bar graph, if we're not scanning,
						// as there is no need to redraw the rest of the screen
						if (doRendering)
						{
							if (uiNotificationIsVisible())
							{
								displayRender();
							}
							else
							{
								displayRenderRows(((uiDataGlobal.Scan.active && (uiDataGlobal.Scan.state == SCAN_STATE_PAUSED)) ? 0 : 1), 2);
							}
						}
					}

					m = ev->time;
				}

			}

			if (uiDataGlobal.Scan.toneActive)
			{
				toneScan();
			}

#if ! (defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017))
			if (uiDataGlobal.Scan.active)
			{
				if (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP)
				{
					scanning();
				}
				else
				{
					sweepScanStep();
				}
			}
#endif
		}
		else
		{
			if (ev->hasEvent)
			{
				// Scanning barrier
				if (uiDataGlobal.Scan.toneActive)
				{
					// Left key (alone) reverse tone scan direction
					if ((ev->events & KEY_EVENT) && (BUTTONCHECK_DOWN(ev, BUTTON_SK2) == 0))
					{
						if (
#if defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380)
								(ev->keys.key == KEY_FRONT_DOWN)
#else
								(ev->keys.key == KEY_LEFT)
#endif
						)
						{
							uiDataGlobal.Scan.direction *= -1;
							keyboardReset();
							return MENU_STATUS_SUCCESS;
						}
					}

#if defined(PLATFORM_RD5R) // virtual ORANGE button will be implemented later, this CPP will be removed then.
					if ((ev->keys.key != 0) && (ev->keys.event & KEY_MOD_UP))
#else
					// PTT key is already handled in main().
					if (((ev->events & BUTTON_EVENT) && BUTTONCHECK_SHORTUP(ev, BUTTON_ORANGE)) ||
							((ev->keys.key != 0) && (ev->keys.event & KEY_MOD_UP)))
#endif
					{
						uiVFOModeStopScanning();
					}

					return MENU_STATUS_SUCCESS;
				}

				handleEvent(ev);
			}
		}

#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
		if (uiDataGlobal.Scan.active)
		{
			if (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP)
			{
				SCANPROF_START(tScan);
				scanning();
				SCANPROF_END(SCANPROF_SCANNING, tScan);
			}
			else
			{
				sweepScanStep();
			}
		}
#endif
	}
	return menuVFOExitStatus;
}

void uiVFOModeUpdateScreen(int txTimeSecs)
{
	static bool blink = false;
	static uint32_t blinkTime = 0;
	char buffer[SCREEN_LINE_BUFFER_SIZE];
	bool isTransmitting = isTransmittingIgnoringAPRSBeaconing();

	// We don't want QSO info to be displayed while in Sweep scan, or screen redrawing while Sweep is paused
	if (uiVFOModeSweepScanning(true) && ((uiDataGlobal.displayQSOState >= QSO_DISPLAY_CALLER_DATA) || (uiDataGlobal.Scan.state == SCAN_STATE_PAUSED)))
	{
		uiDataGlobal.displayQSOState = QSO_DISPLAY_IDLE;
		return;
	}

	// Only render the header, then wait for the next run
	// Otherwise the screen could remain blank if TG and PC are == 0
	// since uiDataGlobal.displayQSOState won't be set to QSO_DISPLAY_IDLE
	if ((trxGetMode() == RADIO_MODE_DIGITAL) && (HRC6000GetReceivedTgOrPcId() == 0) &&
			((uiDataGlobal.displayQSOState == QSO_DISPLAY_CALLER_DATA) || (uiDataGlobal.displayQSOState == QSO_DISPLAY_CALLER_DATA_UPDATE)))
	{
		uiUtilityRedrawHeaderOnly(uiVFOModeDualWatchIsScanning(), uiVFOModeSweepScanning(true), false);
		return;
	}

	// We're currently displaying details or entering scan freq limits, and it shouldn't be overridden by QSO data
	if ((uiDataGlobal.displayChannelSettings ||
			((screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN) && (uiDataGlobal.FreqEnter.index != 0)))
			&& ((uiDataGlobal.displayQSOState == QSO_DISPLAY_CALLER_DATA) || (uiDataGlobal.displayQSOState == QSO_DISPLAY_CALLER_DATA_UPDATE)))
	{
		// We will not restore the previous QSO Data as a new caller just arose.
		uiDataGlobal.displayQSOStatePrev = QSO_DISPLAY_DEFAULT_SCREEN;
		uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
	}

	displayClearBuf();
	uiUtilityRenderHeader(uiVFOModeDualWatchIsScanning(), uiVFOModeSweepScanning(true),
#if defined(PLATFORM_MD9600) || defined(CPU_MK22FN512VLL12)
			false
#else
			((buttonsRead() & BUTTON_SK1) == BUTTON_SK1)
#endif
	);

	switch(uiDataGlobal.displayQSOState)
	{
		case QSO_DISPLAY_DEFAULT_SCREEN:
			lastHeardClearLastID();
			if ((uiDataGlobal.Scan.active &&
					(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_DUAL_SCAN) && (uiDataGlobal.Scan.state == SCAN_STATE_SCANNING)))
			{
				uiUtilityDisplayFrequency(DISPLAY_Y_POS_RX_FREQ, false, false, settingsVFOChannel[CHANNEL_VFO_A].rxFreq, true, true, 1);
				uiUtilityDisplayFrequency(DISPLAY_Y_POS_TX_FREQ, false, false, settingsVFOChannel[CHANNEL_VFO_B].rxFreq, true, true, 2);
			}
			else
			{
				uiDataGlobal.displayQSOStatePrev = QSO_DISPLAY_DEFAULT_SCREEN;
				uiDataGlobal.isDisplayingQSOData = false;
				uiDataGlobal.receivedPcId = 0x00;

				if (trxGetMode() == RADIO_MODE_DIGITAL)
				{
					if (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP)
					{
						if (uiDataGlobal.displayChannelSettings)
						{
							uint32_t PCorTG = ((nonVolatileSettings.overrideTG != 0) ? nonVolatileSettings.overrideTG : codeplugContactGetPackedId(&currentContactData));

							snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%s %u",
									(((PCorTG >> 24) == PC_CALL_FLAG) ? currentLanguage->pc : currentLanguage->tg),
									(PCorTG & 0xFFFFFF));
						}
						else
						{
							if ((trxTransmissionEnabled == false) && (aprsBeaconingIsTransmitting() == false) &&
									(uiDataGlobal.Scan.active == false) &&
									((nonVolatileSettings.lastTalkerOnScreenTimer > 0U) &&
											ticksTimerHasExpired(&uiDataGlobal.DMRLastTalkerOnScreen.timer))
							)
							{
								ticksTimerStart(&uiDataGlobal.DMRLastTalkerOnScreen.timer, (nonVolatileSettings.lastTalkerOnScreenTimer * 1000U));
								uiDataGlobal.DMRLastTalkerOnScreen.visible = (((audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF) || (uiDataGlobal.lastHeardCount < 1)) ? false : !uiDataGlobal.DMRLastTalkerOnScreen.visible);

								if (uiDataGlobal.DMRLastTalkerOnScreen.visible)
								{
									if (uiDataGlobal.lastHeardCount > 0)
									{
										bool displayTA = false;
										LinkItem_t *item = LinkHead;

										switch (nonVolatileSettings.contactDisplayPriority)
										{
											case CONTACT_DISPLAY_PRIO_CC_DB_TA:
											case CONTACT_DISPLAY_PRIO_DB_CC_TA:
												// No contact found in codeplug and DMRIDs, use TA as fallback, if any.
												if ((strncmp(item->contact, "ID:", 3) == 0) && (item->talkerAlias[0] != 0x00))
												{
													displayTA = true;
												}
												break;

											case CONTACT_DISPLAY_PRIO_TA_CC_DB:
											case CONTACT_DISPLAY_PRIO_TA_DB_CC:
												if (item->talkerAlias[0] != 0x00)
												{
													displayTA = true;
												}
												break;
										}

										if (displayTA)
										{
											getCallsignOnly(buffer, item->talkerAlias);
										}
										else
										{
											getCallsignOnly(buffer, item->contact);
										}
									}
									else
									{
										goto displayContactName;
									}
								}
								else
								{
									goto displayContactName;
								}
							}
							else
							{
								displayContactName:

								if (nonVolatileSettings.overrideTG != 0)
								{
									uiUtilityBuildTgOrPCDisplayName(buffer, SCREEN_LINE_BUFFER_SIZE);
									uiUtilityDisplayInformation(NULL, DISPLAY_INFO_CONTACT_OVERRIDE_FRAME, (isTransmitting ? DISPLAY_Y_POS_CONTACT_TX_FRAME : -1));
								}
								else
								{
									codeplugUtilConvertBufToString(currentContactData.name, buffer, 16);
								}
							}
						}

						uiUtilityDisplayInformation(buffer, DISPLAY_INFO_CONTACT, (isTransmitting ? DISPLAY_Y_POS_CONTACT_TX : -1));
					}
				}
				else
				{
					if (ticksTimerIsEnabled(&uiDataGlobal.DMRLastTalkerOnScreen.timer))
					{
						ticksTimerReset(&uiDataGlobal.DMRLastTalkerOnScreen.timer);
						uiDataGlobal.DMRLastTalkerOnScreen.visible = false;
					}

					// Display some channel settings
					if (uiDataGlobal.displayChannelSettings && (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP))
					{
						uiUtilityDisplayInformation(NULL, DISPLAY_INFO_CHANNEL_DETAILS, -1);
					}

					if(uiDataGlobal.Scan.toneActive)
					{
						if (toneScanType == CSS_TYPE_CTCSS)
						{
							snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "CTCSS %3d.%dHz", currentChannelData->rxTone / 10, currentChannelData->rxTone % 10);
						}
						else if (toneScanType & CSS_TYPE_DCS)
						{
							dcsPrintf(buffer, SCREEN_LINE_BUFFER_SIZE, "DCS ", currentChannelData->rxTone);
						}
						else
						{
							snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%s", "TONE ERROR");
						}

						uiUtilityDisplayInformation(buffer, DISPLAY_INFO_CONTACT, -1);
					}

				}

				if (uiDataGlobal.FreqEnter.index == 0)
				{
					if (!isTransmitting)
					{
						uiUtilityDisplayFrequency(((screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP) ? DISPLAY_Y_POS_TX_FREQ : DISPLAY_Y_POS_RX_FREQ),
								false, (selectedFreq == VFO_SELECTED_FREQUENCY_INPUT_RX),
								(uiDataGlobal.reverseRepeaterVFO ? currentChannelData->txFreq : currentChannelData->rxFreq), true,
								(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN), 0);
					}
					else
					{
						snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, " %d ", txTimeSecs);
						uiUtilityDisplayInformation(buffer, DISPLAY_INFO_TX_TIMER, -1);
					}

					if (((screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_NORMAL) ||
							(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_DUAL_SCAN) ||
							(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP)) || trxTransmissionEnabled)
					{
						if (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP)
						{
							uiUtilityDisplayFrequency(DISPLAY_Y_POS_TX_FREQ, true, (selectedFreq == VFO_SELECTED_FREQUENCY_INPUT_TX || trxTransmissionEnabled),
									(uiDataGlobal.reverseRepeaterVFO ? currentChannelData->rxFreq : currentChannelData->txFreq), true, false, 0);
						}
					}
					else
					{
						// Low/High scanning freqs
						snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%u.%03u", nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber] / 100000, (nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber] - (nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber] / 100000) * 100000)/100);

						displayPrintAt(2, DISPLAY_Y_POS_TX_FREQ, buffer, FONT_SIZE_3);

						snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%u.%03u", nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber] / 100000, (nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber] - (nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber] / 100000) * 100000)/100);

						displayPrintAt(DISPLAY_SIZE_X - ((7 * 8) + 2), DISPLAY_Y_POS_TX_FREQ, buffer, FONT_SIZE_3);
						// Scanning direction arrow
						static const int16_t scanDirArrow[2][6] = {
								{ // Down
										59 + DISPLAY_H_OFFSET, (DISPLAY_Y_POS_TX_FREQ + (FONT_SIZE_3_HEIGHT / 2) - 1),
										67 + DISPLAY_H_OFFSET, (DISPLAY_Y_POS_TX_FREQ + (FONT_SIZE_3_HEIGHT / 2) - (FONT_SIZE_3_HEIGHT / 4) - 1),
										67 + DISPLAY_H_OFFSET, (DISPLAY_Y_POS_TX_FREQ + (FONT_SIZE_3_HEIGHT / 2) + (FONT_SIZE_3_HEIGHT / 4) - 1)
								}, // Up
								{
										59 + DISPLAY_H_OFFSET, (DISPLAY_Y_POS_TX_FREQ + (FONT_SIZE_3_HEIGHT / 2) + (FONT_SIZE_3_HEIGHT / 4) - 1),
										59 + DISPLAY_H_OFFSET, (DISPLAY_Y_POS_TX_FREQ + (FONT_SIZE_3_HEIGHT / 2) - (FONT_SIZE_3_HEIGHT / 4) - 1),
										67 + DISPLAY_H_OFFSET, (DISPLAY_Y_POS_TX_FREQ + (FONT_SIZE_3_HEIGHT / 2) - 1)
								}
						};

						displayFillTriangle(scanDirArrow[(uiDataGlobal.Scan.direction > 0)][0], scanDirArrow[(uiDataGlobal.Scan.direction > 0)][1],
								scanDirArrow[(uiDataGlobal.Scan.direction > 0)][2], scanDirArrow[(uiDataGlobal.Scan.direction > 0)][3],
								scanDirArrow[(uiDataGlobal.Scan.direction > 0)][4], scanDirArrow[(uiDataGlobal.Scan.direction > 0)][5], true);
					}
				}
				else // Entering digits
				{
					int16_t xCursor = -1;
					int16_t yCursor = -1;
#if defined(HAS_COLOURS)
					bool dblHeight = settingsIsOptionBitSet(BIT_UI_USES_DOUBLE_HEIGHT);
#else
					const bool dblHeight = false;
#endif

					if ((screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_NORMAL) ||
							(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_DUAL_SCAN))
					{
						snprintf(buffer, SCREEN_LINE_BUFFER_SIZE,
#if defined(PLATFORM_RD5R)
								"%c%c%c.%c%c%c%c%c",
#else
								"%c%c%c.%c%c%c%c%c MHz",
#endif
								uiDataGlobal.FreqEnter.digits[0], uiDataGlobal.FreqEnter.digits[1], uiDataGlobal.FreqEnter.digits[2],
								uiDataGlobal.FreqEnter.digits[3], uiDataGlobal.FreqEnter.digits[4], uiDataGlobal.FreqEnter.digits[5], uiDataGlobal.FreqEnter.digits[6], uiDataGlobal.FreqEnter.digits[7]);

						displayPrintCenteredDoubleHeight((dblHeight ? DISPLAY_Y_POS_RX_FREQ : ((selectedFreq == VFO_SELECTED_FREQUENCY_INPUT_TX) ? DISPLAY_Y_POS_TX_FREQ : DISPLAY_Y_POS_RX_FREQ)), buffer, FONT_SIZE_3, dblHeight);

						// Cursor
						if (uiDataGlobal.FreqEnter.index < 8)
						{
							xCursor = ((DISPLAY_SIZE_X - (strlen(buffer) * 8)) >> 1) + ((uiDataGlobal.FreqEnter.index + ((uiDataGlobal.FreqEnter.index > 2) ? 1 : 0)) * 8);
							yCursor = (dblHeight ?
									DISPLAY_Y_POS_RX_FREQ :
									((selectedFreq == VFO_SELECTED_FREQUENCY_INPUT_TX) ? DISPLAY_Y_POS_TX_FREQ : DISPLAY_Y_POS_RX_FREQ))
											+ ((FONT_SIZE_3_HEIGHT * (dblHeight ? 2 : 1)) - 2);
						}
					}
					else
					{
						uint16_t hiX = DISPLAY_SIZE_X - ((7 * 8) + 2) - (DISPLAY_H_OFFSET / 2);
						int labelsVOffset =
#if defined(PLATFORM_RD5R)
								4;
#else
						0;
#endif
#if defined(PLATFORM_VARIANT_DM1701)
						uint16_t dm1701VOffset = (dblHeight ? 8 : 0);
#else
						const uint16_t dm1701VOffset = 0;
#endif

#if defined(HAS_COLOURS)
						if (dblHeight)
						{
							labelsVOffset += (FONT_SIZE_3_HEIGHT / 2);
						}
#endif

						displayPrintAt(5 + (DISPLAY_H_OFFSET / 2), DISPLAY_Y_POS_RX_FREQ - labelsVOffset - dm1701VOffset, currentLanguage->low, FONT_SIZE_3);
						displayDrawFastVLine(0 + (DISPLAY_H_OFFSET / 2), DISPLAY_Y_POS_RX_FREQ - labelsVOffset - dm1701VOffset, DISPLAY_SIZE_Y - (DISPLAY_Y_POS_RX_FREQ - labelsVOffset), true);
						displayDrawFastHLine(1 + (DISPLAY_H_OFFSET / 2), DISPLAY_Y_POS_TX_FREQ - (labelsVOffset / 2) - dm1701VOffset, 57, true);

						sprintf(buffer, "%c%c%c.%c%c%c", uiDataGlobal.FreqEnter.digits[0], uiDataGlobal.FreqEnter.digits[1], uiDataGlobal.FreqEnter.digits[2],
								uiDataGlobal.FreqEnter.digits[3], uiDataGlobal.FreqEnter.digits[4], uiDataGlobal.FreqEnter.digits[5]);

						displayPrintAtDoubleHeight(2 + (DISPLAY_H_OFFSET / 2), DISPLAY_Y_POS_TX_FREQ + (DISPLAY_V_EXTRA_PIXELS / 8) - (dblHeight ? (FONT_SIZE_3_HEIGHT / 2) : 0) - dm1701VOffset, buffer, FONT_SIZE_3, dblHeight);

						displayPrintAt(73 + ((DISPLAY_H_OFFSET / 2) * 3), DISPLAY_Y_POS_RX_FREQ - labelsVOffset - dm1701VOffset, currentLanguage->high, FONT_SIZE_3);
						displayDrawFastVLine(68 + ((DISPLAY_H_OFFSET / 2) * 3), DISPLAY_Y_POS_RX_FREQ - labelsVOffset - dm1701VOffset, DISPLAY_SIZE_Y - (DISPLAY_Y_POS_RX_FREQ - labelsVOffset), true);
						displayDrawFastHLine(69 + ((DISPLAY_H_OFFSET / 2) * 3), DISPLAY_Y_POS_TX_FREQ - (labelsVOffset / 2) - dm1701VOffset, 57, true);

						sprintf(buffer, "%c%c%c.%c%c%c", uiDataGlobal.FreqEnter.digits[6], uiDataGlobal.FreqEnter.digits[7], uiDataGlobal.FreqEnter.digits[8],
								uiDataGlobal.FreqEnter.digits[9], uiDataGlobal.FreqEnter.digits[10], uiDataGlobal.FreqEnter.digits[11]);

						displayPrintAtDoubleHeight(hiX, DISPLAY_Y_POS_TX_FREQ + (DISPLAY_V_EXTRA_PIXELS / 8) - (dblHeight ? (FONT_SIZE_3_HEIGHT / 2) : 0) - dm1701VOffset, buffer, FONT_SIZE_3, dblHeight);

						// Cursor
						if (uiDataGlobal.FreqEnter.index < FREQ_ENTER_DIGITS_MAX)
						{
							xCursor = ((uiDataGlobal.FreqEnter.index < 6) ? 10 + (DISPLAY_H_OFFSET / 2) : hiX) // X start
												+ (((uiDataGlobal.FreqEnter.index < 6) ? (uiDataGlobal.FreqEnter.index - 1) : (uiDataGlobal.FreqEnter.index - 7)) * 8) // Length
												+ ((uiDataGlobal.FreqEnter.index > 2 ? (uiDataGlobal.FreqEnter.index > 8 ? 2 : 1) : 0) * 8); // MHz/kHz separator(s)

							yCursor = DISPLAY_Y_POS_TX_FREQ + (FONT_SIZE_3_HEIGHT - 2) + (DISPLAY_V_EXTRA_PIXELS / 8) + (dblHeight ? (FONT_SIZE_3_HEIGHT / 2) : 0) - dm1701VOffset;
						}
					}

					if ((xCursor >= 0) && (yCursor >= 0))
					{
						displayDrawFastHLine(xCursor + 1, yCursor, 6, blink);

						if ((ticksGetMillis() - blinkTime) > 500)
						{
							blinkTime = ticksGetMillis();
							blink = !blink;
						}
					}

				}
			}
			displayRender();
			break;

		case QSO_DISPLAY_CALLER_DATA:
		case QSO_DISPLAY_CALLER_DATA_UPDATE:
			uiDataGlobal.displayQSOStatePrev = QSO_DISPLAY_CALLER_DATA;
			uiDataGlobal.isDisplayingQSOData = true;
			uiDataGlobal.displayChannelSettings = false;
			uiUtilityRenderQSOData();
			displayRender();
			break;

		case QSO_DISPLAY_IDLE:
			break;
	}

	uiDataGlobal.displayQSOState = QSO_DISPLAY_IDLE;
}

bool uiVFOModeIsTXFocused(void)
{
	return (selectedFreq == VFO_SELECTED_FREQUENCY_INPUT_TX);
}

void uiVFOModeStopScanning(void)
{
	bool resetAPRS = true;

#if defined(ENABLE_FAST_SCAN)
	// Leaving the sweep while parked on a signal must not leave the audio amplifier
	// enabled behind us -- nothing calls trxCheckAnalogSquelch() to close it once
	// Scan.active goes false.
	vfoSweepStopListening();
#endif

#if defined(ENABLE_FAST_SCAN) || defined(ENABLE_SPECTRUM)
	/* Put the RSSI filter back, whichever of the scan or the sweep set it.
	 *
	 * Unconditional on purpose: this is the single exit for both, and the conditions that
	 * chose the value on the way in (analog, reject enabled, sweep vs scan) can all have
	 * changed while scanning. Testing them again here would be a second chance to get the
	 * bracket wrong, and the failure it protects against is silent -- a non-stock
	 * rssi_ct_u left behind does not break reception, it just makes the S-meter twitchier
	 * than the user set it to, which is exactly the kind of thing nobody traces back.
	 * The write costs nothing when the value is already stock: radioWriteReg2byte() drops
	 * it on the register cache. */
	radioSetRssiCountDefault();
#endif

	if (uiDataGlobal.Scan.toneActive)
	{
		if (prevCSSTone != (CODEPLUG_CSS_TONE_NONE - 1))
		{
			currentChannelData->rxTone = prevCSSTone;
			prevCSSTone = (CODEPLUG_CSS_TONE_NONE - 1);
		}

		trxSetRxCSS(RADIO_DEVICE_PRIMARY, currentChannelData->rxTone);
		uiDataGlobal.Scan.toneActive = false;
		trxSetAnalogFilterLevel(nonVolatileSettings.analogFilterLevel);// Restore the filter setting after the tone scan
		resetAPRS = false;
	}

	uiDataGlobal.Scan.active = false;
	uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;

	if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_DUAL_SCAN)
	{
		screenOperationMode[CHANNEL_VFO_A] = screenOperationMode[CHANNEL_VFO_B] = VFO_SCREEN_OPERATION_NORMAL;
		settingsSet(nonVolatileSettings.currentVFONumber, nonVolatileSettings.currentVFONumber);

		rxPowerSavingSetLevel(nonVolatileSettings.ecoLevel);// Level is reduced by 1 when Dual Watch , so re-instate it back to the correct setting
	}
	else if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP)
	{
		screenOperationMode[nonVolatileSettings.currentVFONumber] = VFO_SCREEN_OPERATION_NORMAL;
		trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
		HRC6000ClearColorCodeSynchronisation();
	}

	announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ, PROMPT_THRESHOLD_3);
	uiVFOModeUpdateScreen(0); // Needs to redraw the screen now

	if (resetAPRS)
	{
		aprsBeaconingResetTimers();
	}
}

static void updateFrequency(int frequency, bool announceImmediately)
{
	if (selectedFreq == VFO_SELECTED_FREQUENCY_INPUT_TX)
	{
		if (trxGetBandFromFrequency(frequency) != FREQUENCY_OUT_OF_BAND)
		{
			currentChannelData->txFreq = frequency;
			trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
			soundSetMelody(MELODY_ACK_BEEP);
		}
	}
	else
	{
		int deltaFrequency = frequency - currentChannelData->rxFreq;
		if (trxGetBandFromFrequency(frequency) != FREQUENCY_OUT_OF_BAND)
		{
			currentChannelData->rxFreq = frequency;
			currentChannelData->txFreq = currentChannelData->txFreq + deltaFrequency;
			trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));

			if (trxGetBandFromFrequency(currentChannelData->txFreq) != FREQUENCY_OUT_OF_BAND)
			{
				soundSetMelody(MELODY_ACK_BEEP);
			}
			else
			{
				currentChannelData->txFreq = frequency;
				soundSetMelody(MELODY_ERROR_BEEP);
			}
		}
		else
		{
			soundSetMelody(MELODY_ERROR_BEEP);
		}
	}
	announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ, announceImmediately);

	menuPrivateCallClear();
	settingsSetVFODirty();
}

void uiVFOModeLoadChannelData(bool forceAPRSReset)
{
	trxSetModeAndBandwidth(currentChannelData->chMode, (codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_BW_25K) != 0));

	if (currentChannelData->chMode == RADIO_MODE_ANALOG)
	{
		CodeplugAPRSConfig_t aprsConfig;
		bool aprsConfigUnsetOrQSY = (currentChannelData->aprsConfigIndex == 0) ||
				(codeplugAPRSGetDataForIndex(currentChannelData->aprsConfigIndex, &aprsConfig) && (aprsConfig.txFrequency != 0U));

		if (aprsConfigUnsetOrQSY == false)
		{
			currentChannelData->rxTone = CODEPLUG_CSS_TONE_NONE;
			currentChannelData->txTone = CODEPLUG_CSS_TONE_NONE;
			codeplugChannelSetFlag(currentChannelData, CHANNEL_FLAG_VOX, 0);
		}

		if (!uiDataGlobal.Scan.toneActive)
		{
			trxSetRxCSS(RADIO_DEVICE_PRIMARY, currentChannelData->rxTone);
		}

		if (uiDataGlobal.Scan.active == false)
		{
			uiDataGlobal.Scan.state = SCAN_STATE_SCANNING;
		}
	}
	else
	{
		uint32_t channelDMRId = codeplugChannelGetOptionalDMRID(currentChannelData);

		if (uiDataGlobal.manualOverrideDMRId == 0)
		{
			if (channelDMRId == 0)
			{
				trxDMRID = uiDataGlobal.userDMRId;
			}
			else
			{
				trxDMRID = channelDMRId;
			}
		}
		else
		{
			trxDMRID = uiDataGlobal.manualOverrideDMRId;
		}

		// Set CC when:
		//  - scanning
		//  - CC Filter is ON
		//  - CC Filter is OFF but not held anymore or loading a new channel (this avoids restoring Channel's CC when releasing the PTT key, or getting out of menus)
		if (uiDataGlobal.Scan.active ||
				((nonVolatileSettings.dmrCcTsFilter & DMR_CC_FILTER_PATTERN) ||
						(((nonVolatileSettings.dmrCcTsFilter & DMR_CC_FILTER_PATTERN) == 0)
								&& ((HRC6000CCIsHeld() == false) || (previousVFONumber != nonVolatileSettings.currentVFONumber)))))
		{
			trxSetDMRColourCode(currentChannelData->txColor);
			HRC6000ClearColorCodeSynchronisation();
		}

		if (nonVolatileSettings.overrideTG == 0)
		{
			uiVFOLoadContact(&currentContactData);

			// Check whether the contact data seems valid
			if ((currentContactData.name[0] == 0) || (currentContactData.tgNumber == 0) || (currentContactData.tgNumber > 9999999))
			{
				settingsSet(nonVolatileSettings.overrideTG, 9);// If the VFO does not have an Rx Group list assigned to it. We can't get a TG from the codeplug. So use TG 9.
				trxTalkGroupOrPcId = nonVolatileSettings.overrideTG;
				trxSetDMRTimeSlot(codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_TIMESLOT_TWO), true);
				tsSetContactHasBeenOverriden(((Channel_t)nonVolatileSettings.currentVFONumber), false);
			}
			else
			{
				trxTalkGroupOrPcId = codeplugContactGetPackedId(&currentContactData);
				trxUpdateTsForCurrentChannelWithSpecifiedContact(&currentContactData);
			}
		}
		else
		{
			int manTS = tsGetManualOverrideFromCurrentChannel();

			trxTalkGroupOrPcId = nonVolatileSettings.overrideTG;
			trxSetDMRTimeSlot((manTS ? (manTS - 1) : codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_TIMESLOT_TWO)), true);
		}
	}

	if (uiDataGlobal.Scan.active == false)
	{
		if (forceAPRSReset || ((previousVFONumber != nonVolatileSettings.currentVFONumber) && (currentChannelData->chMode == RADIO_MODE_ANALOG)))
		{
			aprsBeaconingResetTimers();
		}
	}

	previousVFONumber = nonVolatileSettings.currentVFONumber;
}

static void checkAndFixIndexInRxGroup(void)
{
	if ((currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup > 0) &&
			(nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber] > (currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup - 1)))
	{
		settingsSet(nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber], 0);
	}
}

void uiVFOLoadContact(CodeplugContact_t *contact)
{
	// Check if this channel has an Rx Group
	if ((currentRxGroupData.name[0] != 0) && (nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber] < currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup))
	{
		codeplugContactGetDataForIndex(currentRxGroupData.contacts[nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber]], contact);
	}
	else
	{
		/* 2020.10.27 vk3kyy. The Contact should not be forced to none just because the Rx group list is none
		if (currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup == 0)
		{
			currentChannelData->contact = 0;
		}*/

		codeplugContactGetDataForIndex(currentChannelData->contact, contact);
	}
}

static void toggleAnalogBandwidth(void)
{
	uint8_t bw25k = codeplugChannelSetFlag(currentChannelData, CHANNEL_FLAG_BW_25K, !(codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_BW_25K)));

	if (bw25k)
	{
		nextKeyBeepMelody = (int16_t *)MELODY_KEY_BEEP_FIRST_ITEM;
	}

	// ToDo announce VP for bandwidth perhaps
	trxSetModeAndBandwidth(RADIO_MODE_ANALOG, (bw25k != 0));
}

static void handleEvent(uiEvent_t *ev)
{
	if (uiDataGlobal.Scan.active && (ev->events & KEY_EVENT))
	{
		if (BUTTONCHECK_DOWN(ev, BUTTON_SK2) == 0)
		{
#if defined(ENABLE_FAST_SCAN)
			// Same key, same meaning, in the sweep screen: skip the signal we are parked
			// on and carry on. Without it a persistent carrier holds the sweep until the
			// hang timer gives up, over and over, and the only way out is to leave the
			// screen entirely. STAR is otherwise unused while sweeping -- the nuisance
			// delete below excludes VFO_SCREEN_OPERATION_SWEEP.
			// STAR with the sweep running and nothing parked: toggle the auto scroll.
			// It is the only key already exempt from the "any key stops the scan" barrier
			// below and otherwise unused on this screen, so it needs no new exemption --
			// and a control that stopped the sweep in order to configure the sweep would
			// be useless here.
			if ((vfoSweepListening == false) && (ev->keys.key == KEY_STAR) &&
					(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP))
			{
				// Cycles off -> scroll -> scroll with dwell -> off. A third state rather
				// than a second key: the two differ only in how long each window is
				// listened to, so they belong on one control, and this keypad has nothing
				// spare anyway.
				if (vfoSweepAutoScroll == false)
				{
					vfoSweepAutoScroll = true;
					vfoSweepDwellPasses = 1;
				}
				else if (vfoSweepDwellPasses <= 1)
				{
					vfoSweepDwellPasses = VFO_SWEEP_DWELL_PASSES;
				}
				else
				{
					vfoSweepAutoScroll = false;
					vfoSweepDwellPasses = 1;
				}

				vfoSweepPassesDone = 0;
				uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
				headerRowIsDirty = true;
				keyboardReset();
				return;
			}

			if (vfoSweepListening && (ev->keys.key == KEY_STAR))
			{
				vfoSweepStopListening();
				vfoSweepDrawPeakMarker();
				displayRenderRows(1, ((8 + VFO_SWEEP_GRAPH_HEIGHT_Y) / 8) + 1);
				keyboardReset();
				return;
			}
#endif

			// Right key sets the current frequency as a 'nuisance' frequency.
			if((uiDataGlobal.Scan.state == SCAN_STATE_PAUSED) &&
#if defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380)
					(ev->keys.key == KEY_STAR)
#else
					(ev->keys.key == KEY_RIGHT)
#endif
					&&
					(screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_DUAL_SCAN)
					&& (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP))
			{
				uiDataGlobal.Scan.nuisanceDelete[uiDataGlobal.Scan.nuisanceDeleteIndex] = currentChannelData->rxFreq;
				uiDataGlobal.Scan.nuisanceDeleteIndex = (uiDataGlobal.Scan.nuisanceDeleteIndex + 1) % MAX_ZONE_SCAN_NUISANCE_CHANNELS;
				uiDataGlobal.Scan.timer.timeout = SCAN_SKIP_CHANNEL_INTERVAL;//force scan to continue;
				uiDataGlobal.Scan.state = SCAN_STATE_SCANNING;
				keyboardReset();
				return;
			}

			// Left key reverses the scan direction
			if (
#if defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380)
					(ev->keys.key == KEY_FRONT_DOWN)
#else
					(ev->keys.key == KEY_LEFT)
#endif
					&&
					(screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_DUAL_SCAN)
					&& (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP))
			{
				uiDataGlobal.Scan.direction *= -1;
				keyboardReset();
				return;
			}
		}

		// Stop the scan on any key except UP/ROTARY_INC without SK2 (allows scan to be manually continued)
		// or SK2 on its own (allows Backlight to be triggered)
		if (
#if defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				(((
						(ev->keys.key == KEY_FRONT_UP)
#if defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
						|| (ev->keys.key == KEY_ROTARY_INCREMENT)
#endif
					) && (BUTTONCHECK_DOWN(ev, BUTTON_SK2) == 0) &&
						(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN)) == false)
				&&
				((((ev->keys.key == KEY_ROTARY_INCREMENT) || (ev->keys.key == KEY_ROTARY_DECREMENT) || (ev->keys.key == KEY_FRONT_UP) || (ev->keys.key == KEY_FRONT_DOWN)
#if defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
						|| (ev->keys.key == KEY_LEFT) || (ev->keys.key == KEY_RIGHT) || (ev->keys.key == KEY_UP) || (ev->keys.key == KEY_DOWN)
#endif
						|| (ev->keys.key == KEY_STAR)) &&
						(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP)) == false)

#else // NOT STM32 PLATFORMS, a.k.a MK22 HTs, and MD9600
				(((ev->keys.key == KEY_UP) && (BUTTONCHECK_DOWN(ev, BUTTON_SK2) == 0) &&
						(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN)) == false)
				&&
						((((ev->keys.key == KEY_LEFT) || (ev->keys.key == KEY_RIGHT) || (ev->keys.key == KEY_UP) || (ev->keys.key == KEY_DOWN) || (ev->keys.key == KEY_STAR)) &&
								(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP)) == false)
#endif
		)
		{
			uiVFOModeStopScanning();
			keyboardReset();
			announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ, PROMPT_THRESHOLD_3);
			return;
		}
	}

	if (ev->events & FUNCTION_EVENT)
	{
		if (ev->function == FUNC_START_SCANNING)
		{
			scanInit();
			setCurrentFreqToScanLimits();
			uiDataGlobal.Scan.active = true;
			return;
		}
		else if (ev->function == FUNC_REDRAW)
		{
			uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			uiVFOModeUpdateScreen(0);
			return;
		}
	}

	if (handleMonitorMode(ev))
	{
		uiDataGlobal.displayChannelSettings = false;
		uiDataGlobal.reverseRepeaterVFO = false;
		return;
	}

	if (ev->events & BUTTON_EVENT)
	{

#if ! defined(PLATFORM_RD5R)
		// Stop the scan if any button is pressed.
		if (uiDataGlobal.Scan.active && BUTTONCHECK_DOWN(ev, BUTTON_ORANGE))
		{
			uiVFOModeStopScanning();
			return;
		}
#endif

		if (rebuildVoicePromptOnExtraLongSK1(ev))
		{
			return;
		}

		if (repeatVoicePromptOnSK1(ev))
		{
			return;
		}

		uint32_t tg = (LinkHead->talkGroupOrPcId & 0xFFFFFF);

		// If Blue button is pressed during reception it sets the Tx TG to the incoming TG
		if (uiDataGlobal.isDisplayingQSOData && BUTTONCHECK_SHORTUP(ev, BUTTON_SK2) && (trxGetMode() == RADIO_MODE_DIGITAL) &&
				((trxTalkGroupOrPcId != tg) ||
						((dmrMonitorCapturedTS != -1) && (dmrMonitorCapturedTS != trxGetDMRTimeSlot())) ||
						(trxGetDMRColourCode() != currentChannelData->txColor)))
		{
			lastHeardClearLastID();

			// Set TS to overriden TS
			if ((dmrMonitorCapturedTS != -1) && (dmrMonitorCapturedTS != trxGetDMRTimeSlot()))
			{
				trxSetDMRTimeSlot(dmrMonitorCapturedTS, false);
				tsSetManualOverride(((Channel_t)nonVolatileSettings.currentVFONumber), (dmrMonitorCapturedTS + 1));
			}

			if (trxTalkGroupOrPcId != tg)
			{
				trxTalkGroupOrPcId = tg;
				settingsSet(nonVolatileSettings.overrideTG, trxTalkGroupOrPcId);
			}

			currentChannelData->txColor = trxGetDMRColourCode();// Set the CC to the current CC, which may have been determined by the CC finding algorithm in C6000.c

			announceItem(PROMPT_SEQUENCE_CONTACT_TG_OR_PC,PROMPT_THRESHOLD_NEVER_PLAY_IMMEDIATELY);

			uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			uiVFOModeUpdateScreen(0);
			uiDataGlobal.displayQSOState = QSO_DISPLAY_CALLER_DATA_UPDATE;
			soundSetMelody(MELODY_ACK_BEEP);
			return;
		}

		if ((uiVFOModeSweepScanning(true) == false) && (monitorModeData.isEnabled == false) && (uiDataGlobal.reverseRepeaterVFO == false) && (BUTTONCHECK_DOWN(ev, BUTTON_SK1) && BUTTONCHECK_DOWN(ev, BUTTON_SK2)))
		{
			int prevQSODisp = -1;

			trxSetFrequency(currentChannelData->txFreq, currentChannelData->rxFreq, DMR_MODE_DMO);// Swap Tx and Rx freqs but force DMR Active mode
			uiDataGlobal.reverseRepeaterVFO = true;
			uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;

			if (uiDataGlobal.displayChannelSettings == false)
			{
				prevQSODisp = uiDataGlobal.displayQSOStatePrev;
				uiDataGlobal.displayChannelSettings = true;
				headerRowIsDirty = true;
			}

			uiVFOModeUpdateScreen(0);

			if (prevQSODisp != -1)
			{
				uiDataGlobal.displayQSOStatePrev = prevQSODisp;
			}
			return;
		}
		else if (uiDataGlobal.reverseRepeaterVFO && ((BUTTONCHECK_DOWN(ev, BUTTON_SK1) == 0) || (BUTTONCHECK_DOWN(ev, BUTTON_SK2) == 0)))
		{
			trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
			uiDataGlobal.reverseRepeaterVFO = false;

			// We are still displaying channel details (SK1 has been released), force to update the screen
			if (uiDataGlobal.displayChannelSettings && (BUTTONCHECK_DOWN(ev, BUTTON_SK1) == 0))
			{
				uiDataGlobal.displayChannelSettings = false;
			}

			uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			uiVFOModeUpdateScreen(0);
			return;
		}
		// Display channel settings (CTCSS, Squelch) while SK1 is pressed
		else if ((uiVFOModeSweepScanning(true) == false) && (monitorModeData.isEnabled == false) && (uiDataGlobal.displayChannelSettings == false) && BUTTONCHECK_DOWN(ev, BUTTON_SK1))
		{
			int prevQSODisp = uiDataGlobal.displayQSOStatePrev;

			uiDataGlobal.displayChannelSettings = true;
			headerRowIsDirty = true;
			uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			uiVFOModeUpdateScreen(0);
			uiDataGlobal.displayQSOStatePrev = prevQSODisp;
			return;
		}
		else if (uiDataGlobal.displayChannelSettings && BUTTONCHECK_DOWN(ev, BUTTON_SK1) == 0)
		{
			uiDataGlobal.displayChannelSettings = false;
			uiDataGlobal.displayQSOState = uiDataGlobal.displayQSOStatePrev;

			// Maybe QSO State has been overridden, double check if we could now
			// display QSO Data
			if (uiDataGlobal.displayQSOState == QSO_DISPLAY_DEFAULT_SCREEN)
			{
				if (isQSODataAvailableForCurrentTalker())
				{
					uiDataGlobal.displayQSOState = QSO_DISPLAY_CALLER_DATA;
				}
			}

			// Leaving Channel Details disable reverse repeater feature
			if (uiDataGlobal.reverseRepeaterVFO)
			{
				trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
				uiDataGlobal.reverseRepeaterVFO = false;
			}

			uiVFOModeUpdateScreen(0);
			return;
		}

#if !defined(PLATFORM_RD5R)
		if (BUTTONCHECK_SHORTUP(ev, BUTTON_ORANGE))
		{
			if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
			{
				announceItem(PROMPT_SEQUENCE_BATTERY, AUDIO_PROMPT_MODE_VOICE_LEVEL_1);
			}
			else
			{
				menuSystemPushNewMenu(UI_VFO_QUICK_MENU);

				// Trick to beep (AudioAssist), since ORANGE button doesn't produce any beep event
				ev->keys.event |= KEY_MOD_UP;
				ev->keys.key = 127;
				menuVFOExitStatus |= (MENU_STATUS_LIST_TYPE | MENU_STATUS_FORCE_FIRST);
				// End Trick
			}

			return;
		}
#endif
	}

	if (ev->events & KEY_EVENT)
	{
		int keyval = 99;

		if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
		{
			if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
			{
				menuSystemPushNewMenu(MENU_CHANNEL_DETAILS);
				freqEnterReset();
				return;
			}
			else
			{
				if (uiDataGlobal.FreqEnter.index == 0)
				{
					menuSystemPushNewMenu(MENU_MAIN_MENU);
					return;
				}
			}
		}

		if (uiDataGlobal.FreqEnter.index == 0)
		{
#if defined(PLATFORM_MD9600)
			if (KEYCHECK_LONGDOWN(ev->keys, KEY_GREEN))
			{
				if (uiDataGlobal.Scan.active)
				{
					uiVFOModeStopScanning();
				}

				menuSystemPushNewMenu(UI_VFO_QUICK_MENU);

				// Trick to beep (AudioAssist), since ORANGE button doesn't produce any beep event
				ev->keys.event |= KEY_MOD_UP;
				ev->keys.key = 127;
				menuVFOExitStatus |= (MENU_STATUS_LIST_TYPE | MENU_STATUS_FORCE_FIRST);
				// End Trick
				return;
			}
			else
#endif
			if (KEYCHECK_LONGDOWN(ev->keys, KEY_HASH) && (KEYCHECK_LONGDOWN_REPEAT(ev->keys, KEY_HASH) == false))
			{
				if (uiDataGlobal.Scan.active && (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP))
				{
					uiVFOModeStopScanning();
				}

				if (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP)
				{
					sweepScanInit();
					soundSetMelody(MELODY_KEY_LONG_BEEP);
				}
				return;
			}
			else if (KEYCHECK_SHORTUP(ev->keys, KEY_HASH))
			{
				if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
				{
					menuSystemPushNewMenu(MENU_CONTACT_QUICKLIST);
				}
				else
				{
					menuSystemPushNewMenu(MENU_NUMERICAL_ENTRY);
				}
				return;
			}
			else if (KEYCHECK_SHORTUP(ev->keys, KEY_STAR))
			{
				if (uiVFOModeSweepScanning(true) == false)
				{
					if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
					{
						if (trxGetMode() == RADIO_MODE_ANALOG)
						{
							currentChannelData->chMode = RADIO_MODE_DIGITAL;
							checkAndFixIndexInRxGroup();
							uiVFOModeLoadChannelData(true);
							updateTrxID();

							menuVFOExitStatus |= MENU_STATUS_FORCE_FIRST;
						}
						else
						{
							currentChannelData->chMode = RADIO_MODE_ANALOG;
							trxSetModeAndBandwidth(currentChannelData->chMode, (codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_BW_25K) != 0));
						}

						announceItem(PROMPT_SEQUENCE_MODE, PROMPT_THRESHOLD_1);
						uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
					}
					else
					{
						if (trxGetMode() == RADIO_MODE_DIGITAL)
						{
							// Toggle TimeSlot
							trxSetDMRTimeSlot(1 - trxGetDMRTimeSlot(), true);
							tsSetManualOverride(((Channel_t)nonVolatileSettings.currentVFONumber), (trxGetDMRTimeSlot() + 1));

							if ((nonVolatileSettings.overrideTG == 0) && (currentContactData.reserve1 & CODEPLUG_CONTACT_FLAG_NO_TS_OVERRIDE) == 0x00)
							{
								tsSetContactHasBeenOverriden(((Channel_t)nonVolatileSettings.currentVFONumber), true);
							}

							audioAmpDisable(AUDIO_AMP_CHANNEL_RF);
							lastHeardClearLastID();
							uiDataGlobal.displayQSOState = uiDataGlobal.displayQSOStatePrev;
							uiVFOModeUpdateScreen(0);

							if (trxGetDMRTimeSlot() == 0)
							{
								menuVFOExitStatus |= MENU_STATUS_FORCE_FIRST;
							}
							announceItem(PROMPT_SEQUENCE_TS,PROMPT_THRESHOLD_3);
						}
						else
						{
							toggleAnalogBandwidth();
							uiDataGlobal.displayQSOState = uiDataGlobal.displayQSOStatePrev;
							headerRowIsDirty = true;
							uiVFOModeUpdateScreen(0);
						}
					}
				}
				else
				{
					if (trxGetMode() == RADIO_MODE_ANALOG)
					{
						toggleAnalogBandwidth();
					}
				}
			}
			else if ((uiVFOModeSweepScanning(true) == false) && KEYCHECK_LONGDOWN(ev->keys, KEY_STAR))
			{
				if (trxGetMode() == RADIO_MODE_DIGITAL)
				{
					tsSetManualOverride(((Channel_t)nonVolatileSettings.currentVFONumber), TS_NO_OVERRIDE);
					tsSetContactHasBeenOverriden(((Channel_t)nonVolatileSettings.currentVFONumber), false);

					// Check if this channel has an Rx Group
					if ((currentRxGroupData.name[0] != 0) &&
							(nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber] < currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup))
					{
						codeplugContactGetDataForIndex(currentRxGroupData.contacts[nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber]], &currentContactData);
					}
					else
					{
						codeplugContactGetDataForIndex(currentChannelData->contact, &currentContactData);
					}

					trxUpdateTsForCurrentChannelWithSpecifiedContact(&currentContactData);

					lastHeardClearLastID();
					uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
					uiVFOModeUpdateScreen(0);
					announceItem(PROMPT_SEQUENCE_TS, PROMPT_THRESHOLD_1);
				}
			}
			else if(uiVFOModeSweepScanning(true) &&  // Reset Sweep noise floor or Sweep Gain
					((KEYCHECK_SHORTUP(ev->keys, KEY_DOWN) || KEYCHECK_SHORTUP(ev->keys, KEY_UP)
#if defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
							|| KEYCHECK_SHORTUP(ev->keys, KEY_ROTARY_INCREMENT) || KEYCHECK_SHORTUP(ev->keys, KEY_ROTARY_DECREMENT)
#if defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
							|| KEYCHECK_SHORTUP(ev->keys, KEY_FRONT_UP) || KEYCHECK_SHORTUP(ev->keys, KEY_FRONT_DOWN)
#endif
#endif
					)
							&& BUTTONCHECK_DOWN(ev, BUTTON_SK1)))
			{
				vfoSweepRssiNoiseFloor = VFO_SWEEP_RSSI_NOISE_FLOOR_DEFAULT;
				vfoSweepGain = VFO_SWEEP_GAIN_DEFAULT;
#if defined(ENABLE_FAST_SCAN)
				// This is already the "put the display back to a clean state" gesture, so
				// it is also where the max hold gets cleared and automatic scaling comes
				// back on. Saves inventing controls on a keypad that has none free.
				memset(vfoSweepHold, 0x00, sizeof(vfoSweepHold));
				vfoSweepAutoScale = true;
				vfoSweepAutoPrimed = false;
#endif
				settingsSet(nonVolatileSettings.vfoSweepSettings,
						VFO_SWEEP_SETTINGS_WORD(uiDataGlobal.Scan.sweepStepSizeIndex,
								vfoSweepRssiNoiseFloor, vfoSweepGain));
				vfoSweepUpdateSamples(0, true, 0);
			}
			else if (
#if defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
					KEYCHECK_SHORTUP(ev->keys, KEY_ROTARY_DECREMENT)
#if defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
					|| KEYCHECK_SHORTUP(ev->keys, KEY_FRONT_DOWN)
#endif
#else
					KEYCHECK_SHORTUP(ev->keys, KEY_DOWN) || KEYCHECK_LONGDOWN_REPEAT(ev->keys, KEY_DOWN)
#endif
			)
			{
#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380)
				if (BUTTONCHECK_DOWN(ev, BUTTON_SK2) && uiVFOModeSweepScanning(true))
				{
					setSweepIncDecSetting(SWEEP_SETTING_STEP, false);
					headerRowIsDirty = true;
				}
				else
				{
					handleDownKey(ev);
				}
				return;
#else
				if (uiVFOModeSweepScanning(true) == false)
				{
					handleDownKey(ev);
				}
				else
				{
					if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
					{
						// Sweep noise floor
						setSweepIncDecSetting(SWEEP_SETTING_RSSI, false);
					}
					else
					{
						// Sweep gain
						setSweepIncDecSetting(SWEEP_SETTING_GAIN, false);
					}
					return;
				}
#endif
			}
			else if (
#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
					KEYCHECK_LONGDOWN(ev->keys, KEY_FRONT_DOWN)
#else
					KEYCHECK_LONGDOWN(ev->keys, KEY_DOWN)
#endif
			)
			{
				if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN)
				{
					screenOperationMode[nonVolatileSettings.currentVFONumber] = VFO_SCREEN_OPERATION_NORMAL;
					nextKeyBeepMelody = (int16_t *)MELODY_ACK_BEEP;
					uiVFOModeStopScanning();
					return;
				}
			}
			else if (
#if defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
					KEYCHECK_SHORTUP(ev->keys, KEY_ROTARY_INCREMENT)
#if defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
					|| KEYCHECK_SHORTUP(ev->keys, KEY_FRONT_UP)
#endif
#else
					KEYCHECK_SHORTUP(ev->keys, KEY_UP) || KEYCHECK_LONGDOWN_REPEAT(ev->keys, KEY_UP)
#endif
			)
			{
#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380)
				if (BUTTONCHECK_DOWN(ev, BUTTON_SK2) && uiVFOModeSweepScanning(true))
				{
					setSweepIncDecSetting(SWEEP_SETTING_STEP, true);
					headerRowIsDirty = true;
				}
				else
				{
					handleUpKey(ev);
				}
				return;
#else
				if (uiVFOModeSweepScanning(true) == false)
				{
					handleUpKey(ev);
				}
				else
				{
					if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
					{
						// Sweep noise floor
						setSweepIncDecSetting(SWEEP_SETTING_RSSI, true);
					}
					else
					{
						// Sweep gain
						setSweepIncDecSetting(SWEEP_SETTING_GAIN, true);
					}
					return;
				}
#endif
			}
			else if (KEYCHECK_LONGDOWN(ev->keys,
#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
					KEY_FRONT_UP
#else
					KEY_UP
#endif
					) && (BUTTONCHECK_DOWN(ev, BUTTON_SK2) == 0))
			{
				if ((screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SCAN) &&
						(screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_DUAL_SCAN) &&
						(screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP))
				{
					scanInit();
					return;
				}
				else
				{
					if ((screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_DUAL_SCAN) &&
							(screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP))
					{
						setCurrentFreqToScanLimits();
						if (uiDataGlobal.Scan.active == false)
						{
#if defined(ENABLE_SPECTRUM)
							// Re-derive the step time here, not only in scanInit(). This is
							// the path taken on every long press after the first (once the
							// VFO scan screen is already up), and it computed dwellTime from
							// a Scan.stepTimeMilliseconds latched by the last scanInit() --
							// so any change to the scan step time since then was silently
							// ignored. Guarded to keep stock byte-identical, but this looks
							// like a real upstream bug worth fixing there too.
							uiDataGlobal.Scan.stepTimeMilliseconds = settingsGetScanStepTimeMilliseconds();
#endif
							// User maybe has change the mode, update.
							// In DIGITAL mode, we need at least 120ms to see the HR-C6000 to start the TS ISR.
							if (trxGetMode() == RADIO_MODE_DIGITAL)
							{
								int dwellTime;
								if(uiDataGlobal.Scan.stepTimeMilliseconds > 150)				// if >150ms use DMR Slow mode
								{
									dwellTime = ((currentRadioDevice->trxDMRModeRx == DMR_MODE_DMO) ? SCAN_DMR_SIMPLEX_SLOW_MIN_DWELL_TIME : SCAN_DMR_DUPLEX_SLOW_MIN_DWELL_TIME);
								}
								else
								{
									dwellTime = ((currentRadioDevice->trxDMRModeRx == DMR_MODE_DMO) ? SCAN_DMR_SIMPLEX_FAST_MIN_DWELL_TIME : SCAN_DMR_DUPLEX_FAST_MIN_DWELL_TIME);
								}

								uiDataGlobal.Scan.dwellTime = ((uiDataGlobal.Scan.stepTimeMilliseconds < dwellTime) ? dwellTime : uiDataGlobal.Scan.stepTimeMilliseconds);
							}
							else
							{
								uiDataGlobal.Scan.dwellTime = uiDataGlobal.Scan.stepTimeMilliseconds;
							}

							clearNuisance();

							uiDataGlobal.Scan.active = true;
							if (voicePromptsIsPlaying())
							{
								voicePromptsTerminate();
							}
							soundSetMelody(MELODY_KEY_LONG_BEEP);
							keyboardReset();
						}
					}
				}
			}
#if defined(PLATFORM_DM1801)
			else if (KEYCHECK_LONGDOWN(ev->keys, KEY_RED) && (KEYCHECK_LONGDOWN_REPEAT(ev->keys, KEY_RED) == false))
			{
				if (BUTTONCHECK_DOWN(ev, BUTTON_SK1))
				{
					uiChannelModeOrVFOModeThemeDaytimeChange(false, false);
					return;
				}
			}
			else if (KEYCHECK_SHORTUP(ev->keys, KEY_A_B))
			{
#else
			else if (KEYCHECK_LONGDOWN(ev->keys, KEY_RED) && (KEYCHECK_LONGDOWN_REPEAT(ev->keys, KEY_RED) == false))
			{
				if (BUTTONCHECK_DOWN(ev, BUTTON_SK1))
				{
					uiChannelModeOrVFOModeThemeDaytimeChange(false, false);
					return;
				}
				else
#endif
				{
					settingsSet(nonVolatileSettings.currentVFONumber, (1 - nonVolatileSettings.currentVFONumber));// Switch to other VFO
					currentChannelData = &settingsVFOChannel[nonVolatileSettings.currentVFONumber];
					uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;

					menuVFOExitStatus = MENU_STATUS_SUCCESS;
					menuSystemPopAllAndDisplayRootMenu(); // Force to set all TX/RX settings.

#if defined(PLATFORM_DM1801)
					if (nonVolatileSettings.currentVFONumber == 0)
#else
					if (nonVolatileSettings.currentVFONumber == 1) // Yes, inverted here, as the beep will apply to other VFO
#endif
					{
						menuVFOExitStatus |= MENU_STATUS_FORCE_FIRST;
					}
					else
					{
						menuVFOExitStatus = MENU_STATUS_SUCCESS;
					}
				}
				return;
			}
			else if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
			{
				if (BUTTONCHECK_DOWN(ev, BUTTON_SK1))
				{
					uiChannelModeOrVFOModeThemeDaytimeChange(true, false);
					return;
				}
				if (BUTTONCHECK_DOWN(ev, BUTTON_SK2) && (uiDataGlobal.tgBeforePcMode != 0))
				{
					settingsSet(nonVolatileSettings.overrideTG, uiDataGlobal.tgBeforePcMode);
					updateTrxID();
					uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;// Force redraw
					menuPrivateCallClear();
					uiVFOModeUpdateScreen(0);
					return;// The event has been handled
				}

#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801A) || defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				if ((trxGetMode() == RADIO_MODE_DIGITAL) && (audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF))
				{
					HRC6000ClearActiveDMRID();
				}
				menuVFOExitStatus |= MENU_STATUS_FORCE_FIRST;// Audible signal that the Channel screen has been selected
				menuSystemSetCurrentMenu(UI_CHANNEL_MODE);
				aprsBeaconingResetTimers();
#endif
				return;
			}
#if defined(PLATFORM_DM1801) || defined(PLATFORM_RD5R)
			else if (KEYCHECK_SHORTUP(ev->keys, KEY_VFO_MR))
			{
				if ((trxGetMode() == RADIO_MODE_DIGITAL) && (audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF))
				{
					HRC6000ClearActiveDMRID();
				}
				menuVFOExitStatus |= MENU_STATUS_FORCE_FIRST;// Audible signal that the Channel screen has been selected
				menuSystemSetCurrentMenu(UI_CHANNEL_MODE);
				aprsBeaconingResetTimers();
				return;
			}
#endif
#if defined(PLATFORM_RD5R)
			else if (KEYCHECK_LONGDOWN(ev->keys, KEY_VFO_MR) && (BUTTONCHECK_DOWN(ev, BUTTON_SK1) == 0))
			{
				if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
				{
					announceItem(PROMPT_SEQUENCE_BATTERY, AUDIO_PROMPT_MODE_VOICE_LEVEL_1);
				}
				else
				{
					menuSystemPushNewMenu(UI_VFO_QUICK_MENU);

					// Trick to beep (AudioAssist), since ORANGE button doesn't produce any beep event
					ev->keys.event |= KEY_MOD_UP;
					ev->keys.key = 127;
					menuVFOExitStatus |= (MENU_STATUS_LIST_TYPE | MENU_STATUS_FORCE_FIRST);
					// End Trick
				}

				return;
			}
#endif
			else if (KEYCHECK_LONGDOWN(ev->keys, KEY_INCREASE) && BUTTONCHECK_DOWN(ev, BUTTON_SK2)) // set as KEY_RIGHT on some platforms + SK2
			{
				// Long press allows the 5W+ power setting to be selected immediately (but not while sweep scanning)
				if ((uiVFOModeSweepScanning(true) == false) && increasePowerLevel(true))
				{
					headerRowIsDirty = true;
				}
			}
			else if (KEYCHECK_SHORTUP(ev->keys, KEY_INCREASE)) // set as KEY_RIGHT on some platforms
			{
				if (uiVFOModeSweepScanning(true))
				{
					if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380)
					// In Sweep scan, Right increase RSSI or GAIN
					{
						setSweepIncDecSetting(SWEEP_SETTING_RSSI, true);
						return;
					}
					else
					{
						setSweepIncDecSetting(SWEEP_SETTING_GAIN, true);
						return;
					}
#else
					// In Sweep scan, Right increase RX freq
					{
						setSweepIncDecSetting(SWEEP_SETTING_STEP, false);
						headerRowIsDirty = true;
						return;
					}
					else
					{
						handleUpKey(ev);
					}
#endif
				}
				else
				{
					if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
					{
						if (increasePowerLevel(false))
						{
							headerRowIsDirty = true;
						}
					}
					else
					{
#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380)
						if (uiDataGlobal.Scan.active == false)
						{
#endif
							if (trxGetMode() == RADIO_MODE_DIGITAL)
							{
								if (currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup != 0)
								{
									if (currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup > 1)
									{
										if (nonVolatileSettings.overrideTG == 0)
										{
											settingsIncrement(nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber], 1);
											checkAndFixIndexInRxGroup();
										}

										if (nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber] == 0)
										{
											menuVFOExitStatus |= (MENU_STATUS_LIST_TYPE | MENU_STATUS_FORCE_FIRST);
										}
									}
									settingsSet(nonVolatileSettings.overrideTG, 0);// setting the override TG to 0 indicates the TG is not overridden
								}
								menuPrivateCallClear();
								updateTrxID();
								// We're in digital mode, RXing, and current talker is already at the top of last heard list,
								// hence immediately display complete contact/TG info on screen
								uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;//(isQSODataAvailableForCurrentTalker() ? QSO_DISPLAY_CALLER_DATA : QSO_DISPLAY_DEFAULT_SCREEN);
								if (isQSODataAvailableForCurrentTalker())
								{
									(void)addTimerCallback(uiUtilityRenderQSODataAndUpdateScreen, 2000, UI_VFO_MODE, true);
								}
								uiVFOModeUpdateScreen(0);
								announceItem(PROMPT_SEQUENCE_CONTACT_TG_OR_PC,PROMPT_THRESHOLD_3);
							}
							else
							{
								if(currentChannelData->sql == 0) //If we were using default squelch level
								{
									currentChannelData->sql = nonVolatileSettings.squelchDefaults[currentRadioDevice->trxCurrentBand[TRX_RX_FREQ_BAND]];//start the adjustment from that point.
								}

								if (currentChannelData->sql < CODEPLUG_MAX_VARIABLE_SQUELCH)
								{
									currentChannelData->sql++;
								}

								announceItem(PROMPT_SQUENCE_SQUELCH,PROMPT_THRESHOLD_3);

								uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
								uiNotificationShow(NOTIFICATION_TYPE_SQUELCH, NOTIFICATION_ID_SQUELCH, 1000, NULL, false);
								uiVFOModeUpdateScreen(0);
							}
#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380)
						}
						else
						{
							handleUpKey(ev);      //Up key while scan paused continues the scan
						}
#endif
					}
				}
			}
			else if (KEYCHECK_SHORTUP(ev->keys, KEY_DECREASE)) // set as KEY_LEFT on some platforms
			{
				if (uiVFOModeSweepScanning(true))
				{
					if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380)
					// In Sweep scan, Right decrease RSSI or GAIN
					{
						setSweepIncDecSetting(SWEEP_SETTING_RSSI, false);
					}
					else
					{
						setSweepIncDecSetting(SWEEP_SETTING_GAIN, false);
					}
					return;
#else
					// In Sweep scan, Left decrease RX freq
					{
						setSweepIncDecSetting(SWEEP_SETTING_STEP, true);
						headerRowIsDirty = true;
						return;
					}
					else
					{
						handleDownKey(ev);
					}
#endif
				}
				else
				{
					if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
					{
						if (decreasePowerLevel())
						{
							headerRowIsDirty = true;
						}

						if (trxGetPowerLevel() == 0)
						{
							menuVFOExitStatus |= (MENU_STATUS_LIST_TYPE | MENU_STATUS_FORCE_FIRST);
						}
					}
					else
					{
						if (trxGetMode() == RADIO_MODE_DIGITAL)
						{
							if (currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup != 0)
							{
								if (currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup > 1)
								{
									// To Do change TG in on same channel freq
									if (nonVolatileSettings.overrideTG == 0)
									{
										settingsDecrement(nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber], 1);
										if (nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber] < 0)
										{
											settingsSet(nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber],
													(int16_t) (currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup - 1));
										}

										if(nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber] == 0)
										{
											menuVFOExitStatus |= MENU_STATUS_FORCE_FIRST;
										}
									}
								}
								settingsSet(nonVolatileSettings.overrideTG, 0);// setting the override TG to 0 indicates the TG is not overridden
							}
							menuPrivateCallClear();
							updateTrxID();
							// We're in digital mode, RXing, and current talker is already at the top of last heard list,
							// hence immediately display complete contact/TG info on screen
							uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;//(isQSODataAvailableForCurrentTalker() ? QSO_DISPLAY_CALLER_DATA : QSO_DISPLAY_DEFAULT_SCREEN);
							if (isQSODataAvailableForCurrentTalker())
							{
								(void)addTimerCallback(uiUtilityRenderQSODataAndUpdateScreen, 2000, UI_VFO_MODE, true);
							}
							uiVFOModeUpdateScreen(0);
							announceItem(PROMPT_SEQUENCE_CONTACT_TG_OR_PC,PROMPT_THRESHOLD_3);
						}
						else
						{
							if(currentChannelData->sql == 0) //If we were using default squelch level
							{
								currentChannelData->sql = nonVolatileSettings.squelchDefaults[currentRadioDevice->trxCurrentBand[TRX_RX_FREQ_BAND]];//start the adjustment from that point.
							}

							if (currentChannelData->sql > CODEPLUG_MIN_VARIABLE_SQUELCH)
							{
								currentChannelData->sql--;
							}

							announceItem(PROMPT_SQUENCE_SQUELCH,PROMPT_THRESHOLD_3);

							uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
							uiNotificationShow(NOTIFICATION_TYPE_SQUELCH, NOTIFICATION_ID_SQUELCH, 1000, NULL, false);
							uiVFOModeUpdateScreen(0);
						}
					}
				}
			}
			else if (KEYCHECK_LONGDOWN(ev->keys, KEY_0) && (KEYCHECK_LONGDOWN_REPEAT(ev->keys, KEY_0) == false))
			{
				char buf[SCREEN_LINE_BUFFER_SIZE];
				bool muted = audioAmpIsMuted();

				audioAmpMute(!muted);

				snprintf(buf, SCREEN_LINE_BUFFER_SIZE, "%s: %s", currentLanguage->mute, (muted ? currentLanguage->no : currentLanguage->yes));
				uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 1000U, buf, true);

				if (muted)
				{
					soundSetMelody(MELODY_KEY_BEEP_FIRST_ITEM);
					nextKeyBeepMelody = NULL;
					ev->keys.event &= ~(KEY_MOD_LONG | KEY_MOD_DOWN | KEY_MOD_PRESS | KEY_MOD_UP);
					ev->keys.key = 0;

					voicePromptsInit();
					voicePromptsAppendPrompt(PROMPT_SILENCE);
					voicePromptsAppendLanguageString(currentLanguage->mute);
					voicePromptsAppendPrompt(PROMPT_SILENCE);
					voicePromptsAppendLanguageString(currentLanguage->no);
					voicePromptsPlay();
				}
			}
		}
		else // (uiDataGlobal.FreqEnter.index == 0)
		{
			if (KEYCHECK_PRESS(ev->keys, KEY_DECREASE)) // set as KEY_LEFT on some platforms
			{
				uiDataGlobal.FreqEnter.index--;
				uiDataGlobal.FreqEnter.digits[uiDataGlobal.FreqEnter.index] = '-';
				uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			}
			else if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
			{
				freqEnterReset();
				soundSetMelody(MELODY_NACK_BEEP);
				uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
				announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ, PROMPT_THRESHOLD_NEVER_PLAY_IMMEDIATELY);
			}
			else if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
			{
				if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_NORMAL)
				{
					int newFrequency = freqEnterRead(0, 8, false);

					if (trxGetBandFromFrequency(newFrequency) != FREQUENCY_OUT_OF_BAND)
					{
						updateFrequency(newFrequency, PROMPT_THRESHOLD_3);
						HRC6000ClearColorCodeSynchronisation();
						freqEnterReset();
					}
					else
					{
						menuVFOExitStatus |= MENU_STATUS_ERROR;
					}

					uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
				}
				else if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN)
				{
					// Complete frequencies with zeros

					// Low
					if (uiDataGlobal.FreqEnter.index > 0 && uiDataGlobal.FreqEnter.index < 6)
					{
						memset(uiDataGlobal.FreqEnter.digits + uiDataGlobal.FreqEnter.index, '0', (6 - uiDataGlobal.FreqEnter.index) - 1);
						uiDataGlobal.FreqEnter.index = 5;
						keyval = 0;
					} // High
					else if (uiDataGlobal.FreqEnter.index > 6 && uiDataGlobal.FreqEnter.index < 12)
					{
						memset(uiDataGlobal.FreqEnter.digits + uiDataGlobal.FreqEnter.index, '0', (6 - (uiDataGlobal.FreqEnter.index - 6)) - 1);
						uiDataGlobal.FreqEnter.index = 11;
						keyval = 0;
					}
					else
					{
						if (uiDataGlobal.FreqEnter.index != 0)
						{
							menuVFOExitStatus |= MENU_STATUS_ERROR;
						}
					}
				}
			}
		}

		if (uiDataGlobal.FreqEnter.index < ((screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_NORMAL) ? 8 : 12))
		{
			// GREEN key was pressed while entering scan freq if keyval is != 99
			if (keyval == 99)
			{
				keyval = menuGetKeypadKeyValue(ev, true);

#if !defined(PLATFORM_GD77S)
				if ((keyval < 10) && BUTTONCHECK_DOWN(ev, BUTTON_SK1) && (BUTTONCHECK_DOWN(ev, BUTTON_SK2) == false))
				{
					if (keyval == 1)
					{
						aprsBeaconingToggles();
					}
					else if (keyval == 2)
					{
						switch (aprsBeaconingGetMode())
						{
							case APRS_BEACONING_MODE_MANUAL:
							case APRS_BEACONING_MODE_AUTO:
							case APRS_BEACONING_MODE_SMART_BEACONING:
								if (txInhibitCheckAndWarn() == false)
								{
									aprsBeaconingSendBeacon(false, true);
								}
								break;

							default: // Ignore other APRS modes.
								break;
						}
					}

					return;
				}
#endif
			}

			if ((keyval != 99) &&
					// Not first '0' digit in frequencies: we don't support < 100 MHz
					((((uiDataGlobal.FreqEnter.index == 0) && (keyval == 0)) == false) &&
							(((screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN) && (uiDataGlobal.FreqEnter.index == 6) && (keyval == 0)) == false)) &&
							(BUTTONCHECK_DOWN(ev, BUTTON_SK2) == 0))
			{
				voicePromptsInit();
				voicePromptsAppendPrompt(PROMPT_0 +  keyval);
				if ((uiDataGlobal.FreqEnter.index == 2) || (uiDataGlobal.FreqEnter.index == 8))
				{
					voicePromptsAppendPrompt(PROMPT_POINT);
				}
				voicePromptsPlay();

				uiDataGlobal.FreqEnter.digits[uiDataGlobal.FreqEnter.index] = (char) keyval + '0';
				uiDataGlobal.FreqEnter.index++;

				if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_NORMAL)
				{
					if (uiDataGlobal.FreqEnter.index == 8)
					{
						int newFreq = freqEnterRead(0, 8, false);

						if (trxGetBandFromFrequency(newFreq) != FREQUENCY_OUT_OF_BAND)
						{
							updateFrequency(newFreq, AUDIO_PROMPT_MODE_BEEP);
							HRC6000ClearColorCodeSynchronisation();
							freqEnterReset();
							soundSetMelody(MELODY_ACK_BEEP);
						}
						else
						{
							uiDataGlobal.FreqEnter.index--;
							uiDataGlobal.FreqEnter.digits[uiDataGlobal.FreqEnter.index] = '-';
							soundSetMelody(MELODY_ERROR_BEEP);
							menuVFOExitStatus |= MENU_STATUS_ERROR;
						}
					}
				}
				else if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN)
				{
					// Check low boundary
					if (uiDataGlobal.FreqEnter.index == 6)
					{
						int fLower = freqEnterRead(0, 6, false) * 100;

						if (trxGetBandFromFrequency(fLower) == FREQUENCY_OUT_OF_BAND)
						{
							uiDataGlobal.FreqEnter.index--;
							uiDataGlobal.FreqEnter.digits[uiDataGlobal.FreqEnter.index] = '-';
							soundSetMelody(MELODY_ERROR_BEEP);
							menuVFOExitStatus |= MENU_STATUS_ERROR;
						}
					}
					else if (uiDataGlobal.FreqEnter.index == FREQ_ENTER_DIGITS_MAX)
					{
						int fStep = VFO_FREQ_STEP_TABLE[(currentChannelData->VFOflag5 >> 4)];
						int fLower = freqEnterRead(0, 6, false) * 100;
						int fUpper = freqEnterRead(6, 12, false) * 100;

						// Reorg min/max
						if (fLower > fUpper)
						{
							SAFE_SWAP(fLower, fUpper);
						}

						// At least on step diff
						if ((fUpper - fLower) < fStep)
						{
							fUpper = fLower + fStep;
						}

						// Refresh on every step if scan boundaries is equal to one frequency step.
						uiDataGlobal.Scan.refreshOnEveryStep = ((fUpper - fLower) <= fStep);

						if ((trxGetBandFromFrequency(fLower) != FREQUENCY_OUT_OF_BAND) && (trxGetBandFromFrequency(fUpper) != FREQUENCY_OUT_OF_BAND))
						{
							settingsSet(nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber], (uint32_t) fLower);
							settingsSet(nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber], (uint32_t) fUpper);

							freqEnterReset();
							soundSetMelody(MELODY_ACK_BEEP);
							announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ, PROMPT_THRESHOLD_3);
						}
						else
						{
							uiDataGlobal.FreqEnter.index--;
							uiDataGlobal.FreqEnter.digits[uiDataGlobal.FreqEnter.index] = '-';
							soundSetMelody(MELODY_ERROR_BEEP);
							menuVFOExitStatus |= MENU_STATUS_ERROR;
						}
					}
				}

				uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			}
		}
	}
}

static void handleUpKey(uiEvent_t *ev)
{
	uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
	if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
	{
		// Don't permit to switch from RX/TX while scanning
		if ((screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SCAN) &&
				(screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_DUAL_SCAN) &&
				(screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP))
		{
			selectedFreq = VFO_SELECTED_FREQUENCY_INPUT_RX;
			announceItem(PROMPT_SEQUENCE_DIRECTION_RX, PROMPT_THRESHOLD_1);
		}
	}
	else
	{
		if (uiDataGlobal.Scan.active)
		{
			if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP)
			{
				stepFrequency(VFO_SWEEP_SCAN_FREQ_STEP_TABLE[uiDataGlobal.Scan.sweepStepSizeIndex]);
				uiVFOModeUpdateScreen(0);
				vfoSweepUpdateSamples(1, false, 0);
			}
			else
			{
				SCANPROF_START(tStep);
				stepFrequency(VFO_FREQ_STEP_TABLE[(currentChannelData->VFOflag5 >> 4)] * uiDataGlobal.Scan.direction);
				SCANPROF_END(SCANPROF_STEPFREQ, tStep);

				uiDataGlobal.Scan.timer.timeout = 500;
				uiDataGlobal.Scan.state = SCAN_STATE_SCANNING;

#if defined(ENABLE_FAST_SCAN)
				if (uiDataGlobal.Scan.refreshOnEveryStep || ticksTimerHasExpired(&vfoScanDisplayTimer))
				{
					ticksTimerStart(&vfoScanDisplayTimer, VFO_SCAN_DISPLAY_INTERVAL_MS);
#endif
					SCANPROF_START(tDraw);
					uiVFOModeUpdateScreen(0);
					SCANPROF_END(SCANPROF_UPDSCREEN, tDraw);
#if defined(ENABLE_FAST_SCAN)
				}
				else
				{
					// handleUpKey() set this on entry and uiVFOModeUpdateScreen() is what
					// normally clears it. Left set, the next uiVFOModeTick() would perform
					// exactly the redraw that was just skipped, and nothing would be saved.
					uiDataGlobal.displayQSOState = QSO_DISPLAY_IDLE;
				}
#endif
			}
		}
		else
		{
			stepFrequency(VFO_FREQ_STEP_TABLE[(currentChannelData->VFOflag5 >> 4)]);
			uiVFOModeUpdateScreen(0);
		}

	}

	settingsSetVFODirty();
}

static void handleDownKey(uiEvent_t *ev)
{
	uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
	if (BUTTONCHECK_DOWN(ev, BUTTON_SK2))
	{
		// Don't permit to switch from RX/TX while scanning
		if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_NORMAL)
		{
			selectedFreq = VFO_SELECTED_FREQUENCY_INPUT_TX;
			announceItem(PROMPT_SEQUENCE_DIRECTION_TX, PROMPT_THRESHOLD_1);
		}
	}
	else
	{
		if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP)
		{
			stepFrequency(VFO_SWEEP_SCAN_FREQ_STEP_TABLE[uiDataGlobal.Scan.sweepStepSizeIndex] * -1);
		}
		else
		{
			stepFrequency(VFO_FREQ_STEP_TABLE[(currentChannelData->VFOflag5 >> 4)] * -1);
		}

		uiVFOModeUpdateScreen(0);
		settingsSetVFODirty();

		if (uiDataGlobal.Scan.active && (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP))
		{
			vfoSweepUpdateSamples(-1, false, 0);
		}
	}
}

#if defined(ENABLE_FAST_SCAN)
/* Frequency of sweep sample `index`, in OpenGD77's 10 Hz units. Same arithmetic
 * sweepScanStep() uses to tune each sample -- kept in one place so the marker can never
 * disagree with where the receiver actually was. */
/* Move the display on by exactly one span, wrapping inside the band.
 *
 * Exactly one span is the point: less and consecutive windows overlap, which wastes the
 * sweep time the wide span already spends a lot of; more and there are slices of the band
 * it never looks at, which is worse because nothing on screen says so. */
static void vfoSweepAdvanceWindow(void)
{
	uint32_t span = vfoSweepSpan();
	uint32_t band = trxGetBandFromFrequency(currentChannelData->rxFreq);

	vfoSweepScrollPending = false;
	vfoSweepPassesDone = 0;      // the new window gets its own full dwell

	if (band == FREQUENCY_OUT_OF_BAND)
	{
		return;
	}

	// ★ HARDWARE bands, not USER_FREQUENCY_BANDS. The point of this mode is to survey
	// everything the radio can hear, and the user bands are the amateur allocations --
	// measured on this radio, UHF 420-450 against a hardware 400-520. Worse, the index
	// comes from trxGetBandFromFrequency(), which looks up the HARDWARE table, so reading
	// the user table with it mixes two different tables. That combination left a 20 MHz
	// span with valid centres of only 430-440: every step overshot, it wrapped straight
	// back, and the sweep sat on one window looking like it had hung.
	uint32_t lo = RADIO_HARDWARE_FREQUENCY_BANDS[band].minFreq + (span / 2);
	uint32_t hi = RADIO_HARDWARE_FREQUENCY_BANDS[band].maxFreq - (span / 2);
	uint32_t next = currentChannelData->rxFreq + span;

	if (hi <= lo)
	{
		// Span wider than the whole band: there is nothing to tile, so sit in the middle
		// and let the edges fall outside rather than refusing to show anything.
		currentChannelData->rxFreq = (RADIO_HARDWARE_FREQUENCY_BANDS[band].minFreq +
				RADIO_HARDWARE_FREQUENCY_BANDS[band].maxFreq) / 2;
	}
	else if (next <= hi)
	{
		currentChannelData->rxFreq = next;
	}
	else if (currentChannelData->rxFreq < hi)
	{
		// Last window: clamp to the top edge instead of stepping past it. That overlaps
		// the previous window a little, which costs a few seconds; the alternative is
		// never looking at the top of the band at all, and nothing on screen would say so.
		currentChannelData->rxFreq = hi;
	}
	else
	{
		// Already on the last window -- wrap. Clamping here instead would leave the sweep
		// on the top window for ever, which on screen is indistinguishable from a crash.
		currentChannelData->rxFreq = lo;
	}

	// ★ Move the TX frequency with it, exactly as stepFrequency() does for every other
	// way the VFO is retuned. The sweep only ever receives, so leaving TX behind looks
	// harmless -- it is not. MEASURED: with only rxFreq moved, the survey walked up to
	// ~513 MHz and then snapped back to the stored VFO frequency every time, because the
	// VFO is an rx/tx PAIR and something downstream re-synced the two. Chasing which
	// resync it was would be beside the point: the established contract for moving this
	// VFO is to move both, and not following it is the bug.
	currentChannelData->txFreq = currentChannelData->rxFreq;
	settingsSetVFODirty();

	// Every bin now covers a different frequency, so nothing measured carries over. The
	// hold especially: showing a held peak against the new window would be a claim about
	// a frequency it was never measured at.
	memset(vfoSweepSamples, 0x00, sizeof(vfoSweepSamples));
	memset(vfoSweepHold, 0x00, sizeof(vfoSweepHold));
	vfoSweepShownPeakLevel = 0;
	vfoSweepShownPeakIndex = -1;
	vfoSweepPeakLevel = 0;
	vfoSweepPeakIndex = -1;
	vfoSweepAutoPrimed = false;   // re-take the scale outright rather than slewing to it
	uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
}

static uint32_t vfoSweepFreqForOrdinal(int32_t ordinal)
{
	// `ordinal` counts MEASUREMENTS across the whole pass, not columns: 0 .. (160*N - 1).
	// With N == 1 this is the column index and the arithmetic is identical to what it has
	// always been, which is what keeps every existing caller correct.
	return currentChannelData->rxFreq +
			((vfoSweepSubStep() *
					(ordinal - ((VFO_SWEEP_NUM_SAMPLES * vfoSweepSubSamples()) / 2)))
							/ VFO_SWEEP_PIXELS_PER_STEP);
}

static uint32_t vfoSweepFreqForSample(int16_t index)
{
	// The CENTRE of the band this column covers. A column spans N measurements, so naming
	// it by its first one would bias every peak label half a column low -- 62.5 kHz at the
	// 20 MHz span, which is more than the label's own resolution and would look like a
	// calibration error rather than an off-by-one.
	uint8_t n = vfoSweepSubSamples();

	return vfoSweepFreqForOrdinal(((int32_t)index * n) + (n / 2));
}

/* Re-derive the display scaling from the pass just finished.
 *
 * The floor estimate is the mean of the samples at or below the overall mean. That is a
 * one-extra-loop way to get something robust without a sort or a histogram -- neither of
 * which fits in the RAM this build has left -- and it is far steadier than the raw
 * minimum, which a single glitched sample drags down and takes the whole trace with it.
 * A handful of strong signals cannot move it much either, since they all land above the
 * mean and are excluded by construction. */
static void vfoSweepUpdateAutoScale(void)
{
	uint32_t sum = 0;
	uint32_t lowSum = 0;
	uint16_t lowCount = 0;
	uint8_t highest = 0;
	uint8_t mean, noise, floorTarget;
	int32_t span;

	for (int i = 0; i < VFO_SWEEP_NUM_SAMPLES; i++)
	{
		sum += vfoSweepSamples[i];
		if (vfoSweepSamples[i] > highest)
		{
			highest = vfoSweepSamples[i];
		}
	}
	mean = (uint8_t)(sum / VFO_SWEEP_NUM_SAMPLES);

	for (int i = 0; i < VFO_SWEEP_NUM_SAMPLES; i++)
	{
		if (vfoSweepSamples[i] <= mean)
		{
			lowSum += vfoSweepSamples[i];
			lowCount++;
		}
	}
	noise = (lowCount ? (uint8_t)(lowSum / lowCount) : mean);

	// Sit just under the noise rather than on it, so the noise still has some texture
	// instead of being clipped flat against the baseline.
	floorTarget = ((noise > VFO_SWEEP_AUTO_FLOOR_MARGIN) ? (noise - VFO_SWEEP_AUTO_FLOOR_MARGIN) : 0);

	// Span that puts the strongest sample at VFO_SWEEP_AUTO_HEADROOM_PCT of full height.
	span = ((int32_t)highest - (int32_t)floorTarget);
	if (span < VFO_SWEEP_AUTO_MIN_SPAN)
	{
		// An empty band is all noise, and stretching it to full height turns a quiet
		// display into a wall of grass that looks like signal everywhere.
		span = VFO_SWEEP_AUTO_MIN_SPAN;
	}
	span = ((span * 100) / VFO_SWEEP_AUTO_HEADROOM_PCT);
	if (span > 255)
	{
		span = 255;
	}

	if (vfoSweepAutoPrimed)
	{
		// Slew, or the whole trace jumps every pass as the peak comes and goes.
		vfoSweepAutoFloor = (uint8_t)(((vfoSweepAutoFloor * (VFO_SWEEP_AUTO_SLEW - 1)) + floorTarget) / VFO_SWEEP_AUTO_SLEW);
		vfoSweepAutoGain = (uint8_t)(((vfoSweepAutoGain * (VFO_SWEEP_AUTO_SLEW - 1)) + span) / VFO_SWEEP_AUTO_SLEW);
	}
	else
	{
		// First pass after entering the sweep: take it outright. Slewing from the manual
		// defaults would spend several seconds visibly settling every single time.
		vfoSweepAutoFloor = floorTarget;
		vfoSweepAutoGain = (uint8_t)span;
		vfoSweepAutoPrimed = true;
	}

	if (vfoSweepAutoGain == 0)
	{
		vfoSweepAutoGain = 1;   // it is a divisor
	}
}

/* Leave the parked-on signal and carry on sweeping.
 *
 * Shutting the audio explicitly matters: trxCheckAnalogSquelch() stops being called the
 * moment the state goes back to SCANNING, so whatever it last left enabled would stay
 * enabled and the sweep would run with the amplifier open on noise. */
static void vfoSweepStopListening(void)
{
	if (vfoSweepListening)
	{
		vfoSweepListening = false;
		trxTerminateCheckAnalogSquelch(RADIO_DEVICE_PRIMARY);
		uiDataGlobal.Scan.state = SCAN_STATE_SCANNING;
		// No retune needed here: the next sweepScanStep() recomputes scanSweepCurrentFreq
		// from sweepSampleIndex and tunes there itself.
	}
}

/* Marker over the strongest sample of the last completed pass, with its frequency.
 *
 * Drawn at the top of the graph rather than on the trace: the trace is what moves, and a
 * marker sitting on it is unreadable at a glance. The label goes on whichever side of the
 * screen the peak is not, so it never covers its own marker. */
static void vfoSweepDrawPeakMarker(void)
{
	char buffer[SCREEN_LINE_BUFFER_SIZE];
	uint32_t f;
	int16_t x;

	// Nothing else writes into the band, so nothing else will erase the previous marker
	// and label when the peak moves. Clear it here, unconditionally, before deciding
	// whether there is anything to draw -- a peak that drops back into the noise has to
	// take its label with it.
	// ★ `true` clears here. displayFillRect() takes isInverted literally --
	// `isInverted ? backgroundColour : foregroundColour` -- while the line helpers
	// (displayDrawFastVLine/HLine, and displayFillTriangle through them) invert it before
	// passing it down, so for those `true` DRAWS. The two conventions are opposite. Getting
	// this backwards fills the band solid instead of clearing it, which looks like the
	// marker never being drawn rather than like a colour mistake.
	displayFillRect(0, VFO_SWEEP_GRAPH_START_Y, DISPLAY_SIZE_X, VFO_SWEEP_LABEL_BAND_H, true);

	if ((vfoSweepShownPeakIndex < 0) || (vfoSweepShownPeakIndex >= VFO_SWEEP_NUM_SAMPLES))
	{
		return;
	}

	// Nothing above the noise floor is not a peak, it is just the loudest noise. Saying
	// so would be worse than saying nothing: it invites chasing a frequency that has
	// nothing on it.
	if (vfoSweepShownPeakLevel <= (VFO_SWEEP_FLOOR_ACTIVE + VFO_SWEEP_PEAK_MARGIN))
	{
		return;
	}

	x = vfoSweepShownPeakIndex;

	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
	// A small downward wedge whose tip is on the peak column. Clamped so a peak in the
	// first or last few columns still draws a whole marker rather than half of one off
	// the edge; the tip stays on the true column either way.
	displayFillTriangle(SAFE_MAX(0, (x - 3)), VFO_SWEEP_GRAPH_START_Y,
			SAFE_MIN((DISPLAY_SIZE_X - 1), (x + 3)), VFO_SWEEP_GRAPH_START_Y,
			x, (VFO_SWEEP_GRAPH_START_Y + (VFO_SWEEP_LABEL_BAND_H - 2)), true);

	f = vfoSweepFreqForSample(vfoSweepShownPeakIndex);

	// While parked on the signal, say so. Otherwise a stopped trace looks like a crash.
	if (vfoSweepListening)
	{
		snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, ">%u.%04u", (f / 100000), ((f % 100000) / 10));
	}
	else
	{
		snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%u.%04u", (f / 100000), ((f % 100000) / 10));
	}

	// 8 px per character in FONT_SIZE_1; keep the label clear of the marker.
	if (x < (DISPLAY_SIZE_X / 2))
	{
		x = (DISPLAY_SIZE_X - (strlen(buffer) * 8) - 1);
	}
	else
	{
		x = 1;
	}

	displayThemeApply(THEME_ITEM_FG_RX_FREQ, THEME_ITEM_BG);
	displayPrintAt(x, VFO_SWEEP_GRAPH_START_Y, buffer, FONT_SIZE_1);
	displayThemeResetToDefault();
}
#endif

static void vfoSweepDrawSample(int offset)
{
	int16_t graphHeight = MAX(vfoSweepSamples[offset] - VFO_SWEEP_FLOOR_ACTIVE, 0);
	graphHeight = (graphHeight * VFO_SWEEP_TRACE_HEIGHT_Y) / VFO_SWEEP_GAIN_ACTIVE;
	graphHeight = MIN(VFO_SWEEP_TRACE_HEIGHT_Y, graphHeight);

	int16_t levelTop = ((VFO_SWEEP_TRACE_START_Y + VFO_SWEEP_TRACE_HEIGHT_Y) - graphHeight);

	// Draw the level
	displayThemeApply(THEME_ITEM_FG_RSSI_BAR, THEME_ITEM_BG);
	displayDrawFastVLine(offset, VFO_SWEEP_TRACE_START_Y, (VFO_SWEEP_TRACE_HEIGHT_Y - graphHeight), false); // Clear
	displayDrawFastVLine(offset, levelTop, graphHeight, true); // Level

#if defined(ENABLE_FAST_SCAN)
	// Max hold, as a single pixel above the live level. Drawn here, in the same call that
	// just cleared this column, so it survives until this column is next redrawn -- and it
	// is inside the strip the blit already covers, so it costs no extra transfer.
	{
		int16_t holdHeight = MAX(vfoSweepHold[offset] - VFO_SWEEP_FLOOR_ACTIVE, 0);
		holdHeight = ((holdHeight * VFO_SWEEP_TRACE_HEIGHT_Y) / VFO_SWEEP_GAIN_ACTIVE);
		holdHeight = MIN(VFO_SWEEP_TRACE_HEIGHT_Y, holdHeight);

		// Only where the hold is meaningfully above the live level. Drawing it wherever it
		// merely exceeds the trace covers the whole screen in speckle: the noise floor
		// fluctuates by a couple of counts, so max-hold of noise sits just above the live
		// noise in every single bin, and the result reads as dirt on the display rather
		// than as information. A few pixels of required lift leaves only real signals.
		if (holdHeight > (graphHeight + VFO_SWEEP_HOLD_MIN_LIFT))
		{
			displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
			displaySetPixel(offset, ((VFO_SWEEP_TRACE_START_Y + VFO_SWEEP_TRACE_HEIGHT_Y) - holdHeight), true);
		}
	}
#endif

	// center freq marker
	if (offset == (DISPLAY_SIZE_X >> 1))
	{
		bool markerTopPosition = (graphHeight < ((VFO_SWEEP_TRACE_HEIGHT_Y / 3) << 1));
		int16_t markerStarts = (markerTopPosition ? VFO_SWEEP_TRACE_START_Y : ((VFO_SWEEP_TRACE_START_Y + VFO_SWEEP_TRACE_HEIGHT_Y) - (VFO_SWEEP_TRACE_HEIGHT_Y / 3)));
		int16_t markerEnds = (markerTopPosition ? levelTop : (VFO_SWEEP_TRACE_START_Y + VFO_SWEEP_TRACE_HEIGHT_Y));

		displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG);
		for (int16_t y = markerStarts; y < markerEnds; y += 2)
		{
			displaySetPixel(offset, y, true);
			displaySetPixel(offset, (y + 1), false);
		}
	}

	displayThemeResetToDefault();
}

static void vfoSweepUpdateSamples(int offset, bool forceRedraw, int bandwidthRescale)
{
	const int SHIFT_DISTANCE[7] = {6,6,6,6,8,8,8};
	offset *= SHIFT_DISTANCE[uiDataGlobal.Scan.sweepStepSizeIndex];// real offset in samples;

#if defined(ENABLE_FAST_SCAN)
	// Scrolling or rescaling changes what frequency each bin covers. The live samples get
	// shifted and interpolated to match, but a *held* peak carried across would be a claim
	// about a frequency it was never measured at, which is worse than losing it. Drop it.
	if ((offset != 0) || (bandwidthRescale != 0))
	{
		memset(vfoSweepHold, 0x00, sizeof(vfoSweepHold));
	}
#endif

	if (offset != 0)
	{
		if (offset > 0)
		{
			uiDataGlobal.Scan.sweepSampleIndex = VFO_SWEEP_NUM_SAMPLES - 1 - offset;
			memcpy(&vfoSweepSamples[0], &vfoSweepSamples[offset], VFO_SWEEP_NUM_SAMPLES - offset);
			memset(&vfoSweepSamples[VFO_SWEEP_NUM_SAMPLES - offset], 0x00,  offset);
		}
		else
		{
			uiDataGlobal.Scan.sweepSampleIndex = 0;
			offset *= -1;
			memmove(&vfoSweepSamples[offset], &vfoSweepSamples[0], VFO_SWEEP_NUM_SAMPLES - offset);
			memset(&vfoSweepSamples[0], 0x00, offset);
		}
	}

	if (bandwidthRescale != 0)
	{
		uint8_t tmp[VFO_SWEEP_NUM_SAMPLES];
		memset(tmp, 0x00, VFO_SWEEP_NUM_SAMPLES);
		int newStartSample = (VFO_SWEEP_NUM_SAMPLES / 2) - (VFO_SWEEP_NUM_SAMPLES / 4);

		if (bandwidthRescale > 0)
		{
			for(int i = 0; i < VFO_SWEEP_NUM_SAMPLES; i+= 2)
			{
				int average = 0;
				for(int j = 0; j < 2; j++ )
				{
					average += vfoSweepSamples[i + j];
				}
				volatile int outBufPos =  (i / 2) + newStartSample;
				tmp[outBufPos] = average / 2;
			}
			uiDataGlobal.Scan.sweepSampleIndex = ((VFO_SWEEP_NUM_SAMPLES * 3) / 4);// Most efficient is to start filling in from the area revealed on the right side of the screen
		}
		else
		{
			int newValue;
			for(int i = 0; i < (VFO_SWEEP_NUM_SAMPLES - 1); i++)
			{
				newValue = (vfoSweepSamples[newStartSample + (i / 2)] + vfoSweepSamples[newStartSample + (i / 2) + 1]) / 2; // use simple 2 point expansion averaging
				tmp[i] = newValue;
			}
			tmp[VFO_SWEEP_NUM_SAMPLES - 1] = newValue;

			uiDataGlobal.Scan.sweepSampleIndex = 0;
		}

		memcpy(vfoSweepSamples, tmp, VFO_SWEEP_NUM_SAMPLES * sizeof(uint8_t));
	}

	if (forceRedraw || (offset != 0))
	{
		for(int i = 0; i < VFO_SWEEP_NUM_SAMPLES; i++)
		{
			vfoSweepDrawSample(i);
		}

		displayRenderRows(1, ((8 + VFO_SWEEP_GRAPH_HEIGHT_Y) / 8) + 1);
	}

	if (uiDataGlobal.Scan.state == SCAN_STATE_SCANNING)
	{
		uiDataGlobal.Scan.scanSweepCurrentFreq = currentChannelData->rxFreq + (VFO_SWEEP_SCAN_RANGE_SAMPLE_STEP_TABLE[uiDataGlobal.Scan.sweepStepSizeIndex] * (uiDataGlobal.Scan.sweepSampleIndex - (VFO_SWEEP_NUM_SAMPLES / 2))) / VFO_SWEEP_PIXELS_PER_STEP;
		trxSetFrequency(uiDataGlobal.Scan.scanSweepCurrentFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
		ticksTimerStart(&uiDataGlobal.Scan.timer, VFO_SWEEP_STEP_TIME_ACTIVE);
	}
}

static void setSweepIncDecSetting(sweepSetting_t type, bool increment)
{
	bool apply = false;
	uint16_t setting = nonVolatileSettings.vfoSweepSettings;
	int bandwidthRescaleDirection = 0;
	switch (type)
	{
		case SWEEP_SETTING_STEP:
			{
				int oldStepIndex = uiDataGlobal.Scan.sweepStepSizeIndex;
				int topIndex = ((sizeof(VFO_SWEEP_SCAN_RANGE_SAMPLE_STEP_TABLE) / sizeof(VFO_SWEEP_SCAN_RANGE_SAMPLE_STEP_TABLE[0])) - 1);
#if defined(ENABLE_FAST_SCAN)
				bool oldWide = vfoSweepWide;

				// The wide span sits one notch above the widest table entry, so the single
				// existing control walks 0..6 and then straight on into it -- no new key,
				// and the ordering on screen stays monotonic in span.
				if (increment && (uiDataGlobal.Scan.sweepStepSizeIndex == topIndex) && (vfoSweepWide == false))
				{
					vfoSweepWide = true;
					bandwidthRescaleDirection = 0;   // not a rescale of the same span: a new one
				}
				else if ((increment == false) && vfoSweepWide)
				{
					vfoSweepWide = false;
					bandwidthRescaleDirection = 0;
				}
				else if (vfoSweepWide == false)
#endif
				{
					if (increment)
					{
						uiDataGlobal.Scan.sweepStepSizeIndex = SAFE_MIN(topIndex, (uiDataGlobal.Scan.sweepStepSizeIndex + 1));
						bandwidthRescaleDirection = 1;
					}
					else
					{
						uiDataGlobal.Scan.sweepStepSizeIndex = SAFE_MAX(0, (uiDataGlobal.Scan.sweepStepSizeIndex - 1));
						bandwidthRescaleDirection = -1;
					}
				}

				if ((oldStepIndex != uiDataGlobal.Scan.sweepStepSizeIndex)
#if defined(ENABLE_FAST_SCAN)
						|| (oldWide != vfoSweepWide)
#endif
				   )
				{
					apply = true;
					setting = VFO_SWEEP_SETTINGS_WORD(uiDataGlobal.Scan.sweepStepSizeIndex,
							((nonVolatileSettings.vfoSweepSettings >> 7) & 0x1F),
							(nonVolatileSettings.vfoSweepSettings & 0x7F));
#if defined(ENABLE_FAST_SCAN)
					if (oldWide != vfoSweepWide)
					{
						// Entering or leaving the wide span changes what every bin covers,
						// so nothing measured carries over -- same reasoning as a scroll.
						memset(vfoSweepSamples, 0x00, sizeof(vfoSweepSamples));
						memset(vfoSweepHold, 0x00, sizeof(vfoSweepHold));
						vfoSweepShownPeakLevel = 0;
						vfoSweepShownPeakIndex = -1;
						vfoSweepPeakLevel = 0;
						vfoSweepPeakIndex = -1;
						vfoSweepSubIndex = 0;
						vfoSweepColumnPeak = 0;
						vfoSweepAutoPrimed = false;
						uiDataGlobal.Scan.sweepSampleIndex = 0;
					}
#endif
				}
			}
			break;
		case SWEEP_SETTING_RSSI:
			{
#if defined(ENABLE_FAST_SCAN)
				// Touching either knob hands scaling back to the operator. Both key paths
				// funnel through here, so this is the only place it needs saying.
				//
				// The display will jump: the manual floor is capped at 24 and cannot
				// express what auto was using (~35 on this radio), so there is no way to
				// hand over continuously. SK1 + rotary puts auto back.
				vfoSweepAutoScale = false;
#endif
				if (increment)
				{
					if (vfoSweepRssiNoiseFloor > VFO_SWEEP_RSSI_NOISE_FLOOR_MIN)
					{
						vfoSweepRssiNoiseFloor--;
						apply = true;
					}
				}
				else
				{
					if (vfoSweepRssiNoiseFloor < VFO_SWEEP_RSSI_NOISE_FLOOR_MAX)
					{
						vfoSweepRssiNoiseFloor++;
						apply = true;
					}
				}

				if (apply)
				{
					setting &= 0xF07F;
					setting |= (vfoSweepRssiNoiseFloor << 7);
				}
			}
			break;
		case SWEEP_SETTING_GAIN:
			{
#if defined(ENABLE_FAST_SCAN)
				vfoSweepAutoScale = false;   // see SWEEP_SETTING_RSSI
#endif
				if (increment)
				{
					if (vfoSweepGain > VFO_SWEEP_GAIN_STEP)
					{
						vfoSweepGain -= VFO_SWEEP_GAIN_STEP;
						apply = true;
					}

				}
				else
				{
					if (vfoSweepGain < VFO_SWEEP_GAIN_MAX)
					{
						vfoSweepGain += VFO_SWEEP_GAIN_STEP;
						apply = true;
					}
				}

				if (apply)
				{
					setting &= 0xFF80;
					setting |= vfoSweepGain;
				}
			}
			break;
	}

	settingsSet(nonVolatileSettings.vfoSweepSettings, setting);
	settingsSaveIfNeeded(true);

	if (apply)
	{
		vfoSweepUpdateSamples(0, true, bandwidthRescaleDirection);
	}
}

static void stepFrequency(int increment)
{
	int newTxFreq;
	int newRxFreq;

	if (selectedFreq == VFO_SELECTED_FREQUENCY_INPUT_TX)
	{
		newTxFreq  = currentChannelData->txFreq + increment;
		newRxFreq  = currentChannelData->rxFreq; // Needed later for the band limited checking
	}
	else
	{
		// VFO_SELECTED_FREQUENCY_INPUT_RX
		newRxFreq  = currentChannelData->rxFreq + increment;
		if (!uiDataGlobal.QuickMenu.tmpTxRxLockMode)
		{
			newTxFreq  = currentChannelData->txFreq + increment;
		}
		else
		{
			newTxFreq  = currentChannelData->txFreq;// Needed later for the band limited checking
		}
	}

	// Out of frequency in the current band, update freq to the next or prev band.
	if (trxGetBandFromFrequency(newRxFreq) == FREQUENCY_OUT_OF_BAND)
	{
		int band = trxGetNextOrPrevBandFromFrequency(newRxFreq, (increment > 0));

		if (band != -1)
		{
			newRxFreq = ((increment > 0) ? RADIO_HARDWARE_FREQUENCY_BANDS[band].minFreq : RADIO_HARDWARE_FREQUENCY_BANDS[band].maxFreq);
			newTxFreq = newRxFreq;
		}
		else
		{
			soundSetMelody(MELODY_ERROR_BEEP);
			return;
		}
	}

	if (trxGetBandFromFrequency(newRxFreq) != FREQUENCY_OUT_OF_BAND)
	{
		currentChannelData->txFreq = newTxFreq;
		currentChannelData->rxFreq = newRxFreq;

		{
			SCANPROF_START(tTrx);
			trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
			SCANPROF_END(SCANPROF_TRXSETFREQ, tTrx);
		}

		if (screenOperationMode[nonVolatileSettings.currentVFONumber] != VFO_SCREEN_OPERATION_SWEEP)
		{
			HRC6000ClearColorCodeSynchronisation();
		}

		if ((uiDataGlobal.Scan.active == false) || (uiDataGlobal.Scan.active && (uiDataGlobal.Scan.state == SCAN_STATE_PAUSED)))
		{
			announceItem(PROMPT_SEQUENCE_VFO_FREQ_UPDATE, PROMPT_THRESHOLD_3);
		}
	}
	else
	{
		soundSetMelody(MELODY_ERROR_BEEP);
	}
}

// ---------------------------------------- Quick Menu functions -------------------------------------------------------------------
menuStatus_t uiVFOModeQuickMenu(uiEvent_t *ev, bool isFirstRun)
{
	if (isFirstRun)
	{
		if (quickmenuNewChannelHandled)
		{
			quickmenuNewChannelHandled = false;
			menuSystemPopAllAndDisplayRootMenu();
			return MENU_STATUS_SUCCESS;
		}

		uiDataGlobal.QuickMenu.tmpDmrDestinationFilterLevel = nonVolatileSettings.dmrDestinationFilter;
		uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel = nonVolatileSettings.dmrCcTsFilter;
		uiDataGlobal.QuickMenu.tmpAnalogFilterLevel = nonVolatileSettings.analogFilterLevel;
		uiDataGlobal.QuickMenu.tmpTxRxLockMode = settingsIsOptionBitSet(BIT_TX_RX_FREQ_LOCK);
		uiDataGlobal.QuickMenu.tmpVFONumber = nonVolatileSettings.currentVFONumber;
		uiDataGlobal.QuickMenu.tmpToneScanCSS = toneScanCSS;
		uiDataGlobal.QuickMenu.tmpAudioMute = audioAmpIsMuted();
		
		menuDataGlobal.numItems = NUM_VFO_SCREEN_QUICK_MENU_ITEMS;

		menuDataGlobal.menuOptionsSetQuickkey = 0;
		menuDataGlobal.menuOptionsTimeout = 0;
		menuDataGlobal.newOptionSelected = true;

		voicePromptsInit();
		voicePromptsAppendPrompt(PROMPT_SILENCE);
		voicePromptsAppendPrompt(PROMPT_SILENCE);
		voicePromptsAppendLanguageString(currentLanguage->quick_menu);
		voicePromptsAppendPrompt(PROMPT_SILENCE);
		voicePromptsAppendPrompt(PROMPT_SILENCE);

		updateQuickMenuScreen(true);
		return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
	}
	else
	{
		menuQuickVFOExitStatus = MENU_STATUS_SUCCESS;

		if (ev->hasEvent || (menuDataGlobal.menuOptionsTimeout > 0))
		{
			handleQuickMenuEvent(ev);
		}
	}
	return menuQuickVFOExitStatus;
}

static bool validateNewChannel(void)
{
	quickmenuNewChannelHandled = true;

	if (uiDataGlobal.MessageBox.keyPressed == KEY_GREEN)
	{
		int16_t newChannelIndex;

		//look for empty channel
		for (newChannelIndex = CODEPLUG_CHANNELS_MIN; newChannelIndex <= CODEPLUG_CHANNELS_MAX; newChannelIndex++)
		{
			if (!codeplugAllChannelsIndexIsInUse(newChannelIndex))
			{
				break;
			}
		}

		if (newChannelIndex <= CODEPLUG_CONTACTS_MAX)
		{
			int currentTS = trxGetDMRTimeSlot();
			char nameBuf[SCREEN_LINE_BUFFER_SIZE];
			CodeplugChannel_t tempChannel = channelScreenChannelData;

			memcpy(&tempChannel.rxFreq, &settingsVFOChannel[nonVolatileSettings.currentVFONumber].rxFreq, CODEPLUG_CHANNEL_DATA_STRUCT_SIZE - 16);// Don't copy the name of the vfo, which are in the first 16 bytes
			tempChannel.rxTone = currentChannelData->rxTone;
			tempChannel.txTone = currentChannelData->txTone;

			// Codeplug string aren't NULL terminated.
			snprintf(nameBuf, SCREEN_LINE_BUFFER_SIZE, "%s %d", currentLanguage->new_channel, newChannelIndex);
			memset(&tempChannel.name, 0xFF, sizeof(tempChannel.name));
			memcpy(&tempChannel.name, nameBuf, strlen(nameBuf));

			// change the TS on the new channel to whatever the radio is currently set to.
			codeplugChannelSetFlag(&tempChannel, CHANNEL_FLAG_TIMESLOT_TWO, ((currentTS != 0) ? 1 : 0));

			if (codeplugChannelSaveDataForIndex(newChannelIndex, &tempChannel))
			{
				codeplugAllChannelsIndexSetUsed(newChannelIndex); //Set channel index as valid
			}

			// Check if currentZone is initialized
			if (currentZone.NOT_IN_CODEPLUGDATA_indexNumber == 0xDEADBEEF)
			{
				uiChannelInitializeCurrentZone();
			}

			// check if its real zone and or the virtual zone "All Channels" whose index is -1
			if (CODEPLUG_ZONE_IS_ALLCHANNELS(currentZone))
			{
				// All Channels virtual zone
				settingsSet(nonVolatileSettings.currentZone, (int16_t) (codeplugZonesGetCount() - 1));//set zone to all channels and channel index to free channel found

				// Change to the index of the new channel
				codeplugSetLastUsedChannelInZone(currentZone.NOT_IN_CODEPLUGDATA_indexNumber, newChannelIndex);

				settingsSet(nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_CHANNEL_MODE], nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber]);
				currentZone.NOT_IN_CODEPLUGDATA_numChannelsInZone++;
			}
			else
			{
				if (codeplugZoneAddChannelToZoneAndSave(newChannelIndex, &currentZone))
				{
					codeplugSetLastUsedChannelInZone(currentZone.NOT_IN_CODEPLUGDATA_indexNumber, (currentZone.NOT_IN_CODEPLUGDATA_numChannelsInZone - 1));
				}
				else
				{
					// channelScreenChannelData wasn't modified, only a new channel has been added, and it's available in AllZone.
					nextKeyBeepMelody = (int16_t *)MELODY_NACK_BEEP;
					return true;
				}
			}

			// Channel saving succeeded, now we're sure that channel could
			// be used in the channel screen.
			memcpy(&channelScreenChannelData, &tempChannel, sizeof(tempChannel));
			uiDataGlobal.currentSelectedChannelNumber = newChannelIndex;

			channelScreenChannelData.rxFreq = 0; // NOT SURE IF THIS IS NECESSARY... Flag to the Channel screen that the channel data is now invalid and needs to be reloaded
			uiDataGlobal.VoicePrompts.inhibitInitial = true;
			tsSetManualOverride(((Channel_t) CHANNEL_CHANNEL), (currentTS + 1)); //copy current TS

			// Just override TG/PC blindly, if not already set
			if (nonVolatileSettings.overrideTG == 0)
			{
				settingsSet(nonVolatileSettings.overrideTG, trxTalkGroupOrPcId);
			}

			menuSystemPopAllAndDisplaySpecificRootMenu(UI_CHANNEL_MODE, true);
			nextKeyBeepMelody = (int16_t *)MELODY_ACK_BEEP;
			quickmenuNewChannelHandled = false; // Need to do this, as uiVFOModeQuickMenu() won't be re-entered on the next menu iteration
			return true;
		}

		nextKeyBeepMelody = (int16_t *)MELODY_NACK_BEEP;
	}

	return true;
}

static void updateQuickMenuScreen(bool isFirstRun)
{
	int mNum = 0;
	char buf[SCREEN_LINE_BUFFER_SIZE];
	const char *leftSide;// initialise to please the compiler
	const char *rightSideConst;// initialise to please the compiler
	char rightSideVar[SCREEN_LINE_BUFFER_SIZE];
	int prompt;// For voice prompts

	displayClearBuf();
	bool settingOption = uiQuickKeysShowChoices(buf, SCREEN_LINE_BUFFER_SIZE, currentLanguage->quick_menu);

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		if ((settingOption == false) || (i == 0))
		{
			mNum = menuGetMenuOffset(NUM_VFO_SCREEN_QUICK_MENU_ITEMS, i);
			if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY)
			{
				continue;
			}
			else if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)
			{
				break;
			}

			prompt = -1;// Prompt not used
			buf[0] = 0;
			rightSideVar[0] = 0;
			rightSideConst = NULL;
			leftSide = NULL;

			switch(mNum)
			{
#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_RD5R) || defined(PLATFORM_DM1801A) || defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				case VFO_SCREEN_QUICK_MENU_VFO_A_B:
					sprintf(rightSideVar, "VFO %c", ((uiDataGlobal.QuickMenu.tmpVFONumber == 0) ? 'A' : 'B'));
					break;
#endif
				case VFO_SCREEN_QUICK_MENU_TX_SWAP_RX:
					prompt = PROMPT_VFO_EXCHANGE_TX_RX;
					strcpy(rightSideVar, "Tx <--> Rx");
					break;
				case VFO_SCREEN_QUICK_MENU_BOTH_TO_RX:
					prompt = PROMPT_VFO_COPY_RX_TO_TX;
					strcpy(rightSideVar, "Rx --> Tx");
					break;
				case VFO_SCREEN_QUICK_MENU_BOTH_TO_TX:
					prompt = PROMPT_VFO_COPY_TX_TO_RX;
					strcpy(rightSideVar, "Tx --> Rx");
					break;
				case VFO_SCREEN_QUICK_MENU_FILTER_FM:
					leftSide = currentLanguage->filter;
					if (uiDataGlobal.QuickMenu.tmpAnalogFilterLevel == 0)
					{
						rightSideConst = currentLanguage->none;
					}
					else
					{
						snprintf(rightSideVar, SCREEN_LINE_BUFFER_SIZE, "%s", ANALOG_FILTER_LEVELS[uiDataGlobal.QuickMenu.tmpAnalogFilterLevel - 1]);
					}
					break;
				case VFO_SCREEN_QUICK_MENU_FILTER_DMR:
					leftSide = currentLanguage->dmr_filter;
					if (uiDataGlobal.QuickMenu.tmpDmrDestinationFilterLevel == 0)
					{
						rightSideConst = currentLanguage->none;
					}
					else
					{
						snprintf(rightSideVar, SCREEN_LINE_BUFFER_SIZE, "%s", DMR_DESTINATION_FILTER_LEVELS[uiDataGlobal.QuickMenu.tmpDmrDestinationFilterLevel - 1]);
					}
					break;
				case VFO_SCREEN_QUICK_MENU_DMR_CC_SCAN:
					leftSide = currentLanguage->dmr_cc_scan;
					rightSideConst = (uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel & DMR_CC_FILTER_PATTERN) ? currentLanguage->off : currentLanguage->on;
					break;
				case VFO_SCREEN_QUICK_MENU_FILTER_DMR_TS:
					leftSide = currentLanguage->dmr_ts_filter;
					rightSideConst = (uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel & DMR_TS_FILTER_PATTERN) ? currentLanguage->on : currentLanguage->off;
					break;
				case VFO_SCREEN_QUICK_MENU_VFO_TO_NEW:
					rightSideConst = currentLanguage->vfoToNewChannel;
					break;
				case VFO_SCREEN_QUICK_MENU_TONE_SCAN:
					leftSide = currentLanguage->tone_scan;
					if(trxGetMode() == RADIO_MODE_ANALOG)
					{
						const char *scanCSS[] = { currentLanguage->all, "CTCSS", "DCS", "iDCS" };
						uint8_t offset = 0;

						if (uiDataGlobal.QuickMenu.tmpToneScanCSS == CSS_TYPE_NONE)
						{
							offset = 0;
						}
						else if (uiDataGlobal.QuickMenu.tmpToneScanCSS == CSS_TYPE_CTCSS)
						{
							offset = 1;
						}
						else if (uiDataGlobal.QuickMenu.tmpToneScanCSS == CSS_TYPE_DCS)
						{
							offset = 2;
						}
						else if (uiDataGlobal.QuickMenu.tmpToneScanCSS == (CSS_TYPE_DCS | CSS_TYPE_DCS_INVERTED))
						{
							offset = 3;
						}

						snprintf(rightSideVar, SCREEN_LINE_BUFFER_SIZE, "%s", scanCSS[offset]);

						if (uiDataGlobal.QuickMenu.tmpToneScanCSS == CSS_TYPE_NONE)
						{
							rightSideConst = currentLanguage->all;
						}
					}
					else
					{
						rightSideConst = currentLanguage->n_a;
					}
					break;
				case VFO_SCREEN_QUICK_MENU_DUAL_SCAN:
					rightSideConst = currentLanguage->dual_watch;
					break;
				case VFO_SCREEN_QUICK_MENU_FREQ_BIND_MODE:
					leftSide = currentLanguage->vfo_freq_bind_mode;
					rightSideConst = (!uiDataGlobal.QuickMenu.tmpTxRxLockMode) ? currentLanguage->on : currentLanguage->off;
					break;
				case VFO_SCREEN_QUICK_MENU_AUDIO_MUTE:
					leftSide = currentLanguage->mute;
					rightSideConst = (uiDataGlobal.QuickMenu.tmpAudioMute ? currentLanguage->yes : currentLanguage->no);
					break;
				default:
					strcpy(buf, "");
					break;
			}

			if (leftSide != NULL)
			{
				snprintf(buf, SCREEN_LINE_BUFFER_SIZE, "%s:%s", leftSide, (rightSideVar[0] ? rightSideVar : rightSideConst));
			}
			else
			{
				snprintf(buf, SCREEN_LINE_BUFFER_SIZE, "%s", ((rightSideVar[0] != 0) ? rightSideVar : rightSideConst));
			}

			if (i == 0)
			{
				if (!isFirstRun && (menuDataGlobal.menuOptionsSetQuickkey == 0))
				{
					voicePromptsInit();
				}

				if (prompt != -1)
				{
					voicePromptsAppendPrompt(prompt);
				}
				else
				{
					if ((leftSide != NULL) || menuDataGlobal.newOptionSelected)
					{
						voicePromptsAppendLanguageString(leftSide);
					}

					if ((rightSideVar[0] != 0) && (rightSideConst == NULL))
					{
						voicePromptsAppendString(rightSideVar);
					}
					else
					{
						voicePromptsAppendLanguageString(rightSideConst);
					}
				}

				if (menuDataGlobal.menuOptionsTimeout != -1)
				{
					promptsPlayNotAfterTx();
				}
				else
				{
					menuDataGlobal.menuOptionsTimeout = 0;// clear flag indicating that a QuickKey has just been set
				}
			}

			// QuickKeys
			if (menuDataGlobal.menuOptionsTimeout > 0)
			{
				menuDisplaySettingOption(leftSide, (rightSideVar[0] ? rightSideVar : rightSideConst));
			}
			else
			{
				switch (mNum)
				{
					case VFO_SCREEN_QUICK_MENU_FILTER_FM:
					case VFO_SCREEN_QUICK_MENU_FILTER_DMR:
					case VFO_SCREEN_QUICK_MENU_DMR_CC_SCAN:
					case VFO_SCREEN_QUICK_MENU_FILTER_DMR_TS:
					case VFO_SCREEN_QUICK_MENU_TONE_SCAN:
					case VFO_SCREEN_QUICK_MENU_FREQ_BIND_MODE:
					case VFO_SCREEN_QUICK_MENU_AUDIO_MUTE:
						menuDisplayEntry(i, mNum, buf, (strlen(leftSide) + 1), THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
						break;

					default:
						menuDisplayEntry(i, mNum, buf, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_COLOUR_NONE, THEME_ITEM_BG);
						break;
				}
			}
		}
	}
	displayRender();
}

static void handleQuickMenuEvent(uiEvent_t *ev)
{
	bool isDirty = false;
	bool executingQuickKey = false;

	if ((menuDataGlobal.menuOptionsTimeout > 0) && (!BUTTONCHECK_DOWN(ev, BUTTON_SK2)))
	{
		menuDataGlobal.menuOptionsTimeout--;
		if (menuDataGlobal.menuOptionsTimeout == 0)
		{
			// Let the QuickKey's VP playback to ends before
			// going back to the previous menu
			if (voicePromptsIsPlaying())
			{
				menuDataGlobal.menuOptionsTimeout++;
				return;
			}

			menuSystemPopPreviousMenu();
			return;
		}
	}

	if (ev->events & BUTTON_EVENT)
	{
		if (repeatVoicePromptOnSK1(ev))
		{
			return;
		}
	}

	if (ev->events & FUNCTION_EVENT)
	{
		isDirty = true;
		if (ev->function == FUNC_REDRAW)
		{
			updateQuickMenuScreen(false);
			return;
		}
		else if ((QUICKKEY_TYPE(ev->function) == QUICKKEY_MENU) && (QUICKKEY_ENTRYID(ev->function) < NUM_VFO_SCREEN_QUICK_MENU_ITEMS))
		{
			menuDataGlobal.currentItemIndex = QUICKKEY_ENTRYID(ev->function);
		}

		if ((QUICKKEY_FUNCTIONID(ev->function) != 0))
		{
			menuDataGlobal.menuOptionsTimeout = 1000;
			executingQuickKey = true;
		}
	}

	if ((ev->events & (KEY_EVENT | BUTTON_EVENT)) && (menuDataGlobal.menuOptionsSetQuickkey == 0) && (menuDataGlobal.menuOptionsTimeout == 0))
	{
		if (KEYCHECK_PRESS(ev->keys, KEY_DOWN) && (menuDataGlobal.numItems != 0))
		{
			isDirty = true;
			menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, NUM_VFO_SCREEN_QUICK_MENU_ITEMS);
			menuDataGlobal.newOptionSelected = true;
			menuQuickVFOExitStatus |= MENU_STATUS_LIST_TYPE;
		}
		else if (KEYCHECK_PRESS(ev->keys, KEY_UP))
		{
			isDirty = true;
			menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, NUM_VFO_SCREEN_QUICK_MENU_ITEMS);
			menuDataGlobal.newOptionSelected = true;
			menuQuickVFOExitStatus |= MENU_STATUS_LIST_TYPE;
		}
		else if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
		{

			quickKeyApply: // branching here when to quickkey was used.

#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_RD5R) || defined(PLATFORM_DM1801A) || defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
			if (nonVolatileSettings.currentVFONumber != uiDataGlobal.QuickMenu.tmpVFONumber)
			{
				settingsSet(nonVolatileSettings.currentVFONumber, uiDataGlobal.QuickMenu.tmpVFONumber);
				currentChannelData = &settingsVFOChannel[nonVolatileSettings.currentVFONumber];
			}
#endif
			toneScanCSS = uiDataGlobal.QuickMenu.tmpToneScanCSS;

			switch(menuDataGlobal.currentItemIndex)
			{
				case VFO_SCREEN_QUICK_MENU_TX_SWAP_RX:
				{
					int tmpFreq = currentChannelData->txFreq;
					currentChannelData->txFreq = currentChannelData->rxFreq;
					currentChannelData->rxFreq = tmpFreq;
					trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
					announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ, PROMPT_THRESHOLD_3);
				}
				break;

				case VFO_SCREEN_QUICK_MENU_BOTH_TO_RX:
					currentChannelData->txFreq = currentChannelData->rxFreq;
					trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
					announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ, PROMPT_THRESHOLD_3);
					break;

				case VFO_SCREEN_QUICK_MENU_BOTH_TO_TX:
					currentChannelData->rxFreq = currentChannelData->txFreq;
					trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
					announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ, PROMPT_THRESHOLD_3);
					break;

				case VFO_SCREEN_QUICK_MENU_VFO_TO_NEW:
					if (quickmenuNewChannelHandled == false)
					{
						snprintf(uiDataGlobal.MessageBox.message, MESSAGEBOX_MESSAGE_LEN_MAX, "%s\n%s", currentLanguage->new_channel, currentLanguage->please_confirm);
						uiDataGlobal.MessageBox.type = MESSAGEBOX_TYPE_INFO;
						uiDataGlobal.MessageBox.decoration = MESSAGEBOX_DECORATION_FRAME;
						uiDataGlobal.MessageBox.buttons = MESSAGEBOX_BUTTONS_YESNO;
						uiDataGlobal.MessageBox.validatorCallback = validateNewChannel;
						menuSystemPushNewMenu(UI_MESSAGE_BOX);

						voicePromptsInit();
						voicePromptsAppendLanguageString(currentLanguage->new_channel);
						voicePromptsAppendLanguageString(currentLanguage->please_confirm);
						voicePromptsPlay();
					}
					return;
					break;

				case VFO_SCREEN_QUICK_MENU_TONE_SCAN:
					if (trxGetMode() == RADIO_MODE_ANALOG)
					{
						trxSetAnalogFilterLevel(ANALOG_FILTER_CSS);
						bool cssTypesDiffer = false;
						CodeplugCSSTypes_t currentCSSType = codeplugGetCSSType(currentChannelData->rxTone);

						// Check if the current CSS differs from the one set to scan.
						if (((currentCSSType & CSS_TYPE_NONE) == 0) && ((toneScanCSS & CSS_TYPE_NONE) == 0) && (toneScanCSS != currentCSSType))
						{
							cssTypesDiffer = true;
						}

						//                                          CTCSS or DCS         no CSS
						toneScanType = (((toneScanCSS & CSS_TYPE_NONE) == 0) ? toneScanCSS : currentCSSType);
						prevCSSTone = currentChannelData->rxTone;

						if ((currentCSSType == CSS_TYPE_NONE) || cssTypesDiffer)
						{
							// CSS type are different, start from index 0
							scanToneIndex = 0;
							if (toneScanType == CSS_TYPE_NONE)
							{
								toneScanType = CSS_TYPE_CTCSS;
							}
							currentChannelData->rxTone = cssGetToneFromIndex(scanToneIndex, toneScanType);
						}
						else
						{
							// Get the tone index in the current type array.
							scanToneIndex = cssGetToneIndex(currentChannelData->rxTone, toneScanType);

							// Set the tone to the next one
							cssIncrement(&currentChannelData->rxTone, &scanToneIndex, 1, &toneScanType, true, (toneScanCSS != CSS_TYPE_NONE));
						}

						audioAmpDisable(AUDIO_AMP_CHANNEL_RF);
						trxRxAndTxOff(true);
						trxSetRxCSS(RADIO_DEVICE_PRIMARY, currentChannelData->rxTone);
						trxRxOn(true);

						uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
						uiDataGlobal.Scan.toneActive = true;
						uiDataGlobal.Scan.refreshOnEveryStep = false;
						uiDataGlobal.Scan.timer.timeout = ((toneScanType == CSS_TYPE_CTCSS) ? (SCAN_TONE_INTERVAL - (scanToneIndex * 2)) : SCAN_TONE_INTERVAL);
						uiDataGlobal.Scan.direction = 1;
					}
					break;

				case VFO_SCREEN_QUICK_MENU_DUAL_SCAN:
					uiDataGlobal.Scan.active = true;
					uiDataGlobal.Scan.stepTimeMilliseconds = settingsGetScanStepTimeMilliseconds();
					uiDataGlobal.Scan.dwellTime = 135;// for Dual Watch, use a larger step time than normally scanning, and which does not synchronise with the DMR 30ms timeslots
					uiDataGlobal.Scan.timer.timeout = uiDataGlobal.Scan.dwellTime;
					uiDataGlobal.Scan.refreshOnEveryStep = false;
					screenOperationMode[CHANNEL_VFO_A] = screenOperationMode[CHANNEL_VFO_B] = VFO_SCREEN_OPERATION_DUAL_SCAN;
					uiDataGlobal.VoicePrompts.inhibitInitial = true;
					uiDataGlobal.Scan.scanType = SCAN_TYPE_DUAL_WATCH;
					int currentPowerSavingLevel = rxPowerSavingGetLevel();
					if (currentPowerSavingLevel > 1)
					{
						rxPowerSavingSetLevel(currentPowerSavingLevel - 1);
					}
					break;

				default:
					// VFO_SCREEN_QUICK_MENU_FILTER_FM
					if (nonVolatileSettings.analogFilterLevel != uiDataGlobal.QuickMenu.tmpAnalogFilterLevel)
					{
						settingsSet(nonVolatileSettings.analogFilterLevel, uiDataGlobal.QuickMenu.tmpAnalogFilterLevel);
						trxSetAnalogFilterLevel(nonVolatileSettings.analogFilterLevel);
					}

					// VFO_SCREEN_QUICK_MENU_FILTER_DMR
					if (nonVolatileSettings.dmrDestinationFilter != uiDataGlobal.QuickMenu.tmpDmrDestinationFilterLevel)
					{
						settingsSet(nonVolatileSettings.dmrDestinationFilter, uiDataGlobal.QuickMenu.tmpDmrDestinationFilterLevel);
						if (trxGetMode() == RADIO_MODE_DIGITAL)
						{
							HRC6000InitDigitalDmrRx();
							audioAmpDisable(AUDIO_AMP_CHANNEL_RF);
						}
					}

					// VFO_SCREEN_QUICK_MENU_DMR_CC_FILTER
					// VFO_SCREEN_QUICK_MENU_FILTER_DMR_TS
					if (nonVolatileSettings.dmrCcTsFilter != uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel)
					{
						settingsSet(nonVolatileSettings.dmrCcTsFilter, uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel);
						if (trxGetMode() == RADIO_MODE_DIGITAL)
						{
							HRC6000InitDigitalDmrRx();
							HRC6000ResyncTimeSlot();
							audioAmpDisable(AUDIO_AMP_CHANNEL_RF);
						}
					}

					// VFO_SCREEN_QUICK_MENU_FREQ_BIND_MODE
					if (settingsIsOptionBitSet(BIT_TX_RX_FREQ_LOCK) != uiDataGlobal.QuickMenu.tmpTxRxLockMode)
					{
						settingsSetOptionBit(BIT_TX_RX_FREQ_LOCK, uiDataGlobal.QuickMenu.tmpTxRxLockMode);
					}

					// VFO_SCREEN_QUICK_MENU_MUTE
					if (audioAmpIsMuted() != uiDataGlobal.QuickMenu.tmpAudioMute)
					{
						audioAmpMute(uiDataGlobal.QuickMenu.tmpAudioMute);
					}
					break;
			}

			if (executingQuickKey)
			{
				updateQuickMenuScreen(false);
			}
			else
			{
				menuSystemPopPreviousMenu();
			}
			return;
		}
		else if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
		{
			uiVFOModeStopScanning();
			menuSystemPopPreviousMenu();
			return;
		}
#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_RD5R) || defined(PLATFORM_DM1801A) || defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801A) || defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
		else if (((ev->events & BUTTON_EVENT) && BUTTONCHECK_SHORTUP(ev, BUTTON_ORANGE)) && (menuDataGlobal.currentItemIndex == VFO_SCREEN_QUICK_MENU_VFO_A_B))
#elif defined(PLATFORM_RD5R)
		else if (KEYCHECK_SHORTUP(ev->keys, KEY_VFO_MR) && (menuDataGlobal.currentItemIndex == VFO_SCREEN_QUICK_MENU_VFO_A_B))
#endif
		{
			settingsSet(nonVolatileSettings.currentVFONumber, (1 - uiDataGlobal.QuickMenu.tmpVFONumber));// Switch to other VFO
			currentChannelData = &settingsVFOChannel[nonVolatileSettings.currentVFONumber];
			menuSystemPopPreviousMenu();
			if (nonVolatileSettings.currentVFONumber == 0)
			{
				// Trick to beep (AudioAssist), since ORANGE button doesn't produce any beep event
				ev->keys.event |= KEY_MOD_UP;
				ev->keys.key = 127;
				// End Trick

				menuQuickVFOExitStatus |= MENU_STATUS_FORCE_FIRST;
			}

			return;
		}
#endif
		else if (KEYCHECK_SHORTUP_NUMBER(ev->keys) && BUTTONCHECK_DOWN(ev, BUTTON_SK2))
		{
			isDirty = true;
			menuDataGlobal.menuOptionsSetQuickkey = ev->keys.key;
		}
	}


	if ((ev->events & (KEY_EVENT | FUNCTION_EVENT)) && (menuDataGlobal.menuOptionsSetQuickkey == 0))
	{
		if (KEYCHECK_PRESS(ev->keys, KEY_RIGHT)
#if defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				|| KEYCHECK_SHORTUP(ev->keys, KEY_ROTARY_INCREMENT)
#endif
				|| (QUICKKEY_FUNCTIONID(ev->function) == FUNC_RIGHT))
		{
			if (menuDataGlobal.menuOptionsTimeout > 0)
			{
				menuDataGlobal.menuOptionsTimeout = 1000;
			}
			isDirty = true;
			menuDataGlobal.newOptionSelected = false;

			switch(menuDataGlobal.currentItemIndex)
			{
#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_RD5R) || defined(PLATFORM_DM1801A) || defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				case VFO_SCREEN_QUICK_MENU_VFO_A_B:
					if (uiDataGlobal.QuickMenu.tmpVFONumber == 0)
					{
						uiDataGlobal.QuickMenu.tmpVFONumber = 1;
					}
					break;
#endif
				case VFO_SCREEN_QUICK_MENU_FILTER_FM:
					if (uiDataGlobal.QuickMenu.tmpAnalogFilterLevel < NUM_ANALOG_FILTER_LEVELS - 1)
					{
						uiDataGlobal.QuickMenu.tmpAnalogFilterLevel++;
					}
					break;
				case VFO_SCREEN_QUICK_MENU_FILTER_DMR:
					if (uiDataGlobal.QuickMenu.tmpDmrDestinationFilterLevel < NUM_DMR_DESTINATION_FILTER_LEVELS - 1)
					{
						uiDataGlobal.QuickMenu.tmpDmrDestinationFilterLevel++;
					}
					break;
				case VFO_SCREEN_QUICK_MENU_DMR_CC_SCAN:
					if (uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel & DMR_CC_FILTER_PATTERN)
					{
						uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel &= ~DMR_CC_FILTER_PATTERN;
					}
					break;
				case VFO_SCREEN_QUICK_MENU_FILTER_DMR_TS:
					if (!(uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel & DMR_TS_FILTER_PATTERN))
					{
						uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel |= DMR_TS_FILTER_PATTERN;
					}
					break;
				case VFO_SCREEN_QUICK_MENU_TONE_SCAN:
					if (trxGetMode() == RADIO_MODE_ANALOG)
					{
						if (uiDataGlobal.QuickMenu.tmpToneScanCSS == CSS_TYPE_NONE)
						{
							uiDataGlobal.QuickMenu.tmpToneScanCSS = CSS_TYPE_CTCSS;
						}
						else if (uiDataGlobal.QuickMenu.tmpToneScanCSS == CSS_TYPE_CTCSS)
						{
							uiDataGlobal.QuickMenu.tmpToneScanCSS = CSS_TYPE_DCS;
						}
						else if (uiDataGlobal.QuickMenu.tmpToneScanCSS == CSS_TYPE_DCS)
						{
							uiDataGlobal.QuickMenu.tmpToneScanCSS = (CSS_TYPE_DCS | CSS_TYPE_DCS_INVERTED);
						}
					}
					break;
				case VFO_SCREEN_QUICK_MENU_FREQ_BIND_MODE:
					uiDataGlobal.QuickMenu.tmpTxRxLockMode = false;
					break;
				case VFO_SCREEN_QUICK_MENU_AUDIO_MUTE:
					uiDataGlobal.QuickMenu.tmpAudioMute = false;
					break;
			}

			if (executingQuickKey) // Instantly apply new setting
			{
				goto quickKeyApply;
			}
		}
		else if (KEYCHECK_PRESS(ev->keys, KEY_LEFT)
#if defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				|| KEYCHECK_SHORTUP(ev->keys, KEY_ROTARY_DECREMENT)
#endif
				|| (QUICKKEY_FUNCTIONID(ev->function) == FUNC_LEFT))
		{
			if (menuDataGlobal.menuOptionsTimeout > 0)
			{
				menuDataGlobal.menuOptionsTimeout = 1000;
			}
			isDirty = true;
			menuDataGlobal.newOptionSelected = false;

			switch(menuDataGlobal.currentItemIndex)
			{
#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_RD5R) || defined(PLATFORM_DM1801A) || defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				case VFO_SCREEN_QUICK_MENU_VFO_A_B:
					if (uiDataGlobal.QuickMenu.tmpVFONumber == 1)
					{
						uiDataGlobal.QuickMenu.tmpVFONumber = 0;
					}
					menuQuickVFOExitStatus |= MENU_STATUS_FORCE_FIRST;
					break;
#endif
				case VFO_SCREEN_QUICK_MENU_FILTER_FM:
					if (uiDataGlobal.QuickMenu.tmpAnalogFilterLevel > ANALOG_FILTER_NONE)
					{
						uiDataGlobal.QuickMenu.tmpAnalogFilterLevel--;
					}
					break;
				case VFO_SCREEN_QUICK_MENU_FILTER_DMR:
					if (uiDataGlobal.QuickMenu.tmpDmrDestinationFilterLevel > DMR_DESTINATION_FILTER_NONE)
					{
						uiDataGlobal.QuickMenu.tmpDmrDestinationFilterLevel--;
					}
					break;
				case VFO_SCREEN_QUICK_MENU_DMR_CC_SCAN:
					if (!(uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel & DMR_CC_FILTER_PATTERN))
					{
						uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel |= DMR_CC_FILTER_PATTERN;
					}
					break;
				case VFO_SCREEN_QUICK_MENU_FILTER_DMR_TS:
					if (uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel & DMR_TS_FILTER_PATTERN)
					{
						uiDataGlobal.QuickMenu.tmpDmrCcTsFilterLevel &= ~DMR_TS_FILTER_PATTERN;
					}
					break;
				case VFO_SCREEN_QUICK_MENU_TONE_SCAN:
					if (trxGetMode() == RADIO_MODE_ANALOG)
					{
						if (uiDataGlobal.QuickMenu.tmpToneScanCSS == CSS_TYPE_CTCSS)
						{
							uiDataGlobal.QuickMenu.tmpToneScanCSS = CSS_TYPE_NONE;
						}
						else if (uiDataGlobal.QuickMenu.tmpToneScanCSS == CSS_TYPE_DCS)
						{
							uiDataGlobal.QuickMenu.tmpToneScanCSS = CSS_TYPE_CTCSS;
						}
						else if (uiDataGlobal.QuickMenu.tmpToneScanCSS == (CSS_TYPE_DCS | CSS_TYPE_DCS_INVERTED))
						{
							uiDataGlobal.QuickMenu.tmpToneScanCSS = CSS_TYPE_DCS;
						}
					}
					break;
				case VFO_SCREEN_QUICK_MENU_FREQ_BIND_MODE:
					uiDataGlobal.QuickMenu.tmpTxRxLockMode = true;
					break;
				case VFO_SCREEN_QUICK_MENU_AUDIO_MUTE:
					uiDataGlobal.QuickMenu.tmpAudioMute = true;
					break;
			}

			if (executingQuickKey) // Instantly apply new setting
			{
				goto quickKeyApply;
			}
		}
		else if ((ev->keys.event & KEY_MOD_PRESS) && (menuDataGlobal.menuOptionsTimeout > 0))
		{
			menuDataGlobal.menuOptionsTimeout = 0;
			menuSystemPopPreviousMenu();
			return;
		}
	}

	if (uiQuickKeysIsStoring(ev))
	{
		uiQuickKeysStore(ev, &menuQuickVFOExitStatus);
		isDirty = true;
	}

	if (isDirty)
	{
		updateQuickMenuScreen(false);
	}
}

bool uiVFOModeIsScanning(void)
{
	return (uiDataGlobal.Scan.toneActive || uiDataGlobal.Scan.active);
}

bool uiVFOModeDualWatchIsScanning(void)
{
	return ((menuSystemGetCurrentMenuNumber() == UI_VFO_MODE) && uiDataGlobal.Scan.active &&
			(uiDataGlobal.Scan.state == SCAN_STATE_SCANNING) && (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_DUAL_SCAN));
}

bool uiVFOModeSweepScanning(bool includePaused)
{
	return ((menuSystemGetCurrentMenuNumber() == UI_VFO_MODE) &&
			uiDataGlobal.Scan.active &&
			(includePaused ? ((uiDataGlobal.Scan.state == SCAN_STATE_SCANNING) || (uiDataGlobal.Scan.state == SCAN_STATE_PAUSED)) : (uiDataGlobal.Scan.state == SCAN_STATE_SCANNING)) &&
			(screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SWEEP));
}

#if defined(ENABLE_FAST_SCAN)
uint32_t uiVFOModeSweepSpan(void)
{
	return vfoSweepSpan();
}

bool uiVFOModeSweepIsAutoScrolling(void)
{
	return vfoSweepAutoScroll;
}

uint8_t uiVFOModeSweepDwellPasses(void)
{
	return vfoSweepDwellPasses;
}
#endif

/* Needs BOTH flags: the modes it drives are ENABLE_FAST_SCAN state, so an
 * ENABLE_SPECTRUM-only build (which build_spec.sh makes) would not compile without this.
 * Caught by building that configuration, not by reading -- the two dev flags are usually
 * set together and the broken combination is the one nobody exercises. */
#if defined(ENABLE_SPECTRUM) && defined(ENABLE_FAST_SCAN)
/* DEV: drive the two new sweep modes from the host (CPS 0xB1).
 *
 * Not a convenience. The wide span is selected with SK2 + UP, and SK2 is a BUTTON --
 * the keypad injector queues KEYS, and BUTTONCHECK_DOWN() reads the button state, so on
 * the dead-panel guinea-pig there is otherwise no way to reach this control at all and
 * the feature could only ever be verified by eye on the other radio. Compiles to nothing
 * in a release build. */
void uiVFOModeSweepSetModes(bool wide, bool autoScroll, uint8_t stepIndex, bool persist,
		uint8_t dwellPasses)
{
	if ((dwellPasses >= 1) && (dwellPasses <= 32))
	{
		vfoSweepDwellPasses = dwellPasses;
		vfoSweepPassesDone = 0;
	}

	/* ★ Does NOT write the settings unless asked to, and that is not a detail.
	 *
	 * The stored word packs sweepStepSizeIndex, and that variable is only loaded from
	 * settings when the sweep is ENTERED. Rebuilding the word from it at any other moment
	 * persists a zero over whatever the user had chosen. This function is callable from
	 * the host at any time, and the first version did exactly that: one call made outside
	 * the sweep silently reset a stored span index of 6 to 0. The real UI paths cannot hit
	 * this -- both are reachable only while sweeping -- so the hazard belongs to the host
	 * hook alone, and it is fixed here rather than by remembering not to call it wrong. */
	if (stepIndex <= 6)
	{
		uiDataGlobal.Scan.sweepStepSizeIndex = stepIndex;
	}

	if (wide != vfoSweepWide)
	{
		vfoSweepWide = wide;

		// Same invalidation the SK2 path does: every bin covers a different frequency now.
		memset(vfoSweepSamples, 0x00, sizeof(vfoSweepSamples));
		memset(vfoSweepHold, 0x00, sizeof(vfoSweepHold));
		vfoSweepShownPeakLevel = 0;
		vfoSweepShownPeakIndex = -1;
		vfoSweepPeakLevel = 0;
		vfoSweepPeakIndex = -1;
		vfoSweepSubIndex = 0;
		vfoSweepColumnPeak = 0;
		vfoSweepAutoPrimed = false;
		uiDataGlobal.Scan.sweepSampleIndex = 0;
	}

	vfoSweepAutoScroll = autoScroll;

	if (persist)
	{
		settingsSet(nonVolatileSettings.vfoSweepSettings,
				VFO_SWEEP_SETTINGS_WORD(uiDataGlobal.Scan.sweepStepSizeIndex,
						vfoSweepRssiNoiseFloor, vfoSweepGain));
	}
}

bool uiVFOModeSweepIsWide(void)
{
	return vfoSweepWide;
}
#endif

bool uiVFOModeFrequencyScanningIsActiveAndEnabled(uint32_t *lowFreq, uint32_t *highFreq)
{
	bool ret = ((menuSystemGetCurrentMenuNumber() == UI_VFO_MODE) && (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN));

	if (ret && lowFreq && highFreq)
	{
		*lowFreq = nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber];
		*highFreq = nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber];
	}

	return ret;
}

static void toneScan(void)
{
	if (audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF)
	{
		currentChannelData->txTone = currentChannelData->rxTone;
		uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
		uiVFOModeUpdateScreen(0);
		prevCSSTone = (CODEPLUG_CSS_TONE_NONE - 1);
		uiDataGlobal.Scan.toneActive = false;
		return;
	}

	if (uiDataGlobal.Scan.timer.timeout > 0)
	{
		uiDataGlobal.Scan.timer.timeout--;
	}
	else
	{
		if (uiDataGlobal.Scan.direction == 1)
		{
			cssIncrement(&currentChannelData->rxTone, &scanToneIndex, 1, &toneScanType, true, (toneScanCSS != CSS_TYPE_NONE));
		}
		else
		{
			cssDecrement(&currentChannelData->rxTone, &scanToneIndex, 1, &toneScanType, true, (toneScanCSS != CSS_TYPE_NONE));
		}
		trxRxAndTxOff(true);
		trxSetRxCSS(RADIO_DEVICE_PRIMARY, currentChannelData->rxTone);
		uiDataGlobal.Scan.timer.timeout = ((toneScanType == CSS_TYPE_CTCSS) ? (SCAN_TONE_INTERVAL - (scanToneIndex * 2)) : SCAN_TONE_INTERVAL);
		trxRxOn(true);
		uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
		uiVFOModeUpdateScreen(0);
	}
}

static void updateTrxID(void)
{
	if (nonVolatileSettings.overrideTG != 0)
	{
		trxTalkGroupOrPcId = nonVolatileSettings.overrideTG;
	}
	else
	{
		//tsSetManualOverride(((Channel_t)nonVolatileSettings.currentVFONumber), TS_NO_OVERRIDE);

		// Check if this channel has an Rx Group
		if ((currentRxGroupData.name[0] != 0) && (nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber] < currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup))
		{
			codeplugContactGetDataForIndex(currentRxGroupData.contacts[nonVolatileSettings.currentIndexInTRxGroupList[SETTINGS_VFO_A_MODE + nonVolatileSettings.currentVFONumber]], &currentContactData);
		}
		else
		{
			codeplugContactGetDataForIndex(currentChannelData->contact, &currentContactData);
		}

		trxTalkGroupOrPcId = codeplugContactGetPackedId(&currentContactData);

		tsSetContactHasBeenOverriden(((Channel_t)nonVolatileSettings.currentVFONumber), false);

		trxUpdateTsForCurrentChannelWithSpecifiedContact(&currentContactData);
	}
	lastHeardClearLastID();
	menuPrivateCallClear();
}

static void setCurrentFreqToScanLimits(void)
{
	if((currentChannelData->rxFreq < nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber]) ||
			(currentChannelData->rxFreq > nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber]))    //if we are not already inside the Low and High Limit freqs then move to the low limit.
	{
		int offset = currentChannelData->txFreq - currentChannelData->rxFreq;

		currentChannelData->rxFreq = nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber];
		currentChannelData->txFreq = currentChannelData->rxFreq + offset;
		trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
		announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ, PROMPT_THRESHOLD_3);
	}
}

void uiVFOSweepScanModePause(bool pause, bool forceDigitalOnPause)
{
	if (pause)
	{
		uiDataGlobal.Scan.state = SCAN_STATE_PAUSED;
		vfoSweepSavedBandwidth = trxGetBandwidthIs25kHz();
		if (forceDigitalOnPause)
		{
			trxSetModeAndBandwidth(RADIO_MODE_DIGITAL, false);
		}
		trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
		vfoSweepUpdateSamples(0, true, 0);// Force redraw to get rid of the cursor (perhaps we should draw it in the middle);
	}
	else
	{
		trxTerminateCheckAnalogSquelch(RADIO_DEVICE_PRIMARY);
		trxSetModeAndBandwidth(currentChannelData->chMode, vfoSweepSavedBandwidth);
		uiDataGlobal.Scan.state = SCAN_STATE_SCANNING;
		LedWrite(LED_GREEN, 0);
		LedWrite(LED_RED, 0);
		vfoSweepUpdateSamples(0, true, 0);
		headerRowIsDirty = true;
	}
}

static void sweepScanInit(void)
{
	trxTerminateCheckAnalogSquelch(RADIO_DEVICE_PRIMARY);

	uiDataGlobal.Scan.active = true;
	uiDataGlobal.Scan.state = SCAN_STATE_SCANNING;
	uiDataGlobal.Scan.scanType = SCAN_TYPE_NORMAL_STEP;

	uiDataGlobal.VoicePrompts.inhibitInitial = true;

	if (voicePromptsIsPlaying())
	{
		voicePromptsTerminate();
	}

	if (nonVolatileSettings.audioPromptMode >= AUDIO_PROMPT_MODE_VOICE_LEVEL_2)
	{
		voicePromptsInit();
		voicePromptsAppendPrompt(PROMPT_SWEEP_SCAN_MODE);
		voicePromptsPlay();
	}

	uiDataGlobal.Scan.sweepStepSizeIndex = ((nonVolatileSettings.vfoSweepSettings >> 12) & 0x7);
	vfoSweepRssiNoiseFloor = ((nonVolatileSettings.vfoSweepSettings >> 7) & 0x1F);
	vfoSweepGain = (nonVolatileSettings.vfoSweepSettings & 0x7F);
#if defined(ENABLE_FAST_SCAN)
	vfoSweepWide = ((nonVolatileSettings.vfoSweepSettings & VFO_SWEEP_WIDE_FLAG) != 0);
	vfoSweepSubIndex = 0;
	vfoSweepColumnPeak = 0;
	// Auto scroll always starts off -- see the note at its declaration.
	vfoSweepAutoScroll = false;
	vfoSweepScrollPending = false;   // a scroll owed by a previous visit is not owed now
	vfoSweepDwellPasses = 1;
	vfoSweepPassesDone = 0;
#endif

	screenOperationMode[nonVolatileSettings.currentVFONumber] = VFO_SCREEN_OPERATION_SWEEP;

	uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;

	memset(vfoSweepSamples, 0x00, VFO_SWEEP_NUM_SAMPLES * sizeof(uint8_t));

#if defined(ENABLE_FAST_SCAN)
	// The sweep samples far faster than the stock RSSI filter settles -- see
	// VFO_SWEEP_RSSI_COUNT. Restored by uiVFOModeStopScanning().
	radioSetRssiCount(VFO_SWEEP_RSSI_COUNT);

	// Start clean: a stale peak from a previous visit would be marked, and worse, listened
	// to, before this sweep has measured anything.
	vfoSweepListening = false;
	vfoSweepPeakLevel = 0;
	vfoSweepPeakIndex = -1;
	vfoSweepShownPeakLevel = 0;
	vfoSweepShownPeakIndex = -1;
	vfoSweepPassComplete = false;
	memset(vfoSweepHold, 0x00, sizeof(vfoSweepHold));
	// Come back in automatic, and re-prime rather than carrying the last visit's scaling
	// into a band that may be nothing like it. Manual scaling is not persisted and has no
	// indicator on screen, so leaving it latched across visits would mean opening the
	// spectrum screen into a mode you cannot see and did not ask for this time.
	vfoSweepAutoScale = true;
	vfoSweepAutoPrimed = false;
#endif

	menuSystemPopAllAndDisplaySpecificRootMenu(UI_VFO_MODE, true);

	vfoSweepUpdateSamples(0, true, 0);
	headerRowIsDirty = true;

	// trxCheck*Squelch() won't be called while sweeping, blindly turn the
	// green and red LED off, to avoid being lit while scanning.
	LedWrite(LED_GREEN, 0);
	LedWrite(LED_RED, 0);
}


static void sweepScanStep(void)
{
#if defined(ENABLE_FAST_SCAN)
	if (vfoSweepListening)
	{
		// The pass that triggered this ended before the render below could run, so the
		// marker and its ">" listening prefix are still unpainted. Do it here.
		if (vfoSweepPassComplete)
		{
			vfoSweepPassComplete = false;
			vfoSweepDrawPeakMarker();
			displayRenderRows(1, ((8 + VFO_SWEEP_GRAPH_HEIGHT_Y) / 8) + 1);
		}

		// Hold for as long as the audio is actually open, so a transmission is not cut off
		// mid-sentence, then give up HANG ms after it closes. The MIN timer started at
		// entry covers the case where nothing ever opens -- a peak strong enough to stop
		// the sweep but not to break squelch, which is common on a noisy band edge.
		if (audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF)
		{
			ticksTimerStart(&vfoSweepListenTimer, VFO_SWEEP_LISTEN_HANG_MS);
		}

		// While surveying, a park is bounded regardless of the audio -- see
		// VFO_SWEEP_SCROLL_MAX_PARK_MS. The hold above renews itself for as long as the
		// squelch is open, so a carrier that just stays up stops the survey dead.
		if (vfoSweepAutoScroll && ticksTimerHasExpired(&vfoSweepParkLimitTimer))
		{
			vfoSweepStopListening();
			vfoSweepScrollPending = true;   // this window is done; move on
			vfoSweepDrawPeakMarker();
			displayRenderRows(1, ((8 + VFO_SWEEP_GRAPH_HEIGHT_Y) / 8) + 1);
			return;
		}

		if (ticksTimerHasExpired(&vfoSweepListenTimer))
		{
			vfoSweepStopListening();
			vfoSweepDrawPeakMarker();   // repaint without the ">"
			displayRenderRows(1, ((8 + VFO_SWEEP_GRAPH_HEIGHT_Y) / 8) + 1);
		}

		return;
	}
#endif

	if (uiDataGlobal.Scan.state != SCAN_STATE_SCANNING)
	{
		return;
	}

#if defined(ENABLE_FAST_SCAN)
	// A pass that ended by parking on a signal left its scroll owed. Pay it here, on the
	// first scanning tick after the listen ended, so the window moves on however the
	// listen finished -- timed out, squelch closed, or STAR-skipped by the operator.
	if (vfoSweepScrollPending)
	{
		vfoSweepAdvanceWindow();
	}
#endif

	if (ticksTimerHasExpired(&uiDataGlobal.Scan.timer))
	{
		ticksTimerStart(&uiDataGlobal.Scan.timer, VFO_SWEEP_STEP_TIME_ACTIVE);

		if (uiDataGlobal.Scan.sweepSampleIndex < VFO_SWEEP_NUM_SAMPLES)
		{
#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017) || defined(PLATFORM_MD9600)
			radioReadRSSIAndNoiseForBand(currentRadioDevice->trxCurrentBand[TRX_RX_FREQ_BAND]);
#else
			radioReadRSSIAndNoise();
#endif

#if defined(ENABLE_FAST_SCAN)
			// One column may be several measurements (see VFO_SWEEP_WIDE_SUBSAMPLES). Keep
			// the loudest and only commit the column once the last of them is in: peak
			// detect, so a narrow signal anywhere inside the column still shows at full
			// height instead of being averaged into the floor.
			//
			// Done as an early return rather than an inner loop on purpose. Each
			// measurement needs its own retune and settle, and the sweep is cooperative --
			// one measurement per main-loop tick. Looping here would block the loop for
			// N x the settle time and stall the UI, the squelch and the USB.
			if (radioDevices[RADIO_DEVICE_PRIMARY].trxRxSignal > vfoSweepColumnPeak)
			{
				vfoSweepColumnPeak = radioDevices[RADIO_DEVICE_PRIMARY].trxRxSignal;
			}

			vfoSweepSubIndex++;

			if (vfoSweepSubIndex < vfoSweepSubSamples())
			{
				// Not done with this column: retune to the next measurement and come back.
				uiDataGlobal.Scan.scanSweepCurrentFreq = vfoSweepFreqForOrdinal(
						((int32_t)uiDataGlobal.Scan.sweepSampleIndex * vfoSweepSubSamples())
								+ vfoSweepSubIndex);

				trxSetFrequency(uiDataGlobal.Scan.scanSweepCurrentFreq, currentChannelData->txFreq,
						(((currentChannelData->chMode == RADIO_MODE_DIGITAL) &&
								codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO))
										? DMR_MODE_DMO : DMR_MODE_AUTO));
				return;
			}

			vfoSweepSubIndex = 0;
			vfoSweepSamples[uiDataGlobal.Scan.sweepSampleIndex] = vfoSweepColumnPeak;
			vfoSweepColumnPeak = 0;
#else
			vfoSweepSamples[uiDataGlobal.Scan.sweepSampleIndex] = radioDevices[RADIO_DEVICE_PRIMARY].trxRxSignal;// Need to save the samples so for when the freq is changed and we need to scroll the display
#endif

#if defined(ENABLE_FAST_SCAN)
			// Raise the hold BEFORE drawing: vfoSweepDrawSample() paints the level and the
			// hold line together from these two arrays, so updating after it would draw
			// this bin's hold one pass stale.
			if (vfoSweepSamples[uiDataGlobal.Scan.sweepSampleIndex] > vfoSweepHold[uiDataGlobal.Scan.sweepSampleIndex])
			{
				vfoSweepHold[uiDataGlobal.Scan.sweepSampleIndex] = vfoSweepSamples[uiDataGlobal.Scan.sweepSampleIndex];
			}
#endif

			vfoSweepDrawSample(uiDataGlobal.Scan.sweepSampleIndex);

#if defined(ENABLE_FAST_SCAN)
			if (vfoSweepSamples[uiDataGlobal.Scan.sweepSampleIndex] > vfoSweepPeakLevel)
			{
				vfoSweepPeakLevel = vfoSweepSamples[uiDataGlobal.Scan.sweepSampleIndex];
				vfoSweepPeakIndex = uiDataGlobal.Scan.sweepSampleIndex;
			}

			// The three columns this sample touches: the one just drawn and the two-wide
			// cursor ahead of it. Only these change, so only these need blitting.
			int16_t colLo = uiDataGlobal.Scan.sweepSampleIndex;
			int16_t colHi = colLo;
#endif

			uiDataGlobal.Scan.sweepSampleIndex += uiDataGlobal.Scan.sweepSampleIndexIncrement;

			displayThemeApply(THEME_ITEM_FG_RX_FREQ, THEME_ITEM_BG);
			displayDrawFastVLine((uiDataGlobal.Scan.sweepSampleIndex) % VFO_SWEEP_NUM_SAMPLES, VFO_SWEEP_TRACE_START_Y, VFO_SWEEP_TRACE_HEIGHT_Y, true);// draw solid line in the next location
			displayDrawFastVLine((uiDataGlobal.Scan.sweepSampleIndex + uiDataGlobal.Scan.sweepSampleIndexIncrement) % VFO_SWEEP_NUM_SAMPLES, VFO_SWEEP_TRACE_START_Y, VFO_SWEEP_TRACE_HEIGHT_Y, true);// draw solid line in the next location
			displayThemeResetToDefault();

			if (uiNotificationIsVisible())
			{
				displayRender();
			}
			else
			{
#if defined(ENABLE_FAST_SCAN)
				// MEASURED: the full-width blit below moves 25600 bytes and costs ~7.8 ms,
				// which is most of the ~10.75 ms/sample floor the sweep saturates at --
				// the sweep is display-bound, not RF-bound. A strip covering just the
				// changed columns is ~50x less data. displayRenderColumns() falls back to
				// the full-width blit itself when the span is too wide or wraps round the
				// right edge, so no case is left unpainted.
				if (vfoSweepPassComplete)
				{
					// A pass just ended: the marker and its label moved, and they are not
					// in the strip. Repaint them and blit the lot -- once per pass.
					vfoSweepPassComplete = false;
					vfoSweepDrawPeakMarker();
					displayRenderRows(1, ((8 + VFO_SWEEP_GRAPH_HEIGHT_Y) / 8) + 1);
				}
				else
				{
					int16_t c1 = (uiDataGlobal.Scan.sweepSampleIndex % VFO_SWEEP_NUM_SAMPLES);
					int16_t c2 = ((uiDataGlobal.Scan.sweepSampleIndex + uiDataGlobal.Scan.sweepSampleIndexIncrement) % VFO_SWEEP_NUM_SAMPLES);

					colLo = SAFE_MIN(colLo, SAFE_MIN(c1, c2));
					colHi = SAFE_MAX(colHi, SAFE_MAX(c1, c2));

					displayRenderColumns(colLo, (colHi + 1), 1, ((8 + VFO_SWEEP_GRAPH_HEIGHT_Y) / 8) + 1);
				}
#else
				displayRenderRows(1, ((8 + VFO_SWEEP_GRAPH_HEIGHT_Y) / 8) + 1);
#endif
			}
		}
		else
		{
#if defined(ENABLE_FAST_SCAN)
			// End of a pass: latch the peak found during it and repaint the marker. Doing
			// it here rather than per sample means the marker holds still while the trace
			// redraws under it, and the one full-width blit it costs lands once per pass
			// (12.5 ms against a 1-4 s sweep) instead of once per sample.
			vfoSweepShownPeakLevel = vfoSweepPeakLevel;
			vfoSweepShownPeakIndex = vfoSweepPeakIndex;
			vfoSweepPeakLevel = 0;
			vfoSweepPeakIndex = -1;
			vfoSweepPassComplete = true;

			if (vfoSweepAutoScale)
			{
				vfoSweepUpdateAutoScale();
			}

#if (VFO_SWEEP_HOLD_DECAY > 0)
			// Fade the hold one step per pass. Bins below their live level are raised
			// straight back by the max on the next pass, so this only ever eats away at
			// peaks nothing is sustaining.
			//
			// ★ Not while dwelling. Fading is right for a live display, where a stale peak
			// should eventually go; it is wrong for a survey, where a burst three passes
			// ago is precisely what is being looked for. The hold is cleared on every
			// advance anyway, so within a dwelling window it is a true max and cannot
			// accumulate junk indefinitely.
			if (vfoSweepDwellPasses <= 1)
			{
				for (int i = 0; i < VFO_SWEEP_NUM_SAMPLES; i++)
				{
					vfoSweepHold[i] = ((vfoSweepHold[i] > VFO_SWEEP_HOLD_DECAY)
							? (vfoSweepHold[i] - VFO_SWEEP_HOLD_DECAY) : 0);
				}
			}
#endif

			// Strong enough to be worth interrupting the sweep for? Park on it.
			if ((vfoSweepShownPeakIndex >= 0) &&
					(vfoSweepShownPeakLevel > (VFO_SWEEP_FLOOR_ACTIVE + VFO_SWEEP_LISTEN_MARGIN)))
			{
				vfoSweepListenFreq = vfoSweepFreqForSample(vfoSweepShownPeakIndex);
				vfoSweepListening = true;
				ticksTimerStart(&vfoSweepListenTimer, VFO_SWEEP_LISTEN_MIN_MS);
				ticksTimerStart(&vfoSweepParkLimitTimer, VFO_SWEEP_SCROLL_MAX_PARK_MS);

				trxSetFrequency(vfoSweepListenFreq, currentChannelData->txFreq,
						(((currentChannelData->chMode == RADIO_MODE_DIGITAL) &&
								codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO))
										? DMR_MODE_DMO : DMR_MODE_AUTO));

				// Last, and only after the retune: this is what re-enables the squelch and
				// audio path, and it must not do so while still tuned to the old sample.
				uiDataGlobal.Scan.state = SCAN_STATE_PAUSED;
			}
#endif
#if defined(ENABLE_FAST_SCAN)
			// Auto scroll: step the centre on by exactly one span so consecutive passes
			// tile the band edge to edge -- no overlap, no omission.
			//
			// Not while parked on a signal: moving the centre out from under a listen
			// would retune away mid-transmission and leave the ">" label pointing at a
			// frequency the receiver is no longer on.
			// ★ Deferred when the pass ended by parking on a signal, NOT skipped.
			//
			// The listen-on-peak block immediately above has already set SCAN_STATE_PAUSED
			// by the time this runs, and it fires on the loudest sample of the pass being
			// VFO_SWEEP_LISTEN_MARGIN above the floor -- which plain noise clears most
			// passes. MEASURED on the bench: the sweep parked at the end of EVERY pass, so
			// a "only scroll while still scanning" test never once let it advance and the
			// mode looked completely dead.
			//
			// Advancing anyway would be worse: it would wipe the trace and the ">" label
			// naming what is being listened to, at exactly the moment the operator is
			// listening to it. So remember, and advance on the first tick after the listen
			// ends -- see the top of this function.
			if (vfoSweepAutoScroll)
			{
				vfoSweepPassesDone++;

				// Dwell: only move on once this window has had its share of passes. The
				// hold has been accumulating across them undecayed, so what moves on is a
				// window that was listened to N times, not once.
				if (vfoSweepPassesDone >= vfoSweepDwellPasses)
				{
					if (uiDataGlobal.Scan.state == SCAN_STATE_SCANNING)
					{
						vfoSweepAdvanceWindow();
					}
					else
					{
						vfoSweepScrollPending = true;
					}
				}
			}
#endif
			uiDataGlobal.Scan.sweepSampleIndex = 0;
			uiDataGlobal.Scan.sweepSampleIndexIncrement = 1;// go back to normal increment at the end of the special sweep step used just after the graph is zoomed in
#if defined(ENABLE_FAST_SCAN)
			vfoSweepSubIndex = 0;
			vfoSweepColumnPeak = 0;
#endif
		}

#if defined(ENABLE_FAST_SCAN)
		// Same arithmetic as the sub-sample retune above, via the one helper, so the
		// receiver and the peak label can never disagree about where a column is.
		uiDataGlobal.Scan.scanSweepCurrentFreq = vfoSweepFreqForOrdinal(
				((int32_t)uiDataGlobal.Scan.sweepSampleIndex * vfoSweepSubSamples())
						+ vfoSweepSubIndex);
#else
		uiDataGlobal.Scan.scanSweepCurrentFreq = currentChannelData->rxFreq +
				(VFO_SWEEP_SCAN_RANGE_SAMPLE_STEP_TABLE[uiDataGlobal.Scan.sweepStepSizeIndex] *
						(uiDataGlobal.Scan.sweepSampleIndex -
#if defined(PLATFORM_MD380) || defined(PLATFORM_MDUV380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
								(VFO_SWEEP_NUM_SAMPLES / 2)
#else
								64
#endif
						)) / VFO_SWEEP_PIXELS_PER_STEP;
#endif

		trxSetFrequency(uiDataGlobal.Scan.scanSweepCurrentFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
	}
}

static void scanInit(void)
{
	if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_DUAL_SCAN)
	{
		return;
	}

	uiDataGlobal.Scan.stepTimeMilliseconds = settingsGetScanStepTimeMilliseconds();

	// In DIGITAL mode, we need at least 120ms to see the HR-C6000 to start the TS ISR.
	if (trxGetMode() == RADIO_MODE_DIGITAL)
	{
		int dwellTime;
		if(uiDataGlobal.Scan.stepTimeMilliseconds > 150)				// if >150ms use DMR Slow mode
		{
			dwellTime = ((currentRadioDevice->trxDMRModeRx == DMR_MODE_DMO) ? SCAN_DMR_SIMPLEX_SLOW_MIN_DWELL_TIME : SCAN_DMR_DUPLEX_SLOW_MIN_DWELL_TIME);
		}
		else
		{
			dwellTime = ((currentRadioDevice->trxDMRModeRx == DMR_MODE_DMO) ? SCAN_DMR_SIMPLEX_FAST_MIN_DWELL_TIME : SCAN_DMR_DUPLEX_FAST_MIN_DWELL_TIME);
		}

		uiDataGlobal.Scan.dwellTime = ((uiDataGlobal.Scan.stepTimeMilliseconds < dwellTime) ? dwellTime : uiDataGlobal.Scan.stepTimeMilliseconds);
	}
	else
	{
		uiDataGlobal.Scan.dwellTime = uiDataGlobal.Scan.stepTimeMilliseconds;

#if defined(ENABLE_FAST_SCAN) || defined(ENABLE_SPECTRUM)
		/* Sharpen the RSSI filter for the fast reject's early sample -- see
		 * SCAN_REJECT_RSSI_COUNT. Restored by uiVFOModeStopScanning().
		 *
		 * Conditions match the reject's own exactly, so the chip is only ever left in a
		 * non-stock state when the thing that needs it is actually running: analog only
		 * (this branch), and not when the reject is switched off. Dual Watch is already
		 * excluded -- scanInit() returns before this for DUAL_SCAN. */
		if (scanRejectTicks != 0)
		{
			radioSetRssiCount(SCAN_REJECT_RSSI_COUNT);
		}
#endif
	}
	uiDataGlobal.Scan.scanType = SCAN_TYPE_NORMAL_STEP;

	screenOperationMode[nonVolatileSettings.currentVFONumber] = VFO_SCREEN_OPERATION_SCAN;
	uiDataGlobal.Scan.direction = 1;

	// If scan limits have not been defined. Set them to the current Rx freq .. +1MHz
	if ((nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber] == 0) || (nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber] == 0))
	{
		int limitDown = currentChannelData->rxFreq;
		int limitUp = currentChannelData->rxFreq + 100000;

		// If the limitUp in not valid, set it to the next band's minFreq
		if (trxGetBandFromFrequency(limitUp) == FREQUENCY_OUT_OF_BAND)
		{
			int band = trxGetNextOrPrevBandFromFrequency(limitUp, true);

			if (band != -1)
			{
				limitUp = RADIO_HARDWARE_FREQUENCY_BANDS[band].minFreq;
			}
		}

		settingsSet(nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber], SAFE_MIN(limitUp, limitDown));
		settingsSet(nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber], SAFE_MAX(limitUp, limitDown));
	}

	// Refresh on every step if scan boundaries is equal to one frequency step.
	uiDataGlobal.Scan.refreshOnEveryStep = ((nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber] - nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber]) <= VFO_FREQ_STEP_TABLE[(currentChannelData->VFOflag5 >> 4)]);

	clearNuisance();

	selectedFreq = VFO_SELECTED_FREQUENCY_INPUT_RX;

	uiDataGlobal.Scan.timer.timeout = 500;
	uiDataGlobal.Scan.state = SCAN_STATE_SCANNING;

	nextKeyBeepMelody = (int16_t *)MELODY_ACK_BEEP;// Indicate via beep that something different had happened

	menuSystemPopAllAndDisplaySpecificRootMenu(UI_VFO_MODE, true);
}

static void scanning(void)
{
	static bool scanPaused = false;
	static bool voicePromptsAnnounced = true;
#if defined(ENABLE_FAST_SCAN) || defined(ENABLE_SPECTRUM)
	static bool rejectDone = false;
#endif

	if (!rxPowerSavingIsRxOn())
	{
		uiDataGlobal.Scan.dwellTime = 10000;
		uiDataGlobal.Scan.timer.timeout = 0;
		return;
	}

#if defined(ENABLE_FAST_SCAN) || defined(ENABLE_SPECTRUM)
	/* The fast reject (see scanreject.h). RSSI is readable ~2.5 ms after a retune while
	 * `trxRxNoise < squelch` needs ~10-15 ms, and the expensive thing a scanner does is
	 * prove a step EMPTY. So spend one 140 us RSSI read early: if the level has not
	 * lifted at all, end the step now; otherwise let the dwell run and leave the decision
	 * to the stock rule, untouched.
	 *
	 * Analog only -- a digital step has to sit through a DMR timeslot to catch a burst,
	 * so there is nothing to save there and the RSSI of a TDMA carrier in the wrong slot
	 * would reject a channel that is genuinely busy.
	 *
	 * Not in Dual Watch either. That mode alternates between two VFOs, so consecutive
	 * steps are two different frequencies with two different noise floors, and a single
	 * running estimate interleaved across both is an average of neither.
	 *
	 * Once per step: rejectDone is cleared at the step boundary below. Testing on `<=`
	 * rather than `==` so a skipped main-loop tick cannot make the test miss its slot and
	 * silently disable the feature for that step. */
	if ((scanRejectTicks != 0) && (rejectDone == false) &&
			(uiDataGlobal.Scan.state == SCAN_STATE_SCANNING) &&
			(trxGetMode() == RADIO_MODE_ANALOG) &&
			(screenOperationMode[nonVolatileSettings.currentVFONumber] !=
					VFO_SCREEN_OPERATION_DUAL_SCAN) &&
			(uiDataGlobal.Scan.timer.timeout <=
					(uiDataGlobal.Scan.dwellTime - (int)scanRejectTicks)))
	{
		rejectDone = true;
		trxReadRSSIAndNoise(true);

		if (scanRejectStep(currentRadioDevice->trxRxSignal))
		{
			/* Fall straight through to the step boundary below, this same tick. */
			uiDataGlobal.Scan.timer.timeout = 0;
		}
	}
#endif

	//After initial settling time
	if((uiDataGlobal.Scan.state == SCAN_STATE_SCANNING) && (uiDataGlobal.Scan.timer.timeout > SCAN_SKIP_CHANNEL_INTERVAL) && (uiDataGlobal.Scan.timer.timeout < (uiDataGlobal.Scan.dwellTime - SCAN_SETTLING_INTERVAL_ACTIVE)))
	{
		// Test for presence of RF Carrier.

		if (trxGetMode() == RADIO_MODE_DIGITAL)
		{
			if(uiDataGlobal.Scan.stepTimeMilliseconds > 150)				// if >150ms use DMR Slow mode
			{
				//DMR Slow MOde
				if (((nonVolatileSettings.dmrCcTsFilter & DMR_TS_FILTER_PATTERN)
						&&
						((slotState != DMR_STATE_IDLE) && ((dmrMonitorCapturedTS != -1) &&
								(((currentRadioDevice->trxDMRModeRx == DMR_MODE_DMO) && (dmrMonitorCapturedTS == trxGetDMRTimeSlot())) ||
										(currentRadioDevice->trxDMRModeRx == DMR_MODE_RMO)))))
						||
						// As soon as the HRC6000 get sync, timeCode != -1 or TS ISR is running
						HRC6000HasGotSync())
				{
					announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ,
							((nonVolatileSettings.scanModePause == SCAN_MODE_STOP) ? AUDIO_PROMPT_MODE_VOICE_LEVEL_3 : PROMPT_THRESHOLD_NEVER_PLAY_IMMEDIATELY));


#if defined(PLATFORM_MD9600)
					uiDataGlobal.Scan.clickDiscriminator = CLICK_DISCRIMINATOR;
#endif
					uiDataGlobal.Scan.state = SCAN_STATE_SHORT_PAUSED;
					uiDataGlobal.Scan.timer.timeout = ((TIMESLOT_DURATION * 12) + TIMESLOT_DURATION) * 4; // (1 superframe + 1 TS) * 4 = TS Sync + incoming audio
					scanPaused = true;
					voicePromptsAnnounced = false;
				}
			}
			else
			{
				//DMR Fast Mode
				if(trxCarrierDetected(RADIO_DEVICE_PRIMARY))
				{
					announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ,
							((nonVolatileSettings.scanModePause == SCAN_MODE_STOP) ? AUDIO_PROMPT_MODE_VOICE_LEVEL_3 : PROMPT_THRESHOLD_NEVER_PLAY_IMMEDIATELY));

					if ((nonVolatileSettings.dmrCcTsFilter & DMR_TS_FILTER_PATTERN) == 0)
					{
						uiDataGlobal.Scan.timer.timeout = SCAN_SHORT_PAUSE_TIME * 2;	//needs longer delay if in DMR mode and TS Filter is off to allow full detection of signal
					}
					else
					{
						uiDataGlobal.Scan.timer.timeout = SCAN_SHORT_PAUSE_TIME;	//start short delay to allow full detection of signal
					}

					uiDataGlobal.Scan.state = SCAN_STATE_SHORT_PAUSED; //state 1 = pause and test for valid signal that produces audio
					scanPaused = true;
					voicePromptsAnnounced = false;
#if defined(PLATFORM_MD9600)
					uiDataGlobal.Scan.clickDiscriminator = CLICK_DISCRIMINATOR;
#endif

					// Force screen redraw in Analog mode, Dual Watch scanning
					if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_DUAL_SCAN)
					{
						uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
					}
				}
			}
		}
		else
		{
			if(trxCarrierDetected(RADIO_DEVICE_PRIMARY))
			{
				//FM  Mode
				announceItem(PROMPT_SEQUENCE_CHANNEL_NAME_OR_VFO_FREQ,
						((nonVolatileSettings.scanModePause == SCAN_MODE_STOP) ? AUDIO_PROMPT_MODE_VOICE_LEVEL_3 : PROMPT_THRESHOLD_NEVER_PLAY_IMMEDIATELY));

				if (nonVolatileSettings.scanModePause == SCAN_MODE_STOP)
				{
					uiVFOModeStopScanning();
					// Just update the header (to prevent hidden mode)
					displayClearRows(0, 2, false);
					uiUtilityRenderHeader(false, false, false);
					displayRenderRows(0, 2);
					return;
				}
				else
				{
					uiDataGlobal.Scan.timer.timeout = SCAN_SHORT_PAUSE_TIME; //start short delay to allow full detection of signal
					uiDataGlobal.Scan.state = SCAN_STATE_SHORT_PAUSED; //state 1 = pause and test for valid signal that produces audio
					scanPaused = true;
					voicePromptsAnnounced = false;
#if defined(PLATFORM_MD9600)
					uiDataGlobal.Scan.clickDiscriminator = CLICK_DISCRIMINATOR;
#endif

					// Force screen redraw in Analog mode, Dual Watch scanning
					if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_DUAL_SCAN)
					{
						uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
					}
				}
			}
		}
	}

	// Only do this once if scan mode is PAUSE do it every time if scan mode is HOLD
	if(((uiDataGlobal.Scan.state == SCAN_STATE_PAUSED) && (nonVolatileSettings.scanModePause == SCAN_MODE_HOLD)) || (uiDataGlobal.Scan.state == SCAN_STATE_SHORT_PAUSED))
	{
#if defined(PLATFORM_MD9600)
		if (uiDataGlobal.Scan.clickDiscriminator > 0)
		{
			uiDataGlobal.Scan.clickDiscriminator--;
		}
		else
		{
#endif
			if (audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF)
			{
				if (nonVolatileSettings.scanModePause == SCAN_MODE_STOP)
				{
					uiVFOModeStopScanning();
					// Just update the header (to prevent hidden mode)
					displayClearRows(0, 2, false);
					uiUtilityRenderHeader(false, false, false);
					displayRenderRows(0, 2);
					return;
				}
				else
				{
					uiDataGlobal.Scan.timer.timeout = nonVolatileSettings.scanDelay * 1000;
					uiDataGlobal.Scan.state = SCAN_STATE_PAUSED;
				}

				if (voicePromptsAnnounced == false)
				{
					if (nonVolatileSettings.audioPromptMode > AUDIO_PROMPT_MODE_VOICE_LEVEL_2)
					{
						voicePromptsPlay();
					}
					voicePromptsAnnounced = true;
				}
			}
#if defined(PLATFORM_MD9600)
		}
#endif
	}

	if(uiDataGlobal.Scan.timer.timeout > 0)
	{
		uiDataGlobal.Scan.timer.timeout--;
	}
	else
	{
		/* Step boundary. STEP_PERIOD is the interval between successive boundaries --
		 * i.e. the thing that measures dwell + 18 ms -- and STEP_TOTAL is the work done
		 * inside the boundary itself. Whatever STEP_PERIOD has that STEP_TOTAL does not
		 * is being spent in the dwell loop, not here. */
		SCANPROF_PERIOD(SCANPROF_STEP_PERIOD);
		SCANPROF_START(tStepTotal);

#if defined(ENABLE_FAST_SCAN) || defined(ENABLE_SPECTRUM)
		/* A new step begins here, so the fast reject gets one test again. Counted here
		 * rather than at the reject point, so the total includes the steps that were
		 * never eligible -- otherwise the reject fraction is measured against its own
		 * denominator and always looks better than it is. */
		rejectDone = false;
		scanRejectCountStep();
#endif

		// We are in Dual Watch scanning mode
		if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_DUAL_SCAN)
		{
			// Select and set the next VFO
			//
			// Note: nonVolatileSettings.currentVFONumber is not changed using settingsSet(), to prevent crazy EEPROM
			//       writes. uiVFOModeStopScanning() is doing this, when the scanning process ends (for any reason).
			nonVolatileSettings.currentVFONumber = (1 - nonVolatileSettings.currentVFONumber);
			currentChannelData = &settingsVFOChannel[nonVolatileSettings.currentVFONumber];

			currentChannelData->libreDMR_Power = 0x00;// Force channel to the Master power

			trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));

			//Need to load the Rx group if specified even if TG is currently overridden as we may need it later when the left or right button is pressed
			if (currentChannelData->rxGroupList != 0)
			{
				if (currentChannelData->rxGroupList != lastLoadedRxGroup)
				{
					if (codeplugRxGroupGetDataForIndex(currentChannelData->rxGroupList, &currentRxGroupData))
					{
						lastLoadedRxGroup = currentChannelData->rxGroupList;
					}
					else
					{
						lastLoadedRxGroup = -1;
					}
				}
			}
			else
			{
				memset(&currentRxGroupData, 0xFF, sizeof(CodeplugRxGroup_t));// If the VFO doesnt have an Rx Group ( TG List) the global var needs to be cleared, otherwise it contains the data from the previous screen e.g. Channel screen
				lastLoadedRxGroup = -1;
			}

			uiVFOModeLoadChannelData(false);

			// In DIGITAL mode, we need at least 120ms to see the HR-C6000 to start the TS ISR.
			if (trxGetMode() == RADIO_MODE_DIGITAL)
			{
				int dwellTime;
				if(uiDataGlobal.Scan.stepTimeMilliseconds > 150)				// if >150ms use DMR Slow mode
				{
					dwellTime = ((currentRadioDevice->trxDMRModeRx == DMR_MODE_DMO) ? SCAN_DMR_SIMPLEX_SLOW_MIN_DWELL_TIME : SCAN_DMR_DUPLEX_SLOW_MIN_DWELL_TIME);
				}
				else
				{
					dwellTime = ((currentRadioDevice->trxDMRModeRx == DMR_MODE_DMO) ? SCAN_DMR_SIMPLEX_FAST_MIN_DWELL_TIME : SCAN_DMR_DUPLEX_FAST_MIN_DWELL_TIME);
				}
				uiDataGlobal.Scan.dwellTime = ((uiDataGlobal.Scan.stepTimeMilliseconds < dwellTime) ? dwellTime : uiDataGlobal.Scan.stepTimeMilliseconds);
			}
			else
			{
				uiDataGlobal.Scan.dwellTime = uiDataGlobal.Scan.stepTimeMilliseconds;
			}

			if (scanPaused)
			{
				uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN; // Force screen redraw on scan resume
				scanPaused = false;
			}
		}
		else // Frequency scanning mode
		{
			uiEvent_t tmpEvent = { .buttons = 0, .keys = NO_KEYCODE, .rotary = 0, .function = 0, .events = NO_EVENT, .hasEvent = 0, .time = 0 };
			int fStep = VFO_FREQ_STEP_TABLE[(currentChannelData->VFOflag5 >> 4)];

			if (uiDataGlobal.Scan.direction == 1)
			{
				if(currentChannelData->rxFreq + fStep <= nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber])
				{
					SCANPROF_START(tUp);
					handleUpKey(&tmpEvent);
					SCANPROF_END(SCANPROF_HANDLEUP, tUp);
				}
				else
				{
					int offset = currentChannelData->txFreq - currentChannelData->rxFreq;

					currentChannelData->rxFreq = nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber];
					currentChannelData->txFreq = currentChannelData->rxFreq + offset;
					trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
					HRC6000ClearColorCodeSynchronisation();
				}
			}
			else
			{
				if(currentChannelData->rxFreq + fStep >= nonVolatileSettings.vfoScanLow[nonVolatileSettings.currentVFONumber])
				{
					SCANPROF_START(tUp);
					handleUpKey(&tmpEvent);
					SCANPROF_END(SCANPROF_HANDLEUP, tUp);
				}
				else
				{
					int offset = currentChannelData->txFreq - currentChannelData->rxFreq;
					currentChannelData->rxFreq = nonVolatileSettings.vfoScanHigh[nonVolatileSettings.currentVFONumber];
					currentChannelData->txFreq = currentChannelData->rxFreq+offset;
					trxSetFrequency(currentChannelData->rxFreq, currentChannelData->txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
					HRC6000ClearColorCodeSynchronisation();
				}
			}

			if (uiDataGlobal.Scan.refreshOnEveryStep)
			{
				uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			}
		}

		uiDataGlobal.Scan.timer.timeout = uiDataGlobal.Scan.dwellTime;
		uiDataGlobal.Scan.state = SCAN_STATE_SCANNING; // Settling and test for carrier presence.

		if (screenOperationMode[nonVolatileSettings.currentVFONumber] == VFO_SCREEN_OPERATION_SCAN)
		{
			//check all nuisance delete entries and skip channel if there is a match
			for(int i = 0; i < MAX_ZONE_SCAN_NUISANCE_CHANNELS; i++)
			{
				if (uiDataGlobal.Scan.nuisanceDelete[i] == -1)
				{
					break;
				}
				else
				{
					if(uiDataGlobal.Scan.nuisanceDelete[i] == currentChannelData->rxFreq)
					{
						uiDataGlobal.Scan.timer.timeout = SCAN_SKIP_CHANNEL_INTERVAL;
						break;
					}
				}
			}
		}

		SCANPROF_END(SCANPROF_STEP_TOTAL, tStepTotal);
	}
}

static void clearNuisance(void)
{
	//clear all nuisance delete channels at start of scanning
	for(int i = 0; i < MAX_ZONE_SCAN_NUISANCE_CHANNELS; i++)
	{
		uiDataGlobal.Scan.nuisanceDelete[i] = -1;
	}
	uiDataGlobal.Scan.nuisanceDeleteIndex = 0;
}

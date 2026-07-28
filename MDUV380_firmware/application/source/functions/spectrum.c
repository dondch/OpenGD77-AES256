/*
 * spectrum.c — dev-only swept-RSSI receiver. See spectrum.h.
 *
 * Compiles to nothing unless built -DENABLE_SPECTRUM (stock stays byte-identical).
 *
 * Both entry points run from cpsHandleCommand(), i.e. already inside the CPS
 * taskENTER_CRITICAL() section. That is deliberate: a swept measurement needs
 * deterministic timing, and nothing here blocks (the AT1846S I2C helpers pass
 * HAL_MAX_DELAY, which makes the HAL skip its HAL_GetTick() timeout checks, so
 * they do not depend on the FreeRTOS tick advancing). Nothing here may call
 * osDelay(); in particular the AT1846S register tables reached via
 * trxSetModeAndBandwidth() contain no AT_DELAY entries (only the boot-time
 * AT1846InitSettings table does).
 *
 * Because interrupts stay masked for the duration, every call is bounded by
 * SPECTRUM_MAX_BUSY_US so USB and the HR-C6000 are never stalled for long. The
 * host splits a wide sweep into several calls.
 */
#include "functions/spectrum.h"

#if defined(ENABLE_SPECTRUM)

#include <string.h>
#include "functions/trx.h"
#include "functions/ticks.h"
#include "hardware/AT1846S.h"
#include "hardware/radioHardwareInterface.h"
#include "functions/rxPowerSaving.h"
#include "main.h"

/* AT1846S register map bits we touch directly. */
#define AT1846S_REG_RSSI        0x1B   /* [15:8] = RSSI, [7:0] = noise level */
#define AT1846S_REG_FREQ_HI     0x29   /* channel word bits 31:16 */
#define AT1846S_REG_FREQ_LO     0x2A   /* channel word bits 15:0  */

/* Longest we are willing to hold the CPS critical section in one call. */
#define SPECTRUM_MAX_BUSY_US    250000U

/* Saved radio state, so a measurement leaves the radio exactly as it found it. */
typedef struct
{
	uint32_t rxFreq;
	uint32_t txFreq;
	uint32_t mode;
	uint32_t dmrModeRx;
	bool     wide;
	bool     modeChanged;
} spectrumSavedState_t;

static uint32_t s_cyclesPerUs = 72U;   /* refreshed at the start of every measurement */

/* Sweep session (see spectrum.h). While one is open the receiver is left running on
 * the anchor frequency in the requested mode, and sweeps only move the PLL. */
static bool s_sessionActive = false;
static spectrumSavedState_t s_sessionSaved;
static uint32_t s_sessionLastUseMs = 0;

/* How long to let the receiver settle after anything that restarts it. MEASURED on the
 * bench: RSSI is back within a couple of counts of final by ~15 ms and the noise reading
 * stops drifting by ~30 ms, so 30 ms it is. */
#define SPECTRUM_RX_RESTART_SETTLE_US  30000U

/* ------------------------------------------------------------------ timing */

static void spectrumTimerInit(void)
{
	uint32_t hclk = HAL_RCC_GetHCLKFreq();

	s_cyclesPerUs = (hclk >= 1000000U) ? (hclk / 1000000U) : 1U;

	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t spectrumCycles(void)
{
	return DWT->CYCCNT;
}

static inline uint32_t spectrumCyclesToUs(uint32_t cycles)
{
	return cycles / s_cyclesPerUs;
}

/* ------------------------------------------------------- scan-step profiler */
#if defined(ENABLE_SCAN_PROFILER)

/* See the slot list in spectrum.h. Deliberately allocation-free and division-free in
 * the hot path: call sites only ever read CYCCNT and do one subtract/compare/add, so
 * instrumenting a region costs well under a microsecond and cannot itself account for
 * the overhead being measured. Cycles are converted to microseconds only when the host
 * reads the table (CPS 0xA9). */
scanProfSlot_t scanProfSlots[SCANPROF_SLOTS];

static bool s_profTimerReady = false;

uint32_t scanProfNow(void)
{
	/* Self-initialising: the profiler runs from the UI task, which may well sample a
	 * scan step before any sweep or probe has ever configured the DWT. */
	if (!s_profTimerReady)
	{
		spectrumTimerInit();
		s_profTimerReady = true;
	}

	return DWT->CYCCNT;
}

static void scanProfRecord(uint8_t slot, uint32_t cycles)
{
	scanProfSlot_t *s = &scanProfSlots[slot];

	s->lastCycles = cycles;
	s->sumCycles += cycles;

	if ((s->count == 0) || (cycles < s->minCycles))
	{
		s->minCycles = cycles;
	}

	if (cycles > s->maxCycles)
	{
		s->maxCycles = cycles;
	}

	s->count++;
}

void scanProfAdd(uint8_t slot, uint32_t startCycles)
{
	if (slot < SCANPROF_SLOTS)
	{
		scanProfRecord(slot, (scanProfNow() - startCycles));
	}
}

void scanProfMarkPeriod(uint8_t slot)
{
	if (slot < SCANPROF_SLOTS)
	{
		uint32_t now = scanProfNow();
		scanProfSlot_t *s = &scanProfSlots[slot];

		/* The first mark only establishes the origin -- there is no previous edge to
		 * measure against, and counting it would poison min with a garbage value. */
		if (s->marked)
		{
			scanProfRecord(slot, (now - s->markCycles));
		}

		s->markCycles = now;
		s->marked = true;
	}
}

void scanProfReset(void)
{
	memset(scanProfSlots, 0, sizeof(scanProfSlots));
}

uint32_t scanProfCyclesPerUs(void)
{
	(void)scanProfNow();   // make sure s_cyclesPerUs has been computed
	return s_cyclesPerUs;
}
#endif /* ENABLE_SCAN_PROFILER */

/* Busy-wait until `cycles` have elapsed since `since`. Unsigned arithmetic makes
 * the 32-bit CYCCNT wrap (every ~60 s at 72 MHz) harmless. */
static inline void spectrumWaitCycles(uint32_t since, uint32_t cycles)
{
	while ((spectrumCycles() - since) < cycles)
	{
		// busy wait: this is the whole point -- no scheduler, no jitter
	}
}

/* --------------------------------------------------------------- retuning */

/* Bare PLL retune: just the two channel-word registers, RX left running.
 *
 * Exact integer maths on purpose. radioSetFrequency() computes the channel word
 * as `f_in * 0.16f`, and a single-precision float only carries 24 mantissa bits,
 * so above ~16.7M (i.e. any frequency above 167.77 MHz in OpenGD77's 10 Hz
 * units) the conversion loses the low bits and the bins of a fine-stepped sweep
 * would not be evenly spaced. The AT1846S channel word LSB is 62.5 Hz, so with
 * the frequency in 10 Hz units the exact conversion is f * 4 / 25. */
static void spectrumFastRetune(uint32_t freq)
{
	uint32_t f = (freq * 4U) / 25U;

	radioWriteReg2byte(AT1846S_REG_FREQ_HI, (f >> 24) & 0xFF, (f >> 16) & 0xFF);
	radioWriteReg2byte(AT1846S_REG_FREQ_LO, (f >> 8) & 0xFF, f & 0xFF);
}

/* Raw register write, straight onto the bus.
 *
 * radioWriteReg2byte() keeps a value cache and silently drops a write whose value has
 * not changed. That is right for normal use and wrong here twice over: the POKE
 * experiments exist precisely to rewrite a register with its current value, and the
 * override table has to survive radioSetFrequency() rewriting the same registers. */
#define AT1846S_I2C_ADDR_7BIT  (0x5CU)

static bool spectrumRawWrite(uint8_t reg, uint8_t hi, uint8_t lo)
{
	uint8_t data[3] = { reg, hi, lo };

	return (HAL_I2C_Master_Transmit(&hi2c3, AT1846S_I2C_ADDR_7BIT, data, 3,
			HAL_MAX_DELAY) == HAL_OK);
}

bool spectrumReadReg(uint8_t reg, uint8_t *hi, uint8_t *lo)
{
	return radioReadReg2byte(reg, hi, lo);
}

bool spectrumWriteRegRaw(uint8_t reg, uint8_t hi, uint8_t lo)
{
	return spectrumRawWrite(reg, hi, lo);
}

/* Split-dwell scan experiment. See spectrum.h. 0 = stock behaviour. */
uint16_t spectrumScanAnalogDwellMs = 0;

/* Built-in VFO sweep step time override. See spectrum.h. 0 = stock 25 ms. */
uint16_t spectrumSweepStepTimeMs = 0;

/* Scan settling interval override. See spectrum.h. 0 = the stock 1 tick. */
uint16_t spectrumScanSettleTicks = 0;

/* What the scanner decides on. See spectrum.h. 0 = stock. */
uint8_t  spectrumScanDetectMode = SPECTRUM_DETECT_STOCK;
uint8_t  spectrumScanRssiThreshold = 0;
uint8_t  spectrumScanSqReg = 0;
uint16_t spectrumScanSqMask = 0;
bool     spectrumScanSqInvert = false;

bool spectrumScanCarrierDetected(uint8_t rssi, uint8_t noise, uint8_t squelch)
{
	switch (spectrumScanDetectMode)
	{
		case SPECTRUM_DETECT_RSSI:
			return (rssi >= spectrumScanRssiThreshold);

		case SPECTRUM_DETECT_CHIPSQ:
		{
			uint8_t hi = 0, lo = 0;
			bool open;

			/* An unset register would test 0 against 0 and answer "detected" for ever,
			 * which reads as a spectacularly sensitive scanner rather than as a
			 * misconfiguration. Fall back to the stock rule instead. */
			if ((spectrumScanSqMask == 0) || (radioReadReg2byte(spectrumScanSqReg, &hi, &lo) == false))
			{
				return (noise < squelch);
			}

			open = ((((uint16_t)hi << 8) | lo) & spectrumScanSqMask) != 0;
			return (spectrumScanSqInvert ? (open == false) : open);
		}

		default:
			return (noise < squelch);
	}
}

/* Register overrides, re-applied after every retune. See spectrum.h. */
static uint8_t s_overrides[SPECTRUM_MAX_OVERRIDES][3];
static int s_overrideCount = 0;

void spectrumSetOverrides(int count, const uint8_t *triplets)
{
	if (count < 0)
	{
		count = 0;
	}
	if (count > SPECTRUM_MAX_OVERRIDES)
	{
		count = SPECTRUM_MAX_OVERRIDES;
	}

	for (int i = 0; i < count; i++)
	{
		s_overrides[i][0] = triplets[(i * 3) + 0];
		s_overrides[i][1] = triplets[(i * 3) + 1];
		s_overrides[i][2] = triplets[(i * 3) + 2];
	}
	s_overrideCount = count;
}

int spectrumGetOverrideCount(void)
{
	return s_overrideCount;
}

static void spectrumApplyOverrides(void)
{
	for (int i = 0; i < s_overrideCount; i++)
	{
		spectrumRawWrite(s_overrides[i][0], s_overrides[i][1], s_overrides[i][2]);
	}
}

/* Register 0x30 carries the RX enable (bit 5 of the low byte) alongside the bandwidth
 * and power bits, so the bracket has to preserve everything else. Read it once per
 * measurement rather than per point -- it is a 140 us bus transaction. */
static uint8_t s_reg30Hi = 0x60, s_reg30Lo = 0x26;

/* Same reasoning for the registers the 2026-07-28 candidate triggers poke: each is
 * rewritten with the value it already holds, so it has to be read once first. Reading
 * them per point would cost 140 us each and swamp the very saving being looked for. */
static uint8_t s_reg0FHi = 0, s_reg0FLo = 0;   /* band select   */
static uint8_t s_reg2BHi = 0, s_reg2BLo = 0;   /* xtal_freq     */
static uint8_t s_reg2CHi = 0, s_reg2CLo = 0;   /* adclk_freq    */

static void spectrumCacheRegs(void)
{
	uint8_t hi, lo;

	if (radioReadReg2byte(0x30, &hi, &lo))
	{
		s_reg30Hi = hi;
		s_reg30Lo = lo | 0x20U;    /* the cached copy is the RX-ON form */
	}

	if (radioReadReg2byte(0x0F, &hi, &lo))
	{
		s_reg0FHi = hi;
		s_reg0FLo = lo;
	}

	if (radioReadReg2byte(0x2B, &hi, &lo))
	{
		s_reg2BHi = hi;
		s_reg2BLo = lo;
	}

	if (radioReadReg2byte(0x2C, &hi, &lo))
	{
		s_reg2CHi = hi;
		s_reg2CLo = lo;
	}
}

/* PLL registers, low word first. The one thing FAST cannot tell us: FAST writes 0x29
 * then 0x2A and does not retune, but if the chip commits the pair on a write to the
 * HIGH word -- which is how a lot of split channel-word synthesisers behave -- then FAST
 * has been writing the high half of the new frequency against the low half of the old
 * one and discarding it, and the correct order retunes for free. */
static void spectrumFastRetuneHiLast(uint32_t freq)
{
	uint32_t f = (freq * 4U) / 25U;

	radioWriteReg2byte(AT1846S_REG_FREQ_LO, (f >> 8) & 0xFF, f & 0xFF);
	radioWriteReg2byte(AT1846S_REG_FREQ_HI, (f >> 24) & 0xFF, (f >> 16) & 0xFF);
}

/* Retune index: 4 bits, split (see spectrum.h). */
static inline uint8_t spectrumRetuneKind(uint8_t mode)
{
	return (uint8_t)((mode & SPECTRUM_MODE_RETUNE_MASK) |
			((mode & SPECTRUM_MODE_RETUNE_HI) ? 0x08U : 0U));
}

static void spectrumRetune(uint32_t freq, uint8_t mode)
{
	switch (spectrumRetuneKind(mode))
	{
		case SPECTRUM_RETUNE_RADIO:
			radioSetFrequency(freq, false);
			break;

		case SPECTRUM_RETUNE_TRX:
			/* trxSetFrequency() no-ops when the frequency is unchanged, so make
			 * sure it always does the real work. */
			currentRadioDevice->currentRxFrequency = FREQUENCY_UNSET;
			currentRadioDevice->currentTxFrequency = FREQUENCY_UNSET;
			trxSetFrequency(freq, freq, DMR_MODE_DMO);
			break;

		case SPECTRUM_RETUNE_LATCH:
			/* What radioSetFrequency() does, minus everything that is not the
			 * frequency: no LNA re-select, no squelch thresholds, no DAC. */
			spectrumFastRetune(freq);
			spectrumRawWrite(0x30, s_reg30Hi, (uint8_t)(s_reg30Lo & ~0x20U));
			spectrumRawWrite(0x30, s_reg30Hi, s_reg30Lo);
			break;

		case SPECTRUM_RETUNE_LATE30:
			/* Mute for as short a time as possible: RX off, frequency, RX on. If the
			 * settle is dominated by how long the receiver was off rather than by the
			 * restart itself, this beats LATCH. */
			spectrumRawWrite(0x30, s_reg30Hi, (uint8_t)(s_reg30Lo & ~0x20U));
			spectrumFastRetune(freq);
			spectrumRawWrite(0x30, s_reg30Hi, s_reg30Lo);
			break;

		case SPECTRUM_RETUNE_POKE30:
			/* The interesting one: does rewriting 0x30 with the SAME value latch the
			 * PLL without ever muting the receiver? If so the restart cost disappears. */
			spectrumFastRetune(freq);
			spectrumRawWrite(0x30, s_reg30Hi, s_reg30Lo);
			break;

		case SPECTRUM_RETUNE_POKE05:
			/* radioSetFrequency() also rewrites 0x05 ("select normal frequency mode")
			 * on every retune. Maybe that, not 0x30, is what latches. */
			spectrumFastRetune(freq);
			spectrumRawWrite(0x05, 0x87, 0x63);
			break;

		case SPECTRUM_RETUNE_BAND0F:
			/* A band write plausibly reloads the synthesiser without stopping RX. Raw,
			 * so the driver's value cache cannot swallow a write of the same value --
			 * swallowing it is what would make this silently equal FAST. */
			spectrumFastRetune(freq);
			spectrumRawWrite(0x0F, s_reg0FHi, s_reg0FLo);
			break;

		case SPECTRUM_RETUNE_SQTOGGLE:
			/* An edge on 0x30, but on a bit that does not gate the receiver. If this
			 * latches, the ~4.4 ms is the RX restart and not the latch, and the whole
			 * cost goes away. */
			spectrumFastRetune(freq);
			spectrumRawWrite(0x30, s_reg30Hi, (uint8_t)(s_reg30Lo ^ 0x08U));
			spectrumRawWrite(0x30, s_reg30Hi, s_reg30Lo);
			break;

		case SPECTRUM_RETUNE_XTAL:
			spectrumFastRetune(freq);
			spectrumRawWrite(0x2B, s_reg2BHi, s_reg2BLo);
			spectrumRawWrite(0x2C, s_reg2CHi, s_reg2CLo);
			break;

		case SPECTRUM_RETUNE_HILAST:
			spectrumFastRetuneHiLast(freq);
			break;

		case SPECTRUM_RETUNE_FAST:
		default:
			spectrumFastRetune(freq);
			break;
	}

	spectrumApplyOverrides();
}

static inline void spectrumSampleReg(uint8_t reg, uint8_t *rssi, uint8_t *noise)
{
	uint8_t v1 = 0, v2 = 0;

	if (radioReadReg2byte(reg, &v1, &v2))
	{
		*rssi = v1;
		*noise = v2;
	}
	else
	{
		*rssi = 0;
		*noise = 0;
	}
}

/* ------------------------------------------------------- enter / leave ---- */

static void spectrumEnter(spectrumSavedState_t *st, uint8_t mode)
{
	st->rxFreq = currentRadioDevice->currentRxFrequency;
	st->txFreq = currentRadioDevice->currentTxFrequency;
	st->mode = currentRadioDevice->currentMode;
	st->dmrModeRx = currentRadioDevice->trxDMRModeRx;
	st->wide = currentRadioDevice->currentBandWidthIs25kHz;
	st->modeChanged = false;

	/* RSSI reads return nothing while the receiver is powered down. */
	if (rxPowerSavingIsRxOn() == false)
	{
		rxPowerSavingSetState(ECOPHASE_POWERSAVE_INACTIVE);
	}

	if (mode & SPECTRUM_MODE_FORCE_FM)
	{
		bool wide = ((mode & SPECTRUM_MODE_WIDE) != 0);

		if ((st->mode != RADIO_MODE_ANALOG) || (st->wide != wide))
		{
			trxSetModeAndBandwidth(RADIO_MODE_ANALOG, wide);
			st->modeChanged = true;
		}
	}

	spectrumCacheRegs();
	spectrumTimerInit();
}

static void spectrumLeave(spectrumSavedState_t *st)
{
	if (st->modeChanged)
	{
		trxSetModeAndBandwidth(st->mode, st->wide);
	}

	/* Force a genuine retune back: we moved the PLL behind trx.c's back, so its
	 * cached "current frequency" no longer matches the hardware. */
	currentRadioDevice->currentRxFrequency = FREQUENCY_UNSET;
	currentRadioDevice->currentTxFrequency = FREQUENCY_UNSET;
	trxSetFrequency(st->rxFreq, st->txFreq, st->dmrModeRx);
}

/* ------------------------------------------------------ Stage 0: settle --- */

int spectrumSettleProbe(uint32_t fA, uint32_t fB, uint8_t mode, uint16_t intervalUs,
		uint8_t nSamples, uint8_t reg, spectrumSample_t *out, spectrumProbeInfo_t *info)
{
	spectrumSavedState_t saved;
	uint32_t t0, t1, tRead;
	uint32_t intervalCycles;
	uint8_t rssi, noise;
	int i;

	if (nSamples > SPECTRUM_PROBE_MAX_SAMPLES)
	{
		nSamples = SPECTRUM_PROBE_MAX_SAMPLES;
	}

	if (reg == 0)
	{
		reg = AT1846S_REG_RSSI;
	}

	/* The probe drives the radio itself, so it must not run on top of a session. */
	spectrumSessionEnd();

	spectrumEnter(&saved, mode);

	/* Park on fA and let it settle properly, so the step we then measure is a
	 * real retune and not the tail of the previous one. */
	spectrumRetune(fA, mode);
	t0 = spectrumCycles();
	spectrumWaitCycles(t0, 30000U * s_cyclesPerUs);   /* 30 ms */
	spectrumSampleReg(reg, &rssi, &noise);

	/* Cost of one register read, measured on its own. */
	t0 = spectrumCycles();
	spectrumSampleReg(reg, &rssi, &noise);
	tRead = spectrumCycles() - t0;

	/* The step under test. */
	t0 = spectrumCycles();
	spectrumRetune(fB, mode);
	t1 = spectrumCycles();

	intervalCycles = (uint32_t)intervalUs * s_cyclesPerUs;

	for (i = 0; i < (int)nSamples; i++)
	{
		uint32_t ts;

		if (intervalCycles != 0U)
		{
			spectrumWaitCycles(t1, intervalCycles * (uint32_t)(i + 1));
		}

		ts = spectrumCycles();
		spectrumSampleReg(reg, &rssi, &noise);

		{
			uint32_t us = spectrumCyclesToUs(ts - t0);

			out[i].tUs = (us > 65535U) ? 65535U : (uint16_t)us;
			out[i].rssi = rssi;
			out[i].noise = noise;
		}

		/* Never overrun the busy-time budget, however the host asked. */
		if (spectrumCyclesToUs(spectrumCycles() - t0) > SPECTRUM_MAX_BUSY_US)
		{
			i++;
			break;
		}
	}

	info->retuneUs = (uint16_t)spectrumCyclesToUs(t1 - t0);
	info->readUs = (uint16_t)spectrumCyclesToUs(tRead);
	info->count = (uint8_t)i;

	spectrumLeave(&saved);

	return i;
}

/* -------------------------------------------------------- Stage 1: sweep -- */

int spectrumSweep(uint32_t fStart, uint32_t stepHz, uint16_t nPoints, uint16_t dwellUs,
		uint8_t mode, uint8_t *out, uint16_t *elapsedMs)
{
	spectrumSavedState_t saved;
	uint32_t dwellCycles;
	uint32_t tSweep;
	uint16_t i;
	bool ownSession = (s_sessionActive == false);

	if (nPoints > SPECTRUM_SWEEP_MAX_POINTS)
	{
		nPoints = SPECTRUM_SWEEP_MAX_POINTS;
	}

	if (ownSession)
	{
		/* Self-contained call: set the radio up, and pay the receiver-restart settle
		 * so the first points are not just a settling curve. A host that wants a fast
		 * repeat rate opens a session instead and skips all of this. */
		spectrumEnter(&saved, mode);

		/* One full retune to fStart: that is what sets the band, the RX LNA and the
		 * IF path. The per-point retunes then only move the PLL, so a sweep must stay
		 * inside one band -- the caller checks that. */
		radioSetFrequency(fStart, false);
		tSweep = spectrumCycles();
		spectrumWaitCycles(tSweep, SPECTRUM_RX_RESTART_SETTLE_US * s_cyclesPerUs);
	}
	else
	{
		spectrumTimerInit();
		s_sessionLastUseMs = ticksGetMillis();
	}

	dwellCycles = (uint32_t)dwellUs * s_cyclesPerUs;
	tSweep = spectrumCycles();

	for (i = 0; i < nPoints; i++)
	{
		uint32_t tPoint;
		uint8_t rssi, noise;

		spectrumRetune(fStart + ((uint32_t)i * stepHz), mode);

		tPoint = spectrumCycles();
		if (dwellCycles != 0U)
		{
			spectrumWaitCycles(tPoint, dwellCycles);
		}

		spectrumSampleReg(AT1846S_REG_RSSI, &rssi, &noise);
		out[(i * 2) + 0] = rssi;
		out[(i * 2) + 1] = noise;

		if (spectrumCyclesToUs(spectrumCycles() - tSweep) > SPECTRUM_MAX_BUSY_US)
		{
			i++;
			break;
		}
	}

	*elapsedMs = (uint16_t)(spectrumCyclesToUs(spectrumCycles() - tSweep) / 1000U);

	if (ownSession)
	{
		spectrumLeave(&saved);
	}

	return (int)i;
}

/* ------------------------------------------------------------- sessions -- */

void spectrumSessionBegin(uint32_t anchorFreq, uint8_t mode)
{
	if (s_sessionActive)
	{
		spectrumSessionEnd();
	}

	spectrumEnter(&s_sessionSaved, mode);

	radioSetFrequency(anchorFreq, false);
	spectrumWaitCycles(spectrumCycles(), SPECTRUM_RX_RESTART_SETTLE_US * s_cyclesPerUs);

	s_sessionLastUseMs = ticksGetMillis();
	s_sessionActive = true;
}

void spectrumSessionEnd(void)
{
	if (s_sessionActive == false)
	{
		return;
	}

	s_sessionActive = false;
	spectrumLeave(&s_sessionSaved);
}

bool spectrumSessionIsActive(void)
{
	return s_sessionActive;
}

/* Called from the main loop: a session parks the radio off its channel, so never let a
 * host that stopped talking (crashed, unplugged) leave it there. */
void spectrumTick(void)
{
	if (s_sessionActive &&
			((ticksGetMillis() - s_sessionLastUseMs) > SPECTRUM_SESSION_TIMEOUT_MS))
	{
		taskENTER_CRITICAL();
		spectrumSessionEnd();
		taskEXIT_CRITICAL();
	}
}

#endif /* ENABLE_SPECTRUM */

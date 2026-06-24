/*
 * dmr_data.c — DMR data-path harness (Phase 2). See dmr_data.h.
 * Behind -DENABLE_DMR_DATA; empty translation unit in stock builds.
 */
#include "functions/dmr_data.h"

#if defined(ENABLE_DMR_DATA)

#include "main.h"                       // HAL + CMSIS device (SysTick, __set_MSP, SYSCFG)
#include "functions/ticks.h"           // addTimerCallback
#include "functions/trx.h"             // trxEnableTransmission, trxTransmissionEnabled
#include "hardware/HR-C6000.h"         // HRC6000ClearIsWakingState
#include "user_interface/menuSystem.h" // MENU_ANY
#include <string.h>

#define SYS_BOOTLOADER_ADDR  0x1FFF0000U  // STM32F405 system memory (ROM DFU bootloader)

/*
 * One-way jump into the STM32 ROM system bootloader. Runs from the main-task
 * timer-callback context (handleTimerCallbacks), NOT an ISR. De-init is done with
 * interrupts still enabled (HAL_*_DeInit use HAL_GetTick timeouts), then interrupts
 * are disabled, SysTick stopped, system memory mapped to 0, MSP reloaded, and we
 * branch to the bootloader's reset vector. The radio re-enumerates as USB DFU.
 */
static void dmrDataRebootToBootloader(void)
{
	HAL_RCC_DeInit();
	HAL_DeInit();

	__disable_irq();
	SysTick->CTRL = 0U;
	SysTick->LOAD = 0U;
	SysTick->VAL  = 0U;

	__HAL_RCC_SYSCFG_CLK_ENABLE();
	__HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();

	__set_MSP(*(volatile uint32_t *)SYS_BOOTLOADER_ADDR);
	((void (*)(void))(*(volatile uint32_t *)(SYS_BOOTLOADER_ADDR + 4U)))();

	while (1) { } // unreachable
}

void dmrDataTriggerReboot(void)
{
	// Defer ~500 ms so the USB reply to the triggering CPS command is sent first.
	addTimerCallback(dmrDataRebootToBootloader, 500, MENU_ANY, false);
}

/* ---- host-parameterised DMR data-frame TX (bring-up harness) ---------------- */
/* Plain .bss (zero-initialised at boot, so s_dataTxActive can't be a stray 1 that
 * would corrupt a normal voice TX). The resulting ambebuffer shift is harmless —
 * the AMBE codec addresses its buffers via the pointer passed in r1 (proven on-air). */
static uint8_t          s_bursts[DMR_DATA_MAX_BURSTS][1 + DMR_DATA_BURST_LEN];
static uint8_t          s_burstCount;
static uint8_t          s_burstIndex;
static volatile uint8_t s_dataTxActive;
static uint16_t         s_txFinishPolls;

#define DMR_DATA_TX_FINISH_POLL_MS   25
#define DMR_DATA_TX_FINISH_TIMEOUT   120  // * poll period -> ~3 s safety cap

/*
 * Un-key the data call cleanly, mirroring a PTT release. The command path keys via
 * trxEnableTransmission() but, unlike the voice path, has no PTT-release handler to call
 * trxDisableTransmission() -- so without this the PA is left keyed (red LED stuck on) and
 * the half-keyed state hangs the radio after a few triggers. Runs in the main-loop timer
 * context, re-arming itself until the queue has drained (s_dataTxActive) AND the HR-C6000
 * TX-END sequence has returned to RX (trxIsTransmitting), bounded by a timeout so a TX that
 * never started (slot stayed busy) still cleans up instead of latching s_dataTxActive.
 */
static void dmrDataTxFinishPoll(void)
{
	if ((s_dataTxActive || trxIsTransmitting) && (++s_txFinishPolls < DMR_DATA_TX_FINISH_TIMEOUT))
	{
		addTimerCallback(dmrDataTxFinishPoll, DMR_DATA_TX_FINISH_POLL_MS, MENU_ANY, false);
		return;
	}
	dmrDataTxEnd();           // ensure the queue/flags are cleared (idempotent)
	trxDisableTransmission(); // LED_RED off + trxActivateRx() -> back to RX, like PTT release
}

static void dmrDataKeyTx(void)
{
	s_burstIndex = 0;
	s_dataTxActive = 1;
	s_txFinishPolls = 0;
	// hrc6000Tick() starts the DMR TX only when isWaking == NONE AND slotState == IDLE.
	// Clearing isWaking alone isn't enough: on a channel with any stray/recent RX the
	// timeslot ISR keeps slotState in an RX state until END_TICK_TIMEOUT of silence, so
	// trxSetTX() keys the PA (red LED) but the TX state machine never transitions -> a
	// stuck carrier with no bursts (and on a busy slot, no RF at all). Force the slot to
	// IDLE (also clears isWaking) so the next hrc6000Tick() keys deterministically.
	HRC6000ForceDMRIdleForTx();
	trxEnableTransmission(); // LEDs + trxSetTX() (sets trxTransmissionEnabled); state machine -> TX_2 sends our bursts
	// Poll for completion and un-key (the data call has no PTT release to do it).
	addTimerCallback(dmrDataTxFinishPoll, DMR_DATA_TX_FINISH_POLL_MS, MENU_ANY, false);
}

void dmrDataTxLoad(const uint8_t *bursts, uint8_t count)
{
	if (s_dataTxActive)
	{
		return; // a data call is already keyed; ignore overlapping triggers
	}
	if (count > DMR_DATA_MAX_BURSTS)
	{
		count = DMR_DATA_MAX_BURSTS;
	}
	memcpy(s_bursts, bursts, (size_t)count * (1 + DMR_DATA_BURST_LEN));
	s_burstCount = count;
	s_burstIndex = 0;
	// Defer keying out of the CPS critical section into the main-loop callback context.
	addTimerCallback(dmrDataKeyTx, 100, MENU_ANY, false);
}

int dmrDataTxActive(void)
{
	return s_dataTxActive;
}

int dmrDataTxNextBurst(uint8_t *dataTypeOut, uint8_t *payload12Out)
{
	if (!s_dataTxActive || (s_burstIndex >= s_burstCount))
	{
		return 0;
	}
	uint8_t *b = s_bursts[s_burstIndex++];
	*dataTypeOut = b[0];
	memcpy(payload12Out, b + 1, DMR_DATA_BURST_LEN);
	return 1;
}

void dmrDataTxEnd(void)
{
	s_dataTxActive = 0;
	s_burstIndex = 0;
	s_burstCount = 0;
	trxTransmissionEnabled = false; // so hrc.transmissionEnabled clears -> state machine goes to TX_END
}

#endif // ENABLE_DMR_DATA

/*
 * Copyright (C) 2019      Kai Ludwig, DG4KLU
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
#include <stdarg.h>
#include "functions/hotspot.h"
#include "functions/settings.h"
#include "user_interface/uiUtilities.h"
#include "user_interface/menuSystem.h"
#include "usb/usb_com.h"
#ifdef ENABLE_AES
#include "crypto/dmr_aes.h"
#endif
#include "functions/ticks.h"
#include "interfaces/wdog.h"
#include "hardware/HR-C6000.h"
#include "functions/sound.h"
#include "hardware/SPI_Flash.h"
#include "io/display.h"   /* displayReadReg (HX8353E self-test, CPS 0x8A) */
#include "user_interface/uiLocalisation.h"
#include "functions/rxPowerSaving.h"
#include "main.h"
#include <interfaces/clockManager.h>
#include "interfaces/settingsStorage.h"
#if defined(ENABLE_KEY_INJECTION)
#include "usb_device.h"   /* MX_USB_DEVICE_DeInit() for the DFU jump */
#endif
#if defined(ENABLE_SPECTRUM)
#include "functions/spectrum.h"   /* swept-RSSI receiver (CPS 0xA0 / 0xA1) */
#endif
#include "interfaces/gps.h"

//#define DEBUG_POSITION 1


#define GITVERSIONREV GITVERSION

enum CPS_ACCESS_AREA
{
	CPS_ACCESS_FLASH = 1,
	CPS_ACCESS_EEPROM = 2,
	CPS_ACCESS_MCU_ROM = 5,
	CPS_ACCESS_DISPLAY_BUFFER = 6,
	CPS_ACCESS_WAV_BUFFER = 7,
	CPS_COMPRESS_AND_ACCESS_AMBE_BUFFER = 8,
	CPS_ACCESS_RADIO_INFO = 9,
#if ! defined(CPU_MK22FN512VLL12)
	CPS_ACCESS_FLASH_SECURITY_REGISTERS = 10,
#endif
};


#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
#define TASK_LOCK_WRITE()	  do { } while(0)
#define TASK_UNLOCK_WRITE()	  do { } while(0)
#elif defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
#define TASK_LOCK_WRITE()	  do { taskENTER_CRITICAL(); } while(0)
#define TASK_UNLOCK_WRITE()	  do { taskEXIT_CRITICAL(); } while(0)
#else
#error configure this platform about tasks locking
#endif


#include "crypto/dmr_aes_hook.h"
#include "functions/dmr_data.h"
#include "functions/dmr_sms.h"
static void handleCPSRequest(void);

volatile int com_request = 0;
volatile uint8_t com_requestbuffer[COM_REQUESTBUFFER_SIZE];
volatile uint8_t usbComSendBuf[COM_BUFFER_SIZE];

static int sector = -1;
volatile int comRecvMMDVMIndexIn = 0;
volatile int comRecvMMDVMIndexOut = 0;
volatile int comRecvMMDVMFrameCount = 0;
static bool flashingDMRIDs = false;
static bool channelsRewritten = false;
static bool luczRewritten = false;
#if defined(HAS_GPS)
static gpsMode_t previousGPSState = GPS_NOT_DETECTED;
#endif

bool isCompressingAMBE = false;

volatile static bool hasToReply = false;
volatile static uint32_t replyLength = 0;

volatile bool usbIsResetting = false;

#if defined(ENABLE_KEY_INJECTION)
/* ---- DEV: USB remote keypad --------------------------------------------------
 * A small ring of keys pushed by CPS command 0x96 and replayed into the NORMAL UI
 * by usbKeyInjectTick() (called from the main loop). Each key is delivered as a
 * two-phase tap -- DOWN on one iteration, then UP (SHORTUP) on the next -- which is
 * what the menu handlers expect from a real key press. Push and pop both run on the
 * main task, so no locking is needed. Event bits mirror keyboard.h (kept as literals
 * so this stays include-independent): DOWN=0x01, UP=0x02, LONG=0x04. */
#define INJ_MOD_DOWN  0x01
#define INJ_MOD_UP    0x02
#define INJ_MOD_LONG  0x04
#define INJ_MOD_PRESS 0x08
#define INJ_RING     16
static volatile uint8_t s_injKey[INJ_RING];
static volatile uint8_t s_injFlags[INJ_RING];   /* bit0 = long press */
static volatile uint8_t s_injHead = 0, s_injTail = 0;   /* head = write, tail = read */
static volatile uint8_t s_injPhase = 0;         /* 0 = emit DOWN next, 1 = emit UP next */
static volatile uint32_t s_injPhaseTime = 0;    /* when the current phase was emitted */

/* How long to "hold" an injected long press between the DOWN and the LONG event.
 *
 * The event SEQUENCE alone is not enough -- the timing is load-bearing. A UI handler can
 * sit behind state that only clears while the key is held: uiVFOMode's sweep hotkey
 * (long-press #) is gated on FreqEnter.index == 0, and the DOWN|PRESS that starts every
 * press is also what menuGetKeypadKeyValue() turns into the first digit of a frequency
 * entry. On real hardware the ~300 ms pending-entry timeout fires during the hold and
 * clears it, so the LONG event lands with the gate open. Emitting DOWN and LONG on
 * consecutive main-loop iterations (microseconds apart) never let that timeout run, so
 * those handlers were unreachable from USB while apparently-identical ones worked.
 *
 * 600 ms comfortably exceeds that 300 ms timeout and is a realistic hold. */
#define INJ_LONG_HOLD_MS  600U

/* Pending injected FUNCTION event (see usb_com.h). 0 = none, which is safe because every
 * FUNC_* code is non-zero. 16 bits, not 8: the codes are QUICKKEY_MENUVALUE() encodings
 * that set bit 15 (e.g. FUNC_START_SCANNING = (QUICKKEY_MENU << 15) | 1). */
static volatile uint16_t s_injFunction = 0;

static void usbKeyInjectPush(uint8_t key, uint8_t flags)
{
	uint8_t next = (uint8_t)((s_injHead + 1) % INJ_RING);
	if (next == s_injTail) { return; }          /* ring full -> drop */
	s_injKey[s_injHead] = key;
	s_injFlags[s_injHead] = flags;
	s_injHead = next;
}

/* Reboot into DFU with no button combo.
 *
 * This radio does NOT flash via the ST ROM loader at 0x1FFF0000 -- it uses TYT's own
 * DFU bootloader at 0x08000000 (its DFU alt-settings read "@Internal Flash
 * /0x0800C000/..." and "@SPI Flash Memory /0x00000000/...", which the ROM loader has
 * no notion of). Jumping to 0x1FFF0000 is therefore the wrong target and just wedges
 * USB. Disassembly of that bootloader's boot decision (at 0x08004400) shows it stays
 * in DFU on exactly two conditions, with NO magic value, backup register or reset-flag
 * check anywhere:
 *   1. PTT + top button held  (GPIOE bits 10|11 read 0, active low), or
 *   2. the application's initial-SP word at 0x0800C000 fails
 *        (SP & 0x2FFE0000) == 0x20000000.
 * So the only software route is (2): make that word fail the test. Clearing bit 29
 * (0x2001FFFC -> 0x0001FFFC) is a 1->0 only change, which STM32 flash accepts as a
 * plain word program -- NO sector erase, so the rest of the vector table and the whole
 * app stay intact. NVIC_SystemReset() then performs a REAL reset, which resets the USB
 * peripheral and drops the D+ pull-up: exactly the clean disconnect/re-enumerate that a
 * software jump could never produce (that was why the earlier attempts enumerated as
 * "Device Descriptor Request Failed").
 *
 * The firmware loader rewrites 0x0800C000 when it flashes the app, so the word repairs
 * itself on the very next flash. NOTE: between the trigger and that flash the radio
 * boots ONLY to DFU -- which is the intent, and is always recoverable by flashing. */
#define INJ_APP_VECTOR_ADDR  0x0800C000U   /* app vector table (initial SP word) */
#define INJ_APP_SP_VALID_BIT 0x20000000U   /* clearing this fails the bootloader's test */
#define INJ_BOOTLOADER_ADDR  0x08000000U   /* TYT/AnyRoad DFU bootloader vector table */
/* A word in sector 11 (0x080E0000..0x080FFFFF), which is ERASED on this radio: the app
 * ends around 0x080C0000 and the loader never writes up here. Verified 0xFFFFFFFF by
 * reading it over USB (CPS 'R' area 5). Programming it is inert either way. */
#define INJ_FLASH_PROBE_ADDR 0x080FFFF0U
/* Mark the app invalid. Returns the HAL status; fills diag[0..2] with
 * HAL_FLASH_GetError(), FLASH->SR and the SP word read back after the write, so a
 * failure can be diagnosed from the host instead of guessed at. */
static uint8_t usbInjectInvalidateApp(uint32_t *diag)
{
	uint32_t sp = *(volatile uint32_t *)INJ_APP_VECTOR_ADDR;
	HAL_StatusTypeDef st = HAL_OK;

	if ((sp & INJ_APP_SP_VALID_BIT) != 0U)   /* only if still marked valid */
	{
		HAL_FLASH_Unlock();
		/* A stale error flag (WRPERR/PGSERR/...) left set by any earlier flash access
		 * makes HAL_FLASH_Program bail out immediately, which is the most likely reason
		 * a word program silently does nothing. Clear them first. */
		__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
				FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
		st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, INJ_APP_VECTOR_ADDR,
				(uint32_t)(sp & ~INJ_APP_SP_VALID_BIT));
		HAL_FLASH_Lock();
	}

	diag[0] = HAL_FLASH_GetError();
	diag[1] = FLASH->SR;
	diag[2] = *(volatile uint32_t *)INJ_APP_VECTOR_ADDR;
	return (uint8_t)st;
}

static void usbInjectRebootToBootloader(void)
{
	NVIC_SystemReset();

	while (1) { }   /* unreachable */
}

/* Probe whether this firmware can program internal flash AT ALL.
 *
 * Nothing in OpenGD77 ever writes internal flash (every persistent store lives in the
 * external SPI flash), so the internal-flash path above is entirely untested code and
 * its silent failure could be generic rather than specific to 0x0800C000. This writes
 * one word into a known-ERASED location and reports what the hardware said, so the two
 * cases can be told apart. Returns the HAL status; diag = { HAL error, FLASH->SR,
 * FLASH->CR, word read back }. The data cache is flushed before the read-back, because
 * DCEN is on and HAL_FLASH_Program does not flush it (only the erase path does). */
static uint8_t usbInjectFlashProbe(uint32_t addr, uint32_t value, uint32_t *diag)
{
	HAL_StatusTypeDef st;

	HAL_FLASH_Unlock();
	__HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
			FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
	st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, value);
	diag[0] = HAL_FLASH_GetError();
	diag[1] = FLASH->SR;
	diag[2] = FLASH->CR;
	HAL_FLASH_Lock();

	__HAL_FLASH_DATA_CACHE_DISABLE();
	__HAL_FLASH_DATA_CACHE_RESET();
	__HAL_FLASH_DATA_CACHE_ENABLE();
	diag[3] = *(volatile uint32_t *)addr;

	return (uint8_t)st;
}

/* Enter DFU by ERASING the app's first sector, then resetting.
 *
 * The bootloader stays in DFU when the word at 0x0800C000 fails
 * (SP & 0x2FFE0000) == 0x20000000. Clearing bit 29 of the live value (0x2001FFFC) is
 * the only 1->0 way to fail that test, and it does not work: this STM32F405 silently
 * refuses to re-program an already-programmed word -- HAL returns HAL_OK with no error
 * flag and the word is unchanged (measured; an ERASED word in the same firmware
 * programs perfectly). So the word has to be ERASED instead: 0xFFFFFFFF gives
 * 0x2FFE0000 != 0x20000000, which fails the test outright.
 *
 * Sector 3 (0x0800C000..0x0800FFFF) is the app's first 16 KB, i.e. its vector table.
 * That means:
 *   - this routine must run from RAM (.data is copied to RAM at startup and SRAM is
 *     executable), because the whole flash bank stalls for the duration of the erase;
 *   - interrupts must be off, since VTOR still points into the sector being erased;
 *   - it must touch nothing in flash -- no HAL, no library calls -- hence the raw
 *     register sequence below;
 *   - it must never return. It resets, and the bootloader then finds an invalid app.
 * The firmware loader erases and rewrites exactly this sector on every flash, so the
 * radio repairs itself on the next flash. It cannot be bricked: sectors 0-2 hold the
 * bootloader, are untouched, and always come up in DFU. */
__attribute__((section(".data.ramfunc"), noinline, used))
static void usbInjectEraseAppSector(void)
{
	__disable_irq();

	while ((FLASH->SR & FLASH_SR_BSY) != 0U) { }

	FLASH->KEYR = 0x45670123U;                  /* FLASH_KEY1 */
	FLASH->KEYR = 0xCDEF89ABU;                  /* FLASH_KEY2 */

	FLASH->SR = (FLASH_SR_EOP | FLASH_SR_SOP | FLASH_SR_WRPERR |
			FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR);

	FLASH->CR = FLASH_CR_SER | (3UL << FLASH_CR_SNB_Pos) | FLASH_PSIZE_WORD;
	FLASH->CR |= FLASH_CR_STRT;

	while ((FLASH->SR & FLASH_SR_BSY) != 0U) { }

	FLASH->CR &= ~FLASH_CR_SER;
	FLASH->CR |= FLASH_CR_LOCK;

	__DSB();
	SCB->AIRCR = (0x5FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
	__DSB();

	while (1) { }   /* the reset lands before this matters */
}

/* Enter DFU by JUMPING into TYT's bootloader with the button combo spoofed.
 *
 * The bootloader's boot decision (0x08004400) stays in DFU when GPIOE bits 10|11 both
 * read 0 -- PTT (PE11) and the top button, which shares PE10 with LCD_D7. A reset can
 * never help there, because a reset also returns the GPIO block to its input default;
 * a JUMP keeps whatever the app configured. Disassembly of the whole 48 KB bootloader
 * shows GPIOE is referenced in exactly two places -- the two ReadPin calls in the
 * decision and the LED WritePins in the DFU loop -- so it never re-initialises those
 * pins and cannot undo the spoof.
 *
 * So: drive PE10/PE11 low as outputs, put the clock tree back to its post-reset state
 * (HAL_RCC_DeInit -> HSI, which is what the bootloader's SystemInit at 0x08004A20
 * expects to find), then jump to the bootloader's reset vector. USB is soft-
 * disconnected first: the OTG_FS D+ pull-up IS internal to the STM32F405, so the host
 * sees a real disconnect and re-enumerates the DFU device.
 *
 * Unlike the SP-invalidation route this changes NOTHING persistent -- if it fails, a
 * power cycle boots the app as usual. */
static void usbInjectJumpToBootloader(void)
{
	uint32_t sp;
	uint32_t entry;
	uint32_t guard;

	/* Hard-reset the USB peripheral rather than MX_USB_DEVICE_DeInit(): a peripheral
	 * reset is what the bootloader expects to find, it drops the D+ pull-up (which IS
	 * internal on the STM32F405) so the host sees a real disconnect, and it avoids
	 * USBD_Stop()'s Error_Handler(), which is an infinite loop. Nothing else is
	 * reset -- in particular not the GPIO ports, because PWR_SW lives on one of them. */
	__HAL_RCC_USB_OTG_FS_FORCE_RESET();
	__HAL_RCC_USB_OTG_FS_RELEASE_RESET();
	__HAL_RCC_USB_OTG_FS_CLK_DISABLE();
	HAL_Delay(200);                    /* let the host see the disconnect */

	__disable_irq();
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;
	for (int i = 0; i < 8; i++)
	{
		NVIC->ICER[i] = 0xFFFFFFFFU;
		NVIC->ICPR[i] = 0xFFFFFFFFU;
	}

	/* Clock tree back to its post-reset state, by hand. HAL_RCC_DeInit() must NOT be
	 * used here: its waits are driven by HAL_GetTick(), which cannot advance with
	 * interrupts masked, and this project routes HAL_InitTick onto a TIM timebase --
	 * that is the most likely reason the first version of this routine hung. Every
	 * poll below is bounded, so a stuck flag costs a bad jump, never a lockup. */
	RCC->CIR = 0x00000000U;
	RCC->CR |= RCC_CR_HSION;
	guard = 1000000U;
	while (((RCC->CR & RCC_CR_HSIRDY) == 0U) && (guard-- != 0U)) { }
	RCC->CFGR = 0x00000000U;                       /* SYSCLK = HSI */
	guard = 1000000U;
	while (((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) && (guard-- != 0U)) { }
	RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_CSSON | RCC_CR_PLLON | RCC_CR_PLLI2SON);
	guard = 1000000U;
	while (((RCC->CR & RCC_CR_PLLRDY) != 0U) && (guard-- != 0U)) { }
	RCC->PLLCFGR = 0x24003010U;                    /* reset value */
	RCC->CR &= ~RCC_CR_HSEBYP;

	/* Spoof the combo LAST, so nothing above can undo it. Direct register writes: no
	 * HAL calls once the clocks have been torn down. PE11 = PTT, PE10 = the second
	 * combo line (shared with LCD_D7). Push-pull outputs driven low = "both held". */
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;
	GPIOE->BSRR = (1UL << (10 + 16)) | (1UL << (11 + 16));
	GPIOE->OTYPER &= ~((1UL << 10) | (1UL << 11));
	GPIOE->MODER = (GPIOE->MODER & ~((3UL << 20) | (3UL << 22))) | (1UL << 20) | (1UL << 22);

	SCB->VTOR = INJ_BOOTLOADER_ADDR;
	__DSB();
	__ISB();
	/* The bootloader runs from reset with interrupts enabled and will never issue its
	 * own cpsie, so PRIMASK has to be cleared here or its USB IRQ can never fire.
	 * Safe because every NVIC source was just disabled and un-pended. */
	__enable_irq();

	sp = *(volatile uint32_t *)INJ_BOOTLOADER_ADDR;
	entry = *(volatile uint32_t *)(INJ_BOOTLOADER_ADDR + 4U);

	/* Done in one asm block: FreeRTOS runs this on the process stack, so CONTROL has
	 * to select MSP before MSP is loaded, and no compiler-inserted stack access may
	 * happen in between. */
	__asm volatile (
			"msr control, %2\n"
			"isb\n"
			"msr msp, %0\n"
			"bx  %1\n"
			: : "r" (sp), "r" (entry), "r" (0U) : );

	while (1) { }   /* unreachable */
}

bool usbKeyInjectTick(uint8_t *outEvent, char *outKey)
{
	if (s_injTail == s_injHead) { return false; }   /* nothing queued */
	uint8_t key = s_injKey[s_injTail];
	bool isLong = ((s_injFlags[s_injTail] & 0x01) != 0);
	bool isHold = ((s_injFlags[s_injTail] & 0x02) != 0);
	*outKey = (char)key;

	/* Replay the SAME event sequence the real keypad driver produces (the state machine
	 * in keyboard.c), because the UI is written against it:
	 *
	 *   short press : DOWN|PRESS                  then UP
	 *   long press  : DOWN|PRESS  ->  LONG|DOWN   then LONG|UP
	 *
	 * The middle event is the one that matters and the one this used to get wrong. A
	 * long press is a SEQUENCE spread over time, not one richer event: the driver emits
	 * DOWN|PRESS as the key goes down (KEY_PRESS), and only later, when the long-press
	 * timer expires (KEY_WAITLONG -> KEY_REPEAT), emits LONG|DOWN -- which carries NO
	 * PRESS bit. Emitting DOWN|PRESS|LONG as a single event, as this did before, is a
	 * combination the hardware never generates: KEYCHECK_PRESS matched it first and
	 * consumed it, so KEYCHECK_LONGDOWN handlers were unreachable from USB on any key
	 * with more than one action (FRONT UP is next-channel AND start-scanning).
	 *
	 * Emitting the honest sequence makes both styles work with no special flags, and a
	 * long press correctly does NOT also fire the key's short-press action -- exactly as
	 * on the real keypad, where the release after a long press reports LONG|UP and so
	 * never satisfies KEYCHECK_SHORTUP. */
	switch (s_injPhase)
	{
		case 0:
			*outEvent = (uint8_t)(INJ_MOD_DOWN | INJ_MOD_PRESS);
			s_injPhase = (uint8_t)(isLong ? 1 : 2);
			s_injPhaseTime = ticksGetMillis();
			break;

		case 1:   /* long presses only: hold before the LONG event -- see INJ_LONG_HOLD_MS */
			if ((ticksGetMillis() - s_injPhaseTime) < INJ_LONG_HOLD_MS)
			{
				return false;   /* still "holding"; nothing to deliver this iteration */
			}
			*outEvent = (uint8_t)(INJ_MOD_LONG | INJ_MOD_DOWN);
			s_injPhase = 2;
			break;

		default:  /* release */
			s_injPhase = 0;
			s_injTail = (uint8_t)((s_injTail + 1) % INJ_RING);   /* key consumed */

			/* flags bit1 = HOLD: deliver the long press with no release event.
			 *
			 * Needed for "press and hold to enter a mode" handlers. uiVFOMode's sweep is
			 * the example: sweepScanInit() sets Scan.active, and handleEvent() then stops
			 * scanning on ANY key event whose key is not in a small exempt list -- which
			 * does not include the very key used to enter it. So the release tears the
			 * mode straight back down, one iteration after entering it. Holding leaves
			 * the mode up; the caller sends a separate key later to leave it. */
			if (isLong && isHold)
			{
				return false;
			}
			*outEvent = (uint8_t)(isLong ? (INJ_MOD_LONG | INJ_MOD_UP) : INJ_MOD_UP);
			break;
	}
	return true;
}

void usbFuncInjectPush(uint16_t function)
{
	s_injFunction = function;
}

bool usbFuncInjectTick(uint16_t *outFunction)
{
	if (s_injFunction == 0)
	{
		return false;
	}
	*outFunction = s_injFunction;
	s_injFunction = 0;
	return true;
}
#endif

/*
static void hexDump2(uint8_t *ptr, int len,char *msg)
{
	char buf[8];
    msg[0] = 0;
    for (int i = 0; i < len; i++)
    {
		sprintf(buf,"%02x",*ptr);
		strcat(msg,buf);
	    ptr++;
    }
}*/

static bool addressInSegment(uint32_t address, uint32_t length, uint32_t segmentStart, uint32_t segmentSize)
{
	return ((address >= segmentStart) && ((address + length) <= (segmentStart + segmentSize)));
}

void tick_com_request(void)
{
	switch (settingsUsbMode)
	{
		case USB_MODE_CPS:
			if (com_request == 1)
			{
				TASK_LOCK_WRITE();
				handleCPSRequest();
				com_request = 0;
				if (hasToReply)
				{
					CDC_Transmit_FS((uint8_t *) usbComSendBuf, replyLength);
					hasToReply = false;
					replyLength = 0;
				}
				TASK_UNLOCK_WRITE();
			}
			break;

		case USB_MODE_HOTSPOT:
			// That will happen once when MMDVMHost send the first frame
			// out of the Hotspot mode.
			if (com_request == 1)
			{
				com_request = 0;

				if ((nonVolatileSettings.hotspotType != HOTSPOT_TYPE_OFF) &&
						((comRecvMMDVMFrameCount >= 1) && (com_requestbuffer[1] == MMDVM_FRAME_START)) &&
						(uiDataGlobal.dmrDisabled == false)) // DMR (digital) is disabled.
				{

					if (menuSystemGetCurrentMenuNumber() != UI_HOTSPOT_MODE)
					{
						menuSystemPushNewMenu(UI_HOTSPOT_MODE);
					}
				}
			}
			break;
	}
}

static void cpsHandleReadCommand(void)
{
	uint32_t address = (com_requestbuffer[2] << 24) + (com_requestbuffer[3] << 16) + (com_requestbuffer[4] << 8) + (com_requestbuffer[5] << 0);
	uint32_t length = (com_requestbuffer[6] << 8) + (com_requestbuffer[7] << 0);
	bool result = false;

	if (length > (COM_REQUESTBUFFER_SIZE - 3))
	{
		length = (COM_REQUESTBUFFER_SIZE - 3);
	}

	switch(com_requestbuffer[1])
	{
		case CPS_ACCESS_FLASH:
		{
			// Calibration register, returns local copy
			if (addressInSegment(address, length, 0x10000, 0x200))
			{
				uint8_t *p = calibrationGetLocalDataPointer();
				memcpy((uint8_t *)&usbComSendBuf[3], (p + (address - 0x10000)), length);
				result = true;
			}
			else
			{
				TASK_UNLOCK_WRITE();
				result = SPI_Flash_read(address, (uint8_t *)&usbComSendBuf[3], length);
				uint32_t end = address + length - 1;
				const uint32_t VFOs_END = CODEPLUG_ADDR_VFO_A_CHANNEL + (sizeof(CodeplugChannel_t) * 2);

				// if CPS is writing the second part of the EEPROM (emulated in Flash) then the VFO's are being updated.
				if ((address <= CODEPLUG_ADDR_VFO_A_CHANNEL) && (end  >= VFOs_END))
				{
					uint32_t offset = CODEPLUG_ADDR_VFO_A_CHANNEL - address;

					CodeplugChannel_t * destPtr = (CodeplugChannel_t *)&usbComSendBuf[3 + offset];

					memcpy((uint8_t *)destPtr, (uint8_t *)&settingsVFOChannel[0], CODEPLUG_CHANNEL_DATA_STRUCT_SIZE);
					codeplugConvertChannelInternalToCodeplug(destPtr, destPtr);

					destPtr = (CodeplugChannel_t *)&usbComSendBuf[3 + offset + CODEPLUG_CHANNEL_DATA_STRUCT_SIZE];

					memcpy((uint8_t *)destPtr, (uint8_t *)&settingsVFOChannel[1], CODEPLUG_CHANNEL_DATA_STRUCT_SIZE);
					codeplugConvertChannelInternalToCodeplug(destPtr, destPtr);
				}

				TASK_LOCK_WRITE();
			}
			}
			break;

		case CPS_ACCESS_EEPROM:
			TASK_UNLOCK_WRITE();
			result = EEPROM_Read(address, (uint8_t *)&usbComSendBuf[3], length);
			TASK_LOCK_WRITE();
			break;

		case CPS_ACCESS_MCU_ROM:
			// Base address of MCU ROM on the STM32 is 0x8000000 not 0x00000000
			memcpy((uint8_t *)&usbComSendBuf[3], (uint8_t *)address + 0x8000000, length);
			result = true;
			break;

		case CPS_ACCESS_DISPLAY_BUFFER:
			rxPowerSavingSetState(ECOPHASE_POWERSAVE_INACTIVE); // Avoiding going into a state that USB doesn't work.
#if defined(PLATFORM_VARIANT_DM1701)
			if (address < (DISPLAY_Y_OFFSET * DISPLAY_SIZE_X * 2))
			{
				memset((uint8_t *)&usbComSendBuf[3], 0xFF, length);// send white for blank area
			}
			else
			{
				memcpy((uint8_t *)&usbComSendBuf[3], (uint8_t *)displayGetPrimaryScreenBuffer() + address - (DISPLAY_Y_OFFSET * DISPLAY_SIZE_X * 2), length);
			}
#else
			memcpy((uint8_t *)&usbComSendBuf[3], (uint8_t *)displayGetPrimaryScreenBuffer() + address, length);
#endif
			result = true;
			break;

		case CPS_ACCESS_WAV_BUFFER:
			memcpy((uint8_t *)&usbComSendBuf[3], (uint8_t *)&audioAndHotspotDataBuffer.rawBuffer[address], length);
			result = true;
			break;

		case CPS_COMPRESS_AND_ACCESS_AMBE_BUFFER:// read from ambe audio buffer
			{
				uint8_t ambeBuf[32];// ambe data is up to 27 bytes long, but the normal transfer length for the CPS is 32, so make the buffer big enough for that transfer size
				memset(ambeBuf, 0, 32);// Clear the ambe output buffer
				codecEncode((uint8_t *)ambeBuf, 3);
				memcpy((uint8_t *)&usbComSendBuf[3], ambeBuf, length);// The ambe data is only 27 bytes long but the normal CPS request size is 32
				memset((uint8_t *)&audioAndHotspotDataBuffer.rawBuffer[0], 0x00, 960);// clear the input wave buffer, in case the next transfer is not a complete AMBE frame. 960 bytes compresses to 27 bytes of AMBE
				result = true;
			}
			break;

		case CPS_ACCESS_RADIO_INFO:
			{
				struct __attribute__((__packed__))
				{
					uint32_t structVersion;
					uint32_t radioType;
					char gitRevision[16];
					char buildDateTime[16];
					uint32_t flashId;
					uint16_t features;
				} radioInfo;

				radioInfo.structVersion = 0x03;
#if defined(PLATFORM_GD77)
				radioInfo.radioType = 0;
#elif defined(PLATFORM_GD77S)
				radioInfo.radioType = 1;
#elif defined(PLATFORM_DM1801)
				radioInfo.radioType = 2;
#elif defined(PLATFORM_RD5R)
				radioInfo.radioType = 3;
#elif defined(PLATFORM_DM1801A)
				radioInfo.radioType = 4;
#elif defined(PLATFORM_MD9600)
				radioInfo.radioType = 5;
#elif defined(PLATFORM_MDUV380)
				radioInfo.radioType = 6;
#elif defined(PLATFORM_MD380)
				radioInfo.radioType = 7;
#elif defined(PLATFORM_RT84_DM1701) // because of BGR565 / RGB656 colour formats
				radioInfo.radioType = ((DISPLAYLCD_TYPE_IS_RGB(displayLCD_Type) != 0) ? 10 : 8);
#elif defined(PLATFORM_MD2017)
				radioInfo.radioType = 9;
#endif
				// 10 is reserved for DM1701 with RGB panel

				snprintf(radioInfo.gitRevision, sizeof(radioInfo.gitRevision), "%s", XSTRINGIFY(GITVERSIONREV));
				snprintf(radioInfo.buildDateTime, sizeof(radioInfo.buildDateTime), "%04d%02d%02d%02d%02d%02d", BUILD_YEAR, BUILD_MONTH, BUILD_DAY, BUILD_HOUR, BUILD_MIN, BUILD_SEC);
				radioInfo.flashId = flashChipPartNumber;
				// Features bitfield (16 bits)
				radioInfo.features = (settingsIsOptionBitSet(BIT_INVERSE_VIDEO) ? 1 : 0);
				radioInfo.features |= (((dmrIDDatabaseMemoryLocation2 == VOICE_PROMPTS_FLASH_HEADER_ADDRESS) ? 1 : 0) << 1);
				radioInfo.features |= ((voicePromptDataIsLoaded ? 1 : 0) << 2);

				length = sizeof(radioInfo);
				memcpy((uint8_t *)&usbComSendBuf[3], &radioInfo, length);
				result = true;
			}
			break;

#if ! defined(CPU_MK22FN512VLL12)
		case CPS_ACCESS_FLASH_SECURITY_REGISTERS:
			TASK_UNLOCK_WRITE();
			result = SPI_Flash_readSecurityRegisters(address, (uint8_t *)&usbComSendBuf[3], length);
			TASK_LOCK_WRITE();
			break;
#endif
	}

	hasToReply = true;
	if (result)
	{
		usbComSendBuf[0] = com_requestbuffer[0];
		usbComSendBuf[1] = (length >> 8) & 0xFF;
		usbComSendBuf[2] = (length >> 0) & 0xFF;
		replyLength = length + 3;
	}
	else
	{
		usbComSendBuf[0] = '-';
		replyLength = 1;
	}
}

static void cpsHandleWriteCommand(void)
{
	bool ok = false;

	switch(com_requestbuffer[1])
	{
		case 1: // Flash Prepare Sector
			if (sector == -1)
			{
				sector = (com_requestbuffer[2] << 16) + (com_requestbuffer[3] << 8) + (com_requestbuffer[4] << 0);

				if ((sector * 4096) == 0x10000) // Local calibration
				{
					ok = true;
					break;
				}
				else if ((sector * 4096) == 0x30000) // start address of DMRIDs DB
				{
					flashingDMRIDs = true;
				}

				TASK_UNLOCK_WRITE();
				ok = SPI_Flash_read(sector * 4096, SPI_Flash_sectorbuffer, 4096);
				TASK_LOCK_WRITE();
			}
			break;

		case 2: // Flash Send Data
			if (sector >= 0)
			{
				uint32_t address = (com_requestbuffer[2] << 24) + (com_requestbuffer[3] << 16) + (com_requestbuffer[4] << 8) + (com_requestbuffer[5] << 0);
				uint32_t length = (com_requestbuffer[6] << 8) + (com_requestbuffer[7] << 0);
				bool calibrationWriting = false;

#if defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				if ((calibrationWriting == false) && addressInSegment(address, length, 0x10000, 0x200)) // Local calibration
				{
					calibrationWriting = true;
				}
				// Channel is going to be rewritten, will need to reset current zone/etc...
				else if ((channelsRewritten == false) && addressInSegment(address, length, CODEPLUG_ADDR_CHANNEL_HEADER_EEPROM, 0x1C10 /*128 first channels*/))
				{
					channelsRewritten = true;
				}
				else if ((luczRewritten == false) && addressInSegment(address, length, CODEPLUG_ADDR_LUCZ, (4 + CODEPLUG_ALL_ZONES_MAX + 1)))
				{
					luczRewritten = true;
				}
				else
#endif
#if !defined(PLATFORM_GD77S)
				// Temporary hack to automatically set Prompt to Level 1
				// A better solution will be added to the CPS and firmware at a later date.
				if ((address == VOICE_PROMPTS_FLASH_HEADER_ADDRESS) || (address == VOICE_PROMPTS_FLASH_OLD_HEADER_ADDRESS))
				{
					if (voicePromptsCheckMagicAndVersion((uint32_t *)&com_requestbuffer[8]))
					{
						nonVolatileSettings.audioPromptMode = AUDIO_PROMPT_MODE_VOICE_LEVEL_1;
					}
				}
#endif

				if (length > (COM_REQUESTBUFFER_SIZE - 8))
				{
					length = (COM_REQUESTBUFFER_SIZE - 8);
				}

#if defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
				// Temporary hack to prevent the QuickKeys getting overwritten by the codeplug
				const int QUICKKEYS_BLOCK_END = (CODEPLUG_ADDR_QUICKKEYS + (CODEPLUG_QUICKKEYS_SIZE * sizeof(uint16_t)) - 1);
				int end = (address + length) - 1;

				if (((address >= CODEPLUG_ADDR_QUICKKEYS) && (address <= QUICKKEYS_BLOCK_END))
						|| ((end >= CODEPLUG_ADDR_QUICKKEYS) && (end <= QUICKKEYS_BLOCK_END))
						|| ((address < CODEPLUG_ADDR_QUICKKEYS) && (end > QUICKKEYS_BLOCK_END)))
				{
					if (address < CODEPLUG_ADDR_QUICKKEYS)
					{
						for (int i = 0; i < (CODEPLUG_ADDR_QUICKKEYS - address); i++)
						{
							if (sector == (address + i) / 4096)
							{
								SPI_Flash_sectorbuffer[(address + i) % 4096] = com_requestbuffer[i + 8];
							}
						}

						if ((end > QUICKKEYS_BLOCK_END))
						{
							for (int i = 0; i <  (end - QUICKKEYS_BLOCK_END); i++)
							{
								if (sector == ((QUICKKEYS_BLOCK_END + 1) + i) / 4096)
								{
									SPI_Flash_sectorbuffer[((QUICKKEYS_BLOCK_END + 1) + i) % 4096] = com_requestbuffer[i + 8 + ((QUICKKEYS_BLOCK_END + 1) - address)];
								}
							}
						}

					}
					else
					{
						if ((address <= QUICKKEYS_BLOCK_END) && (end > QUICKKEYS_BLOCK_END))
						{
							for (int i = 0; i < (end - QUICKKEYS_BLOCK_END); i++)
							{
								if (sector == ((QUICKKEYS_BLOCK_END + 1) + i) / 4096)
								{
									SPI_Flash_sectorbuffer[((QUICKKEYS_BLOCK_END + 1) + i) % 4096] = com_requestbuffer[i + 8 + ((QUICKKEYS_BLOCK_END + 1) - address)];
								}
							}
						}
					}
				}
				else
#endif
				{
#if defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
					if (calibrationWriting)
					{
						uint8_t *p = calibrationGetLocalDataPointer();

						memcpy((p + (address - 0x10000)), (uint8_t *)&com_requestbuffer[8], length);
						sector = -2; // special case in Flash Write;
						ok = true;
						break;
					}
#endif

					for (int i = 0; i < length; i++)
					{
						if (sector == (address + i) / 4096)
						{
							SPI_Flash_sectorbuffer[(address + i) % 4096] = com_requestbuffer[i + 8];
						}
					}
				}

				ok = true;
			}
			break;

		case 3: // Flash Write
			if (sector >= 0)
			{
				TASK_UNLOCK_WRITE();
				ok = SPI_Flash_eraseSector(sector * 4096);
				TASK_LOCK_WRITE();
				if (ok)
				{
					for (int i = 0; i < 16; i++)
					{
						TASK_UNLOCK_WRITE();
						ok = SPI_Flash_writePage(sector * 4096 + i * 256, SPI_Flash_sectorbuffer + i * 256);
						TASK_LOCK_WRITE();
						if (!ok)
						{
							break;
						}
					}
				}
				sector = -1;
			}
			else if (sector == -2)
			{
				TASK_UNLOCK_WRITE();
				calibrationSaveLocal();
				TASK_LOCK_WRITE();
				ok = true;
				sector = -1;
			}
			break;

		case 4: // EEPROM
			{
#if defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
				uint32_t address = (com_requestbuffer[2] << 24) + (com_requestbuffer[3] << 16) + (com_requestbuffer[4] << 8) + (com_requestbuffer[5] << 0);
				uint32_t length = (com_requestbuffer[6] << 8) + (com_requestbuffer[7] << 0);

				// Channel is going to be rewritten, will need to reset current zone/etc...
				if ((channelsRewritten == false) && addressInSegment(address, length, CODEPLUG_ADDR_CHANNEL_HEADER_EEPROM, 0x1C10 /*128 first channels*/))
				{
					channelsRewritten = true;
				}
				else if ((luczRewritten == false) && addressInSegment(address, length, CODEPLUG_ADDR_LUCZ, (4 + CODEPLUG_ALL_ZONES_MAX + 1)))
				{
					luczRewritten = true;
				}

				if (length > (COM_REQUESTBUFFER_SIZE - 8))
				{
					length = (COM_REQUESTBUFFER_SIZE - 8);
				}

				// Temporary hack to prevent the QuickKeys getting overwritten by the codeplug
				const int QUICKKEYS_BLOCK_END = (CODEPLUG_ADDR_QUICKKEYS + (CODEPLUG_QUICKKEYS_SIZE * sizeof(uint16_t)) - 1);
				int end = (address + length) - 1;

				if (((address >= CODEPLUG_ADDR_QUICKKEYS) && (address <= QUICKKEYS_BLOCK_END))
						|| ((end >= CODEPLUG_ADDR_QUICKKEYS) && (end <= QUICKKEYS_BLOCK_END))
						|| ((address < CODEPLUG_ADDR_QUICKKEYS) && (end > QUICKKEYS_BLOCK_END)))
				{
					if (address < CODEPLUG_ADDR_QUICKKEYS)
					{
						TASK_UNLOCK_WRITE();
						ok = EEPROM_Write(address, (uint8_t*)com_requestbuffer + 8, (CODEPLUG_ADDR_QUICKKEYS - address));
						TASK_LOCK_WRITE();

						if (ok && (end > QUICKKEYS_BLOCK_END))
						{
							TASK_UNLOCK_WRITE();
							ok = EEPROM_Write((QUICKKEYS_BLOCK_END + 1), (uint8_t *)com_requestbuffer + 8 + ((QUICKKEYS_BLOCK_END + 1) - address),  (end - QUICKKEYS_BLOCK_END));
							TASK_LOCK_WRITE();
						}

					}
					else
					{
						if ((address <= QUICKKEYS_BLOCK_END) && (end > QUICKKEYS_BLOCK_END))
						{
							TASK_UNLOCK_WRITE();
							ok = EEPROM_Write((QUICKKEYS_BLOCK_END + 1), (uint8_t *)com_requestbuffer + 8 + ((QUICKKEYS_BLOCK_END + 1) - address), (end - QUICKKEYS_BLOCK_END));
							TASK_LOCK_WRITE();
						}
						else
						{
							ok = true;
						}
					}
					//	SEGGER_RTT_printf(0, "0x%06x\t0x%06x\n",address,end);
				}
				else
				{
					TASK_UNLOCK_WRITE();
					ok = EEPROM_Write(address, (uint8_t *)com_requestbuffer + 8, length);
					TASK_LOCK_WRITE();
				}
#else
				ok = true;
#endif
			}
			break;

		case CPS_ACCESS_WAV_BUFFER:// write to raw audio buffer
			{
				uint32_t address = (com_requestbuffer[2] << 24) + (com_requestbuffer[3] << 16) + (com_requestbuffer[4] << 8) + (com_requestbuffer[5] << 0);
				uint32_t length = (com_requestbuffer[6] << 8) + (com_requestbuffer[7] << 0);

				wavbuffer_count = (address + length) / WAV_BUFFER_SIZE;
				memcpy((uint8_t *)&audioAndHotspotDataBuffer.rawBuffer[address], (uint8_t *)&com_requestbuffer[8], length);
				ok = true;
			}
			break;

		case CPS_ACCESS_RADIO_INFO:
			break;
	}

	hasToReply = true;
	if (ok)
	{
		usbComSendBuf[0] = com_requestbuffer[0];
		usbComSendBuf[1] = com_requestbuffer[1];
		replyLength = 2;
	}
	else
	{
		sector = -1;
		usbComSendBuf[0] = '-';
		replyLength = 1;
	}
}
#ifdef USB_DEBUG_COMMANDS
static void cpsHandleDebugCommand(void)
{
	const pins[5] = { GPIO_PIN_11, GPIO_PIN_12, GPIO_PIN_13, GPIO_PIN_14, GPIO_PIN_15};
	int command = com_requestbuffer[1];
	switch(command)
	{
		case 'W':
			HAL_GPIO_WritePin(GPIOE, GPIO_PIN_11,com_requestbuffer[2]=='1'?GPIO_PIN_SET:GPIO_PIN_RESET);
			HAL_GPIO_WritePin(GPIOE, GPIO_PIN_12,com_requestbuffer[3]=='1'?GPIO_PIN_SET:GPIO_PIN_RESET);
			HAL_GPIO_WritePin(GPIOE, GPIO_PIN_13,com_requestbuffer[4]=='1'?GPIO_PIN_SET:GPIO_PIN_RESET);
			HAL_GPIO_WritePin(GPIOE, GPIO_PIN_14,com_requestbuffer[5]=='1'?GPIO_PIN_SET:GPIO_PIN_RESET);
			HAL_GPIO_WritePin(GPIOE, GPIO_PIN_15,com_requestbuffer[6]=='1'?GPIO_PIN_SET:GPIO_PIN_RESET);

			memcpy(usbComSendBuf," OK\n\0",5);
			hasToReply = true;
			replyLength = strlen(usbComSendBuf);
			break;
		case 'R':
			for(int i=0;i<5;i++)
			{
				usbComSendBuf[i]= (HAL_GPIO_ReadPin(GPIOE, pins[i])==GPIO_PIN_SET)?'1':'0';
			}
			usbComSendBuf[5] = '\n';
			usbComSendBuf[6] = 0;
			hasToReply = true;
			replyLength = strlen(usbComSendBuf);
			break;
	}
}
#endif

#if defined(HAS_GPS)
static void cpsStopGPSNMEA(void)
{
	if (SETTINGS_GPS_MODE_GET(nonVolatileSettings) >= GPS_MODE_ON_NMEA)
	{
#if defined(LOG_GPS_DATA)
		TASK_UNLOCK_WRITE();
		gpsLoggingStop();
		TASK_LOCK_WRITE();
#endif
		previousGPSState = SETTINGS_GPS_MODE_GET(nonVolatileSettings);
		nonVolatileSettings.gpsModeAndBaudsIndex = SETTINGS_GPS_MODE_SET(nonVolatileSettings, GPS_MODE_ON);
	}
}
#endif

static void cpsHandleCommand(void)
{
	int command = com_requestbuffer[1];
	switch(command)
	{
#if defined(ENABLE_SPECTRUM)
		case 0xA0: // DEV: swept-RSSI sweep. Request (15 B):
			//   [2..5]=start freq BE, [6..9]=step BE (both in 10 Hz units),
			//   [10..11]=point count BE, [12..13]=dwell us BE, [14]=mode (see spectrum.h).
			// Reply: [cmd, 0xA0, points BE(2), elapsedMs BE(2), points * (rssi, noise)].
			// `points` may come back short of what was asked for -- the firmware caps how
			// long it holds the CPS critical section, so the host just asks for the rest.
			// A count of 0 means refused (a sweep may not straddle two bands: the per-point
			// retune only moves the PLL, it does not re-select the RX front end).
			{
				uint32_t fStart = (com_requestbuffer[2] << 24) | (com_requestbuffer[3] << 16) |
						(com_requestbuffer[4] << 8) | com_requestbuffer[5];
				uint32_t step = (com_requestbuffer[6] << 24) | (com_requestbuffer[7] << 16) |
						(com_requestbuffer[8] << 8) | com_requestbuffer[9];
				uint16_t nPoints = (com_requestbuffer[10] << 8) | com_requestbuffer[11];
				uint16_t dwellUs = (com_requestbuffer[12] << 8) | com_requestbuffer[13];
				uint8_t mode = com_requestbuffer[14];
				uint16_t elapsedMs = 0;
				uint32_t fEnd;
				int n = 0;

				if (nPoints > SPECTRUM_SWEEP_MAX_POINTS)
				{
					nPoints = SPECTRUM_SWEEP_MAX_POINTS;
				}

				fEnd = fStart + ((uint32_t)(nPoints ? (nPoints - 1) : 0) * step);

				if ((nPoints > 0) && (trxGetBandFromFrequency(fStart) != FREQUENCY_OUT_OF_BAND) &&
						(trxGetBandFromFrequency(fStart) == trxGetBandFromFrequency(fEnd)))
				{
					n = spectrumSweep(fStart, step, nPoints, dwellUs, mode,
							(uint8_t *)&usbComSendBuf[6], &elapsedMs);
				}

				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = 0xA0;
				usbComSendBuf[2] = (uint8_t)((n >> 8) & 0xFF);
				usbComSendBuf[3] = (uint8_t)(n & 0xFF);
				usbComSendBuf[4] = (uint8_t)((elapsedMs >> 8) & 0xFF);
				usbComSendBuf[5] = (uint8_t)(elapsedMs & 0xFF);
				hasToReply = true;
				replyLength = 6 + (n * 2);
			}
			return;   /* NOT break: the generic '-' reply below would clobber the data */
		case 0xA1: // DEV: retune -> settle -> register timing probe (Stage 0). Request (15 B):
			//   [2..5]=freq A BE, [6..9]=freq B BE (10 Hz units), [10]=mode,
			//   [11]=sample count, [12..13]=sample interval us BE (0 = as fast as I2C allows),
			//   [14]=AT1846S register to sample (0 = 0x1B, i.e. rssi/noise).
			// Reply: [cmd, 0xA1, count, mode, retuneUs BE(2), readUs BE(2),
			//         count * (tUs BE(2), hi, lo)].
			// [14] is optional for backwards compatibility: a 14-byte request from an older
			// host leaves it zero, which selects 0x1B and is exactly what it used to get.
			{
				uint32_t fA = (com_requestbuffer[2] << 24) | (com_requestbuffer[3] << 16) |
						(com_requestbuffer[4] << 8) | com_requestbuffer[5];
				uint32_t fB = (com_requestbuffer[6] << 24) | (com_requestbuffer[7] << 16) |
						(com_requestbuffer[8] << 8) | com_requestbuffer[9];
				uint8_t mode = com_requestbuffer[10];
				uint8_t nSamples = com_requestbuffer[11];
				uint16_t intervalUs = (com_requestbuffer[12] << 8) | com_requestbuffer[13];
				uint8_t reg = com_requestbuffer[14];
				static spectrumSample_t samples[SPECTRUM_PROBE_MAX_SAMPLES];
				spectrumProbeInfo_t info = { 0, 0, 0 };
				int n = 0;

				if ((trxGetBandFromFrequency(fA) != FREQUENCY_OUT_OF_BAND) &&
						(trxGetBandFromFrequency(fB) != FREQUENCY_OUT_OF_BAND))
				{
					n = spectrumSettleProbe(fA, fB, mode, intervalUs, nSamples, reg, samples, &info);
				}

				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = 0xA1;
				usbComSendBuf[2] = (uint8_t)n;
				usbComSendBuf[3] = mode;
				usbComSendBuf[4] = (uint8_t)((info.retuneUs >> 8) & 0xFF);
				usbComSendBuf[5] = (uint8_t)(info.retuneUs & 0xFF);
				usbComSendBuf[6] = (uint8_t)((info.readUs >> 8) & 0xFF);
				usbComSendBuf[7] = (uint8_t)(info.readUs & 0xFF);
				for (int i = 0; i < n; i++)
				{
					usbComSendBuf[8 + (i * 4)] = (uint8_t)((samples[i].tUs >> 8) & 0xFF);
					usbComSendBuf[9 + (i * 4)] = (uint8_t)(samples[i].tUs & 0xFF);
					usbComSendBuf[10 + (i * 4)] = samples[i].rssi;
					usbComSendBuf[11 + (i * 4)] = samples[i].noise;
				}
				hasToReply = true;
				replyLength = 8 + (n * 4);
			}
			return;   /* NOT break -- see above */
		case 0xA2: // DEV: open a sweep session: [2..5]=anchor freq BE, [6]=mode.
			//        Configures the receiver ONCE and leaves it running, so the 0xA0
			//        sweeps that follow only move the PLL and skip the ~30 ms
			//        receiver-restart settle. The session self-closes after
			//        SPECTRUM_SESSION_TIMEOUT_MS without a sweep.
			{
				uint32_t anchor = (com_requestbuffer[2] << 24) | (com_requestbuffer[3] << 16) |
						(com_requestbuffer[4] << 8) | com_requestbuffer[5];

				if (trxGetBandFromFrequency(anchor) != FREQUENCY_OUT_OF_BAND)
				{
					spectrumSessionBegin(anchor, com_requestbuffer[6]);
				}

				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = 0xA2;
				usbComSendBuf[2] = (uint8_t)(spectrumSessionIsActive() ? 1 : 0);
				hasToReply = true;
				replyLength = 3;
			}
			return;   /* NOT break -- see above */
		case 0xA3: // DEV: close a sweep session and put the radio back on its channel.
			spectrumSessionEnd();
			usbComSendBuf[0] = com_requestbuffer[0];
			usbComSendBuf[1] = 0xA3;
			usbComSendBuf[2] = 0;
			hasToReply = true;
			replyLength = 3;
			return;   /* NOT break -- see above */
		case 0xA4: // DEV: set the AT1846S register overrides re-applied after every
			//        retune: [2]=count, then count * [reg, hi, lo]. Count 0 clears them.
			//        Lets the settle-time search try a candidate register setting per
			//        second instead of per flash cycle.
			{
				int count = com_requestbuffer[2];

				if (count > SPECTRUM_MAX_OVERRIDES)
				{
					count = SPECTRUM_MAX_OVERRIDES;
				}
				spectrumSetOverrides(count, (const uint8_t *)&com_requestbuffer[3]);

				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = 0xA4;
				usbComSendBuf[2] = (uint8_t)spectrumGetOverrideCount();
				hasToReply = true;
				replyLength = 3;
			}
			return;   /* NOT break -- see above */
		case 0xA6: // DEV: set the analog-scan dwell override in ms: [2..3] BE. 0 restores
			//        stock behaviour. Runtime rather than compile-time so stock and
			//        modified scanning can be compared on one firmware image.
			//        Takes effect at the next scanStart().
			spectrumScanAnalogDwellMs = (uint16_t)((com_requestbuffer[2] << 8) |
					com_requestbuffer[3]);
			usbComSendBuf[0] = com_requestbuffer[0];
			usbComSendBuf[1] = 0xA6;
			usbComSendBuf[2] = (uint8_t)((spectrumScanAnalogDwellMs >> 8) & 0xFF);
			usbComSendBuf[3] = (uint8_t)(spectrumScanAnalogDwellMs & 0xFF);
			hasToReply = true;
			replyLength = 4;
			return;   /* NOT break -- see above */
		case 0xA8: // DEV: set the built-in VFO sweep step time in ms: [2..3] BE.
			//        0 restores the stock 25 ms. Takes effect on the next sweep sample,
			//        so 25 ms and a faster value can be compared live without reflashing.
			spectrumSweepStepTimeMs = (uint16_t)((com_requestbuffer[2] << 8) |
					com_requestbuffer[3]);
			usbComSendBuf[0] = com_requestbuffer[0];
			usbComSendBuf[1] = 0xA8;
			usbComSendBuf[2] = (uint8_t)((spectrumSweepStepTimeMs >> 8) & 0xFF);
			usbComSendBuf[3] = (uint8_t)(spectrumSweepStepTimeMs & 0xFF);
			hasToReply = true;
			replyLength = 4;
			return;   /* NOT break -- see above */
		case 0xAA: // DEV: set the scan settling interval in main-loop ticks: [2..3] BE.
			//        0 restores the stock SCAN_FREQ_CHANGE_SETTLING_INTERVAL of 1.
			//        Takes effect on the next scan step, so the detection threshold can be
			//        measured against several values without reflashing. See spectrum.h.
			spectrumScanSettleTicks = (uint16_t)((com_requestbuffer[2] << 8) |
					com_requestbuffer[3]);
			usbComSendBuf[0] = com_requestbuffer[0];
			usbComSendBuf[1] = 0xAA;
			usbComSendBuf[2] = (uint8_t)((spectrumScanSettleTicks >> 8) & 0xFF);
			usbComSendBuf[3] = (uint8_t)(spectrumScanSettleTicks & 0xFF);
			hasToReply = true;
			replyLength = 4;
			return;   /* NOT break -- see above */
#if defined(ENABLE_SCAN_PROFILER)
		case 0xA9: // DEV: read the scan-step profiler table. [2] = action:
			//        bit0 = zero the table after reading, bit1 = zero it and return nothing
			//        useful (arm before a run). Reply:
			//        [cmd, 0xA9, nSlots, cyclesPerUs BE(2),
			//         then per slot: count BE(2), lastUs BE(2), minUs BE(2), maxUs BE(2),
			//                        meanUs BE(2)].
			//        Microseconds are saturated at 65535 (65 ms); nothing being measured
			//        here should come close, and a slot that does is broken anyway.
			{
				uint32_t cpu = scanProfCyclesPerUs();
				uint8_t *p = (uint8_t *)&usbComSendBuf[0];
				int n = 0;

				p[n++] = com_requestbuffer[0];
				p[n++] = 0xA9;
				p[n++] = (uint8_t)SCANPROF_SLOTS;
				p[n++] = (uint8_t)((cpu >> 8) & 0xFF);
				p[n++] = (uint8_t)(cpu & 0xFF);

				if ((com_requestbuffer[2] & 0x02) == 0)
				{
					for (int i = 0; i < SCANPROF_SLOTS; i++)
					{
						const scanProfSlot_t *s = &scanProfSlots[i];
						uint32_t vals[5];

						vals[0] = ((s->count > 65535U) ? 65535U : s->count);
						vals[1] = (s->lastCycles / cpu);
						vals[2] = ((s->count != 0) ? (s->minCycles / cpu) : 0U);
						vals[3] = (s->maxCycles / cpu);
						vals[4] = ((s->count != 0) ? (uint32_t)((s->sumCycles / s->count) / cpu) : 0U);

						for (int k = 0; k < 5; k++)
						{
							uint32_t v = ((vals[k] > 65535U) ? 65535U : vals[k]);

							p[n++] = (uint8_t)((v >> 8) & 0xFF);
							p[n++] = (uint8_t)(v & 0xFF);
						}
					}
				}

				if (com_requestbuffer[2] & 0x03)
				{
					scanProfReset();
				}

				hasToReply = true;
				replyLength = n;
			}
			return;   /* NOT break -- see above */
#endif /* ENABLE_SCAN_PROFILER */
		case 0xA5: // DEV: read one AT1846S register: [2]=reg -> [cmd, 0xA5, reg, hi, lo, ok].
			//        Reads the chip, not the driver's value cache.
			{
				uint8_t hi = 0, lo = 0;
				bool ok = spectrumReadReg(com_requestbuffer[2], &hi, &lo);

				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = 0xA5;
				usbComSendBuf[2] = com_requestbuffer[2];
				usbComSendBuf[3] = hi;
				usbComSendBuf[4] = lo;
				usbComSendBuf[5] = (uint8_t)(ok ? 1 : 0);
				hasToReply = true;
				replyLength = 6;
			}
			return;   /* NOT break -- see above */
		case 0xAB: // DEV: write one AT1846S register raw: [2]=reg, [3]=hi, [4]=lo
			//        -> [cmd, 0xAB, reg, hi, lo, ok]. The counterpart to 0xA5, and the
			//        pair is what makes the hardware squelch testable without a flash
			//        cycle: set sq_on (30H[3]) and the thresholds (48H/49H), then read
			//        candidate status registers with the carrier on and off.
			//        Bypasses the driver's value cache -- see spectrum.h.
			{
				bool ok = spectrumWriteRegRaw(com_requestbuffer[2], com_requestbuffer[3],
						com_requestbuffer[4]);

				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = 0xAB;
				usbComSendBuf[2] = com_requestbuffer[2];
				usbComSendBuf[3] = com_requestbuffer[3];
				usbComSendBuf[4] = com_requestbuffer[4];
				usbComSendBuf[5] = (uint8_t)(ok ? 1 : 0);
				hasToReply = true;
				replyLength = 6;
			}
			return;   /* NOT break -- see above */
		case 0xAC: // DEV: report the analog squelch threshold the scanner is currently
			//        using, the live rssi/noise, the running RSSI noise floor and the
			//        detection mode, as [cmd, 0xAC, squelch, rssi, noise, floor, mode].
			//        `noise < squelch` IS the stock scan/stop decision
			//        (trxCarrierDetected), so a detection experiment that assumes a
			//        threshold is measuring against a number the radio may not be using.
			//        Ask instead. The floor is here so the host can watch mode 3's
			//        estimator converge rather than infer that it has.
			{
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = 0xAC;
				usbComSendBuf[2] = trxGetAnalogSquelchThreshold();
				usbComSendBuf[3] = currentRadioDevice->trxRxSignal;
				usbComSendBuf[4] = currentRadioDevice->trxRxNoise;
				usbComSendBuf[5] = spectrumScanFloor();
				usbComSendBuf[6] = spectrumScanDetectMode;
				/* Whether the scan is running, and whether it has stopped on something.
				 * Without this the host can only infer detection by recomputing the rule
				 * from a slow poll -- against a different sample than the one the radio
				 * actually decided on, which is a guess dressed up as a measurement.
				 * 0 = SCANNING, 1 = SHORT_PAUSED, 2 = PAUSED. */
				usbComSendBuf[7] = (uint8_t)uiDataGlobal.Scan.state;
				usbComSendBuf[8] = (uint8_t)(uiDataGlobal.Scan.active ? 1 : 0);
				hasToReply = true;
				replyLength = 9;
			}
			return;   /* NOT break -- see above */
		case 0xAD: // DEV: choose what the scanner decides on (see spectrum.h):
			//   [2]=mode (0 stock / 1 rssi / 2 chip squelch bit / 3 rssi vs running floor),
			//   [3]=rssi threshold (mode 1), [4]=status register, [5..6]=mask BE,
			//   [7]=invert, [8]=margin over the floor (mode 3), [9]=floor IIR shift.
			//   Reply echoes all of it back. Takes effect on the next carrier test, so a
			//   detection threshold can be searched under each rule without reflashing.
			//   Always forgets the learned floor: a run must never start against an
			//   estimate learned on another band, another carrier or another margin.
			spectrumScanDetectMode = com_requestbuffer[2];
			spectrumScanRssiThreshold = com_requestbuffer[3];
			spectrumScanSqReg = com_requestbuffer[4];
			spectrumScanSqMask = (uint16_t)((com_requestbuffer[5] << 8) | com_requestbuffer[6]);
			spectrumScanSqInvert = (com_requestbuffer[7] != 0);
			if (com_requestbuffer[8] != 0)
			{
				spectrumScanRssiMargin = com_requestbuffer[8];
			}
			if (com_requestbuffer[9] != 0)
			{
				/* A shift of 0 would make the floor follow every sample exactly, so it
				 * could never sit below a carrier -- nothing would ever be detected.
				 * Above 8 it stops moving on this timescale. */
				spectrumScanFloorShift = (com_requestbuffer[9] > 8) ? 8 : com_requestbuffer[9];
			}
			spectrumScanDetectReset();
			usbComSendBuf[0] = com_requestbuffer[0];
			usbComSendBuf[1] = 0xAD;
			usbComSendBuf[2] = spectrumScanDetectMode;
			usbComSendBuf[3] = spectrumScanRssiThreshold;
			usbComSendBuf[4] = spectrumScanSqReg;
			usbComSendBuf[5] = (uint8_t)((spectrumScanSqMask >> 8) & 0xFF);
			usbComSendBuf[6] = (uint8_t)(spectrumScanSqMask & 0xFF);
			usbComSendBuf[7] = (uint8_t)(spectrumScanSqInvert ? 1 : 0);
			usbComSendBuf[8] = spectrumScanRssiMargin;
			usbComSendBuf[9] = spectrumScanFloorShift;
			hasToReply = true;
			replyLength = 10;
			return;   /* NOT break -- see above */
		case 0xAE: // DEV: the fast reject (see spectrum.h).
			//   [2]=action (0 = just read the counters, 1 = set and zero them),
			//   [3..4]=reject ticks BE (0 disables), [5]=reject margin.
			//   Reply: [cmd, 0xAE, ticks BE(2), margin, stepsTotal BE(4),
			//           stepsRejected BE(4)].
			//   The reject fraction IS the speed win and cannot be inferred from
			//   outside, so it is counted in the firmware and read from here.
			{
				if (com_requestbuffer[2] != 0)
				{
					spectrumScanRejectTicks = (uint16_t)((com_requestbuffer[3] << 8) |
							com_requestbuffer[4]);
					if (com_requestbuffer[5] != 0)
					{
						spectrumScanRejectMargin = com_requestbuffer[5];
					}
					spectrumScanDetectReset();   // also zeroes the counters
				}

				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = 0xAE;
				usbComSendBuf[2] = (uint8_t)((spectrumScanRejectTicks >> 8) & 0xFF);
				usbComSendBuf[3] = (uint8_t)(spectrumScanRejectTicks & 0xFF);
				usbComSendBuf[4] = spectrumScanRejectMargin;
				usbComSendBuf[5] = (uint8_t)((spectrumScanStepsTotal >> 24) & 0xFF);
				usbComSendBuf[6] = (uint8_t)((spectrumScanStepsTotal >> 16) & 0xFF);
				usbComSendBuf[7] = (uint8_t)((spectrumScanStepsTotal >> 8) & 0xFF);
				usbComSendBuf[8] = (uint8_t)(spectrumScanStepsTotal & 0xFF);
				usbComSendBuf[9] = (uint8_t)((spectrumScanStepsRejected >> 24) & 0xFF);
				usbComSendBuf[10] = (uint8_t)((spectrumScanStepsRejected >> 16) & 0xFF);
				usbComSendBuf[11] = (uint8_t)((spectrumScanStepsRejected >> 8) & 0xFF);
				usbComSendBuf[12] = (uint8_t)(spectrumScanStepsRejected & 0xFF);
				hasToReply = true;
				replyLength = 13;
			}
			return;   /* NOT break -- see above */
#endif
#ifdef ENABLE_KEY_INJECTION
		case 0x96: // DEV: inject a keypad key: [2]=keycode, [3]=flags (bit0 = long press).
			//        Queued here; usbKeyInjectTick() feeds it to the normal UI (DOWN then UP)
			//        from the main loop. Pairs with the USB display mirror (fbmirror.py).
			usbKeyInjectPush(com_requestbuffer[2], com_requestbuffer[3]);
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			break;
		case 0xA7: // DEV: inject a UI FUNCTION event: [2..3] = FUNC_* code BE
			//        (menuSystem.h; 16-bit QUICKKEY_MENUVALUE encoding). Addresses a
			//        handler directly rather than hoping a key event survives the UI's
			//        else-if chain. FUNC_START_SCANNING = 0x8001 is what this was added
			//        for. Delivered from the main loop by usbFuncInjectTick().
			usbFuncInjectPush((uint16_t)((com_requestbuffer[2] << 8) | com_requestbuffer[3]));
			usbComSendBuf[0] = com_requestbuffer[0];
			usbComSendBuf[1] = 0xA7;
			usbComSendBuf[2] = com_requestbuffer[2];
			usbComSendBuf[3] = com_requestbuffer[3];
			hasToReply = true;
			replyLength = 4;
			return;   /* NOT break -- the generic '-' reply would clobber this */
		case 0x9F: // DEV: reboot into DFU (button-free dev flashing) by marking the app
			//        invalid, then resetting. Replies [cmd, halStatus, err(4), SR(4),
			//        spAfter(4)] so a failed flash write is diagnosable from the host.
			//        The reset is deferred ~500 ms so this reply gets out first, and is
			//        only scheduled when the SP word actually changed.
			{
				uint32_t diag[3];
				uint8_t st = usbInjectInvalidateApp(diag);
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = st;
				for (int i = 0; i < 3; i++)
				{
					usbComSendBuf[2 + i * 4] = (uint8_t)(diag[i] & 0xFF);
					usbComSendBuf[3 + i * 4] = (uint8_t)((diag[i] >> 8) & 0xFF);
					usbComSendBuf[4 + i * 4] = (uint8_t)((diag[i] >> 16) & 0xFF);
					usbComSendBuf[5 + i * 4] = (uint8_t)((diag[i] >> 24) & 0xFF);
				}
				hasToReply = true;
				replyLength = 14;
				if ((diag[2] & INJ_APP_SP_VALID_BIT) == 0U)   /* invalidated -> reboot */
				{
					addTimerCallback(usbInjectRebootToBootloader, 500, MENU_ANY, false);
				}
			}
			return;   /* NOT break: the generic '-' reply below would clobber the diags */
		case 0x9E: // DEV: program one word of internal flash: [2..5]=address (BE),
			//        [6..9]=value (BE). Both default to the erased scratch word when
			//        the address is 0. Replies [cmd, halStatus, err(4), SR(4), CR(4),
			//        readback(4)]. Never reboots.
			{
				uint32_t diag[4];
				uint8_t st;
				uint32_t addr = (com_requestbuffer[2] << 24) | (com_requestbuffer[3] << 16) |
						(com_requestbuffer[4] << 8) | com_requestbuffer[5];
				uint32_t val = (com_requestbuffer[6] << 24) | (com_requestbuffer[7] << 16) |
						(com_requestbuffer[8] << 8) | com_requestbuffer[9];

				if (addr == 0U)
				{
					addr = INJ_FLASH_PROBE_ADDR;
					val = 0xA5A5A5A5U;
				}

				/* Never let a typo reach the bootloader (sectors 0-2): losing that
				 * really would be unrecoverable, since it is what provides DFU. */
				if ((addr < INJ_APP_VECTOR_ADDR) || (addr > 0x080FFFFCU) || ((addr & 3U) != 0U))
				{
					usbComSendBuf[0] = com_requestbuffer[0];
					usbComSendBuf[1] = 0xFF;   /* refused */
					memset((uint8_t *)&usbComSendBuf[2], 0, 16);
					hasToReply = true;
					replyLength = 18;
					return;
				}

				/* Outside the CPS critical section: HAL_FLASH_Program waits on
				 * HAL_GetTick(), which cannot advance with interrupts masked. */
				TASK_UNLOCK_WRITE();
				st = usbInjectFlashProbe(addr, val, diag);
				TASK_LOCK_WRITE();

				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = st;
				for (int i = 0; i < 4; i++)
				{
					usbComSendBuf[2 + i * 4] = (uint8_t)(diag[i] & 0xFF);
					usbComSendBuf[3 + i * 4] = (uint8_t)((diag[i] >> 8) & 0xFF);
					usbComSendBuf[4 + i * 4] = (uint8_t)((diag[i] >> 16) & 0xFF);
					usbComSendBuf[5 + i * 4] = (uint8_t)((diag[i] >> 24) & 0xFF);
				}
				hasToReply = true;
				replyLength = 18;
			}
			return;   /* NOT break -- see above */
		case 0x9C: // DEV: enter DFU by erasing the app's first sector, then resetting.
			//        Persistent BY DESIGN -- the radio then boots only to DFU until the
			//        next flash, which rewrites that sector anyway. Deferred so this
			//        ACK gets out first.
			addTimerCallback(usbInjectEraseAppSector, 500, MENU_ANY, false);
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			break;
		case 0x9D: // DEV: enter DFU by jumping into the bootloader with the PTT + top
			//        button combo spoofed on GPIOE. Changes nothing persistent;
			//        a power cycle undoes it. Deferred so this ACK gets out first.
			addTimerCallback(usbInjectJumpToBootloader, 500, MENU_ANY, false);
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			break;
#endif
#ifdef ENABLE_AES
		case 0x80: // SUB set AES key: [2]=keyId, [3..34]=32-byte key (persistent flash store)
			// dmrAesStoreKey writes flash (SPI_Flash_write -> osDelay), which must NOT run inside
			// the CPS TASK_LOCK_WRITE critical section. Bracket with unlock/lock like the 'X' write.
			TASK_UNLOCK_WRITE();
			dmrAesStoreKey(com_requestbuffer[2], (uint8_t *)&com_requestbuffer[3]);
			TASK_LOCK_WRITE();
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			break;
		case 0x81: // SUB select TX key: [2]=keyId (0 = encrypted TX off) (persistent flash store)
			TASK_UNLOCK_WRITE();
			dmrAesSetTxKeyId(com_requestbuffer[2]);
			TASK_LOCK_WRITE();
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			break;
		case 0x86: // SUB load AES key into RAM only (bench): [2]=keyId, [3..34]=32-byte key
			dmrAesSetKeyRam(com_requestbuffer[2], (uint8_t *)&com_requestbuffer[3]);
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			break;
		case 0x8A: // DIAG: read HX8353E display register [2]=cmd [3]=count [4]=pullUp -> [cmd, count, bytes...]
			{          // display self-test: probes whether the LCD controller responds
				int cnt = com_requestbuffer[3];
				uint8_t regbuf[40];
				if (cnt < 0) { cnt = 0; }
				if (cnt > (int)sizeof(regbuf)) { cnt = sizeof(regbuf); }
				displayReadReg(com_requestbuffer[2], regbuf, cnt, com_requestbuffer[4]);
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = (uint8_t)cnt;
				if (cnt > 0) { memcpy((uint8_t *)&usbComSendBuf[2], regbuf, cnt); }
				hasToReply = true;
				replyLength = 2 + cnt;
				return;   // bypass the trailing generic '-' reply (it overwrites usbComSendBuf)
			}
		case 0x8B: // DIAG: re-run display init to WAKE the panel. [2]=lcdType (0=keep, 2/3=run the
			{      // booster/power-control init path). reply [cmd, lcdType, rddpm_before(2), rddpm_after(2)]
				uint8_t before[2] = {0}, after[2] = {0};
				uint8_t t = com_requestbuffer[2];
				displayReadReg(0x0A, before, 2, 0);               /* RDDPM = power mode */
				if ((t == 2) || (t == 3)) { displayLCD_Type = t; } /* type 2/3 => booster init runs */
				displayInit(false, true);                         // RST, (booster if 2/3), SLPOUT, DISPON
				displayReadReg(0x0A, after, 2, 0);
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = displayLCD_Type;
				usbComSendBuf[2] = before[0]; usbComSendBuf[3] = before[1];
				usbComSendBuf[4] = after[0];  usbComSendBuf[5] = after[1];
				hasToReply = true;
				replyLength = 6;
				return;   // bypass the trailing generic '-' reply
			}
		case 0x92: // DIAG: decrypt data-SMS [2]=keyId [3]=ctLen [4..]=ct -> [cmd, textLen|0xFF, text...]
			{
				int ctlen = com_requestbuffer[3];
				char txt[145];   /* DMR_SMS_TEXT_MAX(144) + NUL (macro not visible in AES-only builds) */
				int tn = dmr_aes_sms_decrypt(com_requestbuffer[2], (uint8_t *)&com_requestbuffer[4], ctlen, txt, sizeof txt);
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = (tn < 0) ? 0xFF : (uint8_t)tn;
				if (tn > 0) { memcpy((uint8_t *)&usbComSendBuf[2], txt, tn); }
				hasToReply = true;
				replyLength = 2 + ((tn > 0) ? tn : 0);
			}
			break;
		case 0x82: // SUB set per-channel AES key: [2..3]=channel index (LE, 1-based), [4]=encrypt
			{          // encrypt: 0=inherit global TX key, 1..15=AES key slot, 0xFF=force clear
				int chIdx = com_requestbuffer[2] | (com_requestbuffer[3] << 8);
				if ((chIdx >= 1) && (chIdx <= CODEPLUG_CHANNELS_MAX))
				{
					CodeplugChannel_t ch;
					// handleCPSRequest() runs inside TASK_LOCK_WRITE() (taskENTER_CRITICAL,
					// interrupts off); the codeplug EEPROM/flash access needs interrupts ON
					// (I2C/SPI timing), so bracket it like every other write in this file.
					// Omitting this corrupts the EEPROM page (a single channel write took out
					// channels 1..8 because EEPROM_Write ran with interrupts disabled).
					TASK_UNLOCK_WRITE();
					codeplugChannelGetDataForIndex(chIdx, &ch);
					ch.encrypt = com_requestbuffer[4];
					codeplugChannelSaveDataForIndex(chIdx, &ch);
					TASK_LOCK_WRITE();
				}
				usbComSendBuf[0] = com_requestbuffer[0];
				hasToReply = true;
				replyLength = 1;
			}
			break;
		case 0x83: // SUB get per-channel AES key: [2..3]=channel index (LE) -> [cmd, encrypt, flags]
			{          // flags bit0 = optional-DMRID flag set (encrypt byte holds the DMR ID, not a key)
				int chIdx = com_requestbuffer[2] | (com_requestbuffer[3] << 8);
				uint8_t enc = 0, fl = 0;
				if ((chIdx >= 1) && (chIdx <= CODEPLUG_CHANNELS_MAX))
				{
					CodeplugChannel_t ch;
					TASK_UNLOCK_WRITE();   // codeplug read needs interrupts ON (see 0x82)
					codeplugChannelGetDataForIndex(chIdx, &ch);
					TASK_LOCK_WRITE();
					enc = ch.encrypt;
					fl = (codeplugChannelGetFlag(&ch, CHANNEL_FLAG_OPTIONAL_DMRID) != 0) ? 0x01 : 0x00;
				}
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = enc;
				usbComSendBuf[2] = fl;
				hasToReply = true;
				replyLength = 3;
				return; // bypass the trailing generic '-' reply (it overwrites usbComSendBuf)
			}
#ifdef DMR_AES_DIAG_RX
		case 0x84: // DIAGNOSTIC: dump the AES-RX event ring -> [cmd, len_hi, len_lo, data...]
			{
				int n = dmrAesGetRxDiag((uint8_t *)&usbComSendBuf[3], COM_BUFFER_SIZE - 3);
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = (n >> 8) & 0xFF;
				usbComSendBuf[2] = n & 0xFF;
				hasToReply = true;
				replyLength = n + 3;
				return; // bypass the trailing generic '-' reply (it overwrites usbComSendBuf)
			}
		case 0x85: // DIAGNOSTIC: clear/arm the AES-RX event ring
			dmrAesResetRxDiag();
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			return;
		case 0x88: // DIAGNOSTIC: arm a one-superframe raw-burst capture
			dmrAesDiagCapArm();
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			return;
		case 0x89: // DIAGNOSTIC: dump the captured raw bursts -> [cmd, len_hi, len_lo, data...]
			{
				int n = dmrAesGetCapData((uint8_t *)&usbComSendBuf[3], COM_BUFFER_SIZE - 3);
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = (n >> 8) & 0xFF;
				usbComSendBuf[2] = n & 0xFF;
				hasToReply = true;
				replyLength = n + 3;
				return;
			}
#endif
#endif
#if defined(ENABLE_DMR_DATA)
		case 0x90: // SUB reboot into the STM32 ROM DFU bootloader (button-free USB flashing)
			dmrDataTriggerReboot();   // deferred ~500 ms so this ACK is sent first
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			break;
		case 0x91: // SUB queue + key a DMR data call: [2]=burst count, [3..]=count x (1 type byte + 12 payload)
			dmrDataTxLoad((const uint8_t *)&com_requestbuffer[3], com_requestbuffer[2]);
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			break;
#if defined(ENABLE_AES)
		case 0x93: // DIAGNOSTIC: dump the SMS RX diag counters -> [cmd, len_hi, len_lo, 7x uint32 LE +
			   //  txActive byte]. Counters: d=all data bursts, hOk,hBad,bOk,bBad,pdu,msg.
			{
				uint32_t d[7];
				int n = 0;
				dmrSmsRxDiag(d);
				for (int i = 0; i < 7; i++)
				{
					usbComSendBuf[3 + n++] = (uint8_t)(d[i]);
					usbComSendBuf[3 + n++] = (uint8_t)(d[i] >> 8);
					usbComSendBuf[3 + n++] = (uint8_t)(d[i] >> 16);
					usbComSendBuf[3 + n++] = (uint8_t)(d[i] >> 24);
				}
				usbComSendBuf[3 + n++] = (uint8_t)(dmrDataTxActive() ? 1 : 0);
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = (uint8_t)((n >> 8) & 0xFF);
				usbComSendBuf[2] = (uint8_t)(n & 0xFF);
				hasToReply = true;
				replyLength = n + 3;
				return; // bypass the trailing generic '-' reply
			}
		case 0x94: // DIAGNOSTIC: zero the SMS RX diag counters (clean test runs)
			dmrSmsRxDiagReset();
			usbComSendBuf[0] = com_requestbuffer[0];
			hasToReply = true;
			replyLength = 1;
			break;
		case 0x95: // DIAGNOSTIC: dump the last reassembled (encrypted) SMS PDU + metadata
			{
				int n = dmrSmsRxLastPdu((uint8_t *)&usbComSendBuf[3], COM_BUFFER_SIZE - 3);
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = (uint8_t)((n >> 8) & 0xFF);
				usbComSendBuf[2] = (uint8_t)(n & 0xFF);
				hasToReply = true;
				replyLength = n + 3;
				return; // bypass the trailing generic '-' reply
			}
		case 0x97: // DIAGNOSTIC: invoke the REAL menu SMS path (dmrSmsSend) from the host, to
			   // A/B bug #3 without button pushing. [2]=flags (bit0: skip the store_add
			   // Sent-folder flash write), [3..6]=dst (LE), [7]=group, [8..]=NUL text
			   // (single USB packet -> text <= ~54 chars). Reply: [cmd, int8 result].
			{
				uint32_t dst = (uint32_t)com_requestbuffer[3] | ((uint32_t)com_requestbuffer[4] << 8) |
				               ((uint32_t)com_requestbuffer[5] << 16) | ((uint32_t)com_requestbuffer[6] << 24);
				com_requestbuffer[COM_REQUESTBUFFER_SIZE - 1] = 0; // bound the strlen in dmrSmsSend
				dmrSmsDiagSetSkipStore(com_requestbuffer[2] & 0x01);
				// dmrSmsSend can flash-write (store_add -> SPI_Flash osDelay) and this handler
				// runs inside TASK_LOCK_WRITE() (interrupts off): bracket like 0x80/0x82.
				TASK_UNLOCK_WRITE();
				int r = dmrSmsSend((const char *)&com_requestbuffer[8], dst,
				                   (com_requestbuffer[7] != 0) ? 1 : 0, 0);
				TASK_LOCK_WRITE();
				dmrSmsDiagSetSkipStore(0);
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = (uint8_t)(int8_t)r;
				hasToReply = true;
				replyLength = 2;
				return; // bypass the trailing generic '-' reply
			}
#endif
#endif
		case 0:
#if defined(HAS_GPS)
			cpsStopGPSNMEA();
#endif

			if (menuSystemGetCurrentMenuNumber() == MENU_SATELLITE)
			{
				menuSystemPopAllAndDisplayRootMenu();
			}

			// Show CPS screen
			menuSystemPushNewMenu(UI_CPS);
			break;
		case 1:
			// Clear CPS screen
			uiCPSUpdate(CPS2UI_COMMAND_CLEARBUF, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
			break;
		case 2:
			// Write a line of text to CPS screen
			uiCPSUpdate(CPS2UI_COMMAND_PRINT, com_requestbuffer[2], com_requestbuffer[3], (ucFont_t)com_requestbuffer[4], (ucTextAlign_t)com_requestbuffer[5], com_requestbuffer[6], (char *)&com_requestbuffer[7]);
			break;
		case 3:
			// Render CPS screen
			uiCPSUpdate(CPS2UI_COMMAND_RENDER_DISPLAY, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
			break;
		case 4:
			// Turn on the display backlight
			uiCPSUpdate(CPS2UI_COMMAND_BACKLIGHT, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
			break;
		case 5:
			// Close
			if (flashingDMRIDs)
			{
				dmrIDCacheInit();
				flashingDMRIDs = false;
			}
			isCompressingAMBE = false;
			rxPowerSavingSetLevel(nonVolatileSettings.ecoLevel);
			uiCPSUpdate(CPS2UI_COMMAND_END, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
			break;
		case 6:
			{
				int subCommand = com_requestbuffer[2];
				uint32_t m = ticksGetMillis();

				// Do some other processing
				switch(subCommand)
				{
					case 0:
						// Channels has be rewritten, switch currentZone to All Channels
						if (channelsRewritten)
						{
							int16_t firstContact = 1;

							//
							// Give it a bit of time before reading the zone count as DM-1801 EEPROM looks slower
							// than GD-77 to write
							m = ticksGetMillis();
							while (true)
							{
								TASK_UNLOCK_WRITE();
								vTaskDelay((10U / portTICK_PERIOD_MS));
								TASK_LOCK_WRITE();

								if ((ticksGetMillis() - m) > 50)
								{
									break;
								}
							}

							TASK_UNLOCK_WRITE();
							codeplugZonesInitCache(); // Re-read zone cache, as we're using codeplugZonesGetCount() below.
							codeplugAllChannelsInitCache(); // Rebuild channels cache
							TASK_LOCK_WRITE();

							nonVolatileSettings.currentZone = (int16_t) (codeplugZonesGetCount() - 1); // Set to All Channels zone

							TASK_UNLOCK_WRITE();
							codeplugInitLastUsedChannelInZone(); // Re-read the LUCZ
							TASK_LOCK_WRITE();

							// Search for the first assigned contact
							int16_t luczAllChannels = codeplugGetLastUsedChannelInZone(-1);
							if (codeplugAllChannelsIndexIsInUse(luczAllChannels))
							{
								firstContact = luczAllChannels;
							}
							else
							{
								for (int16_t i = CODEPLUG_CHANNELS_MIN; i <= CODEPLUG_CHANNELS_MAX; i++)
								{
									if (codeplugAllChannelsIndexIsInUse(i))
									{
										firstContact = i;
										break;
									}

									// Call tick_watchdog() ??
								}
							}
#if defined(PLATFORM_RD5R)
							nonVolatileSettings.currentChannelIndexInAllZone = firstContact;
							nonVolatileSettings.currentChannelIndexInZone = 0;
#endif
							codeplugSetLastUsedChannelInZone(-1, firstContact);

							luczRewritten = false;
						}

#if defined(HAS_GPS)
						// restore GPS state if needed
						if (previousGPSState >= GPS_MODE_ON_NMEA)
						{
							nonVolatileSettings.gpsModeAndBaudsIndex = SETTINGS_GPS_MODE_SET(nonVolatileSettings, previousGPSState);
						}
#endif
						// save current settings and reboot
						m = ticksGetMillis();
						TASK_UNLOCK_WRITE();
						settingsSaveSettings(false);// Need to save these channels prior to reboot, as reboot does not save
						TASK_LOCK_WRITE();

						if (! luczRewritten)
						{
							TASK_UNLOCK_WRITE();
							codeplugSaveLastUsedChannelInZone();
							TASK_LOCK_WRITE();
						}

						// Give it a bit of time before pulling the plug as DM-1801 EEPROM looks slower
						// than GD-77 to write, then quickly power cycling triggers settings reset.
						while (true)
						{
							TASK_UNLOCK_WRITE();
							vTaskDelay((10U / portTICK_PERIOD_MS));
							TASK_LOCK_WRITE();

							if ((ticksGetMillis() - m) > 50)
							{
								break;
							}
						}

						addTimerCallback(NVIC_SystemReset, 500, MENU_ANY, false);
						break;
					case 1:
#if defined(HAS_GPS)
						if (previousGPSState >= GPS_MODE_ON_NMEA)
						{
							nonVolatileSettings.gpsModeAndBaudsIndex = SETTINGS_GPS_MODE_SET(nonVolatileSettings, previousGPSState);
						}
#endif
						addTimerCallback(NVIC_SystemReset, 500, MENU_ANY, false);
						break;
					case 2:
#if defined(HAS_GPS)
						if (previousGPSState >= GPS_MODE_ON_NMEA)
						{
							nonVolatileSettings.gpsModeAndBaudsIndex = SETTINGS_GPS_MODE_SET(nonVolatileSettings, previousGPSState);
						}
#endif
						// Save settings VFO's to codeplug
						TASK_UNLOCK_WRITE();
						settingsSaveSettings(true);
						TASK_LOCK_WRITE();

#if defined(HAS_GPS)
						if (previousGPSState >= GPS_MODE_ON_NMEA)
						{
							nonVolatileSettings.gpsModeAndBaudsIndex = SETTINGS_GPS_MODE_SET(nonVolatileSettings, GPS_MODE_ON);
						}
#endif
						break;
					case 3:
						// flash green LED
						uiCPSUpdate(CPS2UI_COMMAND_GREEN_LED, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
						break;
					case 4:
						// flash red LED
						uiCPSUpdate(CPS2UI_COMMAND_RED_LED, 0, 0, FONT_SIZE_1, TEXT_ALIGN_LEFT, 0, NULL);
						break;
					case 5:
						rxPowerSavingSetLevel(0);
						isCompressingAMBE = true;
						codecInitInternalBuffers();
						break;
					case 6:
						soundInit();// Resets the sound buffers
						memset((uint8_t *)&audioAndHotspotDataBuffer.rawBuffer[0], 0, (6 * WAV_BUFFER_SIZE));// clear 1 dmr frame size of wav buffer memory
						break;
					case 7:
						memcpy(&uiDataGlobal.dateTimeSecs, (uint8_t *)&com_requestbuffer[3], sizeof(uint32_t));// update date with data from the CPS
#if ! defined(PLATFORM_GD77S)
						daytimeThemeChangeUpdate(true);
#endif
#if defined(PLATFORM_MD9600) || defined(PLATFORM_MDUV380) || defined(PLATFORM_MD380) || defined(PLATFORM_RT84_DM1701) || defined(PLATFORM_MD2017)
						setRtc_custom(uiDataGlobal.dateTimeSecs);
#endif
						menuSatelliteScreenClearPredictions(true);
						break;
					// 8:
					// 9:
					case 10: // wait 10ms
						m = ticksGetMillis();
						while (true)
						{
							TASK_UNLOCK_WRITE();
							vTaskDelay((5U / portTICK_PERIOD_MS));
							TASK_LOCK_WRITE();

							if ((ticksGetMillis() - m) > 10)
							{
								break;
							}
						}
						break;
					default:
						break;
				}
			}
			break;
		case 7:
#if defined(HAS_GPS)
			if (previousGPSState >= GPS_MODE_ON_NMEA)
			{
				nonVolatileSettings.gpsModeAndBaudsIndex = SETTINGS_GPS_MODE_SET(nonVolatileSettings, previousGPSState);
				previousGPSState = GPS_NOT_DETECTED;
#if defined(LOG_GPS_DATA)
				TASK_UNLOCK_WRITE();
				gpsLoggingStart();
				TASK_LOCK_WRITE();
#endif
			}
#endif
			break;
#if defined(DEBUG_POSITION)
#warning DEBUG_POSITION is ENABLED
		case 'P':
			{
				int lat,lon;
				com_requestbuffer[COM_REQUESTBUFFER_SIZE-1] = 0;// terminate string ready for sscanf
				// Command to set Lat/Lon to 0.0001 deg
				// In the format CP NNNNNN NNNNNN
				// e.g. CP -234567 12345678 = 23.4567S 123.4567E

				sscanf((char *)&com_requestbuffer[2],"%d %d",&lat,&lon);
				settingsSet(nonVolatileSettings.location.lat, latLonDoubleToFixed32(((double)lat) / 10000.0F));
				settingsSet(nonVolatileSettings.location.Lon, latLonDoubleToFixed32(((double)lon) / 10000.0F));
			}
			break;
#endif
		case 254: // PING
#if defined(HAS_GPS)
			cpsStopGPSNMEA();
#endif
			break;
		default:
			break;
	}
	// Send something generic back.
	// Probably need to send a response code in the future
	usbComSendBuf[0] = '-';
	hasToReply = true;
	replyLength = 1;
}

static void handleCPSRequest(void)
{
	//Handle read
	switch(com_requestbuffer[0])
	{
		case 'R':
			cpsHandleReadCommand();
			break;
		case 'X'://W
			cpsHandleWriteCommand();
			break;
		case 'W': // Fake write
			{
				usbComSendBuf[0] = com_requestbuffer[0];
				usbComSendBuf[1] = com_requestbuffer[1];
				hasToReply = true;
				replyLength = 2;
			}
			break;
		case 'C':
			cpsHandleCommand();
			break;
#ifdef USB_DEBUG_COMMANDS
		case 'D':
			cpsHandleDebugCommand();
			break;
#endif

#if 0 // has to be rewritten
		case '$':// NMEA
			{
				gpsPower(false);

				nonVolatileSettings.gpsModeAndBaudsIndex = SETTINGS_GPS_MODE_SET(nonVolatileSettings, GPS_MODE_ON);
				//HAL_GPIO_TogglePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin);

				int bufPos = 0;
				char rxchar;
				do
				{
					rxchar = com_requestbuffer[bufPos];
					if (rxchar != 13)
					{
						gpsLine[bufPos] = rxchar;
					}
					bufPos++;
				} while((rxchar != 13) && (bufPos < COM_REQUESTBUFFER_SIZE)) ;

				gpsLineLength = bufPos;
				gpsLineReady = true;
			}
			break;
#endif
		default:
			usbComSendBuf[0] = '-';
			hasToReply = true;
			replyLength = 1;
			break;
	}
}
#if 0
__attribute__((section(".ccmram"))) volatile uint8_t com_buffer[COM_BUFFER_SIZE];
int com_buffer_write_idx = 0;
int com_buffer_read_idx = 0;
volatile int com_buffer_cnt = 0;

void send_packet(uint8_t val_0x82, uint8_t val_0x86, int ram)
{
	taskENTER_CRITICAL();
	if ((HR_C6000_datalogging) && ((com_buffer_cnt+8+(ram+1))<=COM_BUFFER_SIZE))
	{
		add_to_commbuffer((com_buffer_cnt >> 8) & 0xff);
		add_to_commbuffer((com_buffer_cnt >> 0) & 0xff);
		add_to_commbuffer(val_0x82);
		add_to_commbuffer(val_0x86);
		add_to_commbuffer(tmp_val_0x51);
		add_to_commbuffer(tmp_val_0x52);
		add_to_commbuffer(tmp_val_0x57);
		add_to_commbuffer(tmp_val_0x5f);
		for (int i=0;i<=ram;i++)
		{
			add_to_commbuffer(DMR_frame_buffer[i]);
		}
	}
	taskEXIT_CRITICAL();
}

uint8_t tmp_ram1[256];
uint8_t tmp_ram2[256];

void send_packet_big(uint8_t val_0x82, uint8_t val_0x86, int ram1, int ram2)
{
	taskENTER_CRITICAL();
	if ((HR_C6000_datalogging) && ((com_buffer_cnt+8+(ram1+1)+(ram2+1))<=COM_BUFFER_SIZE))
	{
		add_to_commbuffer((com_buffer_cnt >> 8) & 0xff);
		add_to_commbuffer((com_buffer_cnt >> 0) & 0xff);
		add_to_commbuffer(val_0x82);
		add_to_commbuffer(val_0x86);
		add_to_commbuffer(tmp_val_0x51);
		add_to_commbuffer(tmp_val_0x52);
		add_to_commbuffer(tmp_val_0x57);
		add_to_commbuffer(tmp_val_0x5f);
		for (int i=0;i<=ram1;i++)
		{
			add_to_commbuffer(tmp_ram1[i]);
		}
		for (int i=0;i<=ram2;i++)
		{
			add_to_commbuffer(tmp_ram2[i]);
		}
	}
	taskEXIT_CRITICAL();
}

void add_to_commbuffer(uint8_t value)
{
	com_buffer[com_buffer_write_idx]=value;
	com_buffer_cnt++;
	com_buffer_write_idx++;
	if (com_buffer_write_idx==COM_BUFFER_SIZE)
	{
		com_buffer_write_idx=0;
	}
}
#endif

void USB_DEBUG_PRINT(const char *str)
{
	strncpy((char *)usbComSendBuf, str, COM_BUFFER_SIZE);
	usbComSendBuf[COM_BUFFER_SIZE - 1] = 0; // SAFETY: strncpy won't NULL terminate the buffer if length is exceeding.

	CDC_Transmit_FS((uint8_t *)usbComSendBuf, strlen((char *)usbComSendBuf));
}

void USB_DEBUG_printf(const char *format, ...)
{
	char buf[COM_BUFFER_SIZE];
	va_list params;

	va_start(params, format);
	vsnprintf(buf, COM_BUFFER_SIZE, format, params);
	va_end(params);
	USB_DEBUG_PRINT(buf);
}


extern USBD_HandleTypeDef hUsbDeviceFS;

bool USB_DeviceIsResetting(void)
{
	if ((hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) && (clockManagerGetRunMode() != CLOCK_MANAGER_SPEED_RUN))
	{
		clockManagerSetRunMode(kAPP_PowerModeRun, CLOCK_MANAGER_SPEED_RUN);
	}

	return usbIsResetting;
}

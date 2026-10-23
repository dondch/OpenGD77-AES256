/*
 * menuAESKeys.c — on-radio management of the DMRA AES-256 key store.
 *
 * Lets the user enter/replace/clear the AES key slots and pick the global TX key,
 * all without a PC. (Per-channel privacy is set in the Channel Details menu.)
 * Keys are stored in the same OpenGD77 custom-data "AESK" block
 * (SPI-flash 0x20000) the host aes_key_store.py / the CHIRP module use, so the
 * format is shared (see crypto/dmr_aes_hook.c). Key material is never displayed.
 *
 * This whole file compiles to nothing unless built with -DENABLE_AES, so stock
 * builds are byte-identical. Persistent menu state lives in CCM (.aes_ccmram) — not
 * required (the AMBE codec addresses its buffers via the pointer passed in r1, so
 * its RAM placement floats freely), just tidy: it keeps this state alongside the
 * other AES state and out of scarce main RAM. CCM is not zeroed at boot, so all of
 * it is initialised on isFirstRun before use.
 *
 * LEGAL: encrypted voice is illegal on amateur bands in most jurisdictions.
 */

#include "user_interface/uiGlobals.h"
#include "user_interface/menuSystem.h"
#include "user_interface/uiLocalisation.h"
#include "user_interface/uiUtilities.h"
#include "functions/codeplug.h"
#include "functions/settings.h"
#include "crypto/dmr_aes.h"
#include "crypto/dmr_aes_hook.h"
#include <string.h>

#if defined(ENABLE_AES)

// List layout: 15 key-slot rows (keyId 1..15) then the global TX-key row.
#define AES_NUM_KEYS        15
enum
{
	AES_ITEM_GLOBAL_TX = AES_NUM_KEYS, // 15
	AES_NUM_ITEMS                      // 16
};

// Hex editor grid (160x128 colour screen). Pixel coords; tweak on hardware if needed.
#define GRID_COLS    16
#define GRID_ROWS    4
#define GRID_X0      16
#define GRID_Y0      26
#define GRID_CHARW   8     // FONT_SIZE_2 = font_8x8
#define GRID_ROWH    14

// Persistent across tick calls. In CCM (NOT auto-zeroed at boot) -> fully
// initialised on isFirstRun before anything reads it.
static struct
{
	uint16_t keyMask;     // bit k set => keyId k (1..15) has a stored key
	uint8_t  txKeyId;     // working copy of the global TX selector (0 = off)
	bool     dirtyTx;     // global TX selector changed (persist on exit)
	bool     editing;     // hex editor active
	uint8_t  editKeyId;   // key id being edited (1..15)
	int16_t  editPos;     // cursor nibble 0..63
	uint8_t  nib[64];     // editor nibble buffer (cleared after save/cancel)
} s_aes DMR_AES_CCM;
// NB: keep ALL persistent menu state in CCM (above). Any net-new main-RAM .bss
// shifts the AMBE codec's hardcoded ambebuffer_* addresses and breaks DMR voice,
// so the per-tick exit code is a LOCAL threaded through handleEvent, not a static.

static void updateScreen(void);
static void handleEvent(uiEvent_t *ev, menuStatus_t *exitCode);
static void editorUpdateScreen(void);
static void editorHandleEvent(uiEvent_t *ev);

static char hexChar(uint8_t nibble)
{
	return (nibble < 10) ? (char)('0' + nibble) : (char)('A' + nibble - 10);
}

static void applyChanges(void)
{
	if (s_aes.dirtyTx)
	{
		dmrAesEnsureCustomDataRegion();
		dmrAesSetTxKeyId(s_aes.txKeyId);
		s_aes.dirtyTx = false;
	}
}

menuStatus_t menuAESKeys(uiEvent_t *ev, bool isFirstRun)
{
	if (isFirstRun)
	{
		s_aes.editing  = false;
		s_aes.dirtyTx  = false;
		s_aes.editPos  = 0;
		s_aes.editKeyId = 0;
		memset(s_aes.nib, 0, sizeof s_aes.nib);

		s_aes.keyMask  = dmrAesGetKeyMask();
		s_aes.txKeyId  = dmrAesTxKeyId();

		menuDataGlobal.currentItemIndex = 0;
		menuDataGlobal.numItems = AES_NUM_ITEMS;

		updateScreen();
		return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
	}

	menuStatus_t exitCode = MENU_STATUS_SUCCESS;

	if (ev->hasEvent)
	{
		if (s_aes.editing)
		{
			editorHandleEvent(ev);
		}
		else
		{
			handleEvent(ev, &exitCode);
		}
	}
	return exitCode;
}

static void updateScreen(void)
{
	char buf[16 + LANGUAGE_TEXTS_LENGTH];  // "Global TX key:" prefix + max language value (off); avoids -Wformat-truncation
	char vb[12];

	displayClearBuf();
	menuDisplayTitle("AES Keys");

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(AES_NUM_ITEMS, i);
		const char *value;

		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY)
		{
			continue;
		}
		else if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)
		{
			break;
		}

		if (mNum < AES_NUM_KEYS)
		{
			int keyId = mNum + 1;
			value = (s_aes.keyMask & (1u << keyId)) ? "set" : "---";
			snprintf(buf, sizeof buf, "Key %d:%s", keyId, value);
		}
		else if (mNum == AES_ITEM_GLOBAL_TX)
		{
			if (s_aes.txKeyId == 0)
			{
				value = currentLanguage->off;
			}
			else
			{
				snprintf(vb, sizeof vb, "%d", s_aes.txKeyId);
				value = vb;
			}
			snprintf(buf, sizeof buf, "Global TX key:%s", value);
		}

		// Colour the value after the ':' like the other option menus.
		const char *colon = strchr(buf, ':');
		int optStart = colon ? (int)(colon - buf + 1) : 0;
		menuDisplayEntry(i, mNum, buf, optStart, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}

	displayRender();
}

static void changeSetting(int idx, int dir)
{
	if (idx == AES_ITEM_GLOBAL_TX)
	{
		int v = (int)s_aes.txKeyId + dir;
		if (v < 0)  { v = 0; }
		if (v > 15) { v = 15; }
		if ((uint8_t)v != s_aes.txKeyId)
		{
			s_aes.txKeyId = (uint8_t)v;
			s_aes.dirtyTx = true;
		}
	}
}

static void handleEvent(uiEvent_t *ev, menuStatus_t *exitCode)
{
	if ((ev->events & KEY_EVENT) == 0)
	{
		return;
	}

	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, AES_NUM_ITEMS);
		*exitCode |= MENU_STATUS_LIST_TYPE;
		updateScreen();
		return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, AES_NUM_ITEMS);
		*exitCode |= MENU_STATUS_LIST_TYPE;
		updateScreen();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		applyChanges();
		menuSystemPopPreviousMenu();
		return;
	}

	int idx = menuDataGlobal.currentItemIndex;

	if (idx < AES_NUM_KEYS)
	{
		int keyId = idx + 1;

		// SK2 + GREEN clears the slot (deliberate combo guards against accidental wipe).
		if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN) && BUTTONCHECK_DOWN(ev, BUTTON_SK2))
		{
			dmrAesEnsureCustomDataRegion();
			dmrAesClearKey((uint8_t)keyId);
			s_aes.keyMask = dmrAesGetKeyMask();
			updateScreen();
			return;
		}
		// GREEN opens the hex editor to enter / replace the key.
		if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
		{
			s_aes.editing  = true;
			s_aes.editKeyId = (uint8_t)keyId;
			s_aes.editPos  = 0;
			memset(s_aes.nib, 0, sizeof s_aes.nib);
			editorUpdateScreen();
			return;
		}
	}
	else
	{
		if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
		{
			applyChanges();
			menuSystemPopPreviousMenu();
			return;
		}
		if (KEYCHECK_PRESS(ev->keys, KEY_RIGHT))
		{
			changeSetting(idx, +1);
			updateScreen();
			return;
		}
		if (KEYCHECK_PRESS(ev->keys, KEY_LEFT))
		{
			changeSetting(idx, -1);
			updateScreen();
			return;
		}
	}
}

static void editorUpdateScreen(void)
{
	char line[GRID_COLS + 1];
	char title[20];

	displayClearBuf();
	snprintf(title, sizeof title, "Enter Key %d", s_aes.editKeyId);
	displayPrintCentered(2, title, FONT_SIZE_2);
	displayPrintCentered(14, "as in TYT CPS", FONT_SIZE_1);

	for (int r = 0; r < GRID_ROWS; r++)
	{
		for (int c = 0; c < GRID_COLS; c++)
		{
			line[c] = hexChar(s_aes.nib[r * GRID_COLS + c]);
		}
		line[GRID_COLS] = 0;
		displayPrintAt(GRID_X0, GRID_Y0 + r * GRID_ROWH, line, FONT_SIZE_2);
	}

	// Cursor box around the focused nibble.
	int cr = s_aes.editPos / GRID_COLS;
	int cc = s_aes.editPos % GRID_COLS;
	displayDrawRect(GRID_X0 + cc * GRID_CHARW - 1, GRID_Y0 + cr * GRID_ROWH - 2, GRID_CHARW + 1, FONT_SIZE_2_HEIGHT + 3, false);

	displayPrintCentered(88, "U/D:0-F  L/R:move", FONT_SIZE_1);
	displayPrintCentered(100, "0-9 type  GRN:save", FONT_SIZE_1);
	displayPrintCentered(112, "RED:cancel", FONT_SIZE_1);

	displayRender();
}

static void editorSave(void)
{
	uint8_t key[DMR_AES_KEY_BYTES];

	for (int i = 0; i < DMR_AES_KEY_BYTES; i++)
	{
		key[i] = (uint8_t)((s_aes.nib[2 * i] << 4) | s_aes.nib[2 * i + 1]);
	}

	dmrAesEnsureCustomDataRegion();
	dmrAesStoreKey(s_aes.editKeyId, key);   // also reloads the key store -> immediate effect, no reboot

	memset(key, 0, sizeof key);
	memset(s_aes.nib, 0, sizeof s_aes.nib);

	s_aes.keyMask = dmrAesGetKeyMask();
	s_aes.editing = false;
	updateScreen();
}

static void editorHandleEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0)
	{
		return;
	}

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		memset(s_aes.nib, 0, sizeof s_aes.nib);
		s_aes.editing = false;
		updateScreen();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		editorSave();
		return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_LEFT))
	{
		if (s_aes.editPos > 0) { s_aes.editPos--; }
		editorUpdateScreen();
		return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_RIGHT))
	{
		if (s_aes.editPos < 63) { s_aes.editPos++; }
		editorUpdateScreen();
		return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		s_aes.nib[s_aes.editPos] = (uint8_t)((s_aes.nib[s_aes.editPos] + 1) & 0x0F);
		editorUpdateScreen();
		return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		s_aes.nib[s_aes.editPos] = (uint8_t)((s_aes.nib[s_aes.editPos] + 15) & 0x0F);
		editorUpdateScreen();
		return;
	}
	if (KEYCHECK_SHORTUP_NUMBER(ev->keys))
	{
		s_aes.nib[s_aes.editPos] = (uint8_t)(ev->keys.key - '0');
		if (s_aes.editPos < 63) { s_aes.editPos++; }
		editorUpdateScreen();
		return;
	}
}

#endif // ENABLE_AES

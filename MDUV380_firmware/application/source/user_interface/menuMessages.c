/*
 * menuMessages.c — on-radio encrypted DMR SMS UX (Inbox / Sent / New Message).
 *
 * Modelled on menuAESKeys.c. A small screen state-machine:
 *   HOME      : Inbox (n) / Sent (n) / New Message
 *   LIST      : the messages of a folder + a "[Delete all]" row
 *   READ      : full text of one message (SK2+GREEN deletes it)
 *   COMPOSE   : keypad text entry (multi-tap, like the contact-name editor)
 *   RECIPIENT : destination DMR ID / talkgroup + Group/Private + send
 *
 * Sending and the store live in functions/dmr_sms.c (the on-air-validated AES-256-ECB
 * scheme a stock TYT decrypts). Compiles to nothing unless -DENABLE_AES -DENABLE_DMR_DATA.
 *
 * LEGAL: encrypted traffic is illegal on amateur bands in most jurisdictions. PMR/commercial.
 */
#include "user_interface/uiGlobals.h"
#include "user_interface/menuSystem.h"
#include "user_interface/uiLocalisation.h"
#include "user_interface/uiUtilities.h"
#include "functions/trx.h"
#include "functions/dmr_sms.h"
#include "crypto/dmr_aes.h"
#include "io/keyboard.h"
#include <string.h>
#include <stdlib.h>

#if defined(ENABLE_AES) && defined(ENABLE_DMR_DATA)

enum { MSG_HOME = 0, MSG_LIST, MSG_READ, MSG_COMPOSE, MSG_RECIPIENT, MSG_RESULT };

static struct
{
	uint8_t  view;
	uint8_t  folder;        // 0 = Inbox, 1 = Sent
	int16_t  readIdx;       // message being read
	char     compose[DMR_SMS_TEXT_MAX + 1];
	int16_t  composePos;
	char     rcpt[10];      // numeric recipient entry
	int16_t  rcptPos;
	uint8_t  rcptGroup;     // 1 = talkgroup, 0 = private call
	int8_t   presetIdx;     // last quick-text preset cycled in (-1 = none)
	int8_t   result;        // dmrSmsSend() return for the result screen
	uint16_t resultTicks;
} s_msg DMR_AES_CCM;

static void homeUpdate(void);
static void listUpdate(void);
static void readUpdate(void);
static void composeUpdate(void);
static void recipientUpdate(void);
static void resultUpdate(void);
static void homeEvent(uiEvent_t *ev, menuStatus_t *ec);
static void listEvent(uiEvent_t *ev);
static void readEvent(uiEvent_t *ev);
static void composeEvent(uiEvent_t *ev);
static void recipientEvent(uiEvent_t *ev);

static void gotoHome(void)
{
	keypadAlphaEnable = false;
	s_msg.view = MSG_HOME;
	menuDataGlobal.currentItemIndex = 0;
	menuDataGlobal.numItems = 3;
	homeUpdate();
}

menuStatus_t menuMessages(uiEvent_t *ev, bool isFirstRun)
{
	if (isFirstRun)
	{
		memset(&s_msg, 0, sizeof s_msg);
		dmrSmsInit();
		gotoHome();
		return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
	}

	menuStatus_t exitCode = MENU_STATUS_SUCCESS;

	if (s_msg.view == MSG_RESULT)
	{
		if (s_msg.resultTicks) { s_msg.resultTicks--; }
		if ((s_msg.resultTicks == 0) || (ev->hasEvent && (ev->events & KEY_EVENT)))
		{
			gotoHome();
			return (MENU_STATUS_LIST_TYPE | MENU_STATUS_SUCCESS);
		}
		return exitCode;
	}

	if (ev->hasEvent)
	{
		switch (s_msg.view)
		{
			case MSG_HOME:      homeEvent(ev, &exitCode); break;
			case MSG_LIST:      listEvent(ev);            break;
			case MSG_READ:      readEvent(ev);            break;
			case MSG_COMPOSE:   composeEvent(ev);         break;
			case MSG_RECIPIENT: recipientEvent(ev);       break;
		}
	}
	return exitCode;
}

/* ============================ HOME ===================================== */
static void homeUpdate(void)
{
	char buf[24];
	displayClearBuf();
	menuDisplayTitle("Messages");

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(3, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY) { continue; }
		if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)   { break; }

		switch (mNum)
		{
			case 0:  snprintf(buf, sizeof buf, "Inbox (%d)", dmrSmsCount(0)); break;
			case 1:  snprintf(buf, sizeof buf, "Sent (%d)",  dmrSmsCount(1)); break;
			default: snprintf(buf, sizeof buf, "New Message");                break;
		}
		menuDisplayEntry(i, mNum, buf, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}

	// RX diagnostic (bench bring-up). d=all data-sync bursts the chip delivered;
	// h/b = data-header / rate-1/2 with CRC ok!bad; p=PDUs; m=decoded messages.
	{
		uint32_t d[7];
		char l1[26], l2[26];
		dmrSmsRxDiag(d);
		snprintf(l1, sizeof l1, "d%lu h%lu!%lu b%lu!%lu",
				(unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2],
				(unsigned long)d[3], (unsigned long)d[4]);
		snprintf(l2, sizeof l2, "p%lu m%lu", (unsigned long)d[5], (unsigned long)d[6]);
		displayPrintCentered(108, l1, FONT_SIZE_1);
		displayPrintCentered(118, l2, FONT_SIZE_1);
	}

	displayRender();
}

static void openFolder(int folder)
{
	s_msg.folder = (uint8_t)folder;
	s_msg.view = MSG_LIST;
	menuDataGlobal.currentItemIndex = 0;
	menuDataGlobal.numItems = dmrSmsCount(folder) + 1;   // +1 for "[Delete all]"
	listUpdate();
}

static void startCompose(void)
{
	memset(s_msg.compose, 0, sizeof s_msg.compose);
	s_msg.composePos = 0;
	s_msg.presetIdx = -1;
	s_msg.view = MSG_COMPOSE;
	keypadAlphaEnable = true;
	composeUpdate();
}

/* Cycle the next non-empty quick-text preset into the compose buffer. */
static void cyclePreset(void)
{
	int total = dmrSmsPresetCount();
	if (total == 0) { return; }
	for (int tries = 0; tries < DMR_SMS_NUM_PRESETS; tries++)
	{
		s_msg.presetIdx = (int8_t)((s_msg.presetIdx + 1) % DMR_SMS_NUM_PRESETS);
		const char *p = dmrSmsPresetGet(s_msg.presetIdx);
		if (p != NULL)
		{
			strncpy(s_msg.compose, p, DMR_SMS_TEXT_MAX);
			s_msg.compose[DMR_SMS_TEXT_MAX] = 0;
			s_msg.composePos = (int16_t)strlen(s_msg.compose);
			if (s_msg.composePos > DMR_SMS_TEXT_MAX - 1) { s_msg.composePos = DMR_SMS_TEXT_MAX - 1; }
			composeUpdate();
			return;
		}
	}
}

static void homeEvent(uiEvent_t *ev, menuStatus_t *ec)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, 3);
		*ec |= MENU_STATUS_LIST_TYPE; homeUpdate(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, 3);
		*ec |= MENU_STATUS_LIST_TYPE; homeUpdate(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		menuSystemPopPreviousMenu(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		switch (menuDataGlobal.currentItemIndex)
		{
			case 0: openFolder(0);  break;
			case 1: openFolder(1);  break;
			case 2: startCompose(); break;
		}
		return;
	}
}

/* ============================ LIST ===================================== */
static void listUpdate(void)
{
	char buf[DMR_SMS_TEXT_MAX + 24];
	int count = dmrSmsCount(s_msg.folder);
	displayClearBuf();
	menuDisplayTitle(s_msg.folder ? "Sent" : "Inbox");

	if (count == 0)
	{
		displayPrintCentered(56, "(empty)", FONT_SIZE_3);
		displayPrintCentered(112, "RED:back", FONT_SIZE_1);
		displayRender();
		return;
	}

	for (int i = MENU_START_ITERATION_VALUE; i <= MENU_END_ITERATION_VALUE; i++)
	{
		int mNum = menuGetMenuOffset(count + 1, i);
		if (mNum == MENU_OFFSET_BEFORE_FIRST_ENTRY) { continue; }
		if (mNum == MENU_OFFSET_AFTER_LAST_ENTRY)   { break; }

		if (mNum == count)
		{
			snprintf(buf, sizeof buf, "[Delete all]");
		}
		else
		{
			const dmrSmsMessage_t *m = dmrSmsGet(s_msg.folder, mNum);
			if (m == NULL) { continue; }
			char txt[DMR_SMS_TEXT_MAX + 1];
			int n = (m->textLen < (int)sizeof txt - 1) ? m->textLen : (int)sizeof txt - 1;
			memcpy(txt, m->text, n); txt[n] = 0;
			char mark = (m->flags & DMR_SMS_FLAG_UNREAD) ? '*' : ' ';
			snprintf(buf, sizeof buf, "%c%lu:%s", mark, (unsigned long)m->peerId, txt);
		}
		menuDisplayEntry(i, mNum, buf, 0, THEME_ITEM_FG_MENU_ITEM, THEME_ITEM_FG_OPTIONS_VALUE, THEME_ITEM_BG);
	}
	displayRender();
}

static void listEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }
	int count = dmrSmsCount(s_msg.folder);

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED)) { gotoHome(); return; }

	if (count == 0) { return; }

	if (KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		menuSystemMenuIncrement(&menuDataGlobal.currentItemIndex, count + 1); listUpdate(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))
	{
		menuSystemMenuDecrement(&menuDataGlobal.currentItemIndex, count + 1); listUpdate(); return;
	}

	int idx = menuDataGlobal.currentItemIndex;

	// SK2+GREEN deletes the selected message (deliberate combo).
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN) && BUTTONCHECK_DOWN(ev, BUTTON_SK2))
	{
		if (idx == count) { dmrSmsDeleteAll(s_msg.folder); }   // delete-all row + SK2
		else              { dmrSmsDelete(s_msg.folder, idx); }
		int newCount = dmrSmsCount(s_msg.folder);
		menuDataGlobal.numItems = newCount + 1;
		if (menuDataGlobal.currentItemIndex >= menuDataGlobal.numItems)
		{
			menuDataGlobal.currentItemIndex = menuDataGlobal.numItems - 1;
		}
		listUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		if (idx == count) { return; }   // need SK2 to confirm delete-all
		s_msg.readIdx = (int16_t)idx;
		dmrSmsMarkRead(s_msg.folder, idx);
		s_msg.view = MSG_READ;
		readUpdate();
		return;
	}
}

/* ============================ READ ===================================== */
static void readUpdate(void)
{
	const dmrSmsMessage_t *m = dmrSmsGet(s_msg.folder, s_msg.readIdx);
	displayClearBuf();
	menuDisplayTitle(s_msg.folder ? "Sent" : "Inbox");

	if (m == NULL) { displayRender(); return; }

	char hdr[24];
	snprintf(hdr, sizeof hdr, "%s %lu", (m->flags & DMR_SMS_FLAG_OUTGOING) ? "To" : "From",
			(unsigned long)m->peerId);
	displayPrintAt(2, 16, hdr, FONT_SIZE_1);

	// word-free char wrap into ~21-char lines
	char txt[DMR_SMS_TEXT_MAX + 1];
	int n = (m->textLen < (int)sizeof txt - 1) ? m->textLen : (int)sizeof txt - 1;
	memcpy(txt, m->text, n); txt[n] = 0;

	const int perLine = 21;
	char line[perLine + 1];
	int y = 30;
	for (int i = 0; i < n && y < 104; i += perLine, y += 12)
	{
		int len = ((n - i) < perLine) ? (n - i) : perLine;
		memcpy(line, txt + i, len); line[len] = 0;
		displayPrintAt(2, y, line, FONT_SIZE_2);
	}

	displayPrintCentered(112, "SK2+GRN:del RED:back", FONT_SIZE_1);
	displayRender();
}

static void readEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN) && BUTTONCHECK_DOWN(ev, BUTTON_SK2))
	{
		dmrSmsDelete(s_msg.folder, s_msg.readIdx);
		openFolder(s_msg.folder);
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED) || KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		openFolder(s_msg.folder);
		return;
	}
}

/* ============================ COMPOSE ================================== */
static void composeUpdate(void)
{
	displayClearBuf();
	menuDisplayTitle("New Message");

	// show the text wrapped
	const int perLine = 21;
	int n = (int)strlen(s_msg.compose);
	char line[perLine + 1];
	int y = 22;
	if (n == 0)
	{
		displayPrintAt(2, y, "_", FONT_SIZE_2);
	}
	for (int i = 0; i < n && y < 92; i += perLine, y += 14)
	{
		int len = ((n - i) < perLine) ? (n - i) : perLine;
		memcpy(line, s_msg.compose + i, len); line[len] = 0;
		displayPrintAt(2, y, line, FONT_SIZE_2);
	}

	char cnt[20];
	snprintf(cnt, sizeof cnt, "%d/%d chars", n, DMR_SMS_TEXT_MAX);
	displayPrintCentered(96, cnt, FONT_SIZE_1);
	if (dmrSmsPresetCount() > 0)
	{
		displayPrintCentered(104, "U:preset  L:del", FONT_SIZE_1);
	}
	displayPrintCentered(116, "GRN:to  RED:cancel", FONT_SIZE_1);
	displayRender();
}

static void prefillRecipient(void)
{
	uint32_t defDst = 0;
	int defGroup = 1;
	dmrSmsDefaultRecipient(&defDst, &defGroup);

	uint32_t id;
	if (defDst != 0)
	{
		id = defDst;                 // CHIRP-configured default recipient
		s_msg.rcptGroup = defGroup ? 1 : 0;
	}
	else
	{
		id = trxTalkGroupOrPcId & 0x00FFFFFF;   // fall back to the current channel
		s_msg.rcptGroup = ((trxTalkGroupOrPcId >> 24) == PC_CALL_FLAG) ? 0 : 1;
	}
	snprintf(s_msg.rcpt, sizeof s_msg.rcpt, "%lu", (unsigned long)id);
	s_msg.rcptPos = (int16_t)strlen(s_msg.rcpt);
}

static void composeEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		gotoHome(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		if (strlen(s_msg.compose) == 0) { return; }
		keypadAlphaEnable = false;
		prefillRecipient();
		s_msg.view = MSG_RECIPIENT;
		recipientUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_LEFT))
	{
		int n = (int)strlen(s_msg.compose);
		if (n > 0) { s_msg.compose[n - 1] = 0; if (s_msg.composePos > 0) s_msg.composePos--; }
		composeUpdate();
		return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP))   // insert a CHIRP-configured quick-text preset
	{
		cyclePreset();
		return;
	}
	// multi-tap alpha entry (keypadAlphaEnable handles cycling; same idiom as contact name)
	if (s_msg.composePos < DMR_SMS_TEXT_MAX)
	{
		if (ev->keys.event == KEY_MOD_PREVIEW)
		{
			s_msg.compose[s_msg.composePos] = ev->keys.key;
			composeUpdate();
			return;
		}
		if (ev->keys.event == KEY_MOD_PRESS)
		{
			s_msg.compose[s_msg.composePos] = ev->keys.key;
			if (s_msg.composePos < (int)strlen(s_msg.compose) && s_msg.composePos < DMR_SMS_TEXT_MAX - 1)
			{
				s_msg.composePos++;
			}
			composeUpdate();
			return;
		}
	}
}

/* ============================ RECIPIENT =============================== */
static void recipientUpdate(void)
{
	char buf[24];
	displayClearBuf();
	menuDisplayTitle("Recipient");

	snprintf(buf, sizeof buf, "To: %s", s_msg.rcpt);
	displayPrintAt(2, 28, buf, FONT_SIZE_3);
	snprintf(buf, sizeof buf, "Type: %s", s_msg.rcptGroup ? "Group" : "Private");
	displayPrintAt(2, 52, buf, FONT_SIZE_2);

	displayPrintCentered(84,  "0-9:id  L:del", FONT_SIZE_1);
	displayPrintCentered(96,  "U/D:Group/Private", FONT_SIZE_1);
	displayPrintCentered(112, "GRN:send  RED:back", FONT_SIZE_1);
	displayRender();
}

static void doSend(void)
{
	uint32_t dst = (uint32_t)strtoul(s_msg.rcpt, NULL, 10);
	if (dst == 0) { return; }
	s_msg.result = (int8_t)dmrSmsSend(s_msg.compose, dst, s_msg.rcptGroup ? 1 : 0, 0);
	s_msg.view = MSG_RESULT;
	s_msg.resultTicks = 600;    // persist (~30 s) so the result/code is readable; any key dismisses
	resultUpdate();
}

static void recipientEvent(uiEvent_t *ev)
{
	if ((ev->events & KEY_EVENT) == 0) { return; }

	if (KEYCHECK_SHORTUP(ev->keys, KEY_RED))
	{
		s_msg.view = MSG_COMPOSE;
		keypadAlphaEnable = true;
		composeUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_GREEN))
	{
		doSend(); return;
	}
	if (KEYCHECK_PRESS(ev->keys, KEY_UP) || KEYCHECK_PRESS(ev->keys, KEY_DOWN))
	{
		s_msg.rcptGroup = s_msg.rcptGroup ? 0 : 1; recipientUpdate(); return;
	}
	if (KEYCHECK_SHORTUP(ev->keys, KEY_LEFT))
	{
		int n = (int)strlen(s_msg.rcpt);
		if (n > 0) { s_msg.rcpt[n - 1] = 0; }
		recipientUpdate();
		return;
	}
	if (KEYCHECK_SHORTUP_NUMBER(ev->keys))
	{
		int n = (int)strlen(s_msg.rcpt);
		if (n < (int)sizeof s_msg.rcpt - 1)
		{
			s_msg.rcpt[n] = (char)ev->keys.key;
			s_msg.rcpt[n + 1] = 0;
		}
		recipientUpdate();
		return;
	}
}

/* ============================ RESULT ================================== */
static void resultUpdate(void)
{
	displayClearBuf();
	menuDisplayTitle("Messages");
	if (s_msg.result == 0)
	{
		displayPrintCentered(44, "Message sent", FONT_SIZE_3);
		displayPrintCentered(72, "(keyed TX)", FONT_SIZE_2);
	}
	else
	{
		char buf[24];
		const char *why = "error";
		switch (s_msg.result)
		{
			case -2: why = "TX busy";   break;
			case -3: why = "no key";    break;
			case -1: why = "bad text";  break;
			case -4: case -5: why = "too long"; break;
		}
		displayPrintCentered(44, "Send failed", FONT_SIZE_3);
		snprintf(buf, sizeof buf, "%s (ret %d)", why, s_msg.result);
		displayPrintCentered(72, buf, FONT_SIZE_2);
	}
	displayPrintCentered(112, "any key: back", FONT_SIZE_1);
	displayRender();
}

#endif // ENABLE_AES && ENABLE_DMR_DATA

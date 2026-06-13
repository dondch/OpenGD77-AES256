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
#include "user_interface/menuSystem.h"
#include "user_interface/uiLocalisation.h"
#include "user_interface/uiUtilities.h"
#include "utils.h"


#if defined(PLATFORM_RD5R)
#define YCENTER         ((DISPLAY_SIZE_Y / 2) + 3)
#define INNERBOX_H      (FONT_SIZE_3_HEIGHT + 7)
#define YBAR            (YCENTER - ((FONT_SIZE_3_HEIGHT / 2) + 2))
#define YTEXT           (YBAR + 1)
#else
#define YCENTER         (DISPLAY_SIZE_Y / 2)
#define INNERBOX_H      (FONT_SIZE_3_HEIGHT + 6)
#define YBAR            (YCENTER - (FONT_SIZE_3_HEIGHT / 2))
#define YTEXT           (YBAR - 2)
#endif
#define XBAR            69
#define YBOX            (YCENTER - (INNERBOX_H / 2))


#if defined(CPU_MK22FN512VLL12) || defined(PLATFORM_MD9600)
#if defined(PLATFORM_RD5R)
#define BEARING_RADIUS              18
#define BEARING_CIRCLE_X_OFFSET     4
#define BEARING_BOX_H             ((BEARING_RADIUS + 5) * 2)
#else
#define BEARING_RADIUS              18
#define BEARING_CIRCLE_X_OFFSET     4
#define BEARING_BOX_H             ((BEARING_RADIUS + 5) * 2)
#endif
#else // defined(CPU_MK22FN512VLL12) || defined(PLATFORM_MD9600)
#define BEARING_RADIUS              24
#define BEARING_CIRCLE_X_OFFSET     13
#define BEARING_BOX_H             ((BEARING_RADIUS + 10) * 2)
#endif
#define BEARING_CIRCLE_X_POS       (BEARING_RADIUS + BEARING_CIRCLE_X_OFFSET)
#define BEARING_BOX_W              (DISPLAY_SIZE_X - 4)
#define BEARING_BOX_Y             ((DISPLAY_SIZE_Y - BEARING_BOX_H) / 2)
#define TOP_ARROW_OFFSET           (BEARING_RADIUS - 8)
#define BASE_ARROW_OFFSET         -(BEARING_RADIUS - 8)
#define BEARING_AND_DIST_X_CENTER (((DISPLAY_SIZE_X - ((BEARING_RADIUS * 2) + BEARING_CIRCLE_X_OFFSET)) / 2) + ((BEARING_RADIUS * 2) + BEARING_CIRCLE_X_OFFSET))


#if defined(PLATFORM_MD9600) || defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
static __attribute__((section(".data.$RAM2"))) uint8_t screenNotificationBufData[((DISPLAY_SIZE_X * DISPLAY_SIZE_Y) >> 3)];
#else
static  __attribute__((section(".ccmram"))) uint16_t screenNotificationBufData[DISPLAY_SIZE_X * DISPLAY_SIZE_Y];
#endif

typedef struct
{
	bool                         visible;
	ticksTimer_t                 hideTimer;
	uiNotificationType_t         type;
	uiNotificationID_t           id;
	char                         message[NOTIFICATION_MESSAGE_LEN_MAX];
	uiNotificationBearingInfo_t  bearingInfo;
} uiNotificationData_t;

static uiNotificationData_t notificationData =
{
		.visible = false,
		.hideTimer = { 0, 0 },
		.type = NOTIFICATION_TYPE_MAX,
		.id = NOTIFICATION_ID_NONE,
		.message = { 0 },
		.bearingInfo = { .bearing = 0.0, .distanceX10 = -1 }
};

static void displayMessage(void);

void uiNotificationShow(uiNotificationType_t type, uiNotificationID_t id, uint32_t msTimeout, const char *message, bool immediateRender)
{
	bool valid = true;

	if (notificationData.visible)
	{
		if (notificationData.id != id)
		{
			notificationData.visible = false;
			displayRender();
		}
	}

	memset(notificationData.message, 0, sizeof(notificationData.message));

	notificationData.type = type;
	notificationData.id = id;

	switch (type)
	{
		case NOTIFICATION_TYPE_SQUELCH:
#if defined(HAS_SOFT_VOLUME)
		case NOTIFICATION_TYPE_VOLUME:
#endif
			break;

		case NOTIFICATION_TYPE_POWER:
			break;

		case NOTIFICATION_TYPE_MESSAGE:
			if (message)
			{
				snprintf(&notificationData.message[0], sizeof(notificationData.message), "%s", message);
			}
			break;

		case NOTIFICATION_TYPE_BEARING:
			break;

		default:
			valid = false;
			break;
	}

	if (valid)
	{
		notificationData.visible = true;
		if (immediateRender)
		{
			uiNotificationRefresh();
		}
		ticksTimerStart(&notificationData.hideTimer, msTimeout);
	}
}

void uiNotificationBearingShow(uiNotificationBearingInfo_t *bearing, uint32_t msTimeout, bool immediateRender)
{
	memcpy(&(notificationData.bearingInfo), bearing, sizeof(uiNotificationBearingInfo_t));
	uiNotificationShow(NOTIFICATION_TYPE_BEARING, NOTIFICATION_ID_BEARING, msTimeout, NULL, immediateRender);
}

void uiNotificationRefresh(void)
{
	if (notificationData.visible)
	{
		// copy the primary screen content
		memcpy(screenNotificationBufData, displayGetPrimaryScreenBuffer(), sizeof(screenNotificationBufData));

#if defined(PLATFORM_MD9600) || defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
		displayOverrideScreenBuffer(screenNotificationBufData);
#endif

		// Draw whatever
		switch (notificationData.type)
		{
			case NOTIFICATION_TYPE_SQUELCH:
#if defined(HAS_SOFT_VOLUME)
			case NOTIFICATION_TYPE_VOLUME:
#endif
			{
				char buffer[SCREEN_LINE_BUFFER_SIZE];
				const int16_t totalBarLength = (((DISPLAY_SIZE_X - XBAR) - 4) - 4);
				double slope;
				int16_t bargraph;

#if defined(HAS_SOFT_VOLUME)
				if (notificationData.type == NOTIFICATION_TYPE_VOLUME)
				{
					uint16_t volValue = CLAMP((lastVolume + 32), 0, 62);

					slope = 1.0 * (totalBarLength) / 62.0;
					bargraph = slope * volValue;

					strncpy(buffer, currentLanguage->volume, 9);
				}
				else
#endif
				{
					slope = 1.0 * (totalBarLength) / (CODEPLUG_MAX_VARIABLE_SQUELCH - CODEPLUG_MIN_VARIABLE_SQUELCH);
					bargraph = slope * (currentChannelData->sql - CODEPLUG_MIN_VARIABLE_SQUELCH);

					strncpy(buffer, currentLanguage->squelch, 9);
				}
				buffer[8] = 0;

				size_t sLen = (strlen(buffer) * 8);

				displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
				displayDrawRoundRectWithDropShadow(1, YBOX, DISPLAY_SIZE_X - 4, INNERBOX_H, 3, true);

				displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);
				displayPrintAt(2 + ((sLen < (XBAR - 2)) ? (((XBAR - 2) - sLen) >> 1) : 0), YTEXT, buffer, FONT_SIZE_3);

				displayDrawRect((XBAR - 2), YBAR, (totalBarLength + 4), (SQUELCH_BAR_H + 4), true);
				displayFillRect(XBAR, (YBAR + 2), bargraph, SQUELCH_BAR_H, false);
			}
			break;

			case NOTIFICATION_TYPE_POWER:
			{
				char buffer[SCREEN_LINE_BUFFER_SIZE];
				uint8_t powerLevel = trxGetPowerLevel();

				displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
				displayDrawRoundRectWithDropShadow((DISPLAY_SIZE_X / 4), YBOX, (DISPLAY_SIZE_X / 2), INNERBOX_H, 3, true);
				sprintf(buffer, "%s%s", getPowerLevel(powerLevel), getPowerLevelUnit(powerLevel));
				displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);
				displayPrintCentered(YTEXT, buffer, FONT_SIZE_3);
			}
			break;

			case NOTIFICATION_TYPE_MESSAGE:
				displayMessage();
				break;

			case NOTIFICATION_TYPE_BEARING:
			{
				char buffer[SCREEN_LINE_BUFFER_SIZE];
				int16_t x1, x2, x3, y1, y2, y3;

				displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
				displayDrawRoundRectWithDropShadow(1, BEARING_BOX_Y, (DISPLAY_SIZE_X - 4), BEARING_BOX_H, 3, true);
				displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);

				// Circle
				for (int16_t i = 0; i < 360; i += 15)
				{
					x1 = (int16_t)(BEARING_CIRCLE_X_POS + (BEARING_RADIUS) * cos((double)(i * DEG_TO_RAD)));
					y1 = (int16_t)(((DISPLAY_SIZE_Y >> 1) - 3) + (BEARING_RADIUS) * sin((double)(i * DEG_TO_RAD)));

					x2 = (int16_t)(BEARING_CIRCLE_X_POS + (BEARING_RADIUS - ((i % 45) ? 2 : 4)) * cos((double)(i * DEG_TO_RAD)));
					y2 = (int16_t)(((DISPLAY_SIZE_Y >> 1) - 3) + (BEARING_RADIUS - ((i % 45) ? 2 : 4)) * sin((double)(i * DEG_TO_RAD)));

					if (i == 270)
					{
						displayThemeApply(THEME_ITEM_FG_GPS_COLOUR, THEME_ITEM_BG_NOTIFICATION);
						displayDrawLine((x1 - 1), y1, (x2 - 1), y2, true);
					}

					displayDrawLine(x1, y1, x2, y2, true);

					if (i == 270)
					{
						displayDrawLine((x1 + 1), y1, (x2 + 1), y2, true);
						displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
					}
				}


				displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
				displayDrawCircle(BEARING_CIRCLE_X_POS, ((DISPLAY_SIZE_Y >> 1) - 3), (BEARING_RADIUS + 1), true);
				displayThemeApply(THEME_ITEM_FG_GPS_COLOUR, THEME_ITEM_BG_NOTIFICATION);

				// Needle
				double needle = (notificationData.bearingInfo.bearing - 90.0);

				if (needle > 360.0)
				{
					needle -= 360.0;
				}

				needle *= DEG_TO_RAD;

				x1 = (int16_t)(BEARING_CIRCLE_X_POS + TOP_ARROW_OFFSET * cos(needle));
				y1 = (int16_t)(((DISPLAY_SIZE_Y >> 1) - 3) + TOP_ARROW_OFFSET * sin(needle));

				x2 = (int16_t)(BEARING_CIRCLE_X_POS + BASE_ARROW_OFFSET * cos(needle + (30 * DEG_TO_RAD)));
				y2 = (int16_t)(((DISPLAY_SIZE_Y >> 1) - 3) + BASE_ARROW_OFFSET * sin(needle + (30 * DEG_TO_RAD)));

				x3 = (int16_t)(BEARING_CIRCLE_X_POS + BASE_ARROW_OFFSET * cos(needle - (30 * DEG_TO_RAD)));
				y3 = (int16_t)(((DISPLAY_SIZE_Y >> 1) - 3) + BASE_ARROW_OFFSET * sin(needle - (30 * DEG_TO_RAD)));

				displayFillTriangle(x1, y1, x2, y2, x3, y3, true);

				// Bearing and Distance values
				displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);

				snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%u%c", ((uint32_t)notificationData.bearingInfo.bearing), 176);
				displayPrintAt((BEARING_AND_DIST_X_CENTER - ((strlen(buffer) * 8) >> 1)),
						(((DISPLAY_SIZE_Y >> 1) - 3) - FONT_SIZE_3_HEIGHT), buffer, FONT_SIZE_3);

				if (notificationData.bearingInfo.distanceX10 >= 0)
				{
					uint32_t intPart = (notificationData.bearingInfo.distanceX10 / 10);
					uint32_t decPart = (notificationData.bearingInfo.distanceX10 - (intPart * 10));

					snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%u.%u km", intPart, decPart);
					displayPrintAt((BEARING_AND_DIST_X_CENTER - ((strlen(buffer) * 8) >> 1)),
							(((DISPLAY_SIZE_Y >> 1) - 3) + 2), buffer, FONT_SIZE_3);
				}
			}
			break;

			default:
				break;
		}

		displayThemeResetToDefault();
		displayRenderWithoutNotification();
#if defined(PLATFORM_MD9600) || defined(PLATFORM_GD77) || defined(PLATFORM_GD77S) || defined(PLATFORM_DM1801) || defined(PLATFORM_DM1801A) || defined(PLATFORM_RD5R)
		displayRestorePrimaryScreenBuffer();
#else
		memcpy(displayGetPrimaryScreenBuffer(), screenNotificationBufData, sizeof(screenNotificationBufData));
#endif
	}
}

bool uiNotificationHasTimedOut(void)
{
	return (notificationData.visible && ticksTimerHasExpired(&notificationData.hideTimer));
}

bool uiNotificationIsVisible(void)
{
	return notificationData.visible;
}

void uiNotificationHide(bool immediateRender)
{
	notificationData.visible = false;
	uiDataGlobal.displayQSOState = uiDataGlobal.displayQSOStatePrev;

	if (immediateRender)
	{
		displayRender();
	}
}

uiNotificationID_t uiNotificationGetId(void)
{
	return notificationData.id;
}

static void displayMessage(void)
{
	char msg[SCREEN_LINE_BUFFER_SIZE];
	size_t len = strlen(notificationData.message);
	char *pb = notificationData.message;
	char *pe = &notificationData.message[0] + len;
	char *p = strchr(notificationData.message, '\n');
	uint16_t count;
	uint16_t y = 0U;
	uint8_t linesCount = 1U;
	uint8_t maxLines =
#if defined(HAS_COLOURS) || defined(PLATFORM_RD5R)
			4U;
#else
			3U;
#endif

	while (p)
	{
		linesCount++;

		if (linesCount == maxLines)
		{
			break;
		}

		p = strchr(p + 1, '\n');
	}

	y = (((DISPLAY_SIZE_Y - (linesCount * FONT_SIZE_3_HEIGHT)) >> 1) - 3);

	displayThemeApply(THEME_ITEM_FG_DECORATION, THEME_ITEM_BG_NOTIFICATION);
	displayDrawRoundRectWithDropShadow(1, (y - 1), (DISPLAY_SIZE_X - 4), ((FONT_SIZE_3_HEIGHT * linesCount) + 6), 3, true);
	displayThemeApply(THEME_ITEM_FG_NOTIFICATION, THEME_ITEM_BG_NOTIFICATION);

	p = strchr(notificationData.message, '\n');

	while (linesCount > 1U)
	{
		count = SAFE_MIN((uint16_t)(p - pb), 14U);

		memcpy(msg, pb, count);
		msg[count] = '\0';

		displayPrintCentered(y, msg, FONT_SIZE_3);
		y += FONT_SIZE_3_HEIGHT;

		pb = p + 1;
		p = strchr(pb, '\n');

		linesCount--;
	}

	if (pb < pe)
	{
		count = SAFE_MIN((uint16_t)(pe - pb), 14U);

		p++;

		// Looking for another new line
		if ((p = strchr(pb, '\n')) != NULL)
		{
			count = (uint16_t)(p - pb);
		}

		memcpy(msg, pb, count);
		msg[count] = '\0';

		displayPrintCentered(y, msg, FONT_SIZE_3);
	}
}

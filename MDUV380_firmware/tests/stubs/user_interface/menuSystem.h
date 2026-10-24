/* Host-test stand-in for the firmware menuSystem.h: only the notification API dmr_sms.c uses. */
#ifndef TEST_STUB_MENUSYSTEM_H
#define TEST_STUB_MENUSYSTEM_H
#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    NOTIFICATION_TYPE_SQUELCH = 0,
    NOTIFICATION_TYPE_POWER,
    NOTIFICATION_TYPE_MESSAGE,
    NOTIFICATION_TYPE_BEARING,
    NOTIFICATION_TYPE_MAX
} uiNotificationType_t;

typedef enum
{
    NOTIFICATION_ID_NONE = 0,
    NOTIFICATION_ID_SQUELCH,
    NOTIFICATION_ID_POWER,
    NOTIFICATION_ID_MESSAGE,
    NOTIFICATION_ID_BEARING,
    NOTIFICATION_ID_TX_INHIBIT,
    NOTIFICATION_ID_USER
} uiNotificationID_t;

void uiNotificationShow(uiNotificationType_t type, uiNotificationID_t id, uint32_t msTimeout, const char *message, bool immediateRender);
#endif

/* Host-test stand-in for the firmware ticks.h (the real one pulls in FreeRTOS). */
#ifndef TEST_STUB_TICKS_H
#define TEST_STUB_TICKS_H
#include <stdint.h>
uint32_t ticksGetMillis(void);
#endif

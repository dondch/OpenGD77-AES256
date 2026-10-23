/*
 * Copyright (C) 2023-2025 Roger Clark, VK3KYY / G4KYF
 *                         Daniel Caujolle-Bert, F1RMB
 *
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

#if !defined(PLATFORM_GD77S)

#include "main.h"
#include "functions/settings.h"
#include "user_interface/uiLocalisation.h"
#include "user_interface/uiUtilities.h"
#include "interfaces/clockManager.h"
#include "functions/rxPowerSaving.h"

#if defined(CPU_MK22FN512VLL12)
#include "fsl_ftm.h"
#else // CPU_MK22FN512VLL12
#if !defined(PLATFORM_MD9600)
#include "hardware/AT1846S.h"
#endif
#endif // CPU_MK22FN512VLL12
#include "functions/aprs.h"
#include "hardware/HR-C6000.h"
#include "functions/satellite.h"
#if defined(HAS_GPS)
#include "interfaces/gps.h"
#endif
#include "user_interface/menuSystem.h"

#define AX25_PACKET_BUFFER_SIZE        256U
#define SMART_BEACONING_SPEED_MIN       54U // more than 1km/h (0.5399568034557235 kn == 1km/h)
#define APRS_DESTINATION            "APOG77" // MAX 6 char (excluding terminator)
#define APRS_CONFIG_SATELLITE            0U
#define APRS_CONFIG_CHANNEL              1U
#define APRS_ANTI_COLLISION_MS_MAX    2000U
#define APRS_USE_COURSETO_FOR_BEARING     1


#if defined(CPU_MK22FN512VLL12)
// On MK22, isnan macro has a bug, circumvent it
#undef isnan
#define isnan(x) (__builtin_isnan(x))

// On MK22, round() doesn't exist, __builtin_round() is also not functionnal
// from newlib-cygwin:
//    https://sourceware.org/git/gitweb.cgi?p=newlib-cygwin.git;a=blob;f=newlib/libm/common/s_round.c;hb=master
typedef union
{
		double value;
		struct
		{
				uint32_t lsw;
				uint32_t msw;
		} parts;
} ieee_double_shape_type;

#define EXTRACT_WORDS(ix0,ix1,d)         \
		do {                             \
			ieee_double_shape_type ew_u; \
			ew_u.value = (d);            \
			(ix0) = ew_u.parts.msw;      \
			(ix1) = ew_u.parts.lsw;      \
		} while (0)

#define INSERT_WORDS(d,ix0,ix1)          \
		do {                             \
			ieee_double_shape_type iw_u; \
			iw_u.parts.msw = (ix0);      \
			iw_u.parts.lsw = (ix1);      \
			(d) = iw_u.value;            \
		} while (0)

double round(double x)
{
	/* Most significant word, least significant word. */
	int32_t msw, exponent_less_1023;
	uint32_t lsw;

	EXTRACT_WORDS(msw, lsw, x);

	/* Extract exponent field. */
	exponent_less_1023 = ((msw & 0x7ff00000) >> 20) - 1023;

	if (exponent_less_1023 < 20)
	{
		if (exponent_less_1023 < 0)
		{
			msw &= 0x80000000;
			if (exponent_less_1023 == -1)
				/* Result is +1.0 or -1.0. */
				msw |= ((int32_t)1023 << 20);
			lsw = 0;
		}
		else
		{
			uint32_t exponent_mask = 0x000fffff >> exponent_less_1023;
			if ((msw & exponent_mask) == 0 && lsw == 0)
				/* x in an integral value. */
				return x;

			msw += 0x00080000 >> exponent_less_1023;
			msw &= ~exponent_mask;
			lsw = 0;
		}
	}
	else if (exponent_less_1023 > 51)
	{
		if (exponent_less_1023 == 1024)
			/* x is NaN or infinite. */
			return x + x;
		else
			return x;
	}
	else
	{
		uint32_t exponent_mask = 0xffffffff >> (exponent_less_1023 - 20);
		uint32_t tmp;

		if ((lsw & exponent_mask) == 0)
			/* x is an integral value. */
			return x;

		tmp = lsw + (1 << (51 - exponent_less_1023));
		if (tmp < lsw)
			msw += 1;
		lsw = tmp;

		lsw &= ~exponent_mask;
	}
	INSERT_WORDS(x, msw, lsw);

	return x;
}
#endif

typedef struct
{
	uint8_t                      packetBuffer[AX25_PACKET_BUFFER_SIZE];
	uint16_t                     packetBufferBitPosition;
	uint16_t                     bitStuffingCounter;
	uint16_t                     crc;
	bool                         currentBitNRZI;
	bool                         baudIs300;
} AX25Encoder_t;

// Beaconing

typedef struct
{
	double                       latitude;
	double                       longitude;
} aprsBeaconingCoordinates_t;

typedef struct
{
	uint32_t                     time; // ms
	uint16_t                     bearing; // hundredth degree
	uint16_t                     speed; // hundredth of knots
	aprsBeaconingCoordinates_t   coords;
} aprsBeaconingLocation_t;

typedef struct
{
	ticksTimer_t                 checkTimer;
	ticksTimer_t                 nextBeaconTimer;

#if defined(APRS_USE_COURSETO_FOR_BEARING)
	aprsBeaconingCoordinates_t   previousBearingPosition;
	uint16_t                     currentCourse;
#endif

	aprsBeaconingLocation_t      currentLocation;
	aprsBeaconingLocation_t      previousLocation;

	CodeplugAPRSConfig_t        aprsConfig[APRS_CONFIG_CHANNEL + 1];

	double                       fixedLocationLat;
	double                       fixedLocationLon;

#if defined(RATE_MESSAGE_FEATURE)
	uint8_t                      rateMessageCount;
#endif

	bool                         triggerBeaconing;
	uint8_t                      decayMult; // 1 to 32

	aprsBeaconingSettings_t      settings;

	bool                         hasDoneQSY;
	bool                         forcedManualBeaconing;
	uint8_t                      beaconingFailureCounter;
}  aprsBeaconingData_t;

typedef struct
{
	uint32_t rxFreq;
	uint32_t txFreq;
	uint16_t rxTone;
	uint8_t  chMode;
	bool     wideBW;
} aprsChannelSettingsInUse_t;

aprsBeaconingData_t aprsBcnData =
{
	.checkTimer = { 0U, 0U },
	.nextBeaconTimer = { 0U, 0U },
#if defined(APRS_USE_COURSETO_FOR_BEARING)
	.previousBearingPosition = { 0.0, 0.0 },
	.currentCourse = 0U,
#endif
	.currentLocation = { .time = 0U, .coords = { .latitude = 0.0, .longitude = 0.0 }, .bearing = UINT16_MAX, .speed = 0U },
	.previousLocation = { .time = 0U, .coords = { .latitude = 0.0, .longitude = 0.0 }, .bearing = UINT16_MAX, .speed = 0U },
	.fixedLocationLat = NAN,
	.fixedLocationLon = NAN,
#if defined(RATE_MESSAGE_FEATURE)
	.rateMessageCount = 0U,
#endif
	.triggerBeaconing = false,
	.decayMult = APRS_BEACON_DECAY_MULT_MIN,
	.settings =
	{
		.state = (APRS_BEACONING_STATE_LOCATION_FROM_CHANNEL | APRS_BEACONING_STATE_DECAY_ALGO_ENABLED | APRS_BEACONING_STATE_COMPRESSED_FORMAT),
		.mode = APRS_BEACONING_MODE_OFF,
		.initialInterval = APRS_BEACON_INITIAL_INTERVAL_DEFAULT, // Offset in initialIntervals[]
#if defined(RATE_MESSAGE_FEATURE)
		.messageInterval = APRS_BEACON_MESSAGE_INTERVAL_DEFAULT,
#endif
		.power = 0U,
		.smart =
		{
			.slowRate = APRS_SMART_BEACON_SLOW_RATE_DEFAULT,
			.fastRate = APRS_SMART_BEACON_FAST_RATE_DEFAULT,
			.lowSpeed = APRS_SMART_BEACON_LOW_SPEED_DEFAULT,
			.highSpeed = APRS_SMART_BEACON_HIGH_SPEED_DEFAULT,
			.turnAngle = APRS_SMART_BEACON_TURN_ANGLE_DEFAULT,
			.turnSlope = APRS_SMART_BEACON_TURN_SLOPE_DEFAULT,
			.turnTime = APRS_SMART_BEACON_TURN_TIME_DEFAULT,
		}
	},
	.hasDoneQSY = false,
	.forcedManualBeaconing = false,
	.beaconingFailureCounter = 0U
};

const uint16_t initialIntervalsInSecs[APRS_BEACON_INITIAL_INTERVAL_MAX + 1] = { 12U, 30U, 60U, 120U, 180U, 300U, 600U, 1200U, 1800U, 3600U };

static char myCall[16];
static int lenBytes = 0;
static volatile uint32_t lastTone;
static volatile int bytePos = 0;
static volatile int bitPos = 0;
static volatile uint8_t dataByte;
static AX25Encoder_t encoderData;
static CodeplugAPRSConfig_t *aprsConfig;

volatile aprsSendProgress_t aprsTxProgress = APRS_TX_IDLE; // used in the ISR


static bool aprsBeaconingStateEnabled(aprsBeaconingStates_t s);
static bool aprsBeaconingLocationIsValid(aprsBeaconingLocation_t *location);
static void enqueueCharNrzi(AX25Encoder_t *encoderData, uint8_t data, bool useBitStuffing);

static void enqueueBit(AX25Encoder_t *encoderData, bool data)
{
	if (data)
	{
		encoderData->packetBuffer[encoderData->packetBufferBitPosition / 8U] |= 0x01 << (encoderData->packetBufferBitPosition % 8U);
	}
	encoderData->packetBufferBitPosition++;
}

static void updateCRC(AX25Encoder_t *encoderData, bool dataBit)
{
	uint16_t crcXorDataBit = (encoderData->crc ^ dataBit);

	encoderData->crc >>= 1;

	if (crcXorDataBit & 0x01)
	{
		encoderData->crc ^= 0x8408;
	}
}

static void enqueueCRC(AX25Encoder_t *encoderData)
{
	uint8_t crc_lo = (encoderData->crc ^ 0xff);
	uint8_t crc_hi = ((encoderData->crc >> 8) ^ 0xff);

	enqueueCharNrzi(encoderData, crc_lo, true);
	enqueueCharNrzi(encoderData, crc_hi, true);
}

static void enqueuePadOfLength(AX25Encoder_t *encoderData, uint8_t len)
{
	for (uint8_t j = 0; j < len; j++)
	{
		enqueueCharNrzi(encoderData, (' ' << 1), true);
	}
}

static void enqueueCharNrzi(AX25Encoder_t *encoderData, uint8_t data, bool useBitStuffing)
{
	bool currentBit;

	for (uint8_t i = 0; i < 8U; i++)
	{
		currentBit = (data & 0x01);

		updateCRC(encoderData, currentBit);

		if (currentBit)
		{
			enqueueBit(encoderData, encoderData->currentBitNRZI);
			encoderData->bitStuffingCounter++;

			if (useBitStuffing && (encoderData->bitStuffingCounter == 5))
			{
				encoderData->currentBitNRZI ^= 1;
				enqueueBit(encoderData, encoderData->currentBitNRZI);

				encoderData->bitStuffingCounter = 0U;
			}
		}
		else
		{
			encoderData->currentBitNRZI ^= 1;
			enqueueBit(encoderData, encoderData->currentBitNRZI);

			encoderData->bitStuffingCounter = 0U;
		}

		data >>= 1;
	}
}

static void enqueueString(AX25Encoder_t *encoderData, const char *str)
{
	uint8_t i = 0;

	while (str[i] != 0)
	{
		enqueueCharNrzi(encoderData, str[i], true);
		i++;
	};
}

static void enqueueFlagOfLength(AX25Encoder_t *encoderData, uint8_t len)
{
	for (uint8_t i = 0; i < len; i++)
	{
		enqueueCharNrzi(encoderData, 0x7E, false); // 0x7E flag
	}
}

static void enqueueHeader(AX25Encoder_t *encoderData)
{
	//int len = MIN(strlen(APRS_DESTINATION), 6U);

	for (uint32_t i = 0; i < strlen(APRS_DESTINATION); i++)
	{
		enqueueCharNrzi(encoderData, (APRS_DESTINATION[i] << 1), true);
	}

	//if (len < 6U)
	//{
	//	enqueuePadOfLength(encoderData, (6U - len));
	//}

	enqueueCharNrzi(encoderData, ('0' << 1), true);

	uint8_t len = MIN(strlen(myCall), 6U);

	for (uint8_t i = 0; i < len; i++)
	{
		enqueueCharNrzi(encoderData, (myCall[i] << 1), true);
	}

	if (len < 6U)
	{
		enqueuePadOfLength(encoderData, (6U - len));
	}

	enqueueCharNrzi(encoderData, ((aprsConfig->senderSSID + '0') << 1), true);

	uint8_t numPaths = ((strlen(aprsConfig->paths[1].name) == 0) ? 1U : 2U);

	for (uint8_t p = 0; p < numPaths; p++)
	{
		len = MIN(strlen(aprsConfig->paths[p].name), 6U);

		for (uint8_t i = 0; i < len; i++)
		{
			enqueueCharNrzi(encoderData, (aprsConfig->paths[p].name[i] << 1), true);
		}

		if (len < 6U)
		{
			enqueuePadOfLength(encoderData, (6U - len));
		}

		uint8_t isEnd = (p == (numPaths - 1)) ? 1U : 0U;

		enqueueCharNrzi(encoderData, (((aprsConfig->paths[p].SSID + '0') << 1) + isEnd), true);
	}

	enqueueCharNrzi(encoderData, 0x03, true);
	enqueueCharNrzi(encoderData, 0xF0, true);
}

static void enqueuePayload(AX25Encoder_t *encoderData, const char *latStr, const char *lonStr, const char *courseAndSpeed, aprsChannelSettingsInUse_t *channelSettings, bool fromSatScreen)
{
	static const uint8_t DT_POS = '!';
	uint8_t symbol = (aprsConfig->iconIndex + '!'); //'+'; // + = cross symbol. Y = yacht etc
	uint8_t symTable = ((aprsConfig->iconTable == 0) ? '/' : '\\'); //' = secondary table
	bool hasComment = (aprsConfig->comment[0] != 0);

	enqueueCharNrzi(encoderData, DT_POS, true);

	if (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_COMPRESSED_FORMAT))
	{
		enqueueCharNrzi(encoderData, symTable, true);
		enqueueString(encoderData, latStr);
		enqueueString(encoderData, lonStr);
		enqueueCharNrzi(encoderData, symbol, true);
		enqueueString(encoderData, (courseAndSpeed ? courseAndSpeed : "  "));
		enqueueCharNrzi(encoderData, (courseAndSpeed ? (/*0x26 (Other)*/ 0x3E /* (RMC)*/ + '!') : '!'), true);
	}
	else
	{
		enqueueString(encoderData, latStr);
		enqueueCharNrzi(encoderData, symTable, true);
		enqueueString(encoderData, lonStr);
		enqueueCharNrzi(encoderData, symbol, true);

		if (courseAndSpeed != NULL)
		{
			enqueueString(encoderData, courseAndSpeed);
		}
	}

	if ((fromSatScreen == false) &&
			(aprsConfig->flags & CODEPLUG_APRS_CONFIG_FLAG_TRANSMIT_QSY_INFO) && (aprsConfig->txFrequency != 0U))
	{
		uint32_t rxFrequency = channelSettings->rxFreq / 100U;
		uint32_t txFrequency = channelSettings->txFreq / 100U;
		int32_t offsetValue = (txFrequency - rxFrequency);
		bool cssEnabled = ((channelSettings->chMode == RADIO_MODE_ANALOG) && (channelSettings->rxTone != CODEPLUG_CSS_TONE_NONE));
		bool wideBW = channelSettings->wideBW;
		uint32_t fMHz = (uint32_t)(rxFrequency * 1E-3);
		uint32_t fKHz = (rxFrequency - (fMHz * 1000U));
		char cssStr[6U] = { ' ', (wideBW ? 'T' : 't'), 'o', 'f', 'f', 0 }; // " Toff"
		char freqStr[23U]; // "FFF.FFFMHz Toff +000 "

		// There is a limit of 9.90MHz, as the document I found stated (why no 9.99MHz, no idea).
		if (abs(offsetValue) > 9900)
		{
			offsetValue = 0;
		}

		if (cssEnabled)
		{
			CodeplugCSSTypes_t RxCSSType = codeplugGetCSSType(channelSettings->rxTone);

			if (RxCSSType == CSS_TYPE_CTCSS)
			{
				snprintf(cssStr, sizeof(cssStr), " %c%03u", (wideBW ? 'C' : 'c'), (channelSettings->rxTone / 10U));
			}
			else if (RxCSSType & CSS_TYPE_DCS)
			{
				snprintf(cssStr, sizeof(cssStr), " %c%03X", (wideBW ? 'D' : 'd'), (channelSettings->rxTone & ~CSS_TYPE_DCS_MASK));
			}
		}

		// Even if the offset is equal to zero, and no CSS is set,
		// we send that information, which permits to give the bandwidth information
		snprintf(freqStr, sizeof(freqStr), "%03u.%03uMHz%s %c%03d%c", fMHz, fKHz, cssStr,
				(offsetValue < 0 ? '-' : '+'), abs((offsetValue / 10)), (hasComment ? ' ' : 0));

		enqueueString(encoderData, freqStr);
	}

	if (hasComment)
	{
		enqueueString(encoderData, aprsConfig->comment);
	}
}

static void aprsTxEnded(void)
{
#if defined(CPU_MK22FN512VLL12)
	trxSelectVoiceChannel(AT1846_VOICE_CHANNEL_MIC);
	audioAmpDisable(AUDIO_AMP_CHANNEL_RF);
#else // CPU_MK22FN512VLL12
#if defined(PLATFORM_MD9600)
	trxDTMFoff(true);
	trxSelectVoiceChannel(AT1846_VOICE_CHANNEL_MIC);
#else // PLATFORM_MD9600
	AT1846sWriteTone1Reg(0);

	audioAmpDisable(AUDIO_AMP_CHANNEL_RF);
	HAL_GPIO_WritePin(RX_AUDIO_MUX_GPIO_Port, RX_AUDIO_MUX_Pin, GPIO_PIN_SET);
	radioWriteReg2byte(0x44, 0x06, 0xCC); // set back to FM default

	trxSelectVoiceChannel(AT1846_VOICE_CHANNEL_MIC);
#endif // PLATFORM_MD9600
	HRC6000SetMic(true);
#endif // CPU_MK22FN512VLL12
}

static void aprsConvertAndSplitCoord(double coord, bool isAmbiguous, uint32_t *degrees, uint32_t *minutesIntegral, uint32_t *minutesFractional)
{
	*degrees = (uint32_t)coord;

	double minutes = ((coord - (double)*degrees) * 60.0);
	double minutesIntegralDouble;

	if (isAmbiguous)
	{
		minutes += 0.000000001;
	}

	*minutesFractional = (uint32_t)abs((int)(modf(minutes, &minutesIntegralDouble) * 100));
	*minutesIntegral = (uint32_t)(minutesIntegralDouble);
}

static void aprsCompressLatitude(double lat, char *str)
{
	int latRes;
	int lat0, lat1, lat2, lat3;

	latRes = (int)round(380926.0 * (90.0 - lat));

	lat0 = (latRes / (91 * 91 * 91));
	latRes -= (lat0 * (91 * 91 * 91));

	lat1 = (latRes / (91 * 91));
	latRes -= (lat1 * (91 * 91));

	lat2 = (latRes / 91);
	latRes -= (lat2 * 91);

	lat3 = latRes;

	str[0] = (lat0 + '!');
	str[1] = (lat1 + '!');
	str[2] = (lat2 + '!');
	str[3] = (lat3 + '!');
	str[4] = 0;
}

static void aprsCompressLongitude(double lon, char *str)
{
	int lonRes;
	int lon0, lon1, lon2, lon3;

	lonRes = (int)round(190463.0 * (180.0 + lon));

	lon0 = (lonRes / (91 * 91 * 91));
	lonRes -= (lon0 * (91 * 91 * 91));

	lon1 = (lonRes / (91 * 91));
	lonRes -= (lon1 * (91 * 91));

	lon2 = (lonRes / 91);
	lonRes -= (lon2 * 91);

	lon3 = lonRes;

	str[0] = (lon0 + '!');
	str[1] = (lon1 + '!');
	str[2] = (lon2 + '!');
	str[3] = (lon3 + '!');
	str[4] = 0;
}

static bool aprsSendPacket(CodeplugAPRSConfig_t *config, aprsBeaconingLocation_t *previousLocation, aprsBeaconingLocation_t *currentLocation, aprsChannelSettingsInUse_t *channelSettings, bool fromSatScreen)
{
	double lat;
	double lon;
	char latStr[16];
	char lonStr[16];
	char courseSpeedStr[16];
	bool courseAndSpeed = false;
	bool gpsAndLocationAreValid = (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_LOCATION_FROM_GPS) && aprsBeaconingLocationIsValid(previousLocation) && aprsBeaconingLocationIsValid(currentLocation));
#if ! defined(PLATFORM_MD9600)
	bool beaconingSilently = (aprsConfig->flags & CODEPLUG_APRS_CONFIG_FLAG_BEACONING_SILENTLY);
#endif

	aprsConfig = config;

	encoderData.bitStuffingCounter = 0;
	encoderData.currentBitNRZI = false; // clear
	memset(encoderData.packetBuffer, 0, AX25_PACKET_BUFFER_SIZE);
	encoderData.packetBufferBitPosition = 0;

	codeplugGetRadioName(myCall);
	myCall[6] = 0; //truncate to 6 chars max

	bool usePosition = (aprsConfig->flags & CODEPLUG_APRS_CONFIG_FLAG_USE_POSITION);
	if (usePosition)
	{
		uint32_t lat24bits, lon24bits;

		lat24bits = ((aprsConfig->latitude[2] << 16) | ( aprsConfig->latitude[1] << 8) | aprsConfig->latitude[0]);
		lat = latLonFixed24ToDouble(lat24bits);

		lon24bits = ((aprsConfig->longitude[2] << 16) | (aprsConfig->longitude[1] << 8) | aprsConfig->longitude[0]);
		lon = latLonFixed24ToDouble(lon24bits);
	}
	else
	{
		lat = currentLocation->coords.latitude;
		lon = currentLocation->coords.longitude;
	}

	uint8_t ambiguity = (aprsConfig->flags & CODEPLUG_APRS_CONFIG_FLAG_AMBIGUITY_MASK) >> 5;
	if (ambiguity != 0U)
	{
		int tmp;
		double ambCoeff;

		switch (ambiguity)
		{
			case 1U: //0.0005 deg
				ambCoeff = 5000.0;
				break;
			case 2U: //0.001 deg
				ambCoeff = 1000.0;
				break;
			case 3U: //0.005 deg
				ambCoeff = 500.0;
				break;
			case 4U: //0.01 deg
				ambCoeff = 100.0;
				break;
			case 5U: //0.05 deg
				ambCoeff = 50.0;
				break;
			case 6U: //0.1 deg
				ambCoeff = 10.0;
				break;
			case 7U: //0.5 deg
				ambCoeff = 5.0;
				break;
			default:
				ambCoeff = 1;
				break;
		}

		tmp = lat * ambCoeff;
		lat = tmp / ambCoeff;

		tmp = lon * ambCoeff;
		lon = tmp / ambCoeff;
	}

	if (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_COMPRESSED_FORMAT))
	{
		aprsCompressLatitude(lat, latStr);
		aprsCompressLongitude(lon, lonStr);

		if (gpsAndLocationAreValid)
		{
			// Altitude ?

			if (currentLocation->speed > 0)
			{
				int iSpeed = (int)(currentLocation->speed * 1E-2);
				int iCourse = (int)(currentLocation->bearing * 1E-2);
				int c; // speed
				int s; // course

				// Course
				c = (iCourse / 4);

				if (c < 0)
				{
					c += 90;
				}

				if (c >= 90)
				{
					c -= 90;
				}

				c += '!';

				// Speed
				s = (int)round(log(iSpeed + 1.0) / log(1.08));
				s += '!';

				courseSpeedStr[0] = c;
				courseSpeedStr[1] = s;
				courseSpeedStr[2] = 0;
				courseAndSpeed = true;
			}

		}
	}
	else
	{
		uint32_t latDeg, lonDeg;
		uint32_t latMinsInt, lonMinsInt;
		uint32_t latMinsDecimal, lonMinsDecimal;
		bool latHemisphere = (lat >= 0.0);
		bool lonHemisphere = (lon >= 0.0);

		lat = fabs(lat);
		lon = fabs(lon);

		aprsConvertAndSplitCoord(lat, (ambiguity > 3), &latDeg, &latMinsInt, &latMinsDecimal);
		aprsConvertAndSplitCoord(lon, (ambiguity > 3), &lonDeg, &lonMinsInt, &lonMinsDecimal);

		snprintf(latStr, 16, "%02u%02u.%02u%c", latDeg, latMinsInt, latMinsDecimal, latHemisphere ? 'N' : 'S');
		snprintf(lonStr, 16, "%03u%02u.%02u%c", lonDeg, lonMinsInt, lonMinsDecimal, lonHemisphere ? 'E' : 'W');

		if (gpsAndLocationAreValid)
		{
			// Altitude ?

			if (currentLocation->speed > 0)
			{
				snprintf(courseSpeedStr, 16, "%03u/%03u", (uint16_t)(currentLocation->bearing * 1E-2), (uint16_t)(currentLocation->speed * 1E-2));
				courseAndSpeed = true;
			}
		}
	}

	enqueueFlagOfLength(&encoderData, 16U);

	encoderData.crc = 0xFFFF; // Initialise CRC now, after data has been sent as CRC does is only for data bytes
	enqueueHeader(&encoderData);
	enqueuePayload(&encoderData, latStr, lonStr, (courseAndSpeed ? courseSpeedStr : NULL), channelSettings, fromSatScreen);
	enqueueCRC(&encoderData);
	enqueueFlagOfLength(&encoderData, 3U);

	lenBytes = encoderData.packetBufferBitPosition / 8;
	bytePos = 0;
	bitPos = 0;
	lastTone = 0xFFFFFFFF;

#if defined(PLATFORM_MD9600)
	encoderData.baudIs300 = false;
#else // PLATFORM_MD9600
	encoderData.baudIs300 = ((aprsConfig->flags & CODEPLUG_APRS_CONFIG_FLAG_300_BAUDS) != 0);
#endif // PLATFORM_MD9600

#if defined(CPU_MK22FN512VLL12)
	ftm_config_t ftmInfo;
	FTM_GetDefaultConfig(&ftmInfo);
	ftmInfo.prescale = kFTM_Prescale_Divide_4;
	FTM_Init(FTM1, &ftmInfo);
	FTM_SetTimerPeriod(FTM1, USEC_TO_COUNT((!encoderData.baudIs300 ? 833U : 3333U), (CLOCK_GetFreq(kCLOCK_BusClk) / 4)));//use 2087U instead of 833 if in HS_RUN clock speed mode
	FTM_EnableInterrupts(FTM1, kFTM_TimeOverflowInterruptEnable);

	GPIO_PinWrite(GPIO_RX_audio_mux, Pin_RX_audio_mux, 1);

	if (beaconingSilently)
	{
		audioAmpDisable(AUDIO_AMP_CHANNEL_RF);
	}
	else
	{
		audioAmpEnable(AUDIO_AMP_CHANNEL_RF);
	}
	AT1846sWriteTone1Reg(lastTone);
	trxSelectVoiceChannel(AT1846_VOICE_CHANNEL_TONE1);

	aprsTxProgress = APRS_TX_IN_PROGRESS;

	EnableIRQ(FTM1_IRQn);
	FTM_StartTimer(FTM1, kFTM_SystemClock);
#else // CPU_MK22FN512VLL12
	htim6.Instance = TIM6;
	htim6.Init.Prescaler = encoderData.baudIs300 ? 7 : 1; // 7 gives 300 baud. 1 gives 1200 baud
	htim6.Init.CounterMode = TIM_COUNTERMODE_UP;

#if defined(PLATFORM_MD9600)
	htim6.Init.Period = 15000;
#else // PLATFORM_MD9600
	htim6.Init.Period = 30000;
#endif // PLATFORM_MD9600

	htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
	if (HAL_TIM_Base_Init(&htim6) != HAL_OK)
	{
		Error_Handler();
	}

	TIM_MasterConfigTypeDef sMasterConfig = { 0 };
	sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
	sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
	if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &sMasterConfig) != HAL_OK)
	{
		Error_Handler();
	}

	HRC6000SetMic(false);

#if defined(PLATFORM_MD9600)
	uint32_t lastTone = 1;
	trxSetTone1(lastTone);
#else // PLATFORM_MD9600
	uint32_t lastTone = 0;
	AT1846sWriteTone1Reg(lastTone);
	trxSelectVoiceChannel(AT1846_VOICE_CHANNEL_TONE1);

	int volume = getVolumeControl() + 30;

	if (volume >= 0)
	{
		radioWriteReg2byte(0x44, 0x06, volume);
		HAL_GPIO_WritePin(RX_AUDIO_MUX_GPIO_Port, RX_AUDIO_MUX_Pin, GPIO_PIN_RESET);

		if (beaconingSilently)
		{
			audioAmpDisable(AUDIO_AMP_CHANNEL_RF);
		}
		else
		{
			audioAmpEnable(AUDIO_AMP_CHANNEL_RF);
		}
	}
#endif // PLATFORM_MD9600

	aprsTxProgress = APRS_TX_IN_PROGRESS;
	HAL_TIM_Base_Start_IT(&htim6);
#endif // CPU_MK22FN512VLL12

	return true;
}

void aprsBitStreamSender(void)
{
	uint32_t newTone;

	if (aprsTxProgress != APRS_TX_IN_PROGRESS)
	{
		return;
	}

	if (bitPos % 8 == 0)
	{
		dataByte = encoderData.packetBuffer[bytePos];
		bytePos++;

		if (bytePos == lenBytes)
		{
//			AT1846SWriteTone1Reg(0);
			// just stop the ISR and flag that the data has been sent.

#if defined(CPU_MK22FN512VLL12)
			FTM_StopTimer(FTM1);
			DisableIRQ(FTM1_IRQn);
#else // CPU_MK22FN512VLL12
			HAL_TIM_Base_Stop_IT(&htim6);
#endif // CPU_MK22FN512VLL12

			aprsTxProgress = APRS_TX_FINISHED;			// Tell the foreground we've finished, so it can do the speaker and other stuff
			return;
		}
	}

#if defined(CPU_MK22FN512VLL12)
	newTone = (encoderData.baudIs300 ? 16000 : 12000) + ((dataByte & 0x01) ? (encoderData.baudIs300 ? 2000 : 10000) : 0);

	if (newTone != lastTone)
	{
		AT1846sWriteTone1Reg(newTone);
	}
#else // CPU_MK22FN512VLL12
#if defined(PLATFORM_MD9600)
	newTone = 1200 + ((dataByte & 0x01) ? 1000 : 0);
	if (newTone != lastTone)
	{
		int tval = (newTone * 65536) / 32000;												//calculate the value required to generate this tone
		uint8_t tH = (tval >> 8) & 0xFF;
		uint8_t tL = tval & 0xFF;
		SPI0WritePageRegByteExtended(0x01, 0x11B, tH);// Set  DTMF tone osc 1 to frequency of the required tone
		SPI0WritePageRegByteExtended(0x01, 0x11A, tL);


		SPI0WritePageRegByteExtended(0x01, 0x123, tH);// Set  DTMF tone osc 2 to frequency of the required tone
		SPI0WritePageRegByteExtended(0x01, 0x122, tL);
		lastTone = newTone;
	}
#else // PLATFORM_MD9600
	newTone = (encoderData.baudIs300 ? 16000 : 12000) + ((dataByte & 0x01) ? (encoderData.baudIs300 ? 2000 : 10000) : 0);
	if (newTone != lastTone)
	{
		AT1846sWriteTone1Reg(newTone);
		lastTone = newTone;
	}
#endif // PLATFORM_MD9600
#endif // CPU_MK22FN512VLL12

	dataByte >>= 1;
	bitPos++;
}

#if defined(CPU_MK22FN512VLL12)
void FTM1_IRQHandler(void)
{
    /* Clear interrupt flag.*/
    FTM_ClearStatusFlags(FTM1, kFTM_TimeOverflowFlag);
	aprsBitStreamSender();
    __DSB();
}

#endif // CPU_MK22FN512VLL12

//
// *****************
// *** Beaconing ***
// *****************
//
// Aknowlegments:
//     - SmartBeaconing™ is a beaconing algorithm invented by Tony Arnerich KD7TA and Steve Bragg KA9MVA.
//     - largely inspired by https://github.com/erstec/APRS-ESP/blob/master/src/smartBeaconing.cpp from Ernest Stepanov LY3PH.
//

static bool aprsBeaconingStateEnabled(aprsBeaconingStates_t s)
{
	return ((aprsBcnData.settings.state & s) == s);
}

// Returns true if the bit value has changed.
static bool aprsBeaconingStateSetEnable(aprsBeaconingStates_t s, bool enabled)
{
	if ((s & (APRS_BEACONING_STATE_LOCATION_FROM_CHANNEL | APRS_BEACONING_STATE_LOCATION_FROM_GPS)) != 0)
	{
		return false;
	}

	bool ret = (((aprsBcnData.settings.state & s) != 0) != enabled);

	if (enabled)
	{
		aprsBcnData.settings.state |= s;
	}
	else
	{
		aprsBcnData.settings.state &= ~s;
	}

	return ret;
}

static bool gpsPVTIsValid(void)
{
#if defined(HAS_GPS)
	if (SETTINGS_GPS_MODE_GET(nonVolatileSettings) > GPS_MODE_OFF)
	{
		return ((gpsData.Status & (GPS_STATUS_HAS_FIX | GPS_STATUS_HAS_POSITION | GPS_STATUS_HAS_SPEED | GPS_STATUS_HAS_TIME)) == (GPS_STATUS_HAS_FIX | GPS_STATUS_HAS_POSITION | GPS_STATUS_HAS_SPEED | GPS_STATUS_HAS_TIME));
	}
#endif

	return false;
}

static bool gpsFixIsValid(void)
{
#if defined(HAS_GPS)
	if (SETTINGS_GPS_MODE_GET(nonVolatileSettings) > GPS_MODE_OFF)
	{
		return ((gpsData.Status & GPS_STATUS_HAS_FIX) == GPS_STATUS_HAS_FIX);
	}
#endif

	return false;
}

static void aprsBeaconingInvalidateLocation(aprsBeaconingLocation_t *location)
{
	location->bearing = UINT16_MAX;
	location->coords.latitude = location->coords.longitude = 0.0;
	location->speed = 0U;
	location->time = 0U;
}

static bool aprsBeaconingLocationIsValid(aprsBeaconingLocation_t *location)
{
	return (location->bearing != UINT16_MAX);
}

static bool aprsBeaconingCurrentPositionIsValid(void)
{
	return ((aprsBeaconingStateEnabled(APRS_BEACONING_STATE_LOCATION_FROM_GPS) && settingsLocationIsValid()) ||
			(aprsBeaconingStateEnabled(APRS_BEACONING_STATE_LOCATION_FROM_CHANNEL) && ((isnan(aprsBcnData.fixedLocationLat) == 0) && (isnan(aprsBcnData.fixedLocationLon) == 0))));
}

static double aprsGetFixedPositionLatitude(void)
{
	double lat = 0.0;

	if (isnan(aprsBcnData.fixedLocationLat) == 0)
	{
		return aprsBcnData.fixedLocationLat;
	}
	else
	{
		if (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_CONFIG))
		{
			if (aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].flags & CODEPLUG_APRS_CONFIG_FLAG_USE_POSITION)
			{
				uint32_t tLat = aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].latitude[2];
				tLat = (tLat << 8) + aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].latitude[1];
				tLat = (tLat << 8) + aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].latitude[0];

				aprsBcnData.fixedLocationLat = latLonFixed24ToDouble(tLat);

				return aprsBcnData.fixedLocationLat;
			}
		}

		// Returns NAN if no position is valid, not even the one stored in the settings.
		if (settingsLocationIsValid() == false)
		{
			return NAN;
		}
		else
		{
			aprsBcnData.fixedLocationLat = lat = settingsLocationGetLatitude();
		}
	}

	return lat;
}

static double aprsGetFixedPositionLongitude(void)
{
	double lon = 0.0;

	if (isnan(aprsBcnData.fixedLocationLon) == 0)
	{
		return aprsBcnData.fixedLocationLon;
	}
	else
	{
		if (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_CONFIG))
		{
			if (aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].flags & CODEPLUG_APRS_CONFIG_FLAG_USE_POSITION)
			{
				uint32_t tLon = aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].longitude[2];
				tLon = (tLon << 8) + aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].longitude[1];
				tLon = (tLon << 8) + aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].longitude[0];

				aprsBcnData.fixedLocationLon = latLonFixed24ToDouble(tLon);

				return aprsBcnData.fixedLocationLon;
			}
		}

		if (settingsLocationIsValid() == false)
		{
			return NAN;
		}
		else
		{
			aprsBcnData.fixedLocationLon = lon = settingsLocationGetLongitude();
		}
	}

	return lon;
}

static double aprsGetBearingAngleDiff(double a, double b)
{
	double delta = fmod(fabs(a - b), 360.0);

	return (delta <= 180.0 ? delta : (360.0 - delta));
}

#if defined(APRS_USE_COURSETO_FOR_BEARING)
uint16_t aprsBeaconingGetBearing(void)
{
	if (distanceBetweenTwoCoords(aprsBcnData.previousBearingPosition.latitude, aprsBcnData.previousBearingPosition.longitude,
			aprsBcnData.currentLocation.coords.latitude, aprsBcnData.currentLocation.coords.longitude, false) > 2.0)
	{
		aprsBcnData.currentCourse = (uint16_t)(courseTo(aprsBcnData.previousBearingPosition.latitude, aprsBcnData.previousBearingPosition.longitude,
				aprsBcnData.currentLocation.coords.latitude, aprsBcnData.currentLocation.coords.longitude) * 1E2);

		memcpy(&aprsBcnData.previousBearingPosition, &aprsBcnData.currentLocation.coords, sizeof(aprsBeaconingCoordinates_t));
	}

	return aprsBcnData.currentCourse;
}
#endif // APRS_USE_COURSETO_FOR_BEARING

static bool aprsSmartBeaconingCornerPegging(double *currentSpeedMPS, uint32_t *timeDiff)
{
	*currentSpeedMPS = ((aprsBcnData.currentLocation.speed * 1E-2) * MPS_PER_KNOT);
	*timeDiff = (aprsBcnData.currentLocation.time - aprsBcnData.previousLocation.time);

	if (aprsBcnData.currentLocation.speed > SMART_BEACONING_SPEED_MIN)
	{
		if (aprsBeaconingLocationIsValid(&aprsBcnData.currentLocation) == false)
		{
			return false;
		}

		// if last bearing unknown, deploy turn_time
		if (aprsBeaconingLocationIsValid(&aprsBcnData.previousLocation) == false)
		{
			return ((*timeDiff / MILLISECS_PER_SEC) >= aprsBcnData.settings.smart.turnTime);
		}

		double turnDiff = aprsGetBearingAngleDiff((aprsBcnData.currentLocation.bearing * 1E-2), (aprsBcnData.previousLocation.bearing * 1E-2));
		double turnThreshold = MIN(120.0, ((aprsBcnData.settings.smart.turnAngle * 1.0) + ((aprsBcnData.settings.smart.turnSlope * 10.0) / (*currentSpeedMPS * MPS_TO_MPH))));

		return (((*timeDiff / MILLISECS_PER_SEC) >= ((uint32_t)aprsBcnData.settings.smart.turnTime)) && (turnDiff > turnThreshold));
	}

	return false;
}

static double aprsSmartBeaconingGetMaxSpeed(double currentSpeedMPS, uint32_t timeDiff)
{
    double dist = distanceBetweenTwoCoords(aprsBcnData.currentLocation.coords.latitude, aprsBcnData.currentLocation.coords.longitude, aprsBcnData.previousLocation.coords.latitude, aprsBcnData.previousLocation.coords.longitude, false);

    return MAX(MAX((dist / (timeDiff / MILLISECS_PER_SEC)), currentSpeedMPS), ((aprsBcnData.previousLocation.speed * 1E-2) * MPS_PER_KNOT));
}

static uint32_t aprsSmartBeaconingSpeedRate(double speedMPS)
{
	int32_t slowRate = (aprsBcnData.settings.smart.slowRate * 60); // min => s
	int32_t fastRate = aprsBcnData.settings.smart.fastRate; // s
	double lowSpeed = (aprsBcnData.settings.smart.lowSpeed * MPS_PER_KMPH); // => m/s
	double highSpeed = (aprsBcnData.settings.smart.highSpeed * MPS_PER_KMPH); // => m/s

	if (speedMPS <= lowSpeed)
	{
		return slowRate;
	}
	else if (speedMPS >= highSpeed)
	{
		return fastRate;
	}

	return (uint32_t)(fastRate + (slowRate - fastRate) * (highSpeed - speedMPS) / (highSpeed - lowSpeed));
}

static bool aprsSmartBeaconingCheck(void)
{
	double currentSpeedMPS;
	uint32_t timeDiff;

	if ((aprsBeaconingLocationIsValid(&aprsBcnData.previousLocation) == false) || aprsSmartBeaconingCornerPegging(&currentSpeedMPS, &timeDiff))
	{
		return true;
	}

	uint32_t speedRate = aprsSmartBeaconingSpeedRate(aprsSmartBeaconingGetMaxSpeed(currentSpeedMPS, timeDiff));

	// timediff in seconds.
	if ((timeDiff / MILLISECS_PER_SEC) >= speedRate)
	{
		return true;
	}

	return false;
}

static void aprsBeaconingDecayTick(bool reset)
{
	if ((aprsBcnData.settings.mode != APRS_BEACONING_MODE_SMART_BEACONING) && aprsBeaconingStateEnabled(APRS_BEACONING_STATE_DECAY_ALGO_ENABLED))
	{
		if (reset)
		{
			aprsBcnData.decayMult = APRS_BEACON_DECAY_MULT_MIN;
		}
		else if (aprsBcnData.decayMult < APRS_BEACON_DECAY_MULT_MAX)
		{
			aprsBcnData.decayMult *= APRS_BEACON_DECAY_MULT_STEP;
		}
	}
}

static void aprsBeaconingTxStateTick(uiEvent_t *ev)
{
	switch(aprsTxProgress)
	{
		case APRS_TX_IDLE:
		case APRS_TX_IN_PROGRESS:
			break;

		case APRS_TX_FINISHED:
			aprsTxEnded();// foreground parts of APRS send finished
			// deliberate fall through
		case APRS_TX_TERMINATE:
			trxTransmissionEnabled = false;
			// In analog mode. Stop transmitting immediately
			LedWrite(LED_RED, 0);
#if defined(HAS_GPS)
			if (SETTINGS_GPS_MODE_GET(nonVolatileSettings) > GPS_MODE_OFF)
			{
				gpsDataInputStartStop(true);
			}
#endif
			// Need to wrap this in Task Critical to avoid bus contention on the I2C bus.
			trxSetRxCSS(RADIO_DEVICE_PRIMARY, currentChannelData->rxTone);
			trxActivateRx(true);
			trxIsTransmitting = false;
			PTTToggledDown = false;
			aprsTxProgress = APRS_TX_WAIT_PTT_OFF;
			break;

		case APRS_TX_WAIT_PTT_OFF:
		case APRS_TX_FAILED_WAITING_TO_BEACON:
			if ((ev->buttons & BUTTON_PTT) == 0)
			{
				aprsTxProgress = APRS_TX_FINAL_STAGE;

				if (aprsBcnData.forcedManualBeaconing || (aprsBcnData.settings.mode == APRS_BEACONING_MODE_MANUAL))
				{
					aprsBcnData.forcedManualBeaconing = false;
				}
			}
			break;

		case APRS_TX_FINAL_STAGE:
			// Needs to restore frequency/CSS/Mode after an APRS's config QSY
			// That never happen in APRS_BEACONING_MODE_PTT or Satellite modes
			if (aprsBcnData.hasDoneQSY)
			{
				radioSetTRxDevice(RADIO_DEVICE_PRIMARY);

				if (menuSystemGetRootMenuNumber() == UI_CHANNEL_MODE)
				{
					uint32_t rxFreq = (uiDataGlobal.reverseRepeaterChannel ? currentChannelData->txFreq : currentChannelData->rxFreq);
					uint32_t txFreq = (uiDataGlobal.talkaround ? rxFreq : (uiDataGlobal.reverseRepeaterChannel ? currentChannelData->rxFreq : currentChannelData->txFreq));

					trxSetFrequency(rxFreq, txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && (uiDataGlobal.reverseRepeaterChannel || codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO))) ? DMR_MODE_DMO : DMR_MODE_AUTO));
				}
				else
				{
					uint32_t rxFreq = (uiDataGlobal.reverseRepeaterVFO ? currentChannelData->txFreq : currentChannelData->rxFreq);
					uint32_t txFreq = (uiDataGlobal.reverseRepeaterVFO ? currentChannelData->rxFreq : currentChannelData->txFreq);

					trxSetFrequency(rxFreq, txFreq, (((currentChannelData->chMode == RADIO_MODE_DIGITAL) && codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_FORCE_DMO)) ? DMR_MODE_DMO : DMR_MODE_AUTO));
				}

				trxSetModeAndBandwidth(currentChannelData->chMode, (codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_BW_25K) != 0));

				if (currentChannelData->chMode == RADIO_MODE_ANALOG)
				{
					trxSetRxCSS(RADIO_DEVICE_PRIMARY, currentChannelData->rxTone);
				}

				vTaskDelay((40U / portTICK_PERIOD_MS));

				if (currentChannelData->chMode == RADIO_MODE_DIGITAL)
				{
					HRC6000ResetTimeSlotDetection();
					HRC6000ClearActiveDMRID();
				}

				aprsBcnData.hasDoneQSY = false;
			}

			uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
			aprsTxProgress = APRS_TX_IDLE;
			break;
	}
}

void aprsBeaconingInit(void)
{
	aprsBcnData.decayMult = APRS_BEACON_DECAY_MULT_MIN;
	aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_ENABLED, false);
	aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_HAS_GPS_FIX, false);
}

void aprsBeaconingStart(void)
{
	if (aprsBcnData.settings.mode != APRS_BEACONING_MODE_OFF)
	{
		aprsBeaconingResetTimers();
		aprsBeaconingInvalidateLocation(&aprsBcnData.currentLocation);
		aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_ENABLED, true);
		aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_HAS_GPS_FIX, false);
	}
}

void aprsBeaconingStop(void)
{
	if (aprsBcnData.settings.mode != APRS_BEACONING_MODE_OFF)
	{
		if (aprsTxProgress != APRS_TX_IDLE)
		{
#if defined(PLATFORM_MD9600)
			uint16_t frontPanelButtons;
			uiEvent_t ev = { .buttons = buttonsRead(&frontPanelButtons, uiDataGlobal.sk2latched), .keys = NO_KEYCODE, .rotary = 0, .function = 0, .events = NO_EVENT, .hasEvent = false, .time = ticksGetMillis() };
#else
			uiEvent_t ev = { .buttons = buttonsRead(), .keys = NO_KEYCODE, .rotary = 0, .function = 0, .events = NO_EVENT, .hasEvent = false, .time = ticksGetMillis() };
#endif

			aprsTxProgress = APRS_TX_FINISHED;
			aprsBeaconingTxStateTick(&ev);
		}

		aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_ENABLED, false);
		ticksTimerReset(&aprsBcnData.checkTimer);
	}
}

void aprsBeaconingResetTimers(void)
{
	if ((uiDataGlobal.Scan.active == false) && (aprsBcnData.settings.mode != APRS_BEACONING_MODE_OFF))
	{
		uint8_t APRSConfigIndex = currentChannelData->aprsConfigIndex;
		bool    hasAPRSCfg = false;

#if defined(RATE_MESSAGE_FEATURE)
		aprsBcnData.rateMessageCount = 0U;
#endif
		aprsBcnData.decayMult = APRS_BEACON_DECAY_MULT_MIN;

		aprsBeaconingInvalidateLocation(&aprsBcnData.previousLocation);
		aprsBeaconingInvalidateFixedPosition();

		ticksTimerStart(&aprsBcnData.checkTimer, MILLISECS_PER_SEC); // 1s timer
		ticksTimerReset(&aprsBcnData.nextBeaconTimer);

		if (APRSConfigIndex != 0)
		{
			if (codeplugAPRSGetDataForIndex(APRSConfigIndex, &aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL]))
			{
				hasAPRSCfg = true;
			}
		}

		aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_HAS_APRS_CONFIG, hasAPRSCfg);
	}

	if (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_SATELLITE_CONFIG))
	{
		aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_HAS_APRS_SATELLITE_CONFIG, false);
	}
}

void aprsBeaconingPrepareSatelliteConfig(void)
{
	bool hasAprsConfig = false;
	int APRSConfigIndex = codeplugAPRSGetIndexOfName(currentActiveSatellite->name);

	if (APRSConfigIndex != 0)
	{
		if (codeplugAPRSGetDataForIndex(APRSConfigIndex, &aprsBcnData.aprsConfig[APRS_CONFIG_SATELLITE]))
		{
			hasAprsConfig = true;
		}
	}
	else
	{
		if (satelliteDataNative[uiDataGlobal.SatelliteAndAlarmData.currentSatellite].AdditionalData[0] != 0)
		{
			aprsBcnData.aprsConfig[APRS_CONFIG_SATELLITE].senderSSID = 7U;
			aprsBcnData.aprsConfig[APRS_CONFIG_SATELLITE].comment[0] = 0U;
			aprsBcnData.aprsConfig[APRS_CONFIG_SATELLITE].iconTable = 0U;
			aprsBcnData.aprsConfig[APRS_CONFIG_SATELLITE].iconIndex = ('0' - '!');

			memcpy(aprsBcnData.aprsConfig[APRS_CONFIG_SATELLITE].paths[0].name, &satelliteDataNative[uiDataGlobal.SatelliteAndAlarmData.currentSatellite].AdditionalData[0], 6U);
			aprsBcnData.aprsConfig[APRS_CONFIG_SATELLITE].paths[0].SSID = satelliteDataNative[uiDataGlobal.SatelliteAndAlarmData.currentSatellite].AdditionalData[6] - '0';
			memcpy(aprsBcnData.aprsConfig[APRS_CONFIG_SATELLITE].paths[1].name, &satelliteDataNative[uiDataGlobal.SatelliteAndAlarmData.currentSatellite].AdditionalData[7], 6U);
			aprsBcnData.aprsConfig[APRS_CONFIG_SATELLITE].paths[1].SSID = satelliteDataNative[uiDataGlobal.SatelliteAndAlarmData.currentSatellite].AdditionalData[13] - '0';

			hasAprsConfig = true;
		}
	}

	aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_HAS_APRS_SATELLITE_CONFIG, hasAprsConfig);

	aprsBeaconingSetCurrentLocation(settingsLocationGetLatitude(), settingsLocationGetLongitude());

	// Ensure to get fixed position updated, when entering the Satellite
	// screen, as aprsBeaconingTick() could invalidate it when Satellite screen
	// isn't running.
	if (hasAprsConfig)
	{
		(void)aprsGetFixedPositionLatitude();
		(void)aprsGetFixedPositionLongitude();
	}
}

void aprsBeaconingSetCurrentLocation(double lat, double lon)
{
	aprsBcnData.currentLocation.coords.latitude = lat;
	aprsBcnData.currentLocation.coords.longitude = lon;
}

void aprsBeaconingInvalidateFixedPosition(void)
{
	aprsBcnData.fixedLocationLat = aprsBcnData.fixedLocationLon = NAN;
}

bool aprsBeaconingIsSuspended(void)
{
	return ((aprsBcnData.settings.mode != APRS_BEACONING_MODE_OFF) && (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_ENABLED) == false));
}

void aprsBeaconingSetSuspend(bool suspend)
{
	if (aprsBcnData.settings.mode != APRS_BEACONING_MODE_OFF)
	{
		if (suspend)
		{
			aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_ENABLED, false);
		}
		else
		{
			if (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_ENABLED) == false)
			{
				//aprsBcnData.decayMult = APRS_BEACON_DECAY_MULT_MIN;
				aprsBcnData.beaconingFailureCounter = 0U;
				ticksTimerStart(&aprsBcnData.checkTimer, MILLISECS_PER_SEC); // 1s timer
				aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_ENABLED, true);
			}
		}
	}
}

void aprsBeaconingToggles(void)
{
	if (aprsBcnData.settings.mode != APRS_BEACONING_MODE_OFF)
	{
		char buf[SCREEN_LINE_BUFFER_SIZE];
		bool running = aprsBeaconingStateEnabled(APRS_BEACONING_STATE_ENABLED);

		aprsBeaconingSetSuspend(running);

		snprintf(buf, SCREEN_LINE_BUFFER_SIZE, "Beaconing: %s", (running ? currentLanguage->off : currentLanguage->on));
		uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 1000, buf, false);
	}
}

aprsBeaconingMode_t aprsBeaconingGetMode(void)
{
	return aprsBcnData.settings.mode;
}

// Returns true if the beacon has been sent.
bool aprsBeaconingSendBeacon(bool fromSatScreen, bool forcedManualBeaconing)
{
	aprsChannelSettingsInUse_t channelSettings;

	if (uiDataGlobal.Scan.active ||
			(trxGetMode() == RADIO_MODE_NONE) ||
			((trxGetMode() == RADIO_MODE_DIGITAL) && (aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].txFrequency == 0U)) ||
			settingsIsOptionBitSet(BIT_TX_INHIBIT) ||
			(fromSatScreen && (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_SATELLITE_CONFIG) == false)) ||
			((fromSatScreen == false) &&
					((aprsBcnData.settings.mode == APRS_BEACONING_MODE_OFF)
							|| (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_ENABLED) == false)
							|| (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_CONFIG) == false)
					)
			)
			|| (aprsTxProgress != APRS_TX_IDLE))
	{
		return false;
	}

	// Ignore PTT rate when in satellite mode.
	if ((fromSatScreen == false) && (aprsBcnData.settings.mode == APRS_BEACONING_MODE_PTT))
	{
		if ((ticksTimerHasExpired(&aprsBcnData.nextBeaconTimer) == false)
				|| (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_CONFIG) == false))
		{
			return false;
		}
	}

	// Copy currentChannelData settings, as it could change because of QSY
	channelSettings.chMode = currentChannelData->chMode;
	channelSettings.rxFreq = currentChannelData->rxFreq;
	channelSettings.txFreq = currentChannelData->txFreq;
	channelSettings.rxTone = currentChannelData->rxTone;
	channelSettings.wideBW = trxGetBandwidthIs25kHz();

	/// Build and Send beacon Here
#if defined(RATE_MESSAGE_FEATURE)
	if (((aprsBcnData.rateMessageCount % aprsBcnData.settings.messageInterval) == 0))
	{

	}
#endif

	bool anyManualMode = (fromSatScreen || forcedManualBeaconing || (aprsBcnData.settings.mode == APRS_BEACONING_MODE_PTT) || (aprsBcnData.settings.mode == APRS_BEACONING_MODE_MANUAL));

	// check config validity
	if (anyManualMode)
	{
		char buffer[SCREEN_LINE_BUFFER_SIZE];
		bool configIsValid = ((fromSatScreen && aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_SATELLITE_CONFIG)) ||
				((fromSatScreen == false) && aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_CONFIG)));
		bool positionIsValid = (fromSatScreen ? false : aprsBeaconingCurrentPositionIsValid());

		aprsBcnData.forcedManualBeaconing = forcedManualBeaconing;

		if (fromSatScreen && (positionIsValid == false))
		{
			// Use current position (from settings and/or GPS)
			if (settingsLocationIsValid())
			{
				aprsBeaconingSetCurrentLocation(settingsLocationGetLatitude(), settingsLocationGetLongitude());
				positionIsValid = true;
			}
		}

		snprintf(buffer, SCREEN_LINE_BUFFER_SIZE, "%s%c",
				((configIsValid && positionIsValid) ? "APRS Tx" : (positionIsValid ? currentLanguage->APRS : currentLanguage->location)),
				((configIsValid && positionIsValid) ? '\0' : '?'));

		if (configIsValid && positionIsValid)
		{
			int16_t yPos = (fromSatScreen ?
#if defined(HAS_COLOURS)
					(DISPLAY_SIZE_Y / 4)
#else
					DISPLAY_Y_POS_CONTACT
#endif
					: DISPLAY_Y_POS_TX_TIMER);
			ucFont_t font = (fromSatScreen ? FONT_SIZE_3 : FONT_SIZE_4);
			int16_t fontHeight = (fromSatScreen ? FONT_SIZE_3_HEIGHT : FONT_SIZE_4_HEIGHT);
#if ! defined(HAS_COLOURS)
			int16_t tBorder = (fromSatScreen ? 0 : 6);
			int16_t bBorder = (fromSatScreen ? -1 : 12);
#endif

			displayThemeApply(THEME_ITEM_FG_TX_COUNTER, THEME_ITEM_BG);
#if defined(HAS_COLOURS)
			displayFillRect(0, yPos, DISPLAY_SIZE_X, fontHeight, true);
#else
			displayFillRect(0, (yPos + tBorder), DISPLAY_SIZE_X, (fontHeight - bBorder), true);
#endif
			displayPrintCentered(yPos, buffer, font);
			displayThemeResetToDefault();
			displayRender();
		}
		else
		{
			uiNotificationShow(NOTIFICATION_TYPE_MESSAGE, NOTIFICATION_ID_MESSAGE, 1500, buffer, true);
			soundSetMelody(MELODY_ERROR_BEEP);
			return false;
		}
	}

	if (fromSatScreen == false)
	{
		bool aprsConfigHasFrequencyQSY = (aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].txFrequency != 0U);
		uint32_t txFrequency = (aprsConfigHasFrequencyQSY ? aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].txFrequency : currentChannelData->txFreq);

		// toggle xmit
		switch (aprsBcnData.settings.mode)
		{
			case APRS_BEACONING_MODE_MANUAL:
			case APRS_BEACONING_MODE_AUTO:
			case APRS_BEACONING_MODE_SMART_BEACONING:
				if ((txFrequency != 0U)
						&&
						(aprsConfigHasFrequencyQSY ? true : (codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_RX_ONLY) == 0))
						&&
						((nonVolatileSettings.txFreqLimited == BAND_LIMITS_NONE) || trxCheckFrequencyInAmateurBand(txFrequency)
#if defined(PLATFORM_MD9600)
								|| (aprsConfigHasFrequencyQSY ? true : (codeplugChannelGetFlag(currentChannelData, CHANNEL_FLAG_OUT_OF_BAND) != 0))
#endif
						)
						&&
						aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_CONFIG))
				{
					rxPowerSavingSetState(ECOPHASE_POWERSAVE_INACTIVE);

#if defined(HAS_GPS)
					if (SETTINGS_GPS_MODE_GET(nonVolatileSettings) > GPS_MODE_OFF)
					{
						gpsDataInputStartStop(false);
					}
#endif

					watchdogRun(false);

					// Needs to QSY ?
					// The current channel configuration will be restored
					// once the frame is sent (see aprsBeaconingTick())
					if (aprsConfigHasFrequencyQSY)
					{
						radioSetTRxDevice(RADIO_DEVICE_PRIMARY);

						trxSetFrequency(txFrequency, txFrequency, DMR_MODE_AUTO);
						trxSetModeAndBandwidth(RADIO_MODE_ANALOG, false);
						trxSetRxCSS(RADIO_DEVICE_PRIMARY, CODEPLUG_CSS_TONE_NONE);
						trxSetTxCSS(CODEPLUG_CSS_TONE_NONE);

						aprsBcnData.hasDoneQSY = true;

						vTaskDelay((40U / portTICK_PERIOD_MS));
					}

					// Avoid collision.
					uint32_t waitingForXmit = (APRS_ANTI_COLLISION_MS_MAX / RSSI_NOISE_SAMPLE_PERIOD_PIT);
					while((waitingForXmit > 0) && (LedRead(LED_GREEN) || ((audioAmpGetStatus() & AUDIO_AMP_CHANNEL_RF) != 0)))
					{
						vTaskDelay((RSSI_NOISE_SAMPLE_PERIOD_PIT / portTICK_PERIOD_MS));
						waitingForXmit--;
						trxCheckAnalogSquelch();
					}

					watchdogRun(true);

					if (waitingForXmit == 0U)
					{
#if defined(HAS_GPS)
						if (SETTINGS_GPS_MODE_GET(nonVolatileSettings) > GPS_MODE_OFF)
						{
							gpsDataInputStartStop(true);
						}
#endif
						aprsTxProgress = APRS_TX_FAILED_WAITING_TO_BEACON;

						return false;
					}

					// Temporary change the TX power output, if needed, and only if
					// the QSY is enabled in the APRS config.
					// The power will be set back to the "normal" value when
					// restoring the frequency.
					if (aprsBcnData.hasDoneQSY && (aprsBcnData.settings.power != 0U))
					{
						trxSetPowerFromLevel((aprsBcnData.settings.power - 1U));
					}

					LedWrite(LED_GREEN, 0);
					LedWrite(LED_RED, 1);

					HRC6000ClearIsWakingState();
					trxSetTX();

					// TX Delay;
					uint32_t m = ticksGetMillis();
					while((ticksGetMillis() - m) < APRS_XMIT_TX_DELAY)
					{
						vTaskDelay((1U / portTICK_PERIOD_MS));
					}
				}
				else
				{
					return false;
				}
				break;

			default:
				break;

		}
	}

	aprsSendPacket(&aprsBcnData.aprsConfig[(fromSatScreen ? APRS_CONFIG_SATELLITE : APRS_CONFIG_CHANNEL)], &aprsBcnData.previousLocation, &aprsBcnData.currentLocation, &channelSettings, fromSatScreen);

#if defined(RATE_MESSAGE_FEATURE)
	aprsBcnData.rateMessageCount = (aprsBcnData.rateMessageCount + 1) % aprsBcnData.settings.messageInterval;
#endif

	if (anyManualMode)
	{
		if (aprsBcnData.settings.mode == APRS_BEACONING_MODE_PTT)
		{
			ticksTimerStart(&aprsBcnData.nextBeaconTimer, (initialIntervalsInSecs[aprsBcnData.settings.initialInterval] * MILLISECS_PER_SEC));
		}

		memcpy(&aprsBcnData.previousLocation, &aprsBcnData.currentLocation, sizeof(aprsBeaconingLocation_t));
	}

	return true;
}

bool aprsBeaconingIsTransmitting(void)
{
	return (((aprsTxProgress == APRS_TX_IDLE) && (aprsBcnData.hasDoneQSY == false)) == false);
}

bool aprsBeaconingForcedManualBeaconingTriggered(void)
{
	return aprsBcnData.forcedManualBeaconing;
}

bool aprsBeaconingOnDifferentFrequency(void)
{
	return ((settingsIsOptionBitSet(BIT_TX_INHIBIT) == false) &&
			(aprsBcnData.settings.mode != APRS_BEACONING_MODE_OFF) &&
			aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_CONFIG) &&
			(aprsBcnData.aprsConfig[APRS_CONFIG_CHANNEL].txFrequency != 0U));
}

bool aprsBeaconingHasSatelliteConfiguration(void)
{
	return (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_SATELLITE_CONFIG));
}

void aprsBeaconingUpdateConfigurationFromSystemSettings(void)
{
	// Unpack
	aprsBcnData.settings.mode            = ((nonVolatileSettings.aprsBeaconingSettingsPart1[0] >> 29) & 0x7);
	aprsBcnData.settings.smart.lowSpeed  = ((nonVolatileSettings.aprsBeaconingSettingsPart1[0] >> 24) & 0x1F);
	aprsBcnData.settings.smart.fastRate  = ((nonVolatileSettings.aprsBeaconingSettingsPart1[0] >> 16) & 0xFF);
	aprsBcnData.settings.smart.turnSlope = ((nonVolatileSettings.aprsBeaconingSettingsPart1[0] >> 8) & 0xFF);
	aprsBcnData.settings.smart.turnTime  =  (nonVolatileSettings.aprsBeaconingSettingsPart1[0] & 0xFF);

	uint8_t messageInterval = 0;

	messageInterval                      = (((nonVolatileSettings.aprsBeaconingSettingsPart1[1] >> 21) & 0x1F) + APRS_BEACON_MESSAGE_INTERVAL_MIN);
	aprsBcnData.settings.smart.slowRate  = ((nonVolatileSettings.aprsBeaconingSettingsPart1[1] >> 14) & 0x7F);
	aprsBcnData.settings.smart.highSpeed = ((nonVolatileSettings.aprsBeaconingSettingsPart1[1] >> 7) & 0x7F);
	aprsBcnData.settings.smart.turnAngle =  (nonVolatileSettings.aprsBeaconingSettingsPart1[1] & 0x7F);

#if defined(RATE_MESSAGE_FEATURE)
	aprsBcnData.settings.messageInterval = messageInterval;
#else
	(void)messageInterval;
#endif

	aprsBcnData.settings.power = ((nonVolatileSettings.aprsBeaconingSettingsPart2 >> 12) & 0x0F);
	aprsBcnData.settings.state = ((nonVolatileSettings.aprsBeaconingSettingsPart2 >> 4) & 0xFF);
	aprsBcnData.settings.initialInterval = (nonVolatileSettings.aprsBeaconingSettingsPart2 & 0x0F);
}

void aprsBeaconingUpdateSystemSettingsFromConfiguration(void)
{
	uint8_t messageInterval = APRS_BEACON_MESSAGE_INTERVAL_MIN;
	uint16_t v16;
	uint32_t v32;

	// Pack
	v32 = (((aprsBcnData.settings.mode & 0x7) << 29) | ((aprsBcnData.settings.smart.lowSpeed & 0x1F) << 24) |
			(aprsBcnData.settings.smart.fastRate << 16) | (aprsBcnData.settings.smart.turnSlope << 8) |
			aprsBcnData.settings.smart.turnTime);

	settingsSet(nonVolatileSettings.aprsBeaconingSettingsPart1[0], v32);

#if defined(RATE_MESSAGE_FEATURE)
	messageInterval = (aprsBcnData.settings.messageInterval - APRS_BEACON_MESSAGE_INTERVAL_MIN);
#endif

	v32 = (((messageInterval & 0x1F) << 21) | ((aprsBcnData.settings.smart.slowRate & 0x7F) << 14) |
			((aprsBcnData.settings.smart.highSpeed & 0x7F) << 7) | (aprsBcnData.settings.smart.turnAngle & 0x7F));

	settingsSet(nonVolatileSettings.aprsBeaconingSettingsPart1[1], v32);

	v16 = (((aprsBcnData.settings.power & 0x0F) << 12) | ((aprsBcnData.settings.state & 0xFF) << 4) | (aprsBcnData.settings.initialInterval & 0x0F));

	settingsSet(nonVolatileSettings.aprsBeaconingSettingsPart2, v16);
}

void aprsBeaconingGetSettings(aprsBeaconingSettings_t *dest)
{
	memcpy(dest, &aprsBcnData.settings, sizeof(aprsBeaconingSettings_t));
}

void aprsBeaconingSetSettings(aprsBeaconingSettings_t *src)
{
	if (memcmp(src, &aprsBcnData.settings, sizeof(aprsBeaconingSettings_t)) != 0)
	{
		aprsBeaconingStop();
		memcpy(&aprsBcnData.settings, src, sizeof(aprsBeaconingSettings_t));
		aprsBeaconingUpdateSystemSettingsFromConfiguration();
		aprsBeaconingStart();
	}
}

void aprsBeaconingTick(uiEvent_t *ev)
{
	if (aprsTxProgress != APRS_TX_IDLE)
	{
		aprsBeaconingTxStateTick(ev);
		return;
	}

	int currentMenu = menuSystemGetCurrentMenuNumber();

	// Do not automatically beaconing while in Satellite or TX screens
	if ((currentMenu == MENU_SATELLITE) || (currentMenu == UI_TX_SCREEN) || (currentMenu == UI_HOTSPOT_MODE) || (currentMenu == UI_CPS))
	{
		return;
	}

	if (((uiDataGlobal.Scan.active == false) && (aprsBcnData.settings.mode != APRS_BEACONING_MODE_OFF) && aprsBeaconingStateEnabled(APRS_BEACONING_STATE_ENABLED))
			&& aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_APRS_CONFIG) &&
			(settingsIsOptionBitSet(BIT_TX_INHIBIT) == false))
	{
		if (ticksTimerHasExpired(&aprsBcnData.checkTimer))
		{
			bool locFromGPS = aprsBeaconingStateEnabled(APRS_BEACONING_STATE_LOCATION_FROM_GPS);
			bool gpsHasPVT = (locFromGPS ? gpsPVTIsValid() : false);
			bool forceEntering = (locFromGPS ? ((isnan(aprsBcnData.fixedLocationLat) == 0) ? false : true) : true); // every second check when GPS is OFF
			uint32_t rearmTimerMilliseconds = MILLISECS_PER_SEC;

			if (gpsHasPVT || forceEntering)
			{
				bool gpsFix             = (locFromGPS ? gpsFixIsValid() : true);
				bool checkForBeaconing  = (locFromGPS ? ((isnan(aprsBcnData.fixedLocationLat) == 0) ? gpsFix : true) : true);

				// Reset stored location when the Fix is lost or just acquired
				if (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_LOCATION_FROM_GPS))
				{
					if (((gpsFix == false) && aprsBeaconingLocationIsValid(&aprsBcnData.previousLocation)) ||
							(gpsFix && (aprsBeaconingStateEnabled(APRS_BEACONING_STATE_HAS_GPS_FIX) == false)))
					{
						aprsBeaconingInvalidateLocation(&aprsBcnData.previousLocation);
						aprsBeaconingStateSetEnable(APRS_BEACONING_STATE_HAS_GPS_FIX, gpsFix);
					}
				}

				if (checkForBeaconing)
				{
					aprsBcnData.currentLocation.time = ticksGetMillis();

					if (locFromGPS)
					{
						aprsBcnData.currentLocation.coords.latitude  =
#if defined(HAS_GPS)
								gpsFix ? gpsData.LatitudeHiRes :
#endif
										aprsGetFixedPositionLatitude();

						aprsBcnData.currentLocation.coords.longitude =
#if defined(HAS_GPS)
								gpsFix ? gpsData.LongitudeHiRes :
#endif
										aprsGetFixedPositionLongitude();

						aprsBcnData.currentLocation.bearing          =
#if defined(HAS_GPS)
#if defined(APRS_USE_COURSETO_FOR_BEARING)
								(gpsFix && (gpsData.SpeedInHundredthKn > SMART_BEACONING_SPEED_MIN)) ? aprsBeaconingGetBearing() :
#else
								(gpsFix && (gpsData.SpeedInHundredthKn > SMART_BEACONING_SPEED_MIN)) ? gpsData.CourseInHundredthDeg :
#endif
#endif
										0U;
						aprsBcnData.currentLocation.speed            =
#if defined(HAS_GPS)
								(gpsFix && (gpsData.SpeedInHundredthKn > GPS_SPEED_THRESHOLD_MIN)) ? gpsData.SpeedInHundredthKn :
#endif
										0U;
					}
					else
					{
						aprsBcnData.currentLocation.coords.latitude  = aprsGetFixedPositionLatitude();
						aprsBcnData.currentLocation.coords.longitude = aprsGetFixedPositionLongitude();
						aprsBcnData.currentLocation.bearing          = 0U;
						aprsBcnData.currentLocation.speed            = 0U; // Fixed position
					}

					// No position is usable (GPS/Channel and settings ones), don't do anything for now.
					if (isnan(aprsBcnData.currentLocation.coords.latitude) != 0)
					{
						aprsBeaconingInvalidateLocation(&aprsBcnData.currentLocation);
						aprsBeaconingInvalidateLocation(&aprsBcnData.previousLocation);
						aprsBeaconingInvalidateFixedPosition();
						goto exitRearmTimer;
					}

					if (aprsBcnData.triggerBeaconing == false)
					{
						if (aprsBcnData.settings.mode == APRS_BEACONING_MODE_SMART_BEACONING)
						{
							if (aprsSmartBeaconingCheck())
							{
								aprsBcnData.triggerBeaconing = true;
							}
						}
						else if ((aprsBcnData.settings.mode == APRS_BEACONING_MODE_AUTO) && ticksTimerHasExpired(&aprsBcnData.nextBeaconTimer))
						{
							aprsBcnData.triggerBeaconing = true;
						}
					}
				}
			}

			if (aprsBcnData.triggerBeaconing)
			{
				bool beaconSent = false;

				// in MANUAL and PTT modes, we just need the updated position (even on fixed one, as aprsBeaconingResetTimers() also invalidate positions).
				if ((aprsBcnData.settings.mode == APRS_BEACONING_MODE_AUTO) || (aprsBcnData.settings.mode == APRS_BEACONING_MODE_SMART_BEACONING))
				{
					beaconSent = aprsBeaconingSendBeacon(false, false);

					if (beaconSent && (aprsBcnData.settings.mode != APRS_BEACONING_MODE_SMART_BEACONING)) // Pure time driven
					{
						double dist = 0.0;

						if (aprsBeaconingLocationIsValid(&aprsBcnData.currentLocation) && aprsBeaconingLocationIsValid(&aprsBcnData.previousLocation))
						{
							dist = distanceBetweenTwoCoords(aprsBcnData.currentLocation.coords.latitude, aprsBcnData.currentLocation.coords.longitude, aprsBcnData.previousLocation.coords.latitude, aprsBcnData.previousLocation.coords.longitude, false);
						}

						ticksTimerStart(&aprsBcnData.nextBeaconTimer, ((initialIntervalsInSecs[aprsBcnData.settings.initialInterval] * MILLISECS_PER_SEC) * aprsBcnData.decayMult));

						// Reset the Decay mult if we moved outside a radius of APRS_BEACON_DECAY_RESET_DISTANCE_MIN meters.
						aprsBeaconingDecayTick((dist > APRS_BEACON_DECAY_RESET_DISTANCE_MIN));
					}
				}

				// store current position as previous one for the next run
				if (beaconSent)
				{
					aprsBcnData.triggerBeaconing = false;
					memcpy(&aprsBcnData.previousLocation, &aprsBcnData.currentLocation, sizeof(aprsBeaconingLocation_t));
					aprsBcnData.beaconingFailureCounter = 0U;
				}
				else
				{
					if (aprsTxProgress == APRS_TX_FAILED_WAITING_TO_BEACON)
					{
						aprsBcnData.beaconingFailureCounter = SAFE_MIN((aprsBcnData.beaconingFailureCounter + 1), APRS_BEACONING_FAILURE_MAX);
						rearmTimerMilliseconds = (MILLISECS_PER_SEC * (aprsBcnData.beaconingFailureCounter * 2));
					}
				}
			}

			exitRearmTimer:
			ticksTimerStart(&aprsBcnData.checkTimer, rearmTimerMilliseconds); // 1s timer
		}
	}
}
#endif // PLATFORM_GD77S

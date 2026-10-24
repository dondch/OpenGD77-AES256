/*
 * Copyright (C) 2026 TA2TWC
 *
 * Based on the OpenGD77 firmware
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

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "user_interface/uiLocalisation.h"
#include "user_interface/uiCustomStrings.h"

/*
 * The texts of the screens which are not part of OpenGD77.
 *
 * They can't be added to stringsTable_t: the language files (.gla) written by the CPS are a copy of
 * that structure, changing it would make all of them unusable.
 *
 * Like the language files, the Turkish texts are using the characters set of the fonts, not UTF-8:
 * 0xFC = u with diaeresis, 0xE7 = c with cedilla, 0xF6 = o with diaeresis, 0x93 = dotless i, 0x96 = s with cedilla
 */

typedef enum
{
	CUSTOM_LANGUAGE_ENGLISH = 0,
	CUSTOM_LANGUAGE_TURKISH,
	CUSTOM_LANGUAGE_MAX
} customLanguage_t;

static const char *CUSTOM_STRINGS[CUSTOM_LANGUAGE_MAX][CUSTOM_STRING_MAX] =
{
		[CUSTOM_LANGUAGE_ENGLISH] =
		{
				[CUSTOM_STRING_BOOT]  = "BOOT",
				[CUSTOM_STRING_READY] = "READY",
				[CUSTOM_STRING_POWER] = "PWR",
				[CUSTOM_STRING_NOISE] = "N",
				[CUSTOM_STRING_RAW]   = "RAW",
				[CUSTOM_STRING_SCAN]  = "SCAN",
				[CUSTOM_STRING_MAIN_SCREEN] = "Main scr",
				[CUSTOM_STRING_NEW]   = "New",
				[CUSTOM_STRING_CLASSIC] = "Classic",
				[CUSTOM_STRING_SHUTDOWN] = "POWER OFF",
		},
		[CUSTOM_LANGUAGE_TURKISH] =
		{
				[CUSTOM_STRING_BOOT]  = "A\xE7\x93l\x93\x96",
				[CUSTOM_STRING_READY] = "HAZIR",
				[CUSTOM_STRING_POWER] = "G\xFC\xE7",
				[CUSTOM_STRING_NOISE] = "G",
				[CUSTOM_STRING_RAW]   = "HAM",
				[CUSTOM_STRING_SCAN]  = "TARA",
				[CUSTOM_STRING_MAIN_SCREEN] = "Ana ekran",
				[CUSTOM_STRING_NEW]   = "Yeni",
				[CUSTOM_STRING_CLASSIC] = "Klasik",
				[CUSTOM_STRING_SHUTDOWN] = "KAPANIYOR",
		}
};

// Beginning of the name of the Turkish language file
static const char TURKISH_LANGUAGE_NAME[] = "T\xFCrk";

const char *uiCustomStringGet(customString_t id)
{
	customLanguage_t language = CUSTOM_LANGUAGE_ENGLISH;

	if (id >= CUSTOM_STRING_MAX)
	{
		return "";
	}

	if ((currentLanguage != NULL) && (strncmp(currentLanguage->LANGUAGE_NAME, TURKISH_LANGUAGE_NAME, (sizeof(TURKISH_LANGUAGE_NAME) - 1)) == 0))
	{
		language = CUSTOM_LANGUAGE_TURKISH;
	}

	return CUSTOM_STRINGS[language][id];
}

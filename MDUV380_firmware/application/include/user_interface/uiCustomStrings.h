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

#ifndef _OPENGD77_UICUSTOMSTRINGS_H_
#define _OPENGD77_UICUSTOMSTRINGS_H_

typedef enum
{
	CUSTOM_STRING_BOOT = 0,
	CUSTOM_STRING_READY,
	CUSTOM_STRING_POWER,  // 3 characters
	CUSTOM_STRING_NOISE,  // 1 character
	CUSTOM_STRING_RAW,
	CUSTOM_STRING_SCAN,         // 4 characters max
	CUSTOM_STRING_MAIN_SCREEN,  // 9 characters max
	CUSTOM_STRING_NEW,          // 6 characters max
	CUSTOM_STRING_CLASSIC,      // 6 characters max
	CUSTOM_STRING_SHUTDOWN,     // 12 characters max
	CUSTOM_STRING_MAX
} customString_t;

// Returns the text in the language currently selected on the radio (Turkish if the Turkish language file is the one in use, English otherwise).
const char *uiCustomStringGet(customString_t id);

#endif // _OPENGD77_UICUSTOMSTRINGS_H_

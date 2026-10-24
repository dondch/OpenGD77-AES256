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

#ifndef _OPENGD77_UIDUALSCREEN_H_
#define _OPENGD77_UIDUALSCREEN_H_

#include "user_interface/menuSystem.h"

#if defined(HAS_DUAL_WATCH_OPTIONS)
bool uiDualScreenIsEnabled(void);
bool uiDualScreenCanDraw(void);                    // option on and a quiet screen state (no TX, no channel details...)
void uiDualScreenDraw(void);                       // the two rows, A on top and B below
void uiDualScreenScreenEntered(bool channelScreen); // call when the VFO / channel screen is (re)entered
void uiDualScreenWatchTick(void);                  // row watch, call from the main loop
void uiDualScreenWatchAbort(void);                 // back to the active row now (before a transmission)
bool uiDualScreenWatchIsTunedAway(void);           // the radio is on the other row right now
bool uiDualScreenWatchIsPeeking(void);             // within the short look at the other row (the squelch must not run)
bool uiDualScreenRxStateChanged(void);             // true when a signal started / stopped being received: the screen has to be redrawn
// Up/Down arrows select the other row, the red key switches the active row between VFO and channel.
// Returns true when the key has been used. 'busy' = the screen is doing something else (entering digits...).
bool uiDualScreenHandleKey(uiEvent_t *ev, bool channelScreen, bool busy);
#endif

#endif // _OPENGD77_UIDUALSCREEN_H_

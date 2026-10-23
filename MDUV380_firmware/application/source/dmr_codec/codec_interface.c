/*
 * Copyright (C) 2019 Kai Ludwig, DG4KLU
 *           (C) 2020-2025 Roger Clark, VK3KYY / G4KYF
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

#include "dmr_codec/codec.h"
#include "functions/voicePrompts.h"
#include "crypto/dmr_aes_hook.h"
#include <string.h>

/* The codec call sites below hand arguments to the precompiled AMBE blob through
 * `register int rN asm("rN")` variables that the inline asm uses BY NAME but never
 * declares as asm operands. Under -Os our toolchain treats those `rN = (int)buf`
 * assignments as dead stores and ELIDES them, so the codec is entered with garbage
 * pointers in r0/r1/r2 -> hard fault on every DMR voice frame (the official build's
 * toolchain/flags happen to keep them). Build this file unoptimized so the register
 * setup is preserved exactly as written. */
#pragma GCC optimize ("O0")

static uint16_t bitbuffer_encode[72];

void codecDecode(uint8_t *indata_ptr, int numbBlocks)
{
	uint16_t bitbuffer_decode[49];


	register int r0 asm ("r0") __attribute__((unused));
	register int r1 asm ("r1") __attribute__((unused));
	register int r2 asm ("r2") __attribute__((unused));

    for (int idx = 0; idx < numbBlocks; idx++)
    {
		initFrame(indata_ptr, bitbuffer_decode);
		indata_ptr += 9;

		// AES: decrypt the 49 decoded AMBE voice bits in place (no-op unless ENABLE_AES
		// and an active encrypted stream). This is the correct layer — validated vs DSD-FME.
		dmrAesRxCodecFrame(bitbuffer_decode, idx);

		soundSetupBuffer();// this just sets currentWaveBuffer but the compiler seems to optimise out the code if I try to do it in this file
		r2 = (int)bitbuffer_decode;
		r0 = (int)currentWaveBuffer;
		r1 = (int)ambebuffer_decode;

		asm volatile (
			"PUSH {R4-R11}\n"
			"SUB SP, SP, #0x10\n"
			"STR R1, [SP, #0x08]\n"
			"LDR R1, =0\n"
			"STR R1, [SP, #0x04]\n"
			"LDR R1, =0\n"
			"STR R1, [SP, #0x00]\n"
			"LDR R3, =0\n"
			"LDR R1, =80\n"
			"BL " QU(AMBE_DECODE)
			"ADD SP, SP, #0x10\n"
			"POP {R4-R11}"
		);

		soundStoreBuffer();

		soundSetupBuffer();// this just sets currentWaveBuffer but the compiler seems to optimise out the code if I try to do it in this file
		r2 = (int)bitbuffer_decode;
		r0 = (int)currentWaveBuffer;
		r1 = (int)ambebuffer_decode;

		asm volatile (
			"PUSH {R4-R11}\n"
			"SUB SP, SP, #0x10\n"
			"STR R1, [SP, #0x08]\n"
			"LDR R1, =1\n"
			"STR R1, [SP, #0x04]\n"
			"LDR R1, =0\n"
			"STR R1, [SP, #0x00]\n"
			"LDR R3, =0\n"
			"LDR R1, =80\n"
			"BL " QU(AMBE_DECODE)
			"ADD SP, SP, #0x10\n"
			"POP {R4-R11}"
		);

		soundStoreBuffer();
    }
}

void codecEncodeBlock(uint8_t *outdata_ptr)
{
	register int r0 asm ("r0") __attribute__((unused));
	register int r1 asm ("r1") __attribute__((unused));
	register int r2 asm ("r2") __attribute__((unused));

	memset((uint8_t *)outdata_ptr, 0, 9);// fills with zeros
	memset(bitbuffer_encode, 0, sizeof(bitbuffer_encode));// faster to call memset as it will be compiled as optimised code


	soundRetrieveBuffer();// gets currentWaveBuffer pointer used as input r2 to the encoder

	r0 = (int)bitbuffer_encode;
	r2 = (int)currentWaveBuffer;//tmp_wavbuffer;
	r1 = (int)ambebuffer_encode;// seems to be a hard coded (defined) memory address of 0x1FFF6B60. I'm not sure why it has to be hard coded, since its passed as a paramater (register)

	asm volatile (
		"PUSH {R4-R11}\n"
		"SUB SP, SP, #0x14\n"
		"STR R1, [SP, #0x0C]\n"
		"LDR R1, =0x00002000\n"
		"STR R1, [SP, #0x08]\n"
		"LDR R1, =0\n"
		"STR R1, [SP, #0x04]\n"
		"LDR R1, =0x00001840\n"
		"STR R1, [SP, #0x00]\n"
		"LDR R3, =80\n"
		"LDR R1, =0\n"
		"BL " QU(AMBE_ENCODE)
		"ADD SP, SP, #0x14\n"
		"POP {R4-R11}"
	);

	soundRetrieveBuffer();// gets currentWaveBuffer pointer used as input r2 to the encoder

	r0 = (int)bitbuffer_encode;
	r2 = (int)currentWaveBuffer;//tmp_wavbuffer;
	r1 = (int)ambebuffer_encode;

	asm volatile (
		"PUSH {R4-R11}\n"
		"SUB SP, SP, #0x14\n"
		"STR R1, [SP, #0x0C]\n"
		"LDR R1, =0x00002000\n"
		"STR R1, [SP, #0x08]\n"
		"LDR R1, =1\n"
		"STR R1, [SP, #0x04]\n"
		"LDR R1, =0x00000800\n"
		"STR R1, [SP, #0x00]\n"
		"LDR R3, =80\n"
		"LDR R1, =0\n"
		"BL " QU(AMBE_ENCODE)
		"ADD SP, SP, #0x14\n"
		"POP {R4-R11}"
	);

#if defined(ENABLE_AES) && defined(DMR_AES_DIAG_PATTERN)
	// DIAGNOSTIC: replace the voice params with a known pattern (unencrypted) so the
	// over-the-air decode reveals the codec encode->decode param permutation.
	dmrAesDiagPattern(bitbuffer_encode);
#elif defined(ENABLE_AES) && defined(DMR_AES_DIAG_ENCPAT)
	// DIAGNOSTIC: set a CONSTANT known plaintext, THEN encrypt it, so the received
	// ciphertext directly reveals the firmware's applied TX keystream (enc ^ const).
	dmrAesDiagConstPattern(bitbuffer_encode);
	dmrAesTxCodecFrame(bitbuffer_encode);
#else
	// AES: encrypt the 49 decoded AMBE voice params before the FEC encode (mirror of the
	// RX decrypt in codecDecode). No-op unless ENABLE_AES and an active encrypted TX.
	dmrAesTxCodecFrame(bitbuffer_encode);
#endif

	r0 = (int)bitbuffer_encode;
	r1 = (int)ambebuffer_encode_ecc;

	asm volatile (
		"PUSH {R4-R11}\n"
		"SUB SP, SP, #0x14\n"
		"MOV R3, R1\n"
		"LDR R2, =0\n"
		"MOV R1, R0\n"
		"BL " QU(AMBE_ENCODE_ECC)
		"ADD SP, SP, #0x14\n"
		"POP {R4-R11}"
	);

	// AES: stuff the DMRA Late-Entry MI into the post-ECC codeword (bitbuffer_encode now
	// holds the 72-bit FEC frame). No-op unless ENABLE_AES and an active encrypted TX.
#if !(defined(ENABLE_AES) && defined(DMR_AES_DIAG_PATTERN))
	dmrAesTxStuffMI(bitbuffer_encode);
#endif

	for (int i = 0; i < 72; i++)
	{
		if (bitbuffer_encode[i] & 1)
		{
			outdata_ptr[i >> 3] |= 128 >> (i & 7);
		}
	}
}

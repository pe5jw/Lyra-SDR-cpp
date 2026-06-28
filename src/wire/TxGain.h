// Lyra-cpp — TxGain.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source file: ChannelMaster/txgain.h (whole file, verbatim)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2015 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ============================================================
// DIRECT PORT 2026-06-28 (TX power model — the ChannelMaster
// fixed-gain + amp-protect block that gives continuous TX-output
// resolution between the HL2's 16 coarse AD9866 drive steps and
// below its PA floor.  This is exactly what the reference uses
// for HL2 power control (Audio.RadioVolume -> SetTXFixedGain),
// and it is PureSignal-safe by construction: the gain operates
// on the TX I/Q INSIDE ChannelMaster, the same place PS taps its
// reference, so PS calibrates against the post-gain signal.
// See docs/architecture/tx_power_model_design.md.)
// ============================================================
//
// This is the reference txgain.h VERBATIM: the `txgain, *TXGAIN`
// twin typedef and the create/destroy/xtxgain/SetTXGainSize/
// SetAmpProtectADCValue declarations.  The PORT-exported setters
// (SetTXFixedGainRun / SetTXFixedGain / GetAndResetAmpProtect /
// SetAmpProtectRun / SetADCSupply) live in txgain.c without an
// txgain.h declaration in the reference; declared here for the
// Lyra call seam (HL2Stream / main drive the fixed gain).
//
// Only Lyra-cpp packaging differences (NOT code changes):
//   * `lyra::wire` namespace (reference is global C).
//   * include "wire/cmcomm.h" carries the reference's complex /
//     malloc0 / CRITICAL_SECTION / Interlocked surface + the
//     opaque `typedef struct _txgain txgain, *TXGAIN;` (this
//     header completes the struct body, mirroring the ILV port).
//
// See NOTICE.md and CREDITS.md (repo root) for full upstream
// attribution.

#pragma once

#include "wire/cmcomm.h"

namespace lyra::wire {

// Reference txgain.h:30-43 (verbatim — completes the opaque
// `typedef struct _txgain txgain, *TXGAIN;` forward-declared in
// cmcomm.h, exactly as the ILV/EER ports complete theirs):

typedef struct _txgain
{
	volatile long run_fixed;
	volatile long run_amp_protect;
	int size;
	double* in;
	double* out;
	double Igain;
	double Qgain;
	int adc_value;
	int adc_supply;
	volatile long amp_protect_warning;
	CRITICAL_SECTION cs_update0, cs_update1;
} txgain, *TXGAIN;

// Reference txgain.h:45-58 (verbatim):

TXGAIN create_txgain(
	int run_fixed,
	int run_amp_protect,
	int size,
	double* in,
	double* out,
	double Igain,
	double Qgain,
	int adc_value,
	int adc_supply
	);

void destroy_txgain(TXGAIN a);

void xtxgain(TXGAIN a);

void SetTXGainSize(TXGAIN p, int size);

void SetAmpProtectADCValue (int txid, int value);

// PORT-exported in the reference txgain.c (no txgain.h decl);
// declared here for the Lyra call seam:

void SetTXFixedGainRun(int txid, int run);
void SetTXFixedGain(int txid, double Igain, double Qgain);
int  GetAndResetAmpProtect(int txid);
void SetAmpProtectRun(int txid, int run);
void SetADCSupply(int txid, int v);

}  // namespace lyra::wire

// Lyra-cpp — TxGain.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source file: ChannelMaster/txgain.c (whole file, verbatim)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2015 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// DIRECT PORT 2026-06-28 (TX power model — see TxGain.h + the
// docs/architecture/tx_power_model_design.md for why this is the
// PureSignal-safe TX-output gain block).  Verbatim reference
// bodies; only Lyra-cpp packaging differs (NOT code):
//   * `lyra::wire` namespace (reference is global C).
//   * include "wire/cmcomm.h" (complex / malloc0 / CRITICAL_
//     SECTION / Interlocked surface) + "wire/CMaster.h" (pcm).
//   * the reference `PORT` export macro is dropped — these are
//     internal namespace functions in Lyra, not cmaster.dll
//     exports (matches the ILV/EER port style).
//
// See NOTICE.md and CREDITS.md (repo root) for full attribution.

#include "wire/cmcomm.h"
#include "wire/TxGain.h"
#include "wire/CMaster.h"   // pcm — the global ChannelMaster instance

#include <cmath>            // pow (reference pulls it via cmcomm.h's math.h)

namespace lyra::wire {

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
	)
{
	TXGAIN a = (TXGAIN)malloc0(sizeof(txgain));
	if (run_fixed)
		InterlockedBitTestAndSet(&a->run_fixed, 0);
	if (run_amp_protect)
		InterlockedBitTestAndSet(&a->run_amp_protect, 0);
	a->size = size;
	a->in = in;
	a->out = out;
	a->Igain = Igain;
	a->Qgain = Qgain;
	a->adc_value = adc_value;
	a->adc_supply = adc_supply;
	InitializeCriticalSectionAndSpinCount(&a->cs_update0, 2500);
	InitializeCriticalSectionAndSpinCount(&a->cs_update1, 2500);
	return a;
}

void destroy_txgain(TXGAIN a)
{
	DeleteCriticalSection(&a->cs_update1);
	DeleteCriticalSection(&a->cs_update0);
	_aligned_free(a);
}

void xtxgain(TXGAIN a)
{
	int i;
	if (_InterlockedAnd(&a->run_fixed, 1))
	{
		EnterCriticalSection(&a->cs_update0);
		for (i = 0; i < a->size; i++)
		{
			a->out[2 * i + 0] = a->Igain * a->in[2 * i + 0];
			a->out[2 * i + 1] = a->Qgain * a->in[2 * i + 1];
		}
		LeaveCriticalSection(&a->cs_update0);
	}
	else if (a->out != a->in)
		memcpy(a->out, a->in, a->size * sizeof(complex));

	if (_InterlockedAnd(&a->run_amp_protect, 1))
	{
		double adc_value, ptn;
		EnterCriticalSection(&a->cs_update1);
		adc_value = a->adc_value;
		LeaveCriticalSection(&a->cs_update1);
		if (adc_value > 0)
		{
			InterlockedBitTestAndSet(&a->amp_protect_warning, 0);
			switch (a->adc_supply)
			{
			case 33:
				ptn = 1.0 / pow (10.0, (double)adc_value / 2730.0);
				break;
			case 50:
				ptn = 1.0 / pow (10.0, (double)adc_value / 1802.0);
				break;
			default:
				ptn = 0.0;
				break;
			}
			for (i = 0; i < 2 * a->size; i++)
			{
				a->out[i] *= ptn;
			}
		}
	}
}


void SetTXGainSize(TXGAIN p, int size)
{
	p->size = size;
}

// Lyra-cpp packaging note (NOT a reference deviation in behaviour):
// the reference creates the xmtr (and thus `pgain`) unconditionally
// in create_cmaster() at startup, so these PORT setters can assume a
// valid `pgain`.  Lyra DEFERS create_xmtr()/destroy_xmtr() until the
// wdsp.dll seam is resolved (CMaster.cpp preamble), so `pgain` is
// null before create_xmtr and after destroy_xmtr.  A caller (e.g. the
// drive-level path) may legitimately fire in that window, so each
// pgain-dereferencing setter no-ops when it is null.

void SetTXFixedGainRun(int txid, int run)
{
	TXGAIN a = pcm->xmtr[txid].pgain;
	if (a == nullptr) return;
	if (run)
		InterlockedBitTestAndSet(&a->run_fixed, 0);
	else
		InterlockedBitTestAndReset(&a->run_fixed, 0);
}

void SetTXFixedGain(int txid, double Igain, double Qgain)
{
	TXGAIN a = pcm->xmtr[txid].pgain;
	if (a == nullptr) return;
	EnterCriticalSection (&a->cs_update0);
	a->Igain = Igain;
	a->Qgain = Qgain;
	LeaveCriticalSection (&a->cs_update0);
}

// call when new ADC value arrives from network
void SetAmpProtectADCValue (int txid, int value)
{
	const int thresh = 20;
	TXGAIN a = pcm->xmtr[txid].pgain;
	if (a == nullptr) return;
	EnterCriticalSection (&a->cs_update1);
	a->adc_value = value - thresh;
	LeaveCriticalSection (&a->cs_update1);
}

int GetAndResetAmpProtect(int txid)
{
	TXGAIN a = pcm->xmtr[txid].pgain;
	if (a == nullptr) return 0;
	return InterlockedBitTestAndReset(&a->amp_protect_warning, 0);
}

void SetAmpProtectRun(int txid, int run)
{
	TXGAIN a = pcm->xmtr[txid].pgain;
	if (a == nullptr) return;
	if (run)
		InterlockedBitTestAndSet(&a->run_amp_protect, 0);
	else
		InterlockedBitTestAndReset(&a->run_amp_protect, 0);
}

void SetADCSupply(int txid, int v)
{
	TXGAIN a = pcm->xmtr[txid].pgain;
	if (a == nullptr) return;
	a->adc_supply = v;
}

}  // namespace lyra::wire

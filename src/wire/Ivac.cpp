// Lyra-cpp — Ivac.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Source file: ChannelMaster/ivac.c
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2015-2025 Warren Pratt, NR0V
// Original copyright: (C) 2015-2016 Doug Wigley, W5WC
// License: GNU General Public License v2 or later
//
// DIRECT PORT 2026-06-14 (#158 Stage 1 — wire-INERT engine).  See
// Ivac.h preamble for the attribution + the packaging-only
// differences + the deferred PortAudio device-I/O surface.  This
// translation unit ports ONLY the engine bodies the Stage-1 plan
// names:
//   create_resamps / create_ivac / destroy_resamps / destroy_ivac
//   xvacIN / xvacOUT / xvac_out / combinebuff / scalebuff
//   + the full SetIVAC* / getIVACdiags / forceIVACvar /
//     resetIVACdiags / SetIVACinitialVars / SetIVACswapIQout /
//     SetIVACExclusiveIn / SetIVACExclusiveOut surface.
// The PortAudio device I/O (CallbackIVAC / StartAudioIVAC /
// StopAudioIVAC) and the create_resampleV / xresampleV /
// destroy_resampleV device-resampler helpers are deferred to
// Stage 2 (Qt) and NOT ported here.
//
// Notes (NOT code changes — packaging):
//   * The reference ivac.c includes only "cmcomm.h" + the WASAPI
//     header.  Lyra-cpp's cmcomm.h carries the base comm surface;
//     `pcm` (the central cmaster) comes from wire/CMaster.h (the
//     P0.d umbrella mapping — same include note as ILV.cpp), and
//     the WDSP rmatchV entry points resolve through the
//     operator-approved wire/wdspcalls.h seam instead of a static
//     link.  All call sites read byte-identical to the reference.
//   * The reference create_ivac uses `calloc(1, sizeof(ivac))` paired
//     with plain `free(a)` in destroy_ivac — kept VERBATIM.  (An early
//     port substituted `malloc0` here; that corrupts the heap, because
//     lyra's malloc0 is _aligned_malloc-based (cmcomm.cpp) and must be
//     freed with _aligned_free, not free — the struct stays calloc/free,
//     while the bitbucket + rmatch buffers use malloc0/_aligned_free.)
//     The reference's printf/exit failure branch is dropped (same
//     no-failure-branch convention as AAMix.cpp / ILV.cpp).

#include "wire/cmcomm.h"
#include "wire/Ivac.h"
#include "wire/AAMix.h"        // create_aamix / destroy_aamix / xMixAudio / SetAAudioMix*
#include "wire/CMaster.h"      // pcm — getbuffsize / cmMAXInRate (see include note above)
#include "wire/wdspcalls.h"    // create_rmatchV / xrmatchIN / xrmatchOUT / forceRMatchVar / getRMatchDiags / ...

namespace lyra::wire {

// Reference ivac.c:31 (verbatim — the central VAC bank, file-static
// to this TU exactly as the reference keeps it private to ivac.c;
// NOT in the header):
__declspec (align (16))			IVAC pvac[MAX_EXT_VACS];

void create_resamps(IVAC a)
{
	a->INringsize = (int)(2 * a->mic_rate * a->in_latency);		// FROM VAC to mic
	a->OUTringsize = (int)(2 * a->vac_rate * a->out_latency);	// TO VAC from rx audio

	a->rmatchIN  = create_rmatchV (a->vac_size, a->mic_size, a->vac_rate, a->mic_rate, a->INringsize, a->initial_INvar);			// data FROM VAC TO TX MIC INPUT
	forceRMatchVar (a->rmatchIN, a->INforce, a->INfvar);
	if (!a->iq_type)
		a->rmatchOUT = create_rmatchV (a->audio_size, a->vac_size, a->audio_rate, a->vac_rate, a->OUTringsize, a->initial_OUTvar);	// data FROM RADIO TO VAC
	else
		a->rmatchOUT = create_rmatchV (a->iq_size, a->vac_size, a->iq_rate, a->vac_rate, a->OUTringsize, a->initial_OUTvar);		// RX I-Q data going to VAC
	forceRMatchVar (a->rmatchOUT, a->OUTforce, a->OUTfvar);
	a->bitbucket = (double *) malloc0 (getbuffsize (pcm->cmMAXInRate) * sizeof (complex));

	if(a->mono_in_to_stereo_buffer != NULL)
	{
		_aligned_free(a->mono_in_to_stereo_buffer);
		a->mono_in_to_stereo_buffer = NULL;
		a->mono_in_to_stereo_capacity = 0;
	}
}

PORT void create_ivac(
	int id,
	int run,
	int iq_type,				// 1 if using raw IQ samples, 0 for audio
	int stereo,					// 1 for stereo, 0 otherwise
	int iq_rate,				// sample rate of RX I-Q data
	int mic_rate,				// sample rate of data from VAC to TX MIC input
	int audio_rate,				// sample rate of data from RCVR Audio data to VAC
	int txmon_rate,				// sample rate of data from TX Monitor to VAC
	int vac_rate,				// VAC sample rate
	int mic_size,				// buffer size for data from VAC to TX MIC input
	int iq_size,				// buffer size for RCVR IQ data to VAC
	int audio_size,				// buffer size for RCVR Audio data to VAC
	int txmon_size,				// buffer size for TX Monitor data to VAC
	int vac_size				// VAC buffer size
	)
{
	IVAC a = (IVAC)calloc(1, sizeof(ivac));	// calloc/free pair (NOT malloc0 — see preamble)
	a->run = run;
	a->iq_type = iq_type;
	a->stereo = stereo;
	a->iq_rate = iq_rate;
	a->mic_rate = mic_rate;
	a->audio_rate = audio_rate;
	a->txmon_rate = txmon_rate;
	a->vac_rate = vac_rate;
	a->mic_size = mic_size;
	a->iq_size = iq_size;
	a->audio_size = audio_size;
	a->txmon_size = txmon_size;
	a->vac_size = vac_size;
	a->INforce = 0;
	a->INfvar = 1.0;
	a->OUTforce = 0;
	a->OUTfvar = 1.0;
	a->initial_INvar = 1.0;
	a->initial_OUTvar = 1.0;
	a->swapIQout = 0;
	a->exclusive_in = 0;
	a->exclusive_out = 0;
	a->mono_in_to_stereo_buffer = NULL;
	a->mono_in_to_stereo_capacity = 0;
	InitializeCriticalSectionAndSpinCount(&a->cs_ivac, 2500);
	create_resamps(a);
	{
		int inrate[2] = { a->audio_rate, a->txmon_rate };
		a->mixer = create_aamix(-1, id, a->audio_size, a->audio_size, 2, 3, 3, 1.0, 4096, inrate, a->audio_rate, xvac_out, 0.0, 0.0, 0.0, 0.0);
	}
	pvac[id] = a;
}

void destroy_resamps(IVAC a)
{
	_aligned_free (a->bitbucket);
	_aligned_free(a->mono_in_to_stereo_buffer);
	a->mono_in_to_stereo_buffer = NULL;
	a->mono_in_to_stereo_capacity = 0;
	destroy_rmatchV (a->rmatchOUT);
	destroy_rmatchV (a->rmatchIN);
}

PORT void destroy_ivac(int id)
{
	IVAC a = pvac[id];
	destroy_resamps(a);
	DeleteCriticalSection(&a->cs_ivac);
	free (a);
}

PORT void xvacIN(int id, double* in_tx, int bypass)
{
	// used for MIC data to TX
	IVAC a = pvac[id];
	if (a->run)
		if (!a->vac_bypass && !bypass)
		{
			xrmatchOUT (a->rmatchIN, in_tx);
			if (a->vac_combine_input)
				combinebuff(a->mic_size, in_tx, in_tx);// , 3); //[2.10.3.6]MW0LGE new 17.11.0 version of VS started complaining about this 3, has been there 8 months
			scalebuff(a->mic_size, in_tx, a->vac_preamp, in_tx);
		}
		else
			xrmatchOUT (a->rmatchIN, a->bitbucket);
}

PORT void xvacOUT(int id, int stream, double* data)
{
	IVAC a = pvac[id];
	// receiver input data (iq_type) -> stream = 0
	// receiver output data (audio)  -> stream = 1
	// transmitter output data (mon) -> stream = 2
	if (a->run)
	{
		if (!a->iq_type)
		{	// call mixer to synchronize the two streams
			if (stream == 1)
				xMixAudio(a->mixer, -1, 0, data);
			else if (stream == 2)
				xMixAudio(a->mixer, -1, 1, data);
		}
		else if (stream == 0)
			xrmatchIN (a->rmatchOUT, data);	// i-q data from RX stream
	}
}

void xvac_out(int id, int nsamples, double* buff)
{	// called by the mixer with a buffer of output data
	IVAC a = pvac[id];
	xrmatchIN (a->rmatchOUT, buff);		// audio data from mixer
	// if (id == 0) WriteAudio (120.0, 48000, a->audio_size, buff, 3);
}

// DEFERRED to Stage 2 (Qt device I/O): CallbackIVAC (ivac.c:196-263),
// StartAudioIVAC (ivac.c:265-359), StopAudioIVAC (ivac.c:367-371),
// and the create_resampleV / xresampleV / destroy_resampleV device-
// resampler helpers.  See Ivac.h preamble + IVAC_PORT_PLAN.md.

PORT void SetIVACRBReset(int id, int reset)
{
	IVAC a = pvac[id];
	(void)a;
	// a->reset = reset;
	(void)reset;
}

PORT void SetIVACrun(int id, int run)
{
	IVAC a = pvac[id];
	a->run = run;
}

PORT void SetIVACiqType(int id, int type)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	if (type != a->iq_type)
	{
		a->iq_type = type;
		destroy_resamps(a);
		create_resamps(a);
	}
	LeaveCriticalSection(&a->cs_ivac);
}

PORT void SetIVACstereo(int id, int stereo)
{
	IVAC a = pvac[id];
	a->stereo = stereo;
}

PORT void SetIVACvacRate(int id, int rate)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	if (rate != a->vac_rate)
	{
		a->vac_rate = rate;
		destroy_resamps(a);
		create_resamps(a);
	}
	LeaveCriticalSection(&a->cs_ivac);
}

PORT void SetIVACmicRate(int id, int rate)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	if (rate != a->mic_rate)
	{
		a->mic_rate = rate;
		destroy_resamps(a);
		create_resamps(a);
	}
	LeaveCriticalSection(&a->cs_ivac);
}

PORT void SetIVACaudioRate(int id, int rate)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	if (rate != a->audio_rate)
	{
		a->audio_rate = rate;
		destroy_aamix(a->mixer, 0);
		{
			int inrate[2] = { a->audio_rate, a->txmon_rate };
			a->mixer = create_aamix(-1, id, a->audio_size, a->audio_size, 2, 3, 3, 1.0, 4096, inrate, a->audio_rate, xvac_out, 0.0, 0.0, 0.0, 0.0);
		}
		destroy_resamps(a);
		create_resamps(a);
	}
	LeaveCriticalSection(&a->cs_ivac);
}

void SetIVACtxmonRate(int id, int rate)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	if (rate != a->txmon_rate)
	{
		a->txmon_rate = rate;
		destroy_aamix(a->mixer, 0);
		{
			int inrate[2] = { a->audio_rate, a->txmon_rate };
			a->mixer = create_aamix(-1, id, a->audio_size, a->audio_size, 2, 3, 3, 1.0, 4096, inrate, a->audio_rate, xvac_out, 0.0, 0.0, 0.0, 0.0);
		}
	}
	LeaveCriticalSection(&a->cs_ivac);
}

PORT void SetIVACvacSize(int id, int size)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	if (size != a->vac_size)
	{
		a->vac_size = size;
		destroy_resamps(a);
		create_resamps(a);
	}
	LeaveCriticalSection(&a->cs_ivac);
}

PORT void SetIVACmicSize(int id, int size)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	if (size != a->mic_size)
	{
		a->mic_size = (unsigned int)size;
		destroy_resamps(a);
		create_resamps(a);
	}
	LeaveCriticalSection(&a->cs_ivac);
}

PORT void SetIVACiqSizeAndRate(int id, int size, int rate)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	if (size != a->iq_size || rate != a->iq_rate)
	{
		a->iq_size = size;
		a->iq_rate = rate;
		if (a->iq_type)
		{
			destroy_resamps(a);
			create_resamps(a);
		}
	}
	LeaveCriticalSection(&a->cs_ivac);
}

PORT void SetIVACaudioSize(int id, int size)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	a->audio_size = (unsigned int)size;
	destroy_aamix(a->mixer, 0);
	{
		int inrate[2] = { a->audio_rate, a->txmon_rate };
		a->mixer = create_aamix(-1, id, a->audio_size, a->audio_size, 2, 3, 3, 1.0, 4096, inrate, a->audio_rate, xvac_out, 0.0, 0.0, 0.0, 0.0);
	}
	destroy_resamps(a);
	create_resamps(a);
	LeaveCriticalSection(&a->cs_ivac);
}

void SetIVACtxmonSize(int id, int size)
{
	IVAC a = pvac[id];
	a->txmon_size = (unsigned int)size;
}

PORT void SetIVAChostAPIindex(int id, int index)
{
	IVAC a = pvac[id];
	a->host_api_index = index;
}

PORT void SetIVACinputDEVindex(int id, int index)
{
	IVAC a = pvac[id];
	a->input_dev_index = index;
}

PORT void SetIVACoutputDEVindex(int id, int index)
{
	IVAC a = pvac[id];
	a->output_dev_index = index;
}

PORT void SetIVACnumChannels(int id, int n)
{
	IVAC a = pvac[id];
	a->num_channels = n;
}

PORT void SetIVACInLatency(int id, double lat, int reset)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	if (a->in_latency != lat)
	{
		a->in_latency = lat;
		destroy_resamps (a);
		create_resamps (a);
	}
	LeaveCriticalSection(&a->cs_ivac);
	(void)reset;
}

PORT void SetIVACOutLatency(int id, double lat, int reset)
{
	IVAC a = pvac[id];
	EnterCriticalSection(&a->cs_ivac);
	if (a->out_latency != lat)
	{
		a->out_latency = lat;
		destroy_resamps (a);
		create_resamps (a);
	}
	LeaveCriticalSection(&a->cs_ivac);
	(void)reset;
}

PORT void SetIVACPAInLatency(int id, double lat, int reset)
{
	IVAC a = pvac[id];

	if (a->pa_in_latency != lat)
	{
		a->pa_in_latency = lat;
	}
	(void)reset;
}

PORT void SetIVACPAOutLatency(int id, double lat, int reset)
{
	IVAC a = pvac[id];

	if (a->pa_out_latency != lat)
	{
		a->pa_out_latency = lat;
	}
	(void)reset;
}

PORT void SetIVACvox(int id, int vox)
{
	IVAC a = pvac[id];
	a->vox = vox;
}

PORT void SetIVACmox(int id, int mox)
{
	IVAC a = pvac[id];
	a->mox = mox;
	if (!a->mox)
	{
		if (a->mon)
		{
			SetAAudioMixWhat(a->mixer, 0, 1, 1);
			SetAAudioMixWhat(a->mixer, 0, 0, 1);
		}
		else
		{
			SetAAudioMixWhat(a->mixer, 0, 1, 0);
			SetAAudioMixWhat(a->mixer, 0, 0, 1);
		}
	}
	else
	{
		if (a->mon)
		{
			SetAAudioMixWhat(a->mixer, 0, 0, 0);
			SetAAudioMixWhat(a->mixer, 0, 1, 1);
		}
		else
		{
			SetAAudioMixWhat(a->mixer, 0, 0, 0);
			SetAAudioMixWhat(a->mixer, 0, 1, 0);
		}
	}
}

PORT void SetIVACmon(int id, int mon)
{
	IVAC a = pvac[id];
	a->mon = mon;
	if (!a->mox)
	{
		if (a->mon)
		{
			SetAAudioMixWhat(a->mixer, 0, 1, 1);
			SetAAudioMixWhat(a->mixer, 0, 0, 1);
		}
		else
		{
			SetAAudioMixWhat(a->mixer, 0, 1, 0);
			SetAAudioMixWhat(a->mixer, 0, 0, 1);
		}
	}
	else
	{
		if (a->mon)
		{
			SetAAudioMixWhat(a->mixer, 0, 0, 0);
			SetAAudioMixWhat(a->mixer, 0, 1, 1);
		}
		else
		{
			SetAAudioMixWhat(a->mixer, 0, 0, 0);
			SetAAudioMixWhat(a->mixer, 0, 1, 0);
		}
	}
}

PORT void SetIVACmonVol(int id, double vol)
{
	IVAC a = pvac[id];
	a->vac_mon_scale = vol;
	SetAAudioMixVol(a->mixer, 0, 1, a->vac_mon_scale);
}

PORT void SetIVACpreamp(int id, double preamp)
{
	IVAC a = pvac[id];
	a->vac_preamp = preamp;
}

PORT void SetIVACrxscale(int id, double scale)
{
	IVAC a = pvac[id];
	a->vac_rx_scale = scale;
	SetAAudioMixVolume(a->mixer, 0, a->vac_rx_scale);
}

PORT void SetIVACbypass(int id, int bypass)
{
	IVAC a = pvac[id];
	a->vac_bypass = bypass;
}

PORT void SetIVACcombine(int id, int combine)
{
	IVAC a = pvac[id];
	a->vac_combine_input = combine;
}

void combinebuff(int n, double* a, double* combined)
{
	int i;
	for (i = 0; i < 2 * n; i += 2)
		combined[i] = combined[i + 1] = a[i] + a[i + 1];
}

void scalebuff(int size, double* in, double scale, double* out)
{
	int i;
	for (i = 0; i < 2 * size; i++)
		out[i] = scale * in[i];
}

PORT
void getIVACdiags (int id, int type, int* underflows, int* overflows, double* var, int* ringsize, int* nring)
{
	// type:  0 - From VAC; 1 - To VAC
	void* a;
	if (type == 0)
		a = pvac[id]->rmatchOUT;
	else
		a = pvac[id]->rmatchIN;
	//EnterCriticalSection(&pvac[id]->cs_ivac);
	getRMatchDiags (a, underflows, overflows, var, ringsize, nring);
	//LeaveCriticalSection(&pvac[id]->cs_ivac);
}

PORT
void forceIVACvar (int id, int type, int force, double fvar)
{
	// type:  0 - From VAC; 1 - To VAC
	IVAC b = pvac[id];
	void* a;
	if (type == 0)
	{
		a = b->rmatchOUT;
		b->OUTforce = force;
		b->OUTfvar = fvar;
	}
	else
	{
		a = b->rmatchIN;
		b->INforce = force;
		b->INfvar = fvar;
	}
	forceRMatchVar (a, force, fvar);
}
PORT
void resetIVACdiags(int id, int type)
{
	// type:  0 - From VAC; 1 - To VAC
	void* a;
	if (type == 0)
		a = pvac[id]->rmatchOUT;
	else
		a = pvac[id]->rmatchIN;
	EnterCriticalSection(&pvac[id]->cs_ivac);
	resetRMatchDiags(a);
	LeaveCriticalSection(&pvac[id]->cs_ivac);
}

//MW0LGE_21h
PORT void SetIVACFeedbackGain(int id, int type, double feedback_gain)
{
	IVAC b = pvac[id];
	// type = 0 out, 1 = in
	void* a;
	if (type == 0)
		a = b->rmatchOUT;
	else
		a = b->rmatchIN;
	EnterCriticalSection(&b->cs_ivac);
	setRMatchFeedbackGain(a, feedback_gain);
	LeaveCriticalSection(&b->cs_ivac);
}
PORT void SetIVACSlewTime(int id, int type, double slew_time)
{
	IVAC b = pvac[id];
	// type = 0 out, 1 = in
	void* a;
	if (type == 0)
		a = b->rmatchOUT;
	else
		a = b->rmatchIN;
	//setRMatchSlewTime(a, slew_time);
	EnterCriticalSection(&b->cs_ivac);
	setRMatchSlewTime1(a, slew_time); // preserve all data in various buffers
	LeaveCriticalSection(&b->cs_ivac);
}
//MW0LGE_21j
PORT void SetIVACPropRingMin(int id, int type, int prop_min)
{
	IVAC b = pvac[id];
	// type = 0 out, 1 = in
	void* a;
	if (type == 0)
		a = b->rmatchOUT;
	else
		a = b->rmatchIN;
	EnterCriticalSection(&b->cs_ivac);
	setRMatchPropRingMin(a, prop_min);
	LeaveCriticalSection(&b->cs_ivac);
}
PORT void SetIVACPropRingMax(int id, int type, int prop_max)
{
	IVAC b = pvac[id];
	// type = 0 out, 1 = in
	void* a;
	if (type == 0)
		a = b->rmatchOUT;
	else
		a = b->rmatchIN;
	EnterCriticalSection(&b->cs_ivac);
	setRMatchPropRingMax(a, prop_max);
	LeaveCriticalSection(&b->cs_ivac);
}
PORT void SetIVACFFRingMin(int id, int type, int ff_ringmin)
{
	IVAC b = pvac[id];
	// type = 0 out, 1 = in
	void* a;
	if (type == 0)
		a = b->rmatchOUT;
	else
		a = b->rmatchIN;
	EnterCriticalSection(&b->cs_ivac);
	setRMatchFFRingMin(a, ff_ringmin);
	LeaveCriticalSection(&b->cs_ivac);
}
PORT void SetIVACFFRingMax(int id, int type, int ff_ringmax)
{
	IVAC b = pvac[id];
	// type = 0 out, 1 = in
	void* a;
	if (type == 0)
		a = b->rmatchOUT;
	else
		a = b->rmatchIN;
	EnterCriticalSection(&b->cs_ivac);
	setRMatchFFRingMax(a, ff_ringmax);
	LeaveCriticalSection(&b->cs_ivac);
}
PORT void SetIVACFFAlpha(int id, int type, double ff_alpha)
{
	IVAC b = pvac[id];
	// type = 0 out, 1 = in
	void* a;
	if (type == 0)
		a = b->rmatchOUT;
	else
		a = b->rmatchIN;
	EnterCriticalSection(&b->cs_ivac);
	setRMatchFFAlpha(a, ff_alpha);
	LeaveCriticalSection(&b->cs_ivac);
}
// DEFERRED: GetIVACControlFlag needs getControlFlag in wdspcalls (follow-up)
// (reference ivac.c:850-862 — calls getControlFlag(a, control_flag), which
//  is not yet in the wire/wdspcalls.h table.)
PORT
void SetIVACinitialVars(int id, double INvar, double OUTvar)
{
	IVAC a = pvac[id];
	int change = 0;

	if (INvar != a->initial_INvar)
	{
		a->initial_INvar = INvar;
		change = 1;
	}
	if (OUTvar != a->initial_OUTvar)
	{
		a->initial_OUTvar = OUTvar;
		change = 1;
	}
	if (change)
	{
		EnterCriticalSection(&a->cs_ivac);
		destroy_resamps(a);
		create_resamps(a);
		LeaveCriticalSection(&a->cs_ivac);
	}
}

PORT
void SetIVACswapIQout(int id, int swap)
{
	IVAC a = pvac[id];
	a->swapIQout = swap;
}

PORT
void SetIVACExclusiveOut(int id, int exclusive_out)
{
	IVAC a = pvac[id];
	a->exclusive_out = exclusive_out;
}

PORT
void SetIVACExclusiveIn(int id, int exclusive_in)
{
	IVAC a = pvac[id];
	a->exclusive_in = exclusive_in;
}

}  // namespace lyra::wire

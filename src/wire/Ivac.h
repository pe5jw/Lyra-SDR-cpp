// Lyra-cpp — Ivac.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source files: ChannelMaster/ivac.h + ChannelMaster/ivac.c
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2015-2025 Warren Pratt, NR0V
// Original copyright: (C) 2015-2016 Doug Wigley, W5WC
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ============================================================
// DIRECT PORT 2026-06-14 (#158 Stage 1 — wire-INERT engine)
// ============================================================
//
// This is the reference ivac.h VERBATIM: the `ivac, *IVAC` twin
// typedef with every scalar/pointer field, plus the free-function
// declaration set the engine needs (combinebuff / scalebuff /
// xvac_out / create_ivac / destroy_ivac / xvacIN / xvacOUT and the
// full SetIVAC* + diag surface).  Stage 1 is the engine + setters
// only — NOTHING calls them yet (no Qt audio, no radio wiring).
//
// Documented deviations from the reference ivac.h (NOT code
// changes — packaging + the deferred device-I/O surface):
//   * The PortAudio fields the reference struct carries
//     (`PaStreamParameters inParam, outParam;  PaStream *Stream;`)
//     are OMITTED here.  The PortAudio device I/O (CallbackIVAC /
//     StartAudioIVAC / StopAudioIVAC) is deferred to Stage 2 and
//     re-homed onto Qt (QAudioSource / QAudioSink) — see
//     docs/architecture/IVAC_PORT_PLAN.md.  Every other struct
//     field (incl. CRITICAL_SECTION cs_ivac, void* mixer,
//     void* rmatchIN/rmatchOUT, the mono_in_to_stereo_* buffer
//     pair, and all scalars) is carried verbatim.
//   * `lyra::wire` namespace (reference is global C; this is the
//     namespace the ChannelMaster family lives in).
//   * `#include "wire/cmcomm.h"` carries the reference's
//     complex / malloc0 / CRITICAL_SECTION / PORT surface (the
//     reference ivac.h includes "ring.h" + "portaudio.h"; ring.h
//     is unported and portaudio.h is the deferred device layer —
//     neither type is referenced by this Stage-1 struct).
//   * The WDSP rmatchV entry points (create_rmatchV / xrmatchIN /
//     xrmatchOUT / ...) resolve through the operator-approved
//     wire/wdspcalls.h seam (Stage 0); the AAMix entry points
//     (create_aamix / xMixAudio / SetAAudioMix*) are the ported
//     wire/AAMix.h surface — the reference statically links both.
//   * The reference's literal `__declspec (dllexport)` / `PORT`
//     prefixes on the exported create_ivac / xvacIN / xvacOUT /
//     SetIVAC* functions are carried as PORT (the documented
//     cmcomm.h mapping: the reference builds ChannelMaster as a
//     DLL; Lyra links the family into the executable, where PORT
//     expands empty).
//   * The reference's create_resampleV / xresampleV /
//     destroy_resampleV declarations (the legacy PortAudio device-
//     resampler helpers) are NOT carried — they are the deferred
//     device-I/O layer (Stage 2 / not needed), exactly as the
//     skipped CallbackIVAC / StartAudioIVAC / StopAudioIVAC.
//
// See NOTICE.md and CREDITS.md (repo root) for full upstream
// attribution.

#pragma once

#include "wire/cmcomm.h"

namespace lyra::wire {

// Reference ivac.h:34 (verbatim):
#define MAX_EXT_VACS			(16)

// Reference ivac.h:36-96 (verbatim, EXCEPT the omitted PortAudio
// device-I/O fields — see preamble):

typedef struct _ivac
{
	int run;
	int iq_type;					// 1 if using raw IQ data; 0 for audio
	int stereo;						// 1 for stereo; 0 otherwise
	int iq_rate;
	int mic_rate;
	int audio_rate;
	int txmon_rate;
	int vac_rate;					// VAC sample rate
	int mic_size;
	int iq_size;
	int audio_size;
	int txmon_size;
	int vac_size;					// VAC buffer size
	void *mixer;					// pointer to async audio mixer
	double* bitbucket;				// dump for un-needed resampler output

	void *rmatchIN;
	void *rmatchOUT;

	int INringsize;
	int OUTringsize;

	// device I/O fields added in Stage 2 (Qt QAudioSource/QAudioSink) — see IVAC_PORT_PLAN.md
	// (reference ivac.h:60-61 here: `PaStreamParameters inParam, outParam;  PaStream *Stream;`)

	size_t mono_in_to_stereo_capacity;		// capacity of mono to stereo buffer
	double* mono_in_to_stereo_buffer;		// buffer for mono to stereo conversion

	int host_api_index;
	int input_dev_index;
	int output_dev_index;
	int num_channels;
	double in_latency;
	double out_latency;
	double pa_in_latency;
	double pa_out_latency;
	int vox;
	int mox;
	int mon;
	int vac_bypass;
	int vac_combine_input;
	double vac_preamp;
	double vac_rx_scale;
	double vac_mon_scale;		// MW0LGE_21k9d the volume level of the vac mon
	int INforce;				// force var ratio for rmatchIN
	double INfvar;				// var value when forced for rmatchIN
	int OUTforce;				// force var ratio for rmatchOUT
	double OUTfvar;				// var value when forced for rmatchOUT

	double initial_INvar;			// init the var ratio
	double initial_OUTvar;			// init the var ratio

	int swapIQout;

	int exclusive_in;				// only use with wasapi right now
	int exclusive_out;				// only use with wasapi right now

	CRITICAL_SECTION cs_ivac;
} ivac, *IVAC;

// Reference ivac.h:98-100 (verbatim) — the buffer helpers + the
// mixer Outbound callback:
void combinebuff (int n, double* a, double* combined);
void scalebuff (int n, double* in, double k, double* out);
void xvac_out(int id, int nsamples, double* buff);

// Reference ivac.h:105-123 (verbatim, minus the deferred
// create_resampleV / xresampleV / destroy_resampleV device-layer
// helpers — see preamble):
PORT void destroy_ivac (int id);
PORT void xvacIN(int id, double* in_tx, int bypass);
PORT void xvacOUT(int id, int stream, double* data);
PORT void create_ivac (
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
	);

// Reference ivac.c SetIVAC* / diag surface (the reference defines
// most of these as PORT in ivac.c WITHOUT an ivac.h declaration —
// the external Thetis C# consumer re-declares them via DllImport;
// Stage 1 declares the full set here so the engine + the unit test
// link against one header).
extern PORT void SetIVACRBReset(int id, int reset);
extern PORT void SetIVACrun(int id, int run);
extern PORT void SetIVACiqType(int id, int type);
extern PORT void SetIVACstereo(int id, int stereo);
extern PORT void SetIVACvacRate(int id, int rate);
extern PORT void SetIVACmicRate(int id, int rate);
extern void SetIVACtxmonRate(int id, int rate);
extern PORT void SetIVACvacSize(int id, int size);
extern PORT void SetIVACmicSize(int id, int size);
extern PORT void SetIVACiqSizeAndRate(int id, int size, int rate);
extern PORT void SetIVACaudioSize(int id, int size);
extern void SetIVACtxmonSize(int id, int size);
extern PORT void SetIVACaudioRate(int id, int rate);
extern PORT void SetIVAChostAPIindex(int id, int index);
extern PORT void SetIVACinputDEVindex(int id, int index);
extern PORT void SetIVACoutputDEVindex(int id, int index);
extern PORT void SetIVACnumChannels(int id, int n);
extern PORT void SetIVACInLatency(int id, double lat, int reset);
extern PORT void SetIVACOutLatency(int id, double lat, int reset);
extern PORT void SetIVACPAInLatency(int id, double lat, int reset);
extern PORT void SetIVACPAOutLatency(int id, double lat, int reset);
extern PORT void SetIVACvox(int id, int vox);
extern PORT void SetIVACmox(int id, int mox);
extern PORT void SetIVACmon(int id, int mon);
extern PORT void SetIVACmonVol(int id, double vol);
extern PORT void SetIVACpreamp(int id, double preamp);
extern PORT void SetIVACrxscale(int id, double scale);
extern PORT void SetIVACbypass(int id, int bypass);
extern PORT void SetIVACcombine(int id, int combine);
extern PORT void getIVACdiags (int id, int type, int* underflows, int* overflows, double* var, int* ringsize, int* nring);
extern PORT void forceIVACvar (int id, int type, int force, double fvar);
extern PORT void resetIVACdiags(int id, int type);
extern PORT void SetIVACFeedbackGain(int id, int type, double feedback_gain);
extern PORT void SetIVACSlewTime(int id, int type, double slew_time);
extern PORT void SetIVACPropRingMin(int id, int type, int prop_min);
extern PORT void SetIVACPropRingMax(int id, int type, int prop_max);
extern PORT void SetIVACFFRingMin(int id, int type, int ff_ringmin);
extern PORT void SetIVACFFRingMax(int id, int type, int ff_ringmax);
extern PORT void SetIVACFFAlpha(int id, int type, double ff_alpha);
// DEFERRED: GetIVACControlFlag needs getControlFlag in wdspcalls (follow-up)
extern PORT void SetIVACinitialVars(int id, double INvar, double OUTvar);
extern PORT void SetIVACswapIQout(int id, int swap);
extern PORT void SetIVACExclusiveOut(int id, int exclusive_out);
extern PORT void SetIVACExclusiveIn(int id, int exclusive_in);

}  // namespace lyra::wire

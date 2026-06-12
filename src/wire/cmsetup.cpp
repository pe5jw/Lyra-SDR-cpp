// Lyra-cpp — cmsetup.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source file: ChannelMaster/cmsetup.c (whole file, verbatim)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
//
// ============================================================
// DIRECT PORT 2026-06-12 (P0.d)
// ============================================================
//
// Every function body is the reference cmsetup.c VERBATIM.  The
// reference's own comments are carried in place.
//
// Only Lyra-cpp packaging differences (NOT code changes):
//   * `lyra::wire` namespace (reference is global C).
//   * `#include "cmsetup.h"` + `#include "cmaster.h"` become the
//     wire/ paths.
//   * PORT carries the reference's `__declspec (dllexport)` (the
//     documented cmcomm.h mapping; expands empty in the exe).
//
// DEFERRED inside otherwise-verbatim bodies (each marked inline):
//   * CreateRadio / DestroyRadio: the create_pipe/create_sync (and
//     destroy_) calls — ChannelMaster pipe.c / sync.c are unported.
//     Lyra-cpp's startup calls create_cmaster()/destroy_cmaster()
//     directly (main.cpp), so these two wrappers currently have no
//     Lyra caller; they are ported for surface completeness with
//     the unported calls commented.
//
// See NOTICE.md and CREDITS.md (repo root) for full attribution.

#include "wire/cmsetup.h"
#include "wire/CMaster.h"

namespace lyra::wire {

// Reference cmsetup.c:29-56 (verbatim):

// set radio structure, call this first
// these parameters are used by create_cmaster() to determine units to create & buffer sizes
PORT
void SetRadioStructure (
	int cmSTREAM,			// total number of input streams to set up
	int cmRCVR,				// number of receivers to set up
	int cmXMTR,				// number of transmitters to set up
	int cmSubRCVR,			// number of sub-receivers per receiver to set up
	int cmNspc,				// number of TYPES of special units
	int* cmSPC,				// number of special units of each type
	int* cmMAXInbound,		// maximum number of samples in a call to Inbound(), per stream
	int cmMAXInRate,		// maximum sample rate of an input stream
	int cmMAXAudioRate,		// maximum channel audio output rate (incl. rcvr and tx monitor audio)
	int cmMAXTxOutRate		// maximum transmitter channel output sample rate
	)
{
	pcm->cmSTREAM = cmSTREAM;
	pcm->cmRCVR = cmRCVR;
	pcm->cmXMTR = cmXMTR;
	pcm->cmSubRCVR = cmSubRCVR;
	pcm->cmNspc = cmNspc;
	memcpy (pcm->cmSPC, cmSPC, pcm->cmNspc * sizeof (int));
	memcpy (pcm->cmMAXInbound, cmMAXInbound, pcm->cmSTREAM * sizeof (int));
	pcm->cmMAXInRate = cmMAXInRate;
	pcm->cmMAXAudioRate = cmMAXAudioRate;
	pcm->cmMAXTxOutRate = cmMAXTxOutRate;
}

// Reference cmsetup.c:58-86 (verbatim):

// set default sample rates, call this before 'create'
PORT
void set_cmdefault_rates (
	int* xcm_inrates,
	int  aud_outrate,
	int* rcvr_ch_outrates,
	int* xmtr_ch_outrates
	)
{
	int i;
	for (i = 0; i < pcm->cmSTREAM; i++)
	{
		pcm->xcm_inrate[i] = xcm_inrates[i];
		pcm->xcm_insize[i] = getbuffsize (pcm->xcm_inrate[i]);
	}
	pcm->audio_outrate = aud_outrate;
	pcm->audio_outsize = getbuffsize (pcm->audio_outrate);
	pcm->audioCodecId = HERMES;
	for (i = 0; i < pcm->cmRCVR; i++)
	{
		pcm->rcvr[i].ch_outrate = rcvr_ch_outrates[i];
		pcm->rcvr[i].ch_outsize = getbuffsize (pcm->rcvr[i].ch_outrate);
	}
	for (i = 0; i < pcm->cmXMTR; i++)
	{
		pcm->xmtr[i].ch_outrate = xmtr_ch_outrates[i];
		pcm->xmtr[i].ch_outsize = getbuffsize (pcm->xmtr[i].ch_outrate);
	}
}

// Reference cmsetup.c:88-102 (verbatim except the two DEFERRED
// pipe/sync call pairs, commented inline):

PORT
void CreateRadio()
{
	create_cmaster();
	// create_pipe();   // DEFERRED — ChannelMaster pipe.c unported
	// create_sync();   // DEFERRED — ChannelMaster sync.c unported
}

PORT
void DestroyRadio()
{
	// destroy_sync();  // DEFERRED — ChannelMaster sync.c unported
	// destroy_pipe();  // DEFERRED — ChannelMaster pipe.c unported
	destroy_cmaster();
}

// Reference cmsetup.c:104-111 (verbatim):

// buffer sizes are a function of sample rate to yield constant latency
PORT
int getbuffsize (int rate)
{
	const int base_rate = 48000;
	const int base_size = 64;
	return base_size * rate / base_rate;
}

// Reference cmsetup.c:113-117 (verbatim):

PORT
int getInputRate (int stype, int id)
{
	return pcm->xcm_inrate[inid (stype, id)];
}

// Reference cmsetup.c:119-133 (verbatim):

// C4701 disabled function-wide (it is a code-gen-stage warning the
// line-level suppress cannot reach): verbatim text wins — the
// reference switch has no default; callers pass stype 0/1 only.
#pragma warning(push)
#pragma warning(disable: 4701)
PORT
int getChannelOutputRate (int stype, int id)
{
	int rate;
	switch (stype)
	{
	case 0:
		rate = pcm->rcvr[id].ch_outrate;
		break;
	case 1:
		rate = pcm->xmtr[id].ch_outrate;
		break;
	}
	return rate;
}
#pragma warning(pop)

// Reference cmsetup.c:135-145 (verbatim — the reference's stream/
// channel numbering contract, operationally load-bearing):
//
// Inbound Stream & Channel IDs
//
// Inbound Data Streams are numbered beginning with receiver streams, followed by transmitter streams, and
// the followed by Special Streams.  An example of a special stream might be a receive data stream that is
// not used in a 'standard receiver' but is instead used only to create a panadapter display.  There can be
// multiple classes of Special Streams, each with a range of values.
//
// Receiver Streams are numbered 0 through (cmMAXrcvr - 1).  Transmitter streams are numbered cmMAXrcvr
// through (cmMAXrcvr + cmMAXxmtr - 1).  Special streams begin at (cmMAXrcvr + cmMAXxmtr).
// ChannelMaster internal 'id's use an 'id' number rather than the stream number.  These 'id's are
// determined as follows:

// Reference cmsetup.c:147-150 (verbatim):

int rxid (int stream)					// ChannelMaster id of a receiver
{
	return stream;
}

// Reference cmsetup.c:152-155 (verbatim):

int txid (int stream)					// ChannelMaster id of a transmitter
{
	return stream - pcm->cmRCVR;
}

// Reference cmsetup.c:157-160 (verbatim):

int sp0id (int stream)
{
	return stream - pcm->cmRCVR - pcm->cmXMTR;
}

// Reference cmsetup.c:162-169 (verbatim):

int stype (int stream)					// the stream type, 'stype', can be inferred from stream number
{
	int type;
	if      (stream < pcm->cmRCVR)						type =  0;	// rx
	else if (stream < pcm->cmRCVR + pcm->cmXMTR)		type =  1;	// tx
	else												type =  2;	// special
	return type;
}

// Reference cmsetup.c:171-173 (verbatim):
//
// DSP Channels must be allocated for receivers and transmitters.  There are cmSubRcvr channels per
// receiver data stream and there is one channel per transmitter data stream.  Channel numbers for
// receiver and transmitter channels are determined as follows:

// Reference cmsetup.c:175-192 (verbatim):

PORT
int chid (int stream, int subrx)		 // id of a dsp channel ('channel' number)
{
	int ch_id;
	switch (stype (stream))
	{
	case 0:
		ch_id = pcm->cmSubRCVR * stream + subrx;
		break;
	case 1:
		ch_id = stream + (pcm->cmSubRCVR - 1) * pcm->cmRCVR;
		break;
	default:
		ch_id = -1;
		break;
	}
	return ch_id;
}

// Reference cmsetup.c:194-214 (verbatim):

PORT
int inid (int stype, int id)	// the id of the input buffer (= id of stream) can be determined
{
	int in_id;
	switch (stype)
	{
	case 0:
		in_id = id;
		break;
	case 1:
		in_id = id + pcm->cmRCVR;
		break;
	case 2:
		in_id = id + pcm->cmRCVR + pcm->cmXMTR;
		break;
	default:
		in_id = -1;
		break;
	}
	return in_id;
}

// Reference cmsetup.c:216-232 (verbatim):

int mixinid (int stream, int subrx)	// audio mixer input id
{
	int mix_in_id;
	switch (stype (stream))
	{
	case 0:
		mix_in_id = pcm->cmSubRCVR * stream + subrx;
		break;
	case 1:
		mix_in_id = stream + (pcm->cmSubRCVR - 1) * pcm->cmRCVR;
		break;
	default:
		mix_in_id = -1;
		break;
	}
	return mix_in_id;
}

}  // namespace lyra::wire

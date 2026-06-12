// Lyra-cpp — cmsetup.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source file: ChannelMaster/cmsetup.h (whole file, verbatim)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2014 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// ============================================================
// DIRECT PORT 2026-06-12 (P0.d)
// ============================================================
//
// This is the reference cmsetup.h VERBATIM: the five cmMAX* sizing
// macros and the eight stream/channel-id helper declarations.
//
// Only Lyra-cpp packaging differences (NOT code changes):
//   * `lyra::wire` namespace (reference is global C).
//   * The cmMAX* sizing macros stay literal `#define`s exactly as
//     the reference writes them (they size arrays inside the
//     `_cmaster` struct at wire/CMaster.h and must be available to
//     the preprocessor the same way).
//   * The reference's literal `__declspec (dllexport)` on chid /
//     inid / getbuffsize is carried as PORT (the documented
//     cmcomm.h mapping: the reference builds ChannelMaster as a
//     DLL; Lyra links the family into the executable, where PORT
//     expands empty).
//   * The PORT-exported configuration functions defined in
//     cmsetup.c (SetRadioStructure / set_cmdefault_rates /
//     CreateRadio / DestroyRadio / getInputRate /
//     getChannelOutputRate) are NOT declared here — the reference
//     declares them consumer-side via C# DllImport.  Lyra
//     consumers (main.cpp) re-declare them consumer-side, the
//     same convention scratch/test_ilv.cpp uses for the ILV PORT
//     setters.
//
// See NOTICE.md and CREDITS.md (repo root) for full upstream
// attribution.

#pragma once

#include "wire/cmcomm.h"   // PORT macro

// Reference cmsetup.h:27-32 (verbatim):

// these parameters are used to size arrays in static structures
#define cmMAXrcvr		(16)				// maximum number of receivers
#define cmMAXxmtr		( 4)				// maximum number of transmitters
#define cmMAXSubRcvr	( 4)				// number of sub-receivers per receiver, including the base receiver
#define cmMAXspc		( 2)				// maximum number of special unit TYPES
#define cmMAXstream		(32)				// maximum number of streams

namespace lyra::wire {

// Reference cmsetup.h:35-50 (verbatim):

extern int rxid (int stream);

extern int txid (int stream);

extern PORT int chid (int stream, int subrx);

extern int sp0id (int stream);

extern int stype (int stream);

extern PORT int inid(int stype, int id);

extern int mixinid (int stream, int subrx);

extern PORT int getbuffsize (int rate);

}  // namespace lyra::wire

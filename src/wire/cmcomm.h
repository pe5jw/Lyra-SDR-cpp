// Lyra-cpp — cmcomm.h
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Upstream: https://github.com/mi0bot/OpenHPSDR-Thetis
// Upstream mainline: https://github.com/ramdor/Thetis
// Source files:
//   - ChannelMaster/cmcomm.h : the ChannelMaster family's common
//     header (every ChannelMaster .c includes it)
//   - wdsp/comm.h            : `complex` typedef + PORT macro
//   - wdsp/utilities.c:37-43 : malloc0 definition (NOT a DLL
//     export — defined above the "// Exported calls" marker, so
//     Lyra-cpp must carry its own verbatim copy)
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2013-2019 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// Lyra-cpp is GPL v3+; redistribution complies with GPL terms
// (preserved copyright, documented modifications, complete source
// at https://github.com/N8SDR1/Lyra-SDR-cpp).
//
// DIRECT-PORT NOTE (operator rule, 2026-06-09): this header
// carries the reference declarations VERBATIM.  The only Lyra-cpp
// packaging differences are (a) the `lyra::wire` namespace (the
// reference is global C; some namespace is unavoidable in a C++
// executable and lyra::wire is where the ChannelMaster family
// lives) and (b) PORT expands empty (the reference builds
// ChannelMaster as a DLL and PORT is __declspec(dllexport);
// Lyra-cpp links the ported family into the executable, where
// dllexport is meaningless).  Both verified non-deviations by
// scratch/_typedef_fidelity_test.cpp.
//
// This header GROWS toward the full reference cmcomm.h surface as
// the direct port proceeds (P0.d adds the _cmaster struct + pcm
// umbrella here).  Until then it carries only the common-comm
// surface the ported translation units consume.

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <intrin.h>
#include <stdlib.h>
#include <string.h>

// Reference wdsp/comm.h: `#define PORT __declspec(dllexport)`.
// Lyra-cpp builds the ported family into the executable — expands
// empty so PORT-tagged reference functions port byte-for-byte.
#ifndef PORT
#define PORT
#endif

namespace lyra::wire {

// Reference wdsp/comm.h:
//   typedef double complex[2];
// Coexists with std::complex (different name lookup scopes) —
// proven by scratch/_typedef_fidelity_test.cpp.
typedef double complex[2];

// Reference wdsp/utilities.c:37-43 (verbatim; defined in
// cmcomm.cpp).  Pairs with `_aligned_free` at every reference
// free site (e.g. ilv.c:53-54).
void *malloc0 (int size);

}  // namespace lyra::wire

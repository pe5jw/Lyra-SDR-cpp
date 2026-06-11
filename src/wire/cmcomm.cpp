// Lyra-cpp — cmcomm.cpp
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Source file: wdsp/utilities.c:37-43 (malloc0 — not a DLL
// export, so the direct port carries its own verbatim copy).
// Source version: 2.10.3.13 (MI0BOT HL2 fork)
// Original copyright: (C) 2013-2019 Warren Pratt, NR0V
// License: GNU General Public License v2 or later
// See cmcomm.h preamble for the full attribution + the two
// packaging-only differences (namespace, PORT-empty).

#include "wire/cmcomm.h"

namespace lyra::wire {

// Reference wdsp/utilities.c:37-43 (verbatim):
//
//   void *malloc0 (int size)
//   {
//       int alignment = 16;
//       void* p = _aligned_malloc (size, alignment);
//       if (p != 0) memset (p, 0, size);
//       return p;
//   }
void *malloc0 (int size)
{
	int alignment = 16;
	void* p = _aligned_malloc (size, alignment);
	if (p != 0) memset (p, 0, size);
	return p;
}

}  // namespace lyra::wire

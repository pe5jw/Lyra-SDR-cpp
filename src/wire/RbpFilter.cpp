// Lyra — Alex filter-board control words.  See RbpFilter.h.
//
// Phase 2 step 13 — populated per the signed §2 parity checkpoint
// (docs/architecture/PARITY_CHECKPOINTS.md).  WIRE-INERT: built
// but not wired into HL2Stream until §10.3 step 14.

#include "wire/RbpFilter.h"

namespace lyra::wire {

// Default ctor/dtor — all bitfields and `enable` carry in-class
// initializers, so zero work beyond letting C++ value-init each
// member.  The `bpfilter` dword default-inits to 0 (which zeros
// every bitfield via the shared union storage); `enable` defaults
// to the Alex0/Alex1 id per the in-class initializer.
RbpFilter::RbpFilter()   = default;
RbpFilter::~RbpFilter()  = default;

RbpFilter2::RbpFilter2()  = default;
RbpFilter2::~RbpFilter2() = default;

// §2.5 — Global instance pointers, names verbatim from the
// reference (network.h:340 + :389).  Assigned at HL2 session
// start by the wire-layer initializer (Phase 2 wire-up);
// stay nullptr until then.
RbpFilter*  prbpfilter  = nullptr;
RbpFilter2* prbpfilter2 = nullptr;

}  // namespace lyra::wire

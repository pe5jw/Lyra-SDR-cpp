// Lyra — MOX-edge cos² fade (§5 / §10.2 tx layer).
//
// cos² up/down ramp wrapper.  NEW placeholder per §10.2; the
// existing top-level `src/mox_edge_fade.{h,cpp}` survives the rip
// and continues to be used until Phase 2 replaces it with this
// new class.  They coexist intentionally during Phase 1.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §5.

#pragma once

namespace lyra::tx {

class MoxEdgeFade {
public:
    MoxEdgeFade();
    ~MoxEdgeFade();
};

}  // namespace lyra::tx

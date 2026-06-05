// Lyra — EP6 receive thread (§5 / §10.2 wire layer).
//
// std::thread, MMCSS Pro Audio.  recvfrom loop on EP6 socket;
// parses each datagram, dispatches per-DDC samples to per-stream
// rings via an INLINE per-`nddc` switch (matches the reference's
// `MetisReadThreadMainLoop_HL2:544-558` switch verbatim — no
// separate DdcMap class per the locked 2026-06-05 "do as the
// reference does" discipline; routing instruction matches §1 +
// §4-Capabilities + §3 DispatchState scattered-inline pattern).
// Mirrors the reference `MetisReadThreadMainLoop_HL2`.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §3 / §8.

#pragma once

namespace lyra::wire {

class Ep6RecvThread {
public:
    Ep6RecvThread();
    ~Ep6RecvThread();
};

}  // namespace lyra::wire

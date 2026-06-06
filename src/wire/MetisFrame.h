// Lyra — MetisFrame wire-emit primitive (§6-B + §7 / §10.2 wire layer).
//
// TU-scope free function + file-scope mutable state mirroring the
// reference's `MetisWriteFrame(int endpoint, char* bufp)` at
// `ChannelMaster/networkproto1.c:216-237` and the reference's
// file-scope `MetisOutBoundSeqNum` at `:30` + the file-scope
// `listenSock` socket descriptor.  Both the EP2 send thread
// (`Ep2SendThread::run_loop` — `sendProtocol1Samples` mirror) AND
// the startup C&C priming pass (`ForceCandC::prime` —
// `ForceCandCFrames` mirror) call into this single shared
// primitive and share the single outbound sequence counter —
// EXACTLY matching the reference's structural pattern where every
// caller (priming, main-loop send, CW priming) funnels through
// the one `MetisWriteFrame` free function with one
// `MetisOutBoundSeqNum` global.
//
// LOCKING:  None.  The reference does NOT lock around
// `MetisWriteFrame` calls.  Concurrency safety is preserved by
// TEMPORAL SEPARATION:  the priming pass (`ForceCandC::prime`)
// runs SYNCHRONOUSLY on the session-open thread BEFORE
// `Ep2SendThread::start()` is called, so the two callers are
// disjoint in time — no race can occur.  Adding a mutex here
// would be a Lyra-native patch with no reference counterpart;
// per the operator-locked "do as the reference does, no
// patching" directive (2026-06-06) it is intentionally omitted.
//
// §6-B parity correction (sign-off 2026-06-06):  this file
// supersedes §6 Q3 (which had `out_seq_num_` as an
// `Ep2SendThread` member) and §6 Q5 (which had a `send_lock_`
// mutex).  The TU-scope `g_metis_out_seq_num` + `g_metis_*`
// socket/dest globals + the lock-less call path are the direct
// reference mirror.

#pragma once

#include <cstddef>
#include <cstdint>

namespace lyra::wire {

// ---- wire-bind setter ----
//
// Sets the TU-scope socket descriptor + destination address used
// by `metis_write_frame()`.  Caller-owned; the underlying socket
// + sockaddr must outlive the wire-thread lifetime.  Direct
// mirror of the reference's per-session initialization of the
// `listenSock` global + `prn->base_outbound_port` field at the
// discovery/StartMetis path.
//
// Idempotent — re-binding to the same values is a no-op.  Safe
// to call from multiple bring-up paths (e.g. session_open AND
// `Ep2SendThread::start()`) during the §7 → step-14 migration.
void metis_wire_bind(int          socket_fd,
                     const void*  dest_addr,
                     std::size_t  dest_addrlen);

// ---- metis_write_frame ----
//
// Composes the 8-byte HPSDR header (`0xEF 0xFE 0x01 endpoint` +
// 4-byte BE outbound sequence number), memcpys the 1024-byte
// payload, calls `sendto` on the bound socket, and post-
// increments `g_metis_out_seq_num`.  Returns the payload bytes
// sent (positive — should equal 1024 on success) or `-1` on a
// socket error.
//
// Direct mirror of `int MetisWriteFrame(int endpoint, char*
// bufp)` at `networkproto1.c:216-237`.  Reference quirks
// preserved verbatim per Rule 24:
//   - post-increment (first emit ships seq=0, second ships
//     seq=1, ...)
//   - 4-byte BE byte-swap of native uint32 into wire bytes
//     `[4..7]` (high byte first)
//   - 1024-byte payload size + 8-byte header = 1032 bytes
//     out the door
//
// Caller MUST have called `metis_wire_bind()` first with a
// valid socket + dest_addr, and `payload_1024` MUST be a valid
// 1024-byte buffer.  Reference does NOT null-check inside
// `MetisWriteFrame` — it would crash on a null `listenSock` /
// `prn`.  Lyra preserves the same contract verbatim — the
// calling discipline IS the safety property per "do as
// reference, period" (operator directive 2026-06-06).
int  metis_write_frame(int            endpoint,
                       const uint8_t* payload_1024);

// ---- diagnostic accessor ----
//
// Returns the current value of the TU-scope outbound sequence
// counter.  Diagnostic only — production callers should not
// branch on this value (the wire path increments it as a side
// effect of `metis_write_frame`).
std::uint32_t metis_out_seq_num();

}  // namespace lyra::wire

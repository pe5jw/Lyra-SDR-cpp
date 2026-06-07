// Lyra — EP6 receive thread (§5 / §10.2 wire layer).
//
// std::thread, MMCSS Pro Audio.  WSAEventSelect-driven event
// loop on the EP6 socket (per §5.10 / §1-C Stage 4B); parses
// each datagram, dispatches per-DDC samples to per-stream rings
// via an INLINE per-`nddc` switch (matches the reference
// `MetisReadThreadMainLoop_HL2:544-558` switch verbatim — no
// separate DdcMap class).
//
// Source mirror:
//   `ChannelMaster/networkproto1.c:422-586`
//   (MetisReadThreadMainLoop_HL2).
//
// §1-C Stage 4B (sign-off 2026-06-06):  the §1.1 networking-
// infrastructure exclusion is reverted.  Buffers (`RxBuff`,
// `TxReadBufp`, `ReadBufp`) live in `prn->...` per the reference
// (`network.h:60-66`).  The WSA event handle (`hDataEvent`) +
// `wsaProcessEvents` cache also live in `prn->...` per
// `network.h:105-106`.  HL2 RX seq tracking (`MetisLastRecvSeq`,
// `SeqError`) + `ControlBytesIn[5]` live as TU-scope statics in
// `Ep6RecvThread.cpp` — direct mirror of reference's file-scope
// globals at `networkproto1.c:26-28` + `network.h:414` (these
// are NOT in `_radionet`).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace lyra::wire {

class Router;  // forward — Router.h pulled by Ep6RecvThread.cpp.

// §1-C Stage 4F: `Ep6IqSink` typedef removed — declared in
// the §5 populate but never instantiated as a class member
// (routing goes through `Router*`/`router_id_` not a per-DDC
// sink).  Dead-code cleanup.

// Sink for the host-side decoded EP6 telemetry payload.  Fires
// at most once per EP6 datagram (telemetry classes decoded from
// the C0 status byte per the §5.5 5-class switch).  Operator
// /host code subscribes for the meter / temp / supply / PA
// metrics.  Carries raw 16-bit values; scaling to physical units
// happens at the consumer (per-family conversion lives there).
struct Ep6Telemetry {
    // Class identifier — value of `C0 & 0xf8`, before shifting
    // (i.e. 0x00 = adc-overload class, 0x08 = fwd-power class,
    // etc.).  See `networkproto1.c:498-517` for the per-class
    // payload meanings on HL2.
    uint8_t  class_id    = 0;
    uint8_t  control[4]  = {0, 0, 0, 0};

    // Convenience decode: a 16-bit BE word from control[i:i+1].
    uint16_t word(int i) const {
        if (i < 0 || i + 1 > 4) return 0;
        return static_cast<uint16_t>(
            (static_cast<uint16_t>(control[i])     << 8) |
             static_cast<uint16_t>(control[i + 1]));
    }
};

using Ep6TelemetrySink = std::function<void(const Ep6Telemetry&)>;

// Sink for mic samples harvested from the per-sample-slot
// trailer.  Mirrors the reference's `Inbound(inid(1,0),
// mic_sample_count, prn->TxReadBufp)` call at
// `networkproto1.c:579`: payload is interleaved IQ-pair
// doubles where I = mic / 2^15 (top 16 bits of the slot's
// 2-byte mic word packed into the high 16 bits of an int32,
// then * 1/2^31 → effective /2^15 normalization) and Q = 0.0.
//
// Sink receives `n_samples` mic samples = `2 * n_samples`
// doubles.  Decimation honours `mic_decimation_factor` per
// `networkproto1.c:566-577`: with factor=0 (BSS default) NO
// harvest fires; HL2 default operating point sets factor=1
// at session open for every-slot harvest.
using Ep6MicSink = std::function<void(int n_samples,
                                      const double* iq_pairs)>;

// Sink for the I2C-readback overlay (when C0 bit 7 is set per
// `MetisReadThreadMainLoop_HL2:500-508`).  Inert on this build
// until I2C consumers register.
using Ep6I2cSink = std::function<void(const uint8_t* bytes,
                                      int n_bytes)>;

// Sink for the HW PTT-in level (C0 bit 0 per `networkproto1.c:496`).
// Fires at most once per EP6 datagram (one C0 status byte per USB
// frame; we fire on the SECOND frame so the sink sees the latest
// level).  Sink receives the level (0 or 1); consumer is
// responsible for edge-detect + downstream MOX dispatch.  HL2Stream
// registers this to drive the operator opt-in HW PTT forwarder
// (mirror of the rxWorkerLoop-based forwarder this layer replaces).
//
// Per CLAUDE.md §10 Q#1 the bench-verified discipline is: do NOT
// auto-key on this level — the N8SDR HL2+/AK4951 unit can carry a
// non-zero ptt_in at RX rest; the consumer must gate on its
// `hwPttEnabled` opt-in atomic before acting.
using Ep6HwPttSink = std::function<void(bool ptt_in)>;

class Ep6RecvThread {
public:
    Ep6RecvThread();
    ~Ep6RecvThread();

    Ep6RecvThread(const Ep6RecvThread&)            = delete;
    Ep6RecvThread& operator=(const Ep6RecvThread&) = delete;

    // ---- lifecycle ----
    //
    // `start(socket_fd)` spawns the reader thread.  Caller owns
    // the socket; the thread reads via WSAEventSelect +
    // WSAWaitForMultipleEvents (Win32) per §1-C Stage 4B.  The
    // wire-layer initializer MUST have set the `prn` global
    // pointer to a valid `RadioNet` instance before calling
    // start — buffer + WSA event state lives in `prn->...`.
    //
    // `stop()` signals the thread to exit, joins, and frees the
    // WSA event handle.  Safe to call multiple times.  Reader
    // thread sets its own MMCSS class to Pro Audio (Windows)
    // when available.
    void start(int socket_fd);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }

    // ---- sink registration (call BEFORE start) ----
    //
    // Router pointer — the SOLE per-DDC dispatch path.  Mirrors
    // the reference's `xrouter(0, ...)` calls verbatim: DDC IQ
    // is dispatched via `xrouter()` (direct, single-DDC paths)
    // and `twist() + xrouter()` (interleaved-pair paths) per the
    // `switch(nddc)` at `networkproto1.c:544-558`.
    //
    // Operator/host code registers per-port consumers on the
    // Router (typically slot 0) to receive RX1 / RX2 / inner-pair
    // samples.  HL2 / nddc=4 routing:
    //   - source 0  =  DDC0          (RX1)
    //   - source 1  =  twist(DDC2, DDC3)  (PS feedback / inner pair)
    //   - source 2  =  DDC1          (RX2)
    //
    // Optional; if null the IQ samples are unpacked and then
    // dropped (no dispatch).
    void set_router(Router* router, int router_id);

    // Telemetry / mic / I2C / HW-PTT sinks.  Mic mirrors the
    // reference's `Inbound(inid(1,0), ...)` (`networkproto1.c:579`).
    // HW PTT mirrors reference's `prn->ptt_in = ControlBytesIn[0]
    // & 0x1` at `networkproto1.c:496` — the sink consumer does its
    // own opt-in gating + edge detect.
    void set_telemetry_sink(Ep6TelemetrySink sink);
    void set_mic_sink(Ep6MicSink sink);
    void set_i2c_sink(Ep6I2cSink sink);
    void set_hw_ptt_sink(Ep6HwPttSink sink);

    // §1-C Stage 4B: diagnostic counter accessors removed.
    // RX seq tracking + datagram count moved to TU-scope
    // statics in `Ep6RecvThread.cpp` mirroring reference's
    // file-scope `MetisLastRecvSeq` + `SeqError` at
    // `networkproto1.c:26-28` (these are NOT in `_radionet`).

private:
    // Reader-thread entry point.
    void run_loop();

    // Parse one EP6 datagram per the reference's
    // `MetisReadDirect` + `MetisReadThreadMainLoop_HL2:475+`
    // split: `raw_header` is the 1032-byte raw recv buffer
    // (used for BE seq tracking at offsets 4..7); `usb_frames`
    // is the 1024-byte header-stripped buffer = 2 × 512-byte
    // USB frames, identical to reference's `FPGAReadBufp`
    // post-`memcpy(... prn->ReadBufp + 8, 1024);`.
    void process_datagram(const uint8_t* raw_header,
                          const uint8_t* usb_frames);

    // Parse one 512-byte USB frame (sync + C0-C4 status header
    // + 19 sample slots).
    void process_usb_frame(const uint8_t* frame);

    // Decode the C0-C4 status header — handles I2C-overlay
    // dispatch when `cc[0] & 0x80` is set (§5.5).
    void decode_status_header(const uint8_t cc[5]);

private:
    // §1-C Stage 4F: `socket_fd_` member removed.  Socket
    // bound TU-scope in `wire/MetisFrame.cpp` via
    // `metis_wire_bind()` and read via `metis_socket_fd()`
    // accessor — direct mirror of reference's file-scope
    // `listenSock` global.  Symmetric with `Ep2SendThread`.

    // Thread control.
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_request_{false};
    std::unique_ptr<std::thread> thread_;

    // §1-C Stage 4B: buffers (rx_buff_ / tx_read_bufp_) and
    // raw receive buffer + control_bytes_in_ + sequence-tracking
    // members ALL MOVED.  Reference _radionet fields (RxBuff,
    // TxReadBufp, ReadBufp) live in `prn->...` per §1-C Stage 4A;
    // reference file-scope globals (ControlBytesIn, MetisLastRecvSeq,
    // SeqError) live as TU-scope statics in Ep6RecvThread.cpp
    // per the §6-B / Stage 4B sister-pattern.

    // Reference's `MAX_DDC` / max-spr-per-frame constants used
    // by the buffer-sizing at start() time + the per-DDC
    // unpack loop.  §1-C Stage 4F: `kMaxDdc` bumped from 4
    // (HL2-tightened) to 8 to match reference's `MAX_DDC=8`
    // for future ANAN-family parity (HL2 will still populate
    // only the first 4 entries since nddc=4 on this hardware).
    static constexpr int kMaxDdc          = 8;
    static constexpr int kMaxSprPerFrame  = 64;

    // ---- sinks ----
    Ep6TelemetrySink                telemetry_sink_;
    Ep6MicSink                      mic_sink_;
    Ep6I2cSink                      i2c_sink_;
    Ep6HwPttSink                    hw_ptt_sink_;
    Router*                         router_     = nullptr;
    int                             router_id_  = 0;
};

}  // namespace lyra::wire

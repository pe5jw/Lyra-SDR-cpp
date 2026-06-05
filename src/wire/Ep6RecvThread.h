// Lyra — EP6 receive thread (§5 / §10.2 wire layer).
//
// std::thread, MMCSS Pro Audio.  recvfrom loop on EP6 socket;
// parses each datagram, dispatches per-DDC samples to per-stream
// rings via an INLINE per-`nddc` switch (matches the reference
// `MetisReadThreadMainLoop_HL2:544-558` switch verbatim — no
// separate DdcMap class per the locked 2026-06-05 "do as the
// reference does" discipline; routing instruction matches §1 +
// §4-Capabilities + §3 DispatchState scattered-inline pattern).
//
// Source mirror:
//   `ChannelMaster/networkproto1.c:422-586`
//   (MetisReadThreadMainLoop_HL2).
//
// Per the signed §5 parity checkpoint, the per-datagram receive
// buffer `RxBuff[nddc][per-DDC]`, the staging buffer
// `TxReadBufp`, and the C&C-in byte cache `ControlBytesIn[5]`
// all live as instance members here (Rule §1.1 networking-buffer
// exclusion from `RadioNet` — buffers are thread-local to the
// EP6 reader, never shared).

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace lyra::wire {

class Router;  // forward — Router.h pulled by Ep6RecvThread.cpp.

// Sink for per-stream IQ-pair samples (interleaved I/Q doubles,
// normalized to ±1.0).  Mirrors the role of the reference's
// `Inbound()` channel-master push (§5.7 idiom translation —
// Lyra has no channel-master; sinks register directly).
using Ep6IqSink = std::function<void(int n_samples,
                                     const double* iq_pairs)>;

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

class Ep6RecvThread {
public:
    Ep6RecvThread();
    ~Ep6RecvThread();

    Ep6RecvThread(const Ep6RecvThread&)            = delete;
    Ep6RecvThread& operator=(const Ep6RecvThread&) = delete;

    // ---- lifecycle ----
    //
    // `start(socket_fd)` spawns the reader thread.  Caller owns
    // the socket; the thread reads via recvfrom but does NOT
    // close it.  `stop()` signals the thread to exit and joins;
    // safe to call multiple times.  Reader thread sets its own
    // MMCSS class to Pro Audio (Windows) when available.
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

    // Telemetry / mic / I2C sinks.  Mic mirrors the reference's
    // `Inbound(inid(1,0), ...)` (`networkproto1.c:579`).
    void set_telemetry_sink(Ep6TelemetrySink sink);
    void set_mic_sink(Ep6MicSink sink);
    void set_i2c_sink(Ep6I2cSink sink);

    // ---- statistics (diagnostic) ----
    //
    // Datagram + sequence-error counters.  Resetable.  Read from
    // any thread (atomic loads).
    uint64_t datagrams_received() const { return rx_datagrams_.load(); }
    uint64_t sequence_errors()    const { return seq_errors_.load(); }

private:
    // Reader-thread entry point.
    void run_loop();

    // Parse one EP6 datagram (2x 512-byte USB frames per UDP
    // datagram, per `MetisReadThreadMainLoop_HL2:475+`).
    void process_datagram(const uint8_t* data, std::size_t size);

    // Parse one 512-byte USB frame (sync + C0-C4 status header
    // + 19 sample slots).
    void process_usb_frame(const uint8_t* frame);

    // Decode the C0-C4 status header — handles I2C-overlay
    // dispatch when `cc[0] & 0x80` is set (§5.5).
    void decode_status_header(const uint8_t cc[5]);

    // Process one 512-byte USB frame: per-DDC unpack into
    // `rx_buff_[iddc]`, dispatch via `xrouter`/`twist` per the
    // `switch(nddc)`, then harvest mic samples and fire the mic
    // sink.  All flushing is per-USB-frame (mirrors the reference
    // — `networkproto1.c:470-580` loops `for (frame = 0; frame
    // < 2; frame++)` and dispatches inside the per-frame body).

private:
    // Socket fd (not owned).
    int              socket_fd_       = -1;

    // Thread control.
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_request_{false};
    std::unique_ptr<std::thread> thread_;

    // Sequence-tracking.  Reference uses `MetisReadDirect()` to
    // track + post seq counters externally; Lyra adds a simple
    // wrap-aware compare for diagnostic counting.
    uint32_t          last_seq_        = 0;
    bool              seq_seen_        = false;

    // ---- staging buffers (§1.1 RadioNet exclusion) ----
    //
    // Per-USB-frame staging, sized for the largest spr (samples-
    // per-DDC-per-frame) any supported nddc produces.  Reference:
    // `spr = 504 / (6*nddc + 2)`.  At nddc=2 (smallest stride 14),
    // spr = 36.  We round up to kMaxSprPerFrame=64 for headroom
    // (covers a hypothetical nddc=1 spr=63).
    static constexpr int kMaxDdc          = 4;
    static constexpr int kMaxSprPerFrame  = 64;

    // Per-DDC: 2 doubles per sample (I,Q); reference is
    // `prn->RxBuff[iddc][2*isample + {0,1}]`.
    std::array<std::vector<double>, kMaxDdc> rx_buff_{};

    // Reference's `prn->TxReadBufp` — single shared scratch buffer
    // reused by BOTH twist() interleave (4 doubles per sample) AND
    // mic harvest (2 doubles per sample, I=mic, Q=0).  Sized to
    // the larger of the two: 4 * kMaxSprPerFrame doubles.  Reused
    // sequentially within a frame: twist writes (xrouter consumes
    // inline), then mic writes (mic_sink_ consumes inline) —
    // exactly the reference's reuse pattern.
    std::vector<double> tx_read_bufp_{};

    // C&C-in cache (5 bytes); refreshed each USB frame.
    uint8_t control_bytes_in_[5] = {0, 0, 0, 0, 0};

    // ---- sinks ----
    Ep6TelemetrySink                telemetry_sink_;
    Ep6MicSink                      mic_sink_;
    Ep6I2cSink                      i2c_sink_;
    Router*                         router_     = nullptr;
    int                             router_id_  = 0;

    // ---- counters ----
    std::atomic<uint64_t> rx_datagrams_{0};
    std::atomic<uint64_t> seq_errors_{0};
};

}  // namespace lyra::wire

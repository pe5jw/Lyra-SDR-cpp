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

// Sink for the per-sample mic word that the AK4951 (HL2+)
// gateware appends at the end of each 26-byte slot.  HL2-only;
// standard HL2 leaves the slot zero per §5.6.  Sink receives one
// signed 16-bit sample per EP6 sample slot.
using Ep6MicSink = std::function<void(int n_samples,
                                      const int16_t* mic_pcm)>;

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
    // Per-DDC IQ sinks: index by DDC 0..3.  Sinks receive
    // interleaved IQ pairs (2 doubles per sample) for that DDC.
    // Mirrors `xrouter`-dispatched terminal consumers in the
    // reference — Lyra wires the sinks directly without the
    // intermediate channel-master `Inbound()` indirection.
    void set_ddc_sink(int ddc_index, Ep6IqSink sink);

    // Router pointer for inline `twist`+`xrouter` dispatch in the
    // nddc=4 HL2 path (case 4 of the §5.4 switch — DDC2+DDC3
    // pair fed through `twist()` to xrouter source 1).  Optional;
    // if null, the inner DDC pairs are dropped (RX-only / non-PS
    // operation default — `xrouter` is exercised only when v0.3
    // PureSignal consumers wire in).
    void set_router(Router* router, int router_id);

    // Telemetry / mic / I2C sinks.
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

    // Per-sample-slot dispatch — mirrors the inline `switch(nddc)`
    // at `MetisReadThreadMainLoop_HL2:544-558` verbatim.  Reads
    // per-DDC IQ from `slot[0..23]` + mic from `slot[24:25]`,
    // pushes into the per-DDC staging buffers, and on a full
    // staging batch fires the registered sinks.
    void dispatch_sample_slot(const uint8_t* slot,
                              int            slot_index_in_frame);

    // Flush per-DDC staging once the EP6 datagram completes
    // (one EP6 datagram = 2 USB frames = 38 sample slots = a
    // full DSP block on HL2-default 48 kHz).  Fires registered
    // sinks with interleaved IQ pairs.
    void flush_per_datagram_staging();

private:
    // Socket fd (not owned).
    int              socket_fd_       = -1;

    // Thread control.
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_request_{false};
    std::unique_ptr<std::thread> thread_;

    // Sequence-tracking.  Reference uses a wrap-aware compare.
    uint32_t          last_seq_        = 0;
    bool              seq_seen_        = false;

    // ---- staging buffers (§1.1 RadioNet exclusion) ----
    //
    // Sized for the reference's per-datagram budget: at HL2-default
    // nddc=4 / 48 kHz, one datagram delivers 2 * 19 = 38 IQ samples
    // per DDC plus 38 mic samples.  Per-DDC staging holds the
    // un-interleaved doubles ready for the sink callback.
    static constexpr int kMaxDdc      = 4;
    static constexpr int kSlotsPerDgm = 38;  // 2 USB frames x 19

    // Per-DDC: 2 doubles per sample (I,Q).
    std::array<std::vector<double>, kMaxDdc> rx_buff_{};

    // Staging for twist() interleave: 4 doubles per sample
    // ({s0_I, s0_Q, s1_I, s1_Q}); used only on the case-4 path.
    std::vector<double> tx_read_bufp_{};

    // Mic staging — 1 int16 per slot.
    std::vector<int16_t> mic_buff_{};

    // C&C-in cache (5 bytes); refreshed each USB frame.
    uint8_t control_bytes_in_[5] = {0, 0, 0, 0, 0};

    // ---- sinks ----
    std::array<Ep6IqSink, kMaxDdc> ddc_sinks_{};
    Ep6TelemetrySink                telemetry_sink_;
    Ep6MicSink                      mic_sink_;
    Ep6I2cSink                      i2c_sink_;
    Router*                         router_     = nullptr;
    int                             router_id_  = 0;

    // ---- staging fill positions (samples, not doubles) ----
    int rx_fill_[kMaxDdc] = {0, 0, 0, 0};
    int mic_fill_         = 0;

    // ---- counters ----
    std::atomic<uint64_t> rx_datagrams_{0};
    std::atomic<uint64_t> seq_errors_{0};
};

}  // namespace lyra::wire

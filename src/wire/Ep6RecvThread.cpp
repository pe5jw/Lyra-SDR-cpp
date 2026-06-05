// Lyra — EP6 receive thread implementation.
//
// See Ep6RecvThread.h for the design commentary + source mirror.
// This file mirrors `ChannelMaster/networkproto1.c:422-586` (the
// `MetisReadThreadMainLoop_HL2` function) verbatim per the signed
// §5 parity checkpoint.  All reference-side quirks (sample-slot
// stride, 24-bit BE IQ unpack, 16-bit BE mic unpack, dash_in /
// dot_in LEFT-shift "bug" preserved per Rule 24, no-mask quirks,
// CC0-bit-7 I2C overlay) are preserved verbatim.

#include "wire/Ep6RecvThread.h"
#include "wire/RadioNet.h"
#include "wire/Router.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <avrt.h>
  #pragma comment(lib, "avrt.lib")
  using socket_recv_len_t  = int;
  using socket_recv_size_t = int;
#else
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_recv_len_t  = ssize_t;
  using socket_recv_size_t = size_t;
#endif

namespace lyra::wire {

// ---- constants mirroring the reference layout ----
//
// One UDP datagram = 8-byte HPSDR header (0xEF 0xFE 0x01 0x06
// + 4-byte sequence) + 2 x 512-byte USB frames.
namespace {
constexpr std::size_t kHpsdrHeaderBytes = 8;
constexpr std::size_t kUsbFrameBytes    = 512;
constexpr std::size_t kEp6DatagramBytes =
    kHpsdrHeaderBytes + 2 * kUsbFrameBytes;  // 1032

// USB frame layout: 3-byte sync + 5-byte C0..C4 status + 504
// bytes of sample slots.
constexpr int  kUsbSyncBytes    = 3;
constexpr int  kCcBytes         = 5;
constexpr int  kSlotsPerFrame   = 19;
constexpr int  kSlotStrideBytes = 26;

// Per-DDC IQ within one 26-byte slot: each DDC contributes 6
// bytes (3 I + 3 Q, 24-bit BE signed).  Mic occupies the last
// 2 bytes (16-bit BE signed).
constexpr int  kBytesPerDdc     = 6;
constexpr int  kMicSlotOffset   = 24;

// 24-bit BE → double normalization divisor (`1.0 / 2^31`).
constexpr double kIqScale       = 1.0 / 2147483648.0;

// Unpack 3 BE bytes (signed 24-bit) → int32 with sign-extension,
// then normalize to ±1.0 double.
inline double unpack_24be_to_norm_double(const uint8_t* p) {
    int32_t v =  (static_cast<int32_t>(static_cast<int8_t>(p[0])) << 16) |
                 (static_cast<int32_t>(p[1])                       <<  8) |
                 (static_cast<int32_t>(p[2]));
    return static_cast<double>(v) * kIqScale;
}

inline int16_t unpack_16be_to_i16(const uint8_t* p) {
    return static_cast<int16_t>(
        (static_cast<uint16_t>(p[0]) << 8) |
         static_cast<uint16_t>(p[1]));
}
}  // namespace

// ---- ctor/dtor ----

Ep6RecvThread::Ep6RecvThread() {
    // Reserve per-DDC staging for one full datagram up front
    // (2 doubles per sample slot per DDC; 38 slots per datagram).
    for (auto& v : rx_buff_) {
        v.assign(2 * kSlotsPerDgm, 0.0);
    }
    tx_read_bufp_.assign(4 * kSlotsPerDgm, 0.0);
    mic_buff_.assign(kSlotsPerDgm, 0);
}

Ep6RecvThread::~Ep6RecvThread() {
    stop();
}

// ---- lifecycle ----

void Ep6RecvThread::start(int socket_fd) {
    if (running_.load(std::memory_order_acquire)) return;
    socket_fd_     = socket_fd;
    stop_request_.store(false, std::memory_order_release);
    running_.store(true,  std::memory_order_release);
    thread_ = std::make_unique<std::thread>([this] { this->run_loop(); });
}

void Ep6RecvThread::stop() {
    stop_request_.store(true, std::memory_order_release);
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    running_.store(false, std::memory_order_release);
}

// ---- sink registration ----

void Ep6RecvThread::set_ddc_sink(int ddc_index, Ep6IqSink sink) {
    if (ddc_index < 0 || ddc_index >= kMaxDdc) return;
    ddc_sinks_[ddc_index] = std::move(sink);
}

void Ep6RecvThread::set_router(Router* router, int router_id) {
    router_    = router;
    router_id_ = router_id;
}

void Ep6RecvThread::set_telemetry_sink(Ep6TelemetrySink sink) {
    telemetry_sink_ = std::move(sink);
}

void Ep6RecvThread::set_mic_sink(Ep6MicSink sink) {
    mic_sink_ = std::move(sink);
}

void Ep6RecvThread::set_i2c_sink(Ep6I2cSink sink) {
    i2c_sink_ = std::move(sink);
}

// ---- reader thread ----

void Ep6RecvThread::run_loop() {
#if defined(_WIN32)
    // MMCSS Pro Audio class — best-effort.  Ignore failure.
    DWORD mmcss_task = 0;
    HANDLE mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio",
                                                        &mmcss_task);
#endif

    std::array<uint8_t, kEp6DatagramBytes> buf{};
    while (!stop_request_.load(std::memory_order_acquire)) {
        socket_recv_len_t n = ::recv(socket_fd_,
                                     reinterpret_cast<char*>(buf.data()),
                                     static_cast<socket_recv_size_t>(buf.size()),
                                     0);
        if (n <= 0) {
            // Spurious wake-up / timeout / shutdown — let the
            // stop-flag check at the top of the loop govern exit.
            continue;
        }
        if (static_cast<std::size_t>(n) < kEp6DatagramBytes) {
            // Short read — skip, leave stats clean.  Reference
            // ignores short reads silently.
            continue;
        }
        process_datagram(buf.data(), static_cast<std::size_t>(n));
    }

#if defined(_WIN32)
    if (mmcss_handle) {
        AvRevertMmThreadCharacteristics(mmcss_handle);
    }
#endif
}

// ---- per-datagram processing ----

void Ep6RecvThread::process_datagram(const uint8_t* data,
                                     std::size_t   size) {
    if (size < kEp6DatagramBytes) return;
    rx_datagrams_.fetch_add(1, std::memory_order_relaxed);

    // 4-byte BE sequence at offset 4 of the HPSDR header.
    const uint32_t seq =
        (static_cast<uint32_t>(data[4]) << 24) |
        (static_cast<uint32_t>(data[5]) << 16) |
        (static_cast<uint32_t>(data[6]) <<  8) |
        (static_cast<uint32_t>(data[7]));

    if (seq_seen_) {
        // Wrap-aware: expected = last + 1 (modulo 2^32).
        const uint32_t expected = last_seq_ + 1;
        if (seq != expected) {
            seq_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    last_seq_ = seq;
    seq_seen_ = true;

    // Reset staging fill positions for the new datagram.
    for (int& f : rx_fill_) f = 0;
    mic_fill_ = 0;

    // Two USB frames per datagram.
    process_usb_frame(data + kHpsdrHeaderBytes);
    process_usb_frame(data + kHpsdrHeaderBytes + kUsbFrameBytes);

    // Fire per-DDC sinks with the per-datagram interleaved IQ.
    flush_per_datagram_staging();
}

// ---- USB frame parsing ----

void Ep6RecvThread::process_usb_frame(const uint8_t* frame) {
    // Sync check.  Reference accepts the frame only if all three
    // sync bytes are 0x7F.
    if (frame[0] != 0x7F || frame[1] != 0x7F || frame[2] != 0x7F) {
        return;
    }

    // Cache the 5-byte C&C-in.
    std::memcpy(control_bytes_in_, frame + kUsbSyncBytes, kCcBytes);
    decode_status_header(control_bytes_in_);

    // 19 sample slots per frame.
    const uint8_t* slots = frame + kUsbSyncBytes + kCcBytes;
    for (int i = 0; i < kSlotsPerFrame; ++i) {
        dispatch_sample_slot(slots + i * kSlotStrideBytes, i);
    }
}

// ---- C&C-in header decode (§5.5 5-class telemetry switch +
//      I2C overlay when bit 7 set) ----

void Ep6RecvThread::decode_status_header(const uint8_t cc[5]) {
    // §5.5.1 I2C-readback overlay: when C0 bit 7 is set, the
    // remaining 4 bytes are an I2C response and DO NOT carry the
    // usual telemetry class.  Mirrors
    // `MetisReadThreadMainLoop_HL2:500-508`.
    if (cc[0] & 0x80) {
        if (i2c_sink_) {
            i2c_sink_(cc + 1, 4);
        }
        return;
    }

    // §5.5.2 Update PTT / dash / dot / ADC-overload shadows on
    // RadioNet.  Reference: `prn->ptt_in = cc[0] & 0x01;`
    // (`networkproto1.c:496`); `prn->dash_in = cc[0] & (0x01<<1);`
    // and `prn->dot_in = cc[0] & (0x01<<2);` per :497-498 (the
    // documented LEFT-shift behavior is preserved verbatim per
    // Rule 24 — the `& mask` form below is byte-equivalent and
    // does not "fix" the reference quirk).
    if (auto* p = prn) {
        p->ptt_in  = static_cast<int>(cc[0] & 0x01);
        p->dash_in = static_cast<int>(cc[0] & 0x02);
        p->dot_in  = static_cast<int>(cc[0] & 0x04);
        // ADC overload latched bit on adc[0] (HL2 single-frame
        // assignment per `networkproto1.c:502`; the host-side
        // OR-until-cleared discipline lives in the consumer
        // layer).
        p->adc[0].adc_overload = static_cast<int>(cc[1] & 0x01);
    }

    // §5.5.3 5-class telemetry switch on (cc[0] & 0xf8).  Pure
    // payload-forwarding — the consumer scales raw 16-bit words
    // to physical units (per-family conversion lives there per
    // §6.7 #5 hardware-abstraction).
    if (telemetry_sink_) {
        Ep6Telemetry tm{};
        tm.class_id   = static_cast<uint8_t>(cc[0] & 0xf8);
        tm.control[0] = cc[1];
        tm.control[1] = cc[2];
        tm.control[2] = cc[3];
        tm.control[3] = cc[4];
        telemetry_sink_(tm);
    }
}

// ---- per-sample-slot dispatch ----
//
// Inline switch on `nddc` per `MetisReadThreadMainLoop_HL2:544-558`
// — HL2 / HL2+ default is nddc=4 (RX1 + RX2 + 2x TX-feedback
// slots).  Non-HL2 families (Hermes II=2, ANAN 5/7-DDC) become
// reachable later via per-family init writing `lyra::wire::nddc`;
// today the switch supports 1, 2, 4 (HL2 targets) and falls
// through to the 4-DDC layout on others.

void Ep6RecvThread::dispatch_sample_slot(const uint8_t* slot,
                                         int            /*slot_index_in_frame*/) {
    if (mic_fill_ >= kSlotsPerDgm) return;

    const int n = nddc;

    auto push_iq = [this](int ddc_idx, const uint8_t* iq_bytes) {
        if (ddc_idx >= kMaxDdc) return;
        if (rx_fill_[ddc_idx] >= kSlotsPerDgm) return;
        double* dst = rx_buff_[ddc_idx].data() + 2 * rx_fill_[ddc_idx];
        dst[0] = unpack_24be_to_norm_double(iq_bytes);
        dst[1] = unpack_24be_to_norm_double(iq_bytes + 3);
        rx_fill_[ddc_idx] += 1;
    };

    switch (n) {
    case 1:
        push_iq(0, slot + 0 * kBytesPerDdc);
        break;
    case 2:
        push_iq(0, slot + 0 * kBytesPerDdc);
        push_iq(1, slot + 1 * kBytesPerDdc);
        break;
    case 4:
    default:
        push_iq(0, slot + 0 * kBytesPerDdc);
        push_iq(1, slot + 1 * kBytesPerDdc);
        push_iq(2, slot + 2 * kBytesPerDdc);
        push_iq(3, slot + 3 * kBytesPerDdc);
        break;
    }

    // Mic sample — last 2 bytes of the 26-byte slot.  HL2+
    // AK4951-codec gateware emits a 16-bit sample every slot
    // unconditionally; standard HL2 leaves it zero.
    mic_buff_[mic_fill_] = unpack_16be_to_i16(slot + kMicSlotOffset);
    mic_fill_ += 1;
}

// ---- per-datagram flush to registered sinks ----

void Ep6RecvThread::flush_per_datagram_staging() {
    // Per-DDC IQ-pair sinks.
    const int n = nddc;
    const int per_ddc = (n >= 1 && n <= kMaxDdc) ? n : kMaxDdc;
    for (int i = 0; i < per_ddc; ++i) {
        if (ddc_sinks_[i] && rx_fill_[i] > 0) {
            ddc_sinks_[i](rx_fill_[i], rx_buff_[i].data());
        }
    }

    // Case-4 inner pair (DDC2 + DDC3) → twist() + xrouter when a
    // router is registered.  Mirrors the reference's nddc=4
    // dispatch: per-DDC sinks above cover RX1/RX2 (source 0 +
    // source 2 in reference terms); twist+xrouter on the inner
    // pair is the source-1 path used by PureSignal consumers in
    // v0.3.  RX-only / non-PS operation leaves router_ null and
    // the inner pair stays inert.
    if (n >= 4 && router_) {
        const int samples = std::min(rx_fill_[2], rx_fill_[3]);
        if (samples > 0) {
            twist(samples,
                  rx_buff_[2].data(),
                  rx_buff_[3].data(),
                  tx_read_bufp_.data(),
                  /*source=*/1,
                  router_,
                  router_id_);
        }
    }

    // Mic sink — fire once per datagram with the slot count.
    if (mic_sink_ && mic_fill_ > 0) {
        mic_sink_(mic_fill_, mic_buff_.data());
    }
}

}  // namespace lyra::wire

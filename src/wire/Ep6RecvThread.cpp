// Lyra — EP6 receive thread implementation.
//
// See Ep6RecvThread.h for the design commentary + source mirror.
// This file mirrors `ChannelMaster/networkproto1.c:422-586` (the
// `MetisReadThreadMainLoop_HL2` function) verbatim per the signed
// §5 parity checkpoint and the §5-A parity-correction audit
// (2026-06-05).  Reference defects (dash_in/dot_in left-shift
// always-zero; HL2 single-frame adc_overload assignment vs the
// generic OR-until-cleared) are PRESERVED verbatim per Rule 24.

#include "wire/Ep6RecvThread.h"
#include "wire/RadioNet.h"
#include "wire/Router.h"

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
namespace {
// One UDP datagram = 8-byte HPSDR header (4 sync 0xEF/0xFE/0x01/
// 0x06 + 4-byte BE sequence) + 2 x 512-byte USB frames.
constexpr std::size_t kHpsdrHeaderBytes = 8;
constexpr std::size_t kUsbFrameBytes    = 512;
constexpr std::size_t kEp6DatagramBytes =
    kHpsdrHeaderBytes + 2 * kUsbFrameBytes;  // 1032

// USB frame layout: 3-byte sync + 5-byte C0..C4 status + 504
// bytes of sample slots.  The reference's per-sample-slot index
// uses `k = 8 + isample * (6*nddc + 2) + iddc * 6` — that is,
// the sample area begins at byte 8 within the 512-byte USB frame
// (3 sync + 5 CC = 8).
constexpr int kFrameSampleAreaOffset = 8;
constexpr int kSampleAreaBytes       = 504;

// Reference IQ-pack divisor.  Per `networkproto1.c:533-540` each
// 24-bit BE IQ value is packed into the TOP 24 bits of an int32
// (`bptr[k+0] << 24 | bptr[k+1] << 16 | bptr[k+2] << 8`, low 8
// bits zero) and then multiplied by `1.0 / 2147483648.0`.  The
// effective normalization is `int24_value / 2^23` → ±1.0 double.
constexpr double kIqDivisor = 1.0 / 2147483648.0;

// Per-DDC stride within a sample slot: 3 bytes I + 3 bytes Q.
constexpr int kBytesPerDdc = 6;
// Mic trailer within a sample slot: 2 bytes (16-bit BE).
constexpr int kMicTrailerBytes = 2;

// Unpack one 24-bit BE IQ word from `p[0..2]` into a normalized
// double, exactly mirroring the reference's bit pattern (top-24
// pack + 1/2^31 divisor → effective /2^23).  Done via uint32
// build + signed conversion to keep the path defined under C++23
// (the reference's direct `unsigned char << 24` into a signed
// int is technically UB but works on every two's-complement
// platform; the uint32 build below is byte-identical and well-
// defined).
inline double unpack_iq_be(const uint8_t* p) {
    const uint32_t raw = (static_cast<uint32_t>(p[0]) << 24) |
                         (static_cast<uint32_t>(p[1]) << 16) |
                         (static_cast<uint32_t>(p[2]) <<  8);
    return static_cast<double>(static_cast<int32_t>(raw)) * kIqDivisor;
}

// Unpack one 16-bit BE mic word from `p[0..1]` into a normalized
// double via the SAME divisor — reference packs `bptr[k+0] << 24
// | bptr[k+1] << 16` (top 16 bits of int32) and scales by
// `1/2^31` → effective /2^15.  Result is in ±1.0.
inline double unpack_mic_be(const uint8_t* p) {
    const uint32_t raw = (static_cast<uint32_t>(p[0]) << 24) |
                         (static_cast<uint32_t>(p[1]) << 16);
    return static_cast<double>(static_cast<int32_t>(raw)) * kIqDivisor;
}
}  // namespace

// ---- ctor/dtor ----

Ep6RecvThread::Ep6RecvThread() {
    // Per-DDC IQ staging: 2 doubles per sample, sized for the
    // largest spr any supported nddc produces.
    for (auto& v : rx_buff_) {
        v.assign(2 * kMaxSprPerFrame, 0.0);
    }
    // Shared scratch (reused by twist + mic harvest).  twist
    // writes 4 doubles per sample (4 streams x I/Q); mic writes
    // 2 doubles per sample (I=mic, Q=0).  Size to the larger.
    tx_read_bufp_.assign(4 * kMaxSprPerFrame, 0.0);
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
    // Reference re-zeroes `mic_decimation_count` at thread entry
    // (`networkproto1.c:424`).  Mirror that here so a session
    // restart starts with a clean decimator phase regardless of
    // where the previous session ended.
    mic_decimation_count = 0;
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
        if (n <= 0) continue;
        if (static_cast<std::size_t>(n) < kEp6DatagramBytes) continue;
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
        const uint32_t expected = last_seq_ + 1;
        if (seq != expected) {
            seq_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    last_seq_ = seq;
    seq_seen_ = true;

    // Two USB frames per datagram.  Each frame is dispatched
    // independently — reference loops `for (frame=0; frame<2;
    // frame++)` and runs the full unpack→switch→mic harvest
    // sequence per frame.
    process_usb_frame(data + kHpsdrHeaderBytes);
    process_usb_frame(data + kHpsdrHeaderBytes + kUsbFrameBytes);
}

// ---- USB frame parsing (mirrors `networkproto1.c:470-580`) ----

void Ep6RecvThread::process_usb_frame(const uint8_t* frame) {
    // Sync check: all three sync bytes must be 0x7F.  Reference
    // `if ((bptr[0]==0x7f) && (bptr[1]==0x7f) && (bptr[2]==0x7f))`.
    if (frame[0] != 0x7F || frame[1] != 0x7F || frame[2] != 0x7F) {
        return;
    }

    // Cache the 5-byte C&C-in (status header) and decode.
    std::memcpy(control_bytes_in_, frame + 3, 5);
    decode_status_header(control_bytes_in_);

    // Dynamic per-frame layout (matches the reference exactly):
    //   stride = 6 * nddc + 2   bytes per sample slot
    //   spr    = 504 / stride   samples per DDC per frame
    const int n = nddc;
    if (n < 1) return;
    const int stride = kBytesPerDdc * n + kMicTrailerBytes;
    int spr = (stride > 0) ? (kSampleAreaBytes / stride) : 0;
    if (spr <= 0) return;
    if (spr > kMaxSprPerFrame) spr = kMaxSprPerFrame;
    const int clamped_ddc = (n <= kMaxDdc) ? n : kMaxDdc;

    // ---- Per-DDC IQ unpack (DDC-major, matches reference structure)
    //
    // Reference `networkproto1.c:528-542`:
    //   for (iddc = 0; iddc < nddc; iddc++)
    //     for (isample = 0; isample < spr; isample++)
    //       k = 8 + isample*(6*nddc+2) + iddc*6
    //       prn->RxBuff[iddc][2*isample+0] = const * (bptr[k+0]<<24 | bptr[k+1]<<16 | bptr[k+2]<<8)
    //       prn->RxBuff[iddc][2*isample+1] = const * (bptr[k+3]<<24 | bptr[k+4]<<16 | bptr[k+5]<<8)
    for (int iddc = 0; iddc < clamped_ddc; ++iddc) {
        double* const dst = rx_buff_[iddc].data();
        for (int isample = 0; isample < spr; ++isample) {
            const int k = kFrameSampleAreaOffset
                        + isample * stride
                        + iddc * kBytesPerDdc;
            dst[2 * isample + 0] = unpack_iq_be(frame + k);
            dst[2 * isample + 1] = unpack_iq_be(frame + k + 3);
        }
    }

    // ---- Per-nddc switch (`networkproto1.c:544-558`) ----
    //
    // Direct xrouter calls for unpaired DDCs; twist() (which
    // calls xrouter internally with 2*spr) for paired DDCs.
    switch (n) {
    case 2:
        if (router_) {
            twist(spr,
                  rx_buff_[0].data(), rx_buff_[1].data(),
                  tx_read_bufp_.data(),
                  /*source=*/0,
                  router_, router_id_);
        }
        break;
    case 4:
        if (router_) {
            // source 0 = DDC0; source 1 = twist(DDC2, DDC3);
            // source 2 = DDC1.  Verbatim from
            // `networkproto1.c:549-552`.
            xrouter(router_, router_id_, 0, spr, rx_buff_[0].data());
            twist(spr,
                  rx_buff_[2].data(), rx_buff_[3].data(),
                  tx_read_bufp_.data(),
                  /*source=*/1,
                  router_, router_id_);
            xrouter(router_, router_id_, 2, spr, rx_buff_[1].data());
        }
        break;
    case 5:
        // ANAN P1 — kept structurally for parity; bench-untested
        // in this Lyra build (no ANAN P1 hardware available).
        // Reference `networkproto1.c:554-557`.
        if (router_ && clamped_ddc >= 5) {
            twist(spr,
                  rx_buff_[0].data(), rx_buff_[1].data(),
                  tx_read_bufp_.data(),
                  /*source=*/0,
                  router_, router_id_);
            twist(spr,
                  rx_buff_[3].data(), rx_buff_[4].data(),
                  tx_read_bufp_.data(),
                  /*source=*/1,
                  router_, router_id_);
            xrouter(router_, router_id_, 2, spr, rx_buff_[2].data());
        }
        break;
    default:
        // Other nddc values are not dispatched (reference's
        // switch has no default body).  IQ has been unpacked
        // into rx_buff_ regardless.
        break;
    }

    // ---- Mic harvest with decimation
    //      (`networkproto1.c:560-579`) ----
    //
    // `mic_decimation_count` is post-incremented and compared
    // against `mic_decimation_factor`; on equality the counter
    // resets and the mic sample is harvested.  Default factor=0
    // (reference BSS-init) yields no harvest; HL2 default
    // operating point sets factor=1 at session open for
    // every-slot harvest.
    //
    // Mic-sample byte offset within the slot is at `k + nddc*6`
    // (mic trailer immediately follows the last DDC's IQ).  IQ
    // pair output is interleaved {I=mic, Q=0.0}, matching the
    // reference's TxReadBufp layout.
    int mic_sample_count = 0;
    for (int isamp = 0; isamp < spr; ++isamp) {
        const int k = kFrameSampleAreaOffset
                    + n * kBytesPerDdc
                    + isamp * stride;
        ++mic_decimation_count;
        if (mic_decimation_count == mic_decimation_factor) {
            mic_decimation_count = 0;
            tx_read_bufp_[2 * mic_sample_count + 0] =
                unpack_mic_be(frame + k);
            tx_read_bufp_[2 * mic_sample_count + 1] = 0.0;
            ++mic_sample_count;
        }
    }
    if (mic_sink_ && mic_sample_count > 0) {
        // Reference: `Inbound(inid(1, 0), mic_sample_count,
        // prn->TxReadBufp);`
        mic_sink_(mic_sample_count, tx_read_bufp_.data());
    }
}

// ---- C&C-in header decode (`networkproto1.c:478-525`) ----
//
// I2C-overlay gate (C0 bit 7 set) takes precedence and replaces
// the telemetry-class switch entirely; otherwise the PTT/dash/
// dot shadow bits land on RadioNet and the 5-case telemetry
// switch populates per-class RADIONET fields verbatim.

void Ep6RecvThread::decode_status_header(const uint8_t cc[5]) {
    // I2C readback overlay — `networkproto1.c:478-493`.
    if (cc[0] & 0x80) {
        if (i2c_sink_) {
            i2c_sink_(cc + 1, 4);
        }
        return;
    }

    if (auto* p = prn) {
        // ptt_in / dash_in / dot_in shadows — reference at
        // `networkproto1.c:496-498`.
        //
        // Note Rule 24 preservation of reference defects:
        // `dash_in = (cc[0] << 1) & 0x01` and
        // `dot_in  = (cc[0] << 2) & 0x01` always evaluate to 0
        // (left-shifting moves bits AWAY from the LSB; the LSB
        // mask then captures the new LSB which is 0).  This is
        // a known reference defect — preserved verbatim so a
        // downstream consumer that depends on "dash/dot always
        // 0" (e.g. as a placeholder for the per-HL2 hardware
        // CW input path that is wired elsewhere) sees the same
        // wire behaviour as the reference.
        p->ptt_in  = static_cast<int>(cc[0] & 0x01);
        p->dash_in = static_cast<int>((cc[0] << 1) & 0x01);
        p->dot_in  = static_cast<int>((cc[0] << 2) & 0x01);

        // ---- 5-class telemetry switch on (cc[0] & 0xf8) ----
        //      Verbatim from `networkproto1.c:499-524`.
        switch (cc[0] & 0xf8) {
        case 0x00:  // C0 0000 0xxx
            // adc_overload + user_dig_in.
            p->adc[0].adc_overload = static_cast<int>(cc[1] & 0x01);
            p->user_dig_in = static_cast<int>((cc[1] >> 1) & 0x0f);
            break;
        case 0x08:  // C0 0000 1xxx
            // AIN5 exciter (drive) power + AIN1 PA coupler fwd.
            p->tx[0].exciter_power =
                static_cast<int>(((static_cast<int>(cc[1]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[2])       & 0x00ff));
            p->tx[0].fwd_power =
                static_cast<int>(((static_cast<int>(cc[3]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[4])       & 0x00ff));
            // FIXME (Task #114 TX-policy plumbing): reference
            // also calls `PeakFwdPower((float)prn->tx[0].fwd_power)`
            // here to maintain a running peak-meter state for
            // consumer-facing readouts.  Lyra-native peak
            // helper lands with the TX-policy plumbing commit;
            // raw fwd_power above is already populated for
            // direct consumers.
            break;
        case 0x10:  // C0 0001 0xxx
            // AIN2 PA reverse power + AIN3 MKII PA volts.
            p->tx[0].rev_power =
                static_cast<int>(((static_cast<int>(cc[1]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[2])       & 0x00ff));
            // FIXME (Task #114): reference calls
            // `PeakRevPower((float)prn->tx[0].rev_power)` here.
            p->user_adc0 =
                static_cast<int>(((static_cast<int>(cc[3]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[4])       & 0x00ff));
            break;
        case 0x18:  // C0 0001 1xxx
            // AIN4 MKII PA amps + AIN6 Hermes (supply) volts.
            p->user_adc1 =
                static_cast<int>(((static_cast<int>(cc[1]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[2])       & 0x00ff));
            p->supply_volts =
                static_cast<int>(((static_cast<int>(cc[3]) << 8) & 0xff00) |
                                 ( static_cast<int>(cc[4])       & 0x00ff));
            break;
        case 0x20:  // C0 0010 0xxx
            // Per-ADC overload bits.  Note the reference's
            // shift-then-write idiom for adc[1]/adc[2] is
            // preserved verbatim (the resulting values are 0 or
            // 2 for adc[1], 0 or 4 for adc[2] — non-1 truthy
            // values that consumers test as bool).
            p->adc[0].adc_overload = static_cast<int>(cc[1] & 0x01);
            p->adc[1].adc_overload = static_cast<int>((cc[2] & 0x01) << 1);
            p->adc[2].adc_overload = static_cast<int>((cc[3] & 0x01) << 2);
            break;
        default:
            // Reference switch has no default body — other
            // class IDs are reserved/unused.
            break;
        }
    }

    // Lyra-native telemetry-sink forwarding (additive — not
    // present in the reference).  Operator-side consumers that
    // want raw payload bytes (independent of the RADIONET
    // shadow writes above) can register a sink to receive them.
    // The shadow writes above are the reference-faithful path;
    // this sink is a convenience layer signed off as an
    // acceptable Lyra-native addition (no impact on wire or
    // RADIONET state).
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

}  // namespace lyra::wire

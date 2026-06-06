// Lyra — EP2 send thread implementation.  See Ep2SendThread.h.
//
// Mirrors `ChannelMaster/networkproto1.c::sendProtocol1Samples`
// (`:1204-1267`) verbatim per the signed §6 parity checkpoint.
// All reference quirks (floor/ceil sign-split quantization, HL2
// 4-bit / non-HL2 3-bit CW state-bit overlay on TX I-LSBs,
// MOX-off IQ zeroing as host-side defense-in-depth) are preserved
// per Rule 24.
//
// §6-B (sign-off 2026-06-06): the `metis_write_frame` wire-emit
// primitive + the outbound seq counter + the bound socket/dest
// all moved to `wire/MetisFrame.{h,cpp}` TU-scope — direct mirror
// of the reference's file-scope `MetisWriteFrame` + the file-
// scope `MetisOutBoundSeqNum` global + the file-scope `listenSock`
// global.  The §6 Q5 `send_lock_` is removed (no reference
// counterpart; temporal separation between priming and main-send
// preserves the no-race property).

#include "wire/Ep2SendThread.h"
#include "wire/FrameComposer.h"   // §1-C Stage 4F.2: now provides free `write_main_loop_hl2`
#include "wire/MetisFrame.h"
#include "wire/OutboundRing.h"
#include "wire/RadioNet.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>   // required for avrt.h prerequisites (HANDLE/DWORD/BOOL)
  #include <avrt.h>
  #pragma comment(lib, "avrt.lib")
#endif

namespace lyra::wire {

namespace {
// EP2 endpoint identifier.  Reference: `MetisWriteFrame(0x02,
// FPGAWriteBufp);` at `:1198`.
constexpr int kEp2Endpoint = 0x02;

// Sample-slot counts mirroring the per-USB-frame budget.
constexpr int kSampleSlotsPerFrame = 63;       // = 504/8 bytes
constexpr int kSampleSlotsPerDatagram = 2 * kSampleSlotsPerFrame;  // 126
constexpr int kLriqBytesPerFrame = 8 * kSampleSlotsPerFrame;       // 504

// §1-C Stage 4C (sign-off 2026-06-06): TU-scope file-scope
// mirror of reference's `char* FPGAWriteBufp;` at
// `network.h:499` (NOT in `_radionet`).  HL2 P1 EP2 raw send
// buffer (1024 bytes, 8-byte header NOT included — that
// header is added by `metis_write_frame()` before sendto).
// Sister of:
//   - §6-B `g_metis_out_seq_num` in `wire/MetisFrame.cpp`
//   - §1-C Stage 4B.1 `g_fpga_read_bufp` in `wire/Ep6RecvThread.cpp`
// — all three are reference file-scope globals correctly
// mirrored as TU-scope statics in their respective wire-layer
// translation units.
std::vector<std::uint8_t> g_fpga_write_bufp;
}  // namespace

// ---- ctor/dtor ----

Ep2SendThread::Ep2SendThread() = default;

Ep2SendThread::~Ep2SendThread() {
    stop();
}

// ---- lifecycle ----

void Ep2SendThread::start(int                socket_fd,
                          const void*        dest_addr,
                          std::size_t        dest_addrlen) {
    if (running_.load(std::memory_order_acquire)) return;

    // §1-C Stage 4C: `prn` must be valid by session-open contract
    // (reference's `prn` is always valid when sendProtocol1Samples
    // runs — Lyra mirrors).
    if (prn == nullptr) return;

    // §6-B (sign-off 2026-06-06): socket/dest are TU-scope in
    // `wire/MetisFrame.cpp` (direct mirror of the reference's
    // file-scope `listenSock` global + `prn->base_outbound_port`).
    // We forward the caller's values through `metis_wire_bind()` —
    // idempotent setter; safe if the session-open path (step 14
    // wire-up) has already bound the same values.
    metis_wire_bind(socket_fd, dest_addr, dest_addrlen);

    // §1-C Stage 4C: size both outbound buffers at thread start
    // — mirrors reference's `prn->OutBufp = (char*)malloc(...)`
    // + `FPGAWriteBufp = (char*)calloc(1024, sizeof(char));` at
    // `networkproto1.c:428`.  `prn->OutBufp` lives in
    // `_radionet`; `g_fpga_write_bufp` is TU-scope per the
    // reference's file-scope global pattern.
    prn->OutBufp.assign(kLriqBytesPerDatagram, 0);
    g_fpga_write_bufp.assign(kFpgaPayloadBytes, 0);

    stop_request_.store(false, std::memory_order_release);
    running_.store(true,  std::memory_order_release);
    thread_ = std::make_unique<std::thread>([this] { this->run_loop(); });
}

void Ep2SendThread::stop() {
    stop_request_.store(true, std::memory_order_release);
    // Wake the consumer parked in `wait_pair_ready()` so it can
    // observe stop_request_ + exit cleanly.
    outbound_unblock();
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    running_.store(false, std::memory_order_release);
}

// ---- reader thread ----

void Ep2SendThread::run_loop() {
#if defined(_WIN32)
    // §6.1 — MMCSS Pro-Audio priority 2 per
    // `networkproto1.c:1207-1208`.  Best-effort; ignore failure.
    DWORD mmcss_task = 0;
    HANDLE mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio",
                                                        &mmcss_task);
    if (mmcss_handle) {
        // §6.1 — reference passes literal `2` which corresponds to
        // `AVRT_PRIORITY_CRITICAL` in the Windows SDK avrt.h enum
        // (`networkproto1.c:1208`).  Preserved verbatim per Rule
        // 24 — the prior `AVRT_PRIORITY_HIGH` (=1) was one tier
        // lower than the reference.
        AvSetMmThreadPriority(mmcss_handle, AVRT_PRIORITY_CRITICAL);
    }
#endif

    while (!stop_request_.load(std::memory_order_acquire)) {
        if (!process_one_pair()) break;
    }

#if defined(_WIN32)
    if (mmcss_handle) {
        AvRevertMmThreadCharacteristics(mmcss_handle);
    }
#endif
}

// ---- per-iteration body (mirrors :1218-1265) ----

bool Ep2SendThread::process_one_pair() {
    // §1-C Stage 4C/4F.2: prn must be valid (assigned at
    // start()); no more `composer_` member to check (dissolved
    // into namespace-scope free functions).
    if (prn == nullptr) return false;

    // §6.2 — wait for BOTH lr_ready + iq_ready (`:1220`).
    // §1-C Stage 4D: dissolved OutboundRing → free function.
    if (!outbound_wait_pair_ready()) {
        // outbound_unblock() was called — clean shutdown.
        return false;
    }

    double* lr = outbound_lr_buf_mut();
    double* iq = outbound_iq_buf_mut();

    // §6.4 — EER mode LR-overwrite-by-IQ (`:1222-1226`).
    //
    // FIXME (Task #114): the reference's gate
    // `pcm->xmtr[0].peer->run && XmitBit` is the channel-master
    // EER/ETR-enabled flag AND the wire-MOX bit.  Lyra has no
    // channel-master equivalent of `pcm->xmtr[0].peer->run`; EER
    // mode is a v0.3+ TX feature requiring upstream WDSP TX
    // channel + envelope-tracking implementation.  Branch shape
    // preserved with a stubbed `if (false &&` guard — when EER
    // lands, the guard flips to the real condition.
    if (false && XmitBit) {
        // Reference: `memcpy(prn->outLRbufp, prn->outIQbufp + 256,
        // sizeof(complex) * 126);` — overwrite LR audio with IQ
        // data offset by 256 doubles into iq buffer.  Shape kept
        // for the eventual EER-mode landing under Task #114.
        std::memcpy(lr, iq + 256, sizeof(double) * 2 * 126);
    }

    // §6.3 — MOX-edge IQ zeroing (`:1227`).  ⚠ Host-side defense-
    // in-depth on RF emission: when not transmitting (XmitBit
    // clear), zero the IQ buffer in the wire path BEFORE
    // quantization → no non-zero TX samples reach the DAC even if
    // a producer thread races ahead of a PTT-release.  Preserved
    // verbatim per Rule 24.
    if (!XmitBit) {
        std::fill_n(iq, kOutboundDoublesPerBuffer, 0.0);
    }

    // §6.5 — optional L/R channel swap (`:1231-1239`).  When
    // `prn->swap_audio_channels` is set, swap L ↔ R in-place to
    // compensate for hardware-firmware variants.  Verbatim loop
    // bounds (4 × 63 = 252 doubles = 126 LR pairs).
    if (auto* p = prn; p && p->swap_audio_channels) {
        for (int i = 0; i < 4 * kSampleSlotsPerFrame; i += 2) {
            const double swap = lr[i + 0];
            lr[i + 0] = lr[i + 1];
            lr[i + 1] = swap;
        }
    }

    // §6.6 + §6.7 — float → int16 quantization + CW state-bit
    // overlay.  Fills `prn->OutBufp` with the 8-bytes-per-sample-slot
    // packed format (L_hi L_lo R_hi R_lo I_hi I_lo Q_hi Q_lo).
    quantize_and_pack(lr, iq);

    // §6.8 — compose C&C bytes into the FPGA write buffer.
    // Per-family dispatch matches the reference's
    // `if (HPSDRModel == HPSDRModel_HERMESLITE) WriteMainLoop_HL2
    // else WriteMainLoop;` branch at `networkproto1.c:1261-1264`.
    //
    // FrameComposer fills offsets [0..7] + [512..519] (sync + C&C)
    // of the 1024-byte buffer.  Body LRIQ at [8..511] + [520..1023]
    // is filled by the memcpy below.
    if (hpsdrModel == HPSDRModel::HERMESLITE) {
        // §1-C Stage 4F.2: free function (FrameComposer
        // dissolved).
        write_main_loop_hl2(
            reinterpret_cast<char*>(g_fpga_write_bufp.data()));
    } else {
        // FIXME (Task #114 / non-HL2 hardware availability): the
        // reference's generic `WriteMainLoop` (`networkproto1.c:
        // 588-866`) handles the non-HL2 (ANAN-class) per-family
        // C&C composition.  Lyra has no ANAN P1 hardware
        // available to bench-verify a port, so the generic
        // emit-path is deferred — when ANAN P1 hardware arrives,
        // `FrameComposer::write_main_loop_generic()` lands as a
        // sibling to `write_main_loop_hl2()` and is invoked from
        // this `else` branch.  For now this branch is a
        // skip-then-send-zero-CC path: the g_fpga_write_bufp is
        // zeroed in-place so the consumer (sendto below) emits a
        // well-formed but inert datagram if the operator ever
        // configures a non-HL2 hpsdrModel in this Lyra build.
        // HL2-only operating point today; non-HL2 never reached.
        std::fill(g_fpga_write_bufp.begin(),
                  g_fpga_write_bufp.end(),
                  static_cast<uint8_t>(0));
    }

    // LRIQ memcpy (`:1193-1195`) — both USB frames.
    std::memcpy(g_fpga_write_bufp.data() + 8,
                prn->OutBufp.data(),
                static_cast<std::size_t>(kLriqBytesPerFrame));
    std::memcpy(g_fpga_write_bufp.data() + 520,
                prn->OutBufp.data() + kLriqBytesPerFrame,
                static_cast<std::size_t>(kLriqBytesPerFrame));

    // §6.9 — submit via shared TU-scope `metis_write_frame`
    // (`networkproto1.c:1198` → `MetisWriteFrame(2, FPGAWriteBufp)`).
    // §6-B (sign-off 2026-06-06): NO LOCK around this call.
    // Reference does not lock around `MetisWriteFrame`; the §6
    // `send_lock_` mutex (Q5 Lyra-native addition) is removed.
    // Concurrency safety is preserved by TEMPORAL SEPARATION:
    // §7 `ForceCandC::prime` runs synchronously on the
    // session-open thread BEFORE `Ep2SendThread::start()` spins
    // this thread — the two callers are disjoint in time, no
    // race possible.
    // §1-C (2026-06-06): diagnostic `send_errors_` /
    // `datagrams_sent_` increments removed.  Reference does not
    // track per-send counters; result is ignored at this site
    // (mirroring `sendProtocol1Samples` at `:1198` which calls
    // `MetisWriteFrame(0x02, FPGAWriteBufp);` and discards the
    // return value).
    (void) metis_write_frame(kEp2Endpoint, g_fpga_write_bufp.data());

    // §6.8 — producer-side release: signal both LR + IQ producers
    // "buffer consumed, free to refill" (`:1199-1200`).
    outbound_notify_consumed_pair();
    return true;
}

// ---- §6.6 + §6.7 quantization + CW state-bit overlay
//      (`:1241-1259`) ----

void Ep2SendThread::quantize_and_pack(const double* lr_buf,
                                      const double* iq_buf) {
    const double* const pbuffs[2] = { lr_buf, iq_buf };

    // Reference triple-nested loop at `:1241-1259`.
    //   i = 0..125  (sample-slot index across both USB frames)
    //   j = 0..1    (j=0 = LR pair, j=1 = IQ pair)
    //   k = 0..1    (k=0 = first component, k=1 = second)
    //
    // Per-sample reads of `prn->cw.cw_enable` + `prn->tx[0].*` +
    // `HPSDRModel` are preserved verbatim per Rule 24 (the
    // reference reads them inside the innermost loop on every
    // sample — 504 reads per datagram).  The CW keyer can flip
    // bits mid-pair on a real-time key edge; matching the
    // reference's read cadence means Lyra observes the same
    // edge timing as the reference.
    for (int i = 0; i < kSampleSlotsPerDatagram; ++i) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k) {
                const double v = pbuffs[j][i * 2 + k];

                // Round-to-nearest with floor/ceil sign-split
                // (`:1245-1246`).  Verbatim from reference.
                int16_t temp = (v >= 0.0)
                    ? static_cast<int16_t>(std::floor(v * 32767.0 + 0.5))
                    : static_cast<int16_t>(std::ceil( v * 32767.0 - 0.5));

                // §6.7 — CW state-bit overlay on TX I-sample
                // LSBs (`:1247-1256`).  Gated on
                // `prn->cw.cw_enable && j == 1` (j=1 = IQ pair
                // only).  When active, the entire 16-bit sample
                // is REPLACED with the CW state word — TX
                // I-sample LSBs are overwritten on the wire.
                // Preserved verbatim per Rule 24 — the HL2
                // gateware reads these bits as CW state during
                // CW transmit.
                //
                // Bit-source shifts mirror the reference
                // verbatim: NO pre-mask with `& 0x01` is applied
                // to the individual `cwx_ptt`/`dot`/`dash`/`cwx`
                // fields before shifting.  The final mask
                // (`& 0x0F` HL2 / `& 0x07` non-HL2) cleans up
                // the result.  If a future caller stuffs
                // multi-bit values into those fields the
                // intermediate bits overlap exactly as they
                // would in the reference (preserved defect).
                if (auto* p = prn; p && p->cw.cw_enable && j == 1) {
                    if (hpsdrModel == HPSDRModel::HERMESLITE) {
                        temp = static_cast<int16_t>(
                            (p->tx[0].cwx_ptt << 3 |
                             p->tx[0].dot     << 2 |
                             p->tx[0].dash    << 1 |
                             p->tx[0].cwx)
                            & 0b00001111);
                    } else {
                        temp = static_cast<int16_t>(
                            (p->tx[0].dot  << 2 |
                             p->tx[0].dash << 1 |
                             p->tx[0].cwx)
                            & 0b00000111);
                    }
                }

                // BE pack into prn->OutBufp (`:1257-1258`).
                const std::size_t off =
                    static_cast<std::size_t>(8 * i + 4 * j + 2 * k);
                prn->OutBufp[off + 0] =
                    static_cast<uint8_t>((temp >> 8) & 0xff);
                prn->OutBufp[off + 1] =
                    static_cast<uint8_t>( temp       & 0xff);
            }
        }
    }
}

}  // namespace lyra::wire

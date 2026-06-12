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
#include "wire/FrameComposer.h"   // write_main_loop_hl2 (full monolithic, includes LRIQ memcpy + wire send + paired release)
#include "wire/OutboundRing.h"    // outbound_wait_pair_ready + outbound_lr/iq_buf_mut
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
// Sample-slot counts mirroring the per-USB-frame budget.
// Used by the L/R swap + MOX-edge IQ zero + quantize_and_pack
// loops in this TU; the LRIQ-byte and FPGA-payload sizing
// constants moved to FrameComposer.cpp alongside the
// `g_fpga_write_bufp` static they size (§10.2-revert,
// 2026-06-08, Task #121, operator directive "do as
// reference, period").
constexpr int kSampleSlotsPerFrame = 63;       // = 504/8 bytes
constexpr int kSampleSlotsPerDatagram = 2 * kSampleSlotsPerFrame;  // 126

// §10.2-revert (2026-06-08, Task #121): `g_fpga_write_bufp`
// moved to `wire/FrameComposer.cpp` TU-scope — it's
// `WriteMainLoop_HL2`'s data (reference: file-scope
// `FPGAWriteBufp` global; Lyra: TU-scope static next to the
// function that uses it).  Sister of §1-C Stage 4B.1
// `g_fpga_read_bufp` in `wire/Ep6RecvThread.cpp` + §6-B
// `g_metis_out_seq_num` in `wire/MetisFrame.cpp`.
}  // namespace

// ---- ctor/dtor ----

Ep2SendThread::Ep2SendThread() = default;

Ep2SendThread::~Ep2SendThread() {
    stop();
}

// ---- lifecycle ----

void Ep2SendThread::start(int socket_fd) {
    if (running_.load(std::memory_order_acquire)) return;

    // §1-C Stage 4C: `prn` must be valid by session-open contract
    // (reference's `prn` is always valid when sendProtocol1Samples
    // runs — Lyra mirrors).
    if (prn == nullptr) return;

    // Socket binding happens ONCE at session-open in
    // `hl2_stream.cpp`'s wire-layer initializer via
    // `metis_wire_bind()` — reference binds `listenSock` once at
    // StartMetis discovery (`network.c` file-scope), NOT in each
    // thread's spawn.
    (void) socket_fd;

    // `prn->OutBufp` allocation moved to create_rnet() per the
    // 2026-06-06 operator audit (reference allocates ALL
    // _radionet buffers in one create_rnet() at startup —
    // netInterface.c:1606 sizes OutBufp to 1440 bytes; prior
    // Lyra split was a §6-Q-class deviation, and the Lyra-side
    // size was undersized at 1008 vs reference 1440).
    // `g_fpga_write_bufp` is sized inside run_loop() at thread
    // entry, mirroring the reference's `FPGAWriteBufp = calloc(
    // 1024,…)` at thread entry in `networkproto1.c:428`.

    stop_request_.store(false, std::memory_order_release);
    running_.store(true,  std::memory_order_release);

    // Reference EP2 spawn is fire-and-forget (`netInterface.c:65-66`
    // creates `hWriteThreadInitSem` via `CreateSemaphore` but
    // there is ZERO `WaitForSingleObject(hWriteThreadInitSem,…)`
    // and ZERO `ReleaseSemaphore(hWriteThreadInitSem,…)` anywhere
    // in the entire ChannelMaster tree).  An earlier Lyra-native
    // drain+acquire+release handshake was a fabrication caught by
    // the 2026-06-06 TX-Agent-3 audit and removed per "do as
    // reference, period."

    thread_ = std::make_unique<std::thread>([this] { this->run_loop(); });
}

void Ep2SendThread::stop() {
    stop_request_.store(true, std::memory_order_release);
    // No explicit wake — `outbound_wait_pair_ready` uses bounded
    // `wait_for` (100 ms poll); the `run_loop`'s
    // `while (!stop_request_)` test re-runs at most one poll
    // interval after this store.  Reference shuts down via
    // `io_keep_running = 0` + process termination, mirrored
    // here by the atomic stop flag + bounded-wait poll
    // (acceptable C++23 idiom translation).
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

    // ---- Per-thread init ----
    //
    // §10.2-revert (2026-06-08, Task #121): the
    // `g_fpga_write_bufp.assign(1024, 0)` allocation moved to
    // FrameComposer.cpp's lazy-init inside `write_main_loop_hl2`
    // (mirrors reference: the FPGAWriteBufp buffer lives next to
    // the function that uses it, not in the EP2 thread that
    // calls it).  Reference allocates `FPGAWriteBufp = calloc(
    // 1024,…)` inside MetisReadThreadMainLoop_HL2 at
    // `networkproto1.c:428` — the EP6 RX thread, not the EP2 TX
    // thread.  Lyra's lazy-init on first call achieves the same
    // effect with cleaner ownership.
    //
    // No init-sem release — reference EP2 thread is fire-and-
    // forget (`netInterface.c:65-66` + `networkproto1.c:1204-1267`:
    // zero ReleaseSemaphore on hWriteThreadInitSem anywhere).

    while (!stop_request_.load(std::memory_order_acquire)) {
        if (!process_one_pair()) break;
    }

    // No `AvRevertMmThreadCharacteristics(mmcss_handle)` —
    // reference NEVER reverts (zero `AvRevert*` matches in the
    // entire ChannelMaster tree).  OS reclaims on thread exit.
    // An earlier Lyra-native cleanup was a deviation caught by
    // 2026-06-06 TX-Agent C.5 audit and removed per "do as
    // reference, period."
}

// ---- per-iteration body (mirrors :1218-1265) ----

bool Ep2SendThread::process_one_pair() {
    // §6.2 — wait for BOTH lr_ready + iq_ready (`:1220`).
    // §1-C Stage 4D: dissolved OutboundRing → free function.
    // Returns false on poll-timeout (no producer activity within
    // one poll interval); run_loop re-tests stop_request_ and
    // either calls back in or exits, mirroring reference's
    // `while (io_keep_running) { WaitForMultipleObjects(..., INFINITE) }`
    // shutdown via process-termination interrupt.
    if (!outbound_wait_pair_ready()) {
        return true;  // poll-timeout; let run_loop re-test stop
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

    // ---- Compose + send the EP2 datagram ----
    //
    // Per-family dispatch matches reference's
    // `if (HPSDRModel == HPSDRModel_HERMESLITE) WriteMainLoop_HL2
    // else WriteMainLoop;` at `networkproto1.c:1261-1264`.
    //
    // §10.2-revert (2026-06-08, Task #121, operator directive
    // "do as reference, period, NO PATCHING"):
    // `write_main_loop_hl2(prn->OutBufp)` is now the
    // FULL MONOLITHIC equivalent of reference
    // `WriteMainLoop_HL2(prn->OutBufp)` at `:1262` — it composes
    // sync + C&C bytes into its own TU-scope FPGA write buffer,
    // memcpys the LRIQ payload from the supplied source pointer
    // into the same buffer at offsets [8..511] + [520..1023],
    // calls `metis_write_frame(0x02, ...)`, then signals the
    // paired release.  All of that lives inside that one call;
    // this site is a thin caller matching reference's thin call
    // site exactly.
    if (hpsdrModel == HPSDRModel::HERMESLITE) {
        write_main_loop_hl2(prn->OutBufp);
    } else {
        // FIXME (Task #119 / non-HL2 hardware availability): the
        // reference's generic `WriteMainLoop(prn->OutBufp)` at
        // `:1264` handles the non-HL2 (ANAN-class) per-family
        // C&C composition AND its own LRIQ memcpy + wire send +
        // paired release.  Lyra has no ANAN P1 hardware
        // available to bench-verify a port, so the generic
        // emit-path is deferred — when ANAN P1 hardware arrives,
        // `write_main_loop_generic()` lands as a sibling to
        // `write_main_loop_hl2()` and is invoked from this
        // `else` branch.  For now this branch is a no-send skip:
        // no datagram leaves the host on non-HL2 hardware until
        // a verified emit path lands.  HL2-only operating point
        // today; non-HL2 never reached.
    }

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
                    static_cast<char>((temp >> 8) & 0xff);
                prn->OutBufp[off + 1] =
                    static_cast<char>( temp       & 0xff);
            }
        }
    }
}

}  // namespace lyra::wire

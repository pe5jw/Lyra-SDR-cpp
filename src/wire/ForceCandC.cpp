// Lyra — startup C&C priming implementation.  See ForceCandC.h.
//
// Reference-verbatim port of `ForceCandCFrames` + `ForceCandCFrame`
// at `ChannelMaster/networkproto1.c:106-139`.  Every byte position
// + every loop count + every sleep duration mirrors the reference
// per Rule 24.

#include "wire/ForceCandC.h"
#include "wire/MetisFrame.h"
#include "wire/RadioNet.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace lyra::wire {

namespace {
// EP2 endpoint identifier.  Reference: `MetisWriteFrame(2, buf)`
// at `networkproto1.c:130` — the `2` is the same EP2 endpoint
// used by the main send loop (`:1198` `MetisWriteFrame(0x02,
// FPGAWriteBufp)`).  Shared funnel through the §6-B TU-scope
// `metis_write_frame()` primitive.
constexpr int kEp2Endpoint = 0x02;

// Priming buffer is 1024 bytes (two 512-byte USB frames).
// Reference: `unsigned char buf[1024]; memset(buf, 0,
// sizeof(buf));` at `:107, :109`.
constexpr std::size_t kPrimingBufBytes = 1024;

// Per-USB-frame size in bytes.  Reference: byte indices use
// `buf[0..7]` (frame 0 header) and `buf[512..519]` (frame 1
// header) — implicit 512-byte USB-frame size.
constexpr std::size_t kUsbFrameBytes = 512;

// Inter-pass sleep matches reference `Sleep(10);` at
// `networkproto1.c:136, :138`.  Win32 `Sleep` takes milliseconds;
// Lyra uses the C++23-portable equivalent — identical 10 ms
// suspension semantics (⚠ acceptable Lyra-native idiom
// translation, no behavior change).
constexpr std::chrono::milliseconds kInterPassSleep{10};
}  // namespace

// ---- ctor/dtor ----

ForceCandC::ForceCandC()  = default;
ForceCandC::~ForceCandC() = default;

// ---- prime_pass ----
//
// Reference-verbatim port of `ForceCandCFrames(int count,
// int c0, int vfofreq)` at `networkproto1.c:106-132`.

void ForceCandC::prime_pass(int count,
                            int c0,
                            std::int32_t vfo_freq) {
    // Reference: `unsigned char buf[1024]; memset(buf, 0,
    // sizeof(buf));` at `:107, :109`.  Lyra: zero-initialized
    // `std::array`.
    std::array<std::uint8_t, kPrimingBufBytes> buf{};

    // ----- USB Frame 0 (buf[0..511]) — fixed C&C header -----
    //
    // Reference (`networkproto1.c:111-118`):
    //   buf[0] = 0x7f;
    //   buf[1] = 0x7f;
    //   buf[2] = 0x7f;
    //   buf[3] = 0;                          /* c0 */
    //   buf[4] = (SampleRateIn2Bits & 3);    /* c1 */
    //   buf[5] = 0;                          /* c2 */
    //   buf[6] = 0;                          /* c3 */
    //   buf[7] = (nddc - 1) << 3;            /* c4 */
    //
    // ⚠ Rule 24 — reference defect preserved verbatim:  the
    // priming frame-0 c4 byte is `(nddc-1)<<3` with NO duplex bit
    // set (= 0x18 for HL2 nddc=4).  Main-loop frame-0 emission
    // sets c4 = 0x1C (bit 2 = duplex; CLAUDE.md §3.2).  HL2
    // gateware accepts the priming VFO writes regardless; the
    // duplex bit becomes required only for the post-priming
    // main-loop freq updates.
    buf[0] = 0x7f;
    buf[1] = 0x7f;
    buf[2] = 0x7f;
    buf[3] = 0x00;                                                 // c0
    buf[4] = static_cast<std::uint8_t>(SampleRateIn2Bits & 0x03);  // c1
    buf[5] = 0x00;                                                 // c2
    buf[6] = 0x00;                                                 // c3
    buf[7] = static_cast<std::uint8_t>((nddc - 1) << 3);           // c4

    // ----- USB Frame 1 (buf[512..1023]) — VFO freq slot -----
    //
    // Reference (`networkproto1.c:120-127`):
    //   buf[512] = 0x7f;
    //   buf[513] = 0x7f;
    //   buf[514] = 0x7f;
    //   buf[515] = c0;                                /* c0 */
    //   buf[516] = (vfofreq >> 24) & 0xff;            /* c1 */
    //   buf[517] = (vfofreq >> 16) & 0xff;            /* c2 */
    //   buf[518] = (vfofreq >>  8) & 0xff;            /* c3 */
    //   buf[519] =  vfofreq        & 0xff;            /* c4 */
    //
    // BE-pack of the 32-bit VFO freq into c1..c4 verbatim.
    const std::size_t f1 = kUsbFrameBytes;  // = 512 (alias for readability)
    buf[f1 + 0] = 0x7f;
    buf[f1 + 1] = 0x7f;
    buf[f1 + 2] = 0x7f;
    buf[f1 + 3] = static_cast<std::uint8_t>(c0);                              // c0
    buf[f1 + 4] = static_cast<std::uint8_t>((vfo_freq >> 24) & 0xff);         // c1
    buf[f1 + 5] = static_cast<std::uint8_t>((vfo_freq >> 16) & 0xff);         // c2
    buf[f1 + 6] = static_cast<std::uint8_t>((vfo_freq >>  8) & 0xff);         // c3
    buf[f1 + 7] = static_cast<std::uint8_t>( vfo_freq        & 0xff);         // c4

    // ----- Send N copies via the shared wire primitive -----
    //
    // Reference (`networkproto1.c:129-131`):
    //   for (i = 0; i < count; i++) {
    //       MetisWriteFrame(2, (char*)buf);
    //   }
    //
    // Per §6-B, `metis_write_frame()` is the shared TU-scope
    // primitive in `wire/MetisFrame.cpp` consuming the shared
    // `g_metis_out_seq_num` — identical seq stream to what the
    // §6 `Ep2SendThread::run_loop` later consumes (matching the
    // reference's `MetisOutBoundSeqNum` global usage).
    for (int i = 0; i < count; ++i) {
        (void) metis_write_frame(kEp2Endpoint, buf.data());
    }
}

// ---- prime ----
//
// Reference-verbatim port of `ForceCandCFrame(int count)` at
// `networkproto1.c:134-139`.

void ForceCandC::prime(int count,
                       std::int32_t tx_freq_hz,
                       std::int32_t rx_freq_hz) {
    // Reference (`networkproto1.c:135-138`):
    //   ForceCandCFrames(count, 2, prn->tx[0].frequency);
    //   Sleep(10);
    //   ForceCandCFrames(count, 4, prn->rx[0].frequency);
    //   Sleep(10);
    //
    // TX freq via case-2 (`c0 = 2`); RX1 freq via case-4
    // (`c0 = 4`).  Two passes with a 10 ms sleep between (and
    // after the second).  The reference's HL2 call site
    // (`:430`) invokes `ForceCandCFrame(3);` — 3 frames per pass,
    // 6 datagrams total + 20 ms of sleeps.
    prime_pass(count, 2, tx_freq_hz);
    std::this_thread::sleep_for(kInterPassSleep);

    prime_pass(count, 4, rx_freq_hz);
    std::this_thread::sleep_for(kInterPassSleep);
}

}  // namespace lyra::wire

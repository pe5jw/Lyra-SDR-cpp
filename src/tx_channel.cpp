// Lyra-cpp — TX-1 component 2: WDSP TXA channel lifecycle (impl).
// See tx_channel.h for the locked design v2 §5.3 init contract +
// the §15.23 / §4.7 / §4.4 omitted-setter list (no
// SetTXABandpassRun, no SetTXAPanelSelect, no SetTXAALCThresh —
// each one has a documented failure mode if added back).

#include "tx_channel.h"

#include <QDebug>

#include <cmath>

namespace lyra::dsp {

namespace {

// WDSP TXA mode integers, source-verified against wdsp/TXA.c
// (TXASetMode switch around line 762-785).  LSB = 0, USB = 1.
constexpr int kTxaModeLSB = 0;
constexpr int kTxaModeUSB = 1;

// Channel-open envelope timing matches the reference TX
// channel-open posture: no up-delay, 10 ms up-slew / 10 ms
// down-slew.  These map cleanly to the FFT-coefficient
// allocations downstream and keep the keydown/keyup ramps
// inside one or two fexchange0 chunks at our wire cadence.
constexpr double kTDelayUp   = 0.000;
constexpr double kTSlewUp    = 0.010;
constexpr double kTDelayDown = 0.000;
constexpr double kTSlewDown  = 0.010;

// fexchange0 in_size + dsp_size match the reference's
// constant-latency rule (in_size = 64 * rate / 48000) and its
// TX dsp_size (4096) at dsp_rate=96 kHz.  At 48 kHz mic, that
// is 64 samples per fexchange0 call.  THESE VALUES MATTER:
//
//   * 64 is a CLEAN DIVISOR of dsp_insize = dsp_size / 2 = 2048
//     at the 48 k↔96 k rate ratio (2048 / 64 = 32 fexchange0
//     calls per DSP block).  Non-divisor in_size values (e.g.
//     the 126-per-datagram count Lyra used to choose) leave
//     residual samples in the reference's r1/r2 rings, and
//     the integer-division resampler/ring math then produces
//     anomalous mid-call sizes that overrun the destination
//     buffer on first fexchange0.  Same root cause class as
//     the §15.23 SSB-bp1 trap: a parameter chosen locally
//     that doesn't compose with WDSP's internal sizing.
//   * 4096 sizes the FFT-coefficient buffers downstream (the
//     reference's `max(2048, dsp_size)` clamp in TXA.c).
//
// Mic samples arrive from the rx-loop in datagram-sized
// chunks (~9-10 samples per HL2+ EP6 datagram at 48 k mic),
// accumulate in the SPSC ring, and the worker thread drains
// the ring in kInSize=64-sample chunks.  Datagram-vs-fexchange0
// alignment is handled BY THE RING — not by matching in_size
// to per-datagram count, which is what bit us.
constexpr int kInSize  = 64;
constexpr int kDspSize = 4096;

// Operator dB → linear gain.
double dbToLin(double db) {
    return std::pow(10.0, db / 20.0);
}

} // namespace

TxChannel::TxChannel(int channelId, WdspNative *wdsp)
    : wdsp_(wdsp), channel_(channelId)
{
}

TxChannel::~TxChannel()
{
    close();
}

int TxChannel::modeToWdsp(Mode m)
{
    return (m == Mode::USB) ? kTxaModeUSB : kTxaModeLSB;
}

std::pair<double, double>
TxChannel::signedEdges(Mode m, double low, double high)
{
    // Operator passes positive Hz; sign per the SSB convention.
    //   USB: l = +low,  h = +high   (positive baseband)
    //   LSB: l = -high, h = -low    (negative baseband)
    if (m == Mode::USB) {
        return { low, high };
    }
    return { -high, -low };
}

void TxChannel::pushBandpassLocked()
{
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetTXABandpassFreqs || !api.SetTXAMode) return;

    const auto [lo, hi] = signedEdges(mode_, opLow_, opHigh_);
    // Freqs FIRST so bp0 doesn't run on stale (-5000,-100)
    // create-time defaults during the window TXASetupBPFilters
    // gets called off SetTXAMode (see design v2 §5.3 step 3-4).
    api.SetTXABandpassFreqs(channel_, lo, hi);
    api.SetTXAMode(channel_, modeToWdsp(mode_));
}

bool TxChannel::open(int micRate, int dspRate, int outRate)
{
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (opened_) return true;
    if (!wdsp_ || !wdsp_->isLoaded()) {
        qWarning("[tx] open: WDSP DLL not loaded");
        return false;
    }
    const WdspApi &api = wdsp_->api();
    if (!api.OpenChannel || !api.SetChannelState) {
        qWarning("[tx] open: required channel symbols not resolved");
        return false;
    }
    // Pin the rates this lifecycle is built against — values
    // outside this band haven't been setter-audited yet.
    if (micRate != 48000 || dspRate != 96000 || outRate != 48000) {
        qWarning("[tx] open: rates outside the audited 48k/96k/48k "
                 "band (mic=%d dsp=%d out=%d) — refusing",
                 micRate, dspRate, outRate);
        return false;
    }
    micRate_ = micRate;
    dspRate_ = dspRate;
    outRate_ = outRate;

    // Pre-allocate the fexchange0 hot-path scratch.  At in_rate ==
    // out_rate (48k → 48k for SSB voice via the 96k DSP rate),
    // out_size equals in_size — both blocks are kInSize frames =
    // 2*kInSize interleaved doubles.  Allocating here (single-
    // thread main path) guarantees process() never touches the
    // allocator on the RT path that component 4c's worker drives.
    inBuf_.assign(static_cast<size_t>(2 * kInSize),  0.0);
    outBuf_.assign(static_cast<size_t>(2 * kInSize), 0.0);

    // OpenChannel(channel, in_size, dsp_size, in_rate, dsp_rate,
    //             out_rate, type=TX(1), state=stopped(0),
    //             tdelayup, tslewup, tdelaydown, tslewdown,
    //             block=1 — fexchange0 blocks until output ready).
    // OpenChannel(channel, in_size, dsp_size, in_rate, dsp_rate,
    //             out_rate, type=TX(1), state=stopped(0),
    //             tdelayup, tslewup, tdelaydown, tslewdown,
    //             block=1 — fexchange0 blocks until output ready).
    //
    // The 13 parameters here match the reference's TX
    // channel-open call BYTE-FOR-BYTE at the HL2+ AK4951 48 kHz
    // mic rate (cmaster.c create_xmtr).  Earlier Lyra picked
    // in_size=126 (per-datagram mic count) and dsp_size=2048;
    // both diverged from the reference's getbuffsize() rule
    // (in_size = 64 * rate / 48000) and dsp_size=4096 standard.
    // The non-integer ratio of dsp_insize / in_size that 126
    // produced caused WDSP's r1/r2 ring math to compute
    // anomalous mid-call sizes (the 93-frame memcpy that
    // overran the output buffer on first fexchange0).
    api.OpenChannel(channel_, kInSize, kDspSize,
                    micRate_, dspRate_, outRate_,
                    1,            // type = TX
                    0,            // state = stopped at open
                    kTDelayUp, kTSlewUp,
                    kTDelayDown, kTSlewDown,
                    1);           // block until output available
    opened_ = true;

    // NO SetTXA* setters here.  This is the second half of
    // matching the reference: cmaster.c::create_xmtr opens the
    // TXA channel and creates the cmaster-layer support objects
    // (txgain / EER / interleaver / sidetone) — it does NOT
    // touch ANY SetTXA* setter at this lifecycle stage.  The
    // TXA chain (bandpass / mode / PHROT / ALC / Leveler) stays
    // at WDSP's create-time defaults until the operator-settings
    // layer wires through (mode change, filter edits, mic gain
    // slider, etc.) and calls the per-setter API exposed on this
    // class (setMode / setBandpass / setMicGainDb / setPhrotOn /
    // setLevelerOn / setAlcMaxGainDb).
    //
    // The earlier Lyra code crammed 17 SetTXA* calls into open()
    // at boot — exercising WDSP code paths the reference doesn't
    // touch at this stage and risking interactions with WDSP's
    // internal state machine before the channel has ever run
    // fexchange0.  All those setters are gone now; the channel
    // comes up with WDSP create-time defaults, exactly as the
    // reference does.

    // open() PARKS the channel — the WDSP state stays 0
    // (stopped) until the PTT/MOX edge handler later calls
    // TxChannel::start().  The reference does the same: the
    // channel is opened in create_xmtr with state=0, and only
    // later does Console.cs / radio.cs flip state=1 when the
    // operator actually keys.  WDSP fexchange0's entry guard
    // `if (exchange & 1)` short-circuits when state=0, so the
    // chain is never invoked, and TxChannel::process() early-
    // returns on its running_ check (still false here).
    running_ = false;

    qInfo("[tx] channel %d opened (TX, mic=%d → dsp=%d → out=%d Hz; "
          "in_size=%d, dsp_size=%d — TXA at WDSP defaults; "
          "start() arms; stop() flushes); PARKED",
          channel_, micRate_, dspRate_, outRate_, kInSize, kDspSize);
    return true;
}

void TxChannel::close()
{
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (!opened_) return;
    if (running_ && wdsp_ && wdsp_->api().SetChannelState) {
        // Inlined stop (same reason as the start() inlining at the
        // end of open()): we're holding the channel mutex; stop()
        // takes the same mutex.  dmode=1 = blocking flush.
        wdsp_->api().SetChannelState(channel_, 0, 1);
        running_ = false;
    }
    if (wdsp_) {
        const WdspApi &api = wdsp_->api();
        if (api.CloseChannel) {
            api.CloseChannel(channel_);
        }
    }
    opened_ = false;
    qInfo("[tx] channel %d closed", channel_);
}

void TxChannel::start()
{
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (!opened_ || running_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetChannelState) return;
    api.SetChannelState(channel_, 1, 0);
    running_ = true;
}

void TxChannel::stop()
{
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (!opened_ || !running_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetChannelState) return;
    // dmode=1 — blocking flush; WDSP waits up to 100 ms for the
    // downslew envelope to complete before returning.
    api.SetChannelState(channel_, 0, 1);
    running_ = false;
}

// Operator setters — each takes channelMtx_ briefly around the
// opened_ check + the WDSP setter call so a close() can't tear the
// channel down between the check and the call.  WDSP's internal
// csDSP/csEXCH then serialises the setter against any in-flight
// fexchange0 on the worker thread.

void TxChannel::setMode(Mode m)
{
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (m == mode_) return;
    mode_ = m;
    pushBandpassLocked();
}

void TxChannel::setBandpass(double opLowHz, double opHighHz)
{
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (opLowHz < 0.0)  opLowHz  = 0.0;
    if (opHighHz < opLowHz) opHighHz = opLowHz + 1.0;
    opLow_  = opLowHz;
    opHigh_ = opHighHz;
    pushBandpassLocked();
}

void TxChannel::setMicGainDb(double db)
{
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetTXAPanelGain1) return;
    api.SetTXAPanelGain1(channel_, dbToLin(db));
}

void TxChannel::setAlcMaxGainDb(double db)
{
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetTXAALCMaxGain) return;
    api.SetTXAALCMaxGain(channel_, dbToLin(db));
}

void TxChannel::setLevelerOn(bool on, double topDb)
{
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (api.SetTXALevelerTop) {
        api.SetTXALevelerTop(channel_, dbToLin(topDb));
    }
    if (api.SetTXALevelerSt) {
        api.SetTXALevelerSt(channel_, on ? 1 : 0);
    }
}

void TxChannel::setPhrotOn(bool on)
{
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetTXAPHROTRun) return;
    api.SetTXAPHROTRun(channel_, on ? 1 : 0);
}

int TxChannel::process(const float *mic_block, int n,
                       std::complex<float> *iq_out)
{
    // THREAD-SAFETY: process() takes channelMtx_ across the whole
    // body — the same mutex open()/close()/start()/stop() take.
    // A close() on the main thread therefore can't tear the
    // channel down between the opened_/running_ check and the
    // fexchange0 call.  WDSP's internal csDSP/csEXCH separately
    // serialise the fexchange0 against any operator setter (each
    // setter ALSO takes channelMtx_, which means in practice an
    // operator setter waits one ~2.6 ms block period for any
    // in-flight process() to finish — fine for the infrequent
    // setter-call cadence on TX).
    std::lock_guard<std::mutex> lk(channelMtx_);
    if (!opened_ || !running_ || !wdsp_) return -1;
    if (!mic_block || !iq_out)           return -1;
    if (n != kInSize)                    return -1;   // contract
    const WdspApi &api = wdsp_->api();
    if (!api.fexchange0)                 return -1;

    // Build the interleaved input frame.  I = mic, Q = 0.0 — the
    // SSB voice path takes a real-valued mic stream; the patch-
    // panel's create-time inselect=2 routes I-from-input + zeros
    // Q downstream regardless of what we write into the Q slot,
    // but we explicitly zero it here for clarity + so a future
    // SetTXAPanelSelect change can't surface as a phantom Q leak.
    for (int i = 0; i < n; ++i) {
        inBuf_[static_cast<size_t>(2 * i + 0)] =
            static_cast<double>(mic_block[i]);
        inBuf_[static_cast<size_t>(2 * i + 1)] = 0.0;
    }

    // The DSP call.  block=1 at open() makes fexchange0 wait for
    // the WDSP TXA chain to produce a full output block before
    // returning — gives a deterministic in-step cadence in
    // exchange for blocking ~one block period.  err is advisory
    // (the RX path in WdspEngine::feedIq doesn't check it either
    // — WDSP writes the output buffer either way; non-zero just
    // means a transient buffer-management hiccup we can't act
    // on at this layer).
    fexErr_ = 0;
    api.fexchange0(channel_, inBuf_.data(), outBuf_.data(), &fexErr_);

    // Unpack interleaved (I,Q,I,Q,...) doubles → complex<float>.
    // At in_rate == out_rate (48k → 48k), out_size == in_size so
    // we emit exactly n complex samples 1:1 with the input.
    for (int i = 0; i < n; ++i) {
        iq_out[i] = std::complex<float>(
            static_cast<float>(outBuf_[static_cast<size_t>(2 * i + 0)]),
            static_cast<float>(outBuf_[static_cast<size_t>(2 * i + 1)]));
    }
    return fexErr_;
}

} // namespace lyra::dsp

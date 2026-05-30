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

// Channel-open envelope timing — same shape as the RX path uses,
// gives a smooth start/stop without producing audible artefacts
// on the channel itself.  uslew is gated to TUN / tone-gen so
// these only matter for non-SSB modes; harmless on SSB voice.
constexpr double kTDelayUp   = 0.010;
constexpr double kTSlewUp    = 0.025;
constexpr double kTDelayDown = 0.000;
constexpr double kTSlewDown  = 0.010;

// fexchange0 in_size at the radio's per-datagram mic-sample
// count for nddc=4 EP6 framing.  dsp_size 2048 leaves
// comfortable WDSP internal headroom at the 96 kHz DSP rate.
constexpr int kInSize  = 126;
constexpr int kDspSize = 2048;

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
    api.OpenChannel(channel_, kInSize, kDspSize,
                    micRate_, dspRate_, outRate_,
                    1,            // type = TX
                    0,            // state = stopped; explicit start() below
                    kTDelayUp, kTSlewUp,
                    kTDelayDown, kTSlewDown,
                    1);           // block until output available
    opened_ = true;

    // ── Init setter sequence (design v2 §5.3 steps 2..7).

    // 2. Pin panel gain explicitly at unity.  WDSP create_panel
    //    default is gain1=1.0; pinning here centralises the
    //    operator mic-gain control through one setter we own.
    if (api.SetTXAPanelGain1) {
        api.SetTXAPanelGain1(channel_, 1.0);
    }

    // 3 + 4. Bandpass freqs + mode, in that order.
    pushBandpassLocked();

    // 5. PHROT on by default — flattens speech PEP-to-PAR by
    //    ~3-4 dB.  Operator Settings toggle wires through
    //    setPhrotOn() later.
    if (api.SetTXAPHROTCorner)  api.SetTXAPHROTCorner(channel_, 338.0);
    if (api.SetTXAPHROTNstages) api.SetTXAPHROTNstages(channel_, 8);
    if (api.SetTXAPHROTRun)     api.SetTXAPHROTRun(channel_, 1);

    // 6. ALC — always on (mandatory splatter protection, no
    //    operator opt-out; operator only tunes MaxGain).
    //    attack/decay/hang are INT ms (do NOT pass doubles —
    //    wrong-cdef-type is the canonical regression class for
    //    these setters).  MaxGain stays at the WDSP create-time
    //    1.0 (0 dB) until the operator dials it.
    if (api.SetTXAALCAttack)  api.SetTXAALCAttack(channel_, 1);
    if (api.SetTXAALCDecay)   api.SetTXAALCDecay(channel_, 10);
    if (api.SetTXAALCHang)    api.SetTXAALCHang(channel_, 500);
    if (api.SetTXAALCMaxGain) api.SetTXAALCMaxGain(channel_, 1.0);
    if (api.SetTXAALCSt)      api.SetTXAALCSt(channel_, 1);

    // 7. Leveler — WIRED but OFF by default.  Operator Settings
    //    toggle flips SetTXALevelerSt live.  Defaults mirror the
    //    WDSP create-time values (top = 1.778 ≈ +5 dB).
    if (api.SetTXALevelerAttack) api.SetTXALevelerAttack(channel_, 1);
    if (api.SetTXALevelerDecay)  api.SetTXALevelerDecay(channel_, 500);
    if (api.SetTXALevelerHang)   api.SetTXALevelerHang(channel_, 500);
    if (api.SetTXALevelerTop)    api.SetTXALevelerTop(channel_, dbToLin(5.0));
    if (api.SetTXALevelerSt)     api.SetTXALevelerSt(channel_, 0);

    // 8. Start the channel.  state=1 (running), dmode=0 ignored.
    start();

    qInfo("[tx] channel %d opened (TX, mic=%d → dsp=%d → out=%d Hz; "
          "USB 200-3100; ALC on, leveler off, PHROT on); in_size=%d",
          channel_, micRate_, dspRate_, outRate_, kInSize);
    return true;
}

void TxChannel::close()
{
    if (!opened_) return;
    if (running_) {
        stop();
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
    if (!opened_ || running_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetChannelState) return;
    api.SetChannelState(channel_, 1, 0);
    running_ = true;
}

void TxChannel::stop()
{
    if (!opened_ || !running_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetChannelState) return;
    // dmode=1 — blocking flush; WDSP waits up to 100 ms for the
    // downslew envelope to complete before returning.
    api.SetChannelState(channel_, 0, 1);
    running_ = false;
}

void TxChannel::setMode(Mode m)
{
    if (m == mode_) return;
    mode_ = m;
    pushBandpassLocked();
}

void TxChannel::setBandpass(double opLowHz, double opHighHz)
{
    if (opLowHz < 0.0)  opLowHz  = 0.0;
    if (opHighHz < opLowHz) opHighHz = opLowHz + 1.0;
    opLow_  = opLowHz;
    opHigh_ = opHighHz;
    pushBandpassLocked();
}

void TxChannel::setMicGainDb(double db)
{
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetTXAPanelGain1) return;
    api.SetTXAPanelGain1(channel_, dbToLin(db));
}

void TxChannel::setAlcMaxGainDb(double db)
{
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetTXAALCMaxGain) return;
    api.SetTXAALCMaxGain(channel_, dbToLin(db));
}

void TxChannel::setLevelerOn(bool on, double topDb)
{
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
    if (!opened_ || !wdsp_) return;
    const WdspApi &api = wdsp_->api();
    if (!api.SetTXAPHROTRun) return;
    api.SetTXAPHROTRun(channel_, on ? 1 : 0);
}

int TxChannel::process(const float *mic_block, int n,
                       std::complex<float> *iq_out)
{
    // THREAD-SAFETY CONTRACT:
    //   This method is NOT thread-safe by itself.  Component 4c
    //   (the dedicated TX DSP worker thread) is the SOLE caller
    //   on the RT path, and 4c coordinates against the operator-
    //   facing setters (setMode/setBandpass/setMicGainDb/etc.,
    //   which may be invoked from the Qt main thread).  Pattern
    //   mirrors WdspEngine's channelMtx_ on the RX side: the
    //   worker holds the lock across fexchange0 so a main-thread
    //   setter cannot fire mid-DSP-frame.
    //
    //   For 4b, nothing calls process() yet — this body is here
    //   for 4c to invoke.  No mutex inside process() itself; the
    //   caller's lock is the discipline.
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

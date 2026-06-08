// Lyra — TxChannel.  See TxChannel.h.
//
// Stage 2b2d — populated body.  Reference-faithful 1:1 with the
// reference's `ChannelMaster/cmaster.c create_xmtr` /
// `destroy_xmtr` channel lifecycle calls.

#include "wdsp/TxChannel.h"
#include "wdsp_native.h"

namespace lyra::wdsp {

TxChannel::TxChannel(lyra::dsp::WdspNative& wdsp, int channel_id,
                     TxConfig cfg)
    : wdsp_(wdsp), channel_(channel_id), cfg_(cfg)
{
    // Sizes derived from rates via the reference
    // `getbuffsize(rate) = 64 * rate / 48000` formula
    // (cmsetup.c:106-111).  HL2 default: inRate==outRate==48000
    // -> inSize_ == outSize_ == 64.
    inSize_  = referenceBuffsize(cfg_.inRate);
    outSize_ = referenceBuffsize(cfg_.outRate);

    // Reference cmaster.c:126-127 allocates ALL THREE output
    // buffers unconditionally at create_xmtr time (even with EER
    // off and CW inactive).  Match that posture; out1/out2 sit
    // unused until EER + sidetone helpers land in their own
    // queued stages.  Buffer length = 2 * pairs (interleaved IQ).
    outBuf_ .assign(static_cast<size_t>(2 * outSize_), 0.0);
    out1Buf_.assign(static_cast<size_t>(2 * outSize_), 0.0);
    out2Buf_.assign(static_cast<size_t>(2 * outSize_), 0.0);
}

TxChannel::~TxChannel()
{
    // Symmetric to `destroy_xmtr` (cmaster.c:255-271): close the
    // channel before the buffers go out of scope so WDSP no
    // longer references them.
    close();
}

bool TxChannel::open()
{
    if (opened_) {
        return true;  // idempotent
    }
    if (!wdsp_.isLoaded()) {
        return false;
    }
    const lyra::dsp::WdspApi &api = wdsp_.api();
    if (!api.OpenChannel || !api.CloseChannel || !api.SetChannelState ||
        !api.fexchange0) {
        return false;
    }

    // OpenChannel parameters mirror reference cmaster.c:177-190
    // byte-for-byte:
    //   channel    = channel_                        (TX slot)
    //   in_size    = inSize_  = getbuffsize(inRate)  (cmsetup.c:71)
    //   dsp_size   = cfg_.dspSize  = 4096            (hardcoded)
    //   in_rate    = cfg_.inRate   = 48000           (xcm_inrate[in_id])
    //   dsp_rate   = cfg_.dspRate  = 96000           (hardcoded)
    //   out_rate   = cfg_.outRate  = 48000           (xmtr.ch_outrate)
    //   type       = 1                               (TX)
    //   state      = 0                               (initially OFF)
    //   tdelayup   = 0.000
    //   tslewup    = 0.010
    //   tdelaydown = 0.000
    //   tslewdown  = 0.010
    //   block      = 1                               (block-until-output)
    api.OpenChannel(channel_, inSize_, cfg_.dspSize,
                    cfg_.inRate, cfg_.dspRate, cfg_.outRate,
                    /*type=*/1, /*state=*/0,
                    cfg_.tDelayUp, cfg_.tSlewUp,
                    cfg_.tDelayDown, cfg_.tSlewDown,
                    cfg_.block);
    opened_ = true;
    return true;
}

void TxChannel::close()
{
    if (!opened_) {
        return;  // idempotent
    }
    const lyra::dsp::WdspApi &api = wdsp_.api();

    // Reference `destroy_xmtr` (cmaster.c:255-271) calls
    // `CloseChannel (chid (inid (1, i), 0))` alone at line 265
    // -- NO preceding SetChannelState.  The blocking-flush
    // SetChannelState(0,1) discipline belongs at keyup
    // (Lyra `stop()`; reference console.cs:30355), NOT at
    // destroy time.  Strict reference-fidelity per operator-
    // locked methodology: do as `destroy_xmtr` does.  If
    // `stop()` ran first the channel is already stopped; if
    // not, whatever CloseChannel's internal flush does is
    // exactly what the reference does at destroy time.
    if (api.CloseChannel) {
        api.CloseChannel(channel_);
    }
    opened_  = false;
    running_ = false;
}

void TxChannel::start()
{
    if (!opened_ || running_) {
        return;
    }
    const lyra::dsp::WdspApi &api = wdsp_.api();
    if (!api.SetChannelState) {
        return;
    }
    // Reference: `console.cs:30346` keydown final step --
    // SetChannelState(id(1,0), 1, 0) -- state=running, dmode=0.
    api.SetChannelState(channel_, /*state=*/1, /*dmode=*/0);
    running_ = true;
}

void TxChannel::stop()
{
    if (!running_) {
        return;
    }
    const lyra::dsp::WdspApi &api = wdsp_.api();
    if (!api.SetChannelState) {
        return;
    }
    // Reference: `console.cs:30355` keyup first step --
    // SetChannelState(id(1,0), 0, 1) -- state=stopped,
    // dmode=1 (blocking flush waits for WDSP downslew).
    // Reference has NO non-blocking TX-stop call site
    // anywhere -- this is unconditional.
    api.SetChannelState(channel_, /*state=*/0, /*dmode=*/1);
    running_ = false;
}

void TxChannel::setMode(TxaMode m)
{
    const lyra::dsp::WdspApi &api = wdsp_.api();
    if (!api.SetTXAMode) {
        return;
    }
    // SetTXAMode internally chains to TXASetupBPFilters
    // (reference TXA.c:827+ -- the body that sets
    // bp0.run=1, bp1.run=0, bp2.run=0 then switches on
    // txa.mode) which reconfigures bp0 + bp1 + bp2 from
    // the new mode.  This is the canonical mechanism; see
    // the LANDMINE note in TxChannel.h re bp1 / SSB
    // sideband selection -- do NOT add SetTXABandpassRun.
    api.SetTXAMode(channel_, static_cast<int>(m));
}

void TxChannel::setBandpass(double lo_hz, double hi_hz)
{
    const lyra::dsp::WdspApi &api = wdsp_.api();
    if (!api.SetTXABandpassFreqs) {
        return;
    }
    // SetTXABandpassFreqs -> TXA.c sets txa.f_low/f_high +
    // calls TXASetupBPFilters (same TXA.c entry point as
    // SetTXAMode; bp0 / bp1 / bp2 reconfigure from mode +
    // current f_low/f_high).  Callers pass signed Hz; the
    // per-mode sign convention (negate for LSB / CWL / DIGL,
    // symmetrize for DSB / AM / SAM / FM) is the caller's
    // responsibility -- matches the reference
    // SetTXABandpassFreqs which itself takes signed Hz from
    // the caller (per
    // `console.cs:8079-8118 UpdateTXLowHighFilterForMode`).
    api.SetTXABandpassFreqs(channel_, lo_hz, hi_hz);
}

int TxChannel::process(const double* mic_iq, int n_samples)
{
    // Rule 26 RAII state guard -- the reference assumes caller
    // has set the channel up; we make that explicit at the API
    // boundary.  When opened_ AND inputs are valid, behaviour
    // is byte-identical to a bare fexchange0 call.
    if (!opened_) {
        return -1;
    }
    const lyra::dsp::WdspApi &api = wdsp_.api();
    if (!api.fexchange0) {
        return -1;
    }
    // Reference: `cmaster.c:389` xcmaster() TX path:
    //     fexchange0(chid(stream, 0), pcm->in[stream],
    //                pcm->xmtr[tx].out[0], &error);
    // Lyra: pass mic_iq through as the input buffer (no copy);
    // out_buf is our internal outBuf_ (2 * outSize_ doubles).
    //
    // const_cast: WDSP's fexchange0 cdef is `double*` for
    // historical C reasons but it only READS through the input
    // pointer (cmaster.c never writes back through it).  Rule
    // 26 C-to-C++23 const-correctness bridge.
    int err = 0;
    api.fexchange0(channel_,
                   const_cast<double*>(mic_iq),
                   outBuf_.data(),
                   &err);
    return err;
}

}  // namespace lyra::wdsp

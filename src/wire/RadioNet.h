// Lyra — radio-network state container (mirrors the reference
// `radionet` struct in `network.h`).
//
// Holds the full C&C register state (host→radio write fields:
// drive_level, step_attn for ADC[0..2], pa, mic_*, BPF/LPF flags,
// puresignal_run, tx[*].frequency, rx[*].frequency, ptt_hang,
// tx_latency, reset_on_disconnect, etc.) PLUS the EP6 telemetry
// fields (radio→host read fields: ptt_in, dot_in, dash_in,
// adc_overload, fwd_power, rev_power, user_adc0..3, supply_volts,
// etc.).  Family-parameterized via the HPSDRModel enum so the
// same struct serves HL2/HL2+ today and ANAN/Hermes/Orion in
// v0.4 (Lyra-cpp v0.4 multi-radio refactor); arrays sized to the
// reference maxima MAX_ADC=3 / MAX_RX_STREAMS=12 / MAX_TX_STREAMS=3
// so a future family swap is a constructor-side change only.
//
// Synchronization is reference-faithful: six `std::mutex` members
// embedded in the struct, mirroring the reference's six
// CRITICAL_SECTIONs (`udpOUT`, `sendOUT`, `rcvpkt`, `sndpkt`,
// `seqErrors`, `rcvpktp1`).  Threads that need them reach into
// the live global instance and lock the relevant member at the
// same call sites the reference does.  `sendOUT` is declared and
// initialized but never locked anywhere in the reference tree;
// included here for byte-for-byte parity and may be retired
// post-Phase-2 if a future audit confirms it is still dead.
//
// Phase 1 empty skeleton — populated in Phase 2 per
// docs/TX_ARCHITECTURAL_MAPPING.md §8.

#pragma once

namespace lyra::wire {

class RadioNet {
public:
    RadioNet();
    ~RadioNet();
};

}  // namespace lyra::wire

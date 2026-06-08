// Lyra — ATT-on-TX policy (§5 / §10.2 tx layer).
//
// Operator-policy layer for HL2 / HL2+ RX-ADC protection during
// TX.  Mirrors reference's `m_bATTonTX` + `udATTOnTX` mechanism
// at `console.cs:30293-30327` (keydown) + `:30391-30410` (keyup)
// + `:19148-19175` (toggle handler); the HL2-specific `31 - x`
// wire-byte inversion lives in `wire/FrameComposer.cpp::
// set_tx_step_attn_db`.
//
// **Per §15.26 (operator-empirically confirmed on N8SDR HL2+ via
// lyra-Python 3-agent round-4 convergence):** on keydown, the
// policy forces `prn->adc[0].tx_step_attn` to the operator-axis
// `+31 dB` protective value (= wire byte 0x40 = gateware
// `rx_gain = 0` per `ad9866.v:144` arbiter = MIN LNA gain = MAX
// RX-ADC protection during TX).  On keyup: no-op (tx_step_attn is
// only read by the wire layer during MOX-gated TX; during RX,
// `compose_case_11` reads `rx_step_attn` instead, so the
// tx_step_attn value left over from keydown has no RX effect).
//
// **Default state matches operator-locked Thetis config**
// (chkATTOnTX=True + udATTOnTX=31 per operator's exported DB):
//   - `enabled_ = true`  — master toggle ON
//   - `att_db_  = 31`    — operator-axis +31 dB (max protection)
//
// **Wire-layer dependency:** requires `prn` allocated (via
// `lyra::wire::create_rnet()`) before any constructor or setter
// call.  Caller (HL2Stream::open or equivalent) MUST construct
// AFTER create_rnet.  No defensive null-guard inside the policy
// per the operator-locked "do as reference, period" rule (the
// reference's HL2 SetTxAttenData call site at console.cs has no
// such guard either; caller-owned precondition).
//
// **What this policy is NOT (deferred):**
//   - PS-A auto-attenuator FSM dynamic value (v0.3 PureSignal
//     work; the policy's `on_mox_keyup` no-op leaves a hook
//     point for the future PS-A restore semantics).
//   - Per-band TX-att memory (the per-band save/restore that
//     Thetis's `getTXstepAttenuatorForBand` provides) —
//     operator-feature work, not safety-critical wire fidelity.
//   - chkForceATTwhenPSAoff / chkForceATTwhenOutPowerChanges
//     conditional clauses — single global +31 today; conditional
//     logic lands with PS-A.
//
// Wire-inert at this stage: nothing constructs AttOnTxPolicy
// yet.  Stage 2b2e+ MOX FSM wires it: constructs the policy at
// HL2Stream::open (after create_rnet), calls `on_mox_keydown` /
// `on_mox_keyup` from the §15.25-correct slots in the FSM,
// surfaces `set_enabled` / `set_att_db` to the Settings → TX UI.

#pragma once

namespace lyra::tx {

class AttOnTxPolicy {
public:
    // Constructs the policy in its default state (enabled, +31 dB)
    // and primes `prn->adc[0].tx_step_attn` to the protective value
    // via `lyra::wire::set_tx_step_attn_db(att_db_)`.  Caller MUST
    // ensure `lyra::wire::create_rnet()` has been called first
    // (otherwise the priming call dereferences a null `prn`).
    //
    // Reference posture: reference's `create_rnet` at
    // `netInterface.c:1657` sets `prn->adc[0].tx_step_attn = 31`
    // directly (unsafe wire encoding — gateware reads gain code
    // 31 = MAX LNA gain).  Reference's operator UI then calls
    // `SetTxAttenData(31 - 31) = SetTxAttenData(0)` at session
    // open to flip to the safe state (`tx_step_attn = 0` = wire
    // byte 0x40 = gateware min gain).  This ctor's priming call
    // is the Lyra equivalent of that session-open SetTxAttenData
    // invocation — it ensures the safe state is reached without
    // requiring an explicit operator-UI action.
    AttOnTxPolicy();
    ~AttOnTxPolicy();

    AttOnTxPolicy(const AttOnTxPolicy&)            = delete;
    AttOnTxPolicy& operator=(const AttOnTxPolicy&) = delete;

    // Operator-facing master toggle.  Default ON (matches operator
    // Thetis chkATTOnTX=True).  When OFF, the `on_mox_keydown`
    // hook is a no-op; operator's normal `tx_step_attn` value
    // (whatever they've set explicitly) flows to the wire during
    // TX.  When flipping OFF→ON, the policy re-primes
    // tx_step_attn to the protective value so the safety becomes
    // effective immediately (no "wait for next keydown" gap).
    void set_enabled(bool enabled);
    bool enabled() const noexcept;

    // Operator-facing ATT-on-TX value in operator-axis dB
    // (positive = more attenuation; HL2 wire-layer
    // `set_tx_step_attn_db` handles the `31 - x` inversion).
    // Default +31 (matches operator Thetis udATTOnTX=31 = max
    // protection).  When changed while enabled, immediately
    // re-pushes the new value to the wire layer.
    void set_att_db(int operator_axis_db);
    int  att_db() const noexcept;

    // MOX-edge hooks.  Called by the MOX FSM / PttFsm at the
    // §15.25-correct moments in the keydown / keyup ordering:
    //
    //   - `on_mox_keydown`: after MOX state flip + AFTER the
    //     wire-MOX bit has gone to 1 (so the next compose_case_11
    //     frame reads `tx_step_attn`, NOT `rx_step_attn`).  Forces
    //     tx_step_attn to the operator-axis protective value via
    //     `set_tx_step_attn_db(att_db())`.  Idempotent — if the
    //     value was already set via ctor priming or `set_att_db`,
    //     this is a no-op write.  Reference: `console.cs:30293-
    //     30327` keydown sequence.
    //
    //   - `on_mox_keyup`: after the keyup blocking flush completes
    //     + after the wire-MOX bit has gone to 0.  Lyra simplified
    //     implementation: NO-OP.  tx_step_attn is only read by the
    //     wire layer during TX (compose_case_11's `if (XmitBit)`
    //     branch); during RX, compose_case_11 reads rx_step_attn
    //     instead, so the post-TX tx_step_attn value has no
    //     observable effect on RX.  Kept for FSM-symmetric API +
    //     future PS-A auto-attenuator hook point that needs to
    //     drive tx_step_attn dynamically during TX (PS) and let
    //     the policy restore at keyup.
    //
    // Both hooks are no-ops if !enabled().
    void on_mox_keydown();
    void on_mox_keyup();

private:
    bool enabled_ = true;    // §15.26 default ON (chkATTOnTX=True)
    int  att_db_  = 31;      // §15.26 default +31 dB (udATTOnTX=31)
};

}  // namespace lyra::tx

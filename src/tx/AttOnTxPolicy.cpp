// Lyra — AttOnTxPolicy.  See AttOnTxPolicy.h for the §15.26
// operator-empirical mechanism + reference citations.
//
// Task #114 operator-policy fix (2026-06-08): Phase-2 populate of
// the Phase-1 empty skeleton.  Wire-inert at this stage; Stage
// 2b2e+ MOX FSM constructs + invokes the hooks.

#include "tx/AttOnTxPolicy.h"
#include "wire/FrameComposer.h"   // lyra::wire::set_tx_step_attn_db

namespace lyra::tx {

AttOnTxPolicy::AttOnTxPolicy()
{
    // Prime tx_step_attn to the protective value at construction.
    // See header docstring for the reference rationale (Lyra's
    // equivalent of reference's session-open SetTxAttenData
    // invocation that flips create_rnet's unsafe default
    // tx_step_attn = 31 to the safe value).
    //
    // Caller MUST have called `lyra::wire::create_rnet()` first
    // (otherwise the setter dereferences a null `prn`).  No
    // defensive guard here per the operator-locked "do as
    // reference, period" rule — reference HL2 path
    // (`console.cs:10657` `SetTxAttenData`) has no such guard.
    if (enabled_) {
        lyra::wire::set_tx_step_attn_db(att_db_);
    }
}

AttOnTxPolicy::~AttOnTxPolicy() = default;

void AttOnTxPolicy::set_enabled(bool enabled)
{
    const bool was_enabled = enabled_;
    enabled_ = enabled;

    // When flipping OFF→ON, immediately push the protective value
    // so the safety becomes effective without waiting for the next
    // keydown.  When flipping ON→OFF (or no transition), leave
    // tx_step_attn at its current value — operator is responsible
    // for the consequences of disabling ATT-on-TX (e.g. setting
    // an explicit tx_step_attn for PS-A operation).
    if (enabled_ && !was_enabled) {
        lyra::wire::set_tx_step_attn_db(att_db_);
    }
}

bool AttOnTxPolicy::enabled() const noexcept { return enabled_; }

void AttOnTxPolicy::set_att_db(int operator_axis_db)
{
    att_db_ = operator_axis_db;

    // Re-push immediately if enabled — the new value should reach
    // the wire layer without waiting for the next keydown.
    if (enabled_) {
        lyra::wire::set_tx_step_attn_db(att_db_);
    }
}

int AttOnTxPolicy::att_db() const noexcept { return att_db_; }

void AttOnTxPolicy::on_mox_keydown()
{
    if (!enabled_) {
        return;
    }

    // Force tx_step_attn to the protective value before the wire
    // emits the first XmitBit=1 frame.  Idempotent re-push of the
    // value already set via ctor priming / set_att_db; this
    // keydown call guarantees the safe state even if some
    // intervening operator action (e.g. a Settings dialog write)
    // changed tx_step_attn between the prior keyup and this
    // keydown.  Reference: `console.cs:30293-30327` keydown
    // sequence forces the same value via SetTxAttenData.
    lyra::wire::set_tx_step_attn_db(att_db_);
}

void AttOnTxPolicy::on_mox_keyup()
{
    // No-op — see header docstring for rationale.  tx_step_attn
    // is only read by compose_case_11 during XmitBit=1; during
    // RX, compose_case_11 reads rx_step_attn instead, so any
    // post-TX tx_step_attn value has no RX-time effect.  Hook
    // point retained for future PS-A auto-attenuator restore
    // semantics (v0.3+ work; PS-A drives tx_step_attn
    // dynamically during TX, then needs the keyup hook to
    // restore the policy default).
}

}  // namespace lyra::tx

// Lyra — C&C frame composer.  See FrameComposer.h.
//
// Source mirror: `networkproto1.c::WriteMainLoop_HL2`
// (lines 869-1191).  Case bodies for §4a-scope cases 0 / 2 / 3
// are byte-by-byte verbatim per the signed §4a parity checkpoint.
// Cases 1 (TX VFO — §4b) and 4-18 (TX att, drive, PA, mic, LNA,
// PS, ANAN-only) compile-time present per Q2 eager but
// `assert(false …)` placeholders until §4b / §4c populate.
//
// WIRE-INERT — not called from anywhere yet (§7 `Ep2SendThread`
// will wire it in once §4b lands the remaining MOX-edge-relevant
// cases).

#include "wire/FrameComposer.h"
#include "wire/RadioNet.h"
#include "wire/RbpFilter.h"

#include <cassert>
#include <mutex>

namespace lyra::wire {

FrameComposer::FrameComposer()  = default;
FrameComposer::~FrameComposer() = default;

// =================== §4a setters =====================================

void FrameComposer::set_rx_freq(int rx_idx, int freq_hz) {
    std::lock_guard<std::mutex> guard(cc_lock_);
    if (rx_idx < 0 || rx_idx >= kMaxRxStreams)
        return;
    if (prn == nullptr)
        return;
    prn->rx[rx_idx].frequency = freq_hz;
    // No payload caching — the per-frame switch reads
    // `prn->rx[N].frequency` directly at compose time, exactly
    // as the reference does (per Q1).
}

// §4b-1.8 setter — TX NCO freq for case 1.
void FrameComposer::set_tx_freq(int freq_hz) {
    std::lock_guard<std::mutex> guard(cc_lock_);
    if (prn == nullptr)
        return;
    prn->tx[0].frequency = freq_hz;
    // No payload caching — case 1 reads `prn->tx[0].frequency`
    // directly at compose time.
}

// =================== §4b-2.5 setters =================================

// §4b-2.5 — Write `prn->tx[0].drive_level` (case 10 C1).
void FrameComposer::set_drive_level(int level) {
    std::lock_guard<std::mutex> guard(cc_lock_);
    if (prn == nullptr)
        return;
    prn->tx[0].drive_level = level;
}

// §4b-2.5 — Write `prn->tx[0].pa` (case 10 C3 bit 7, legacy path).
// Apollo-modded HL2+ PA enable is via `ApolloTuner` C2 bit 3
// (Task #114); this setter only touches the legacy bit.
void FrameComposer::set_pa_on(bool on) {
    std::lock_guard<std::mutex> guard(cc_lock_);
    if (prn == nullptr)
        return;
    prn->tx[0].pa = on ? 1 : 0;
}

// §4b-2.5 — TX step attenuator setter with HL2-family branching.
// Source-verified `console.cs:10657-10663`: HL2 applies the
// (31 - signed_db) inversion; non-HL2 families use the raw value.
void FrameComposer::set_tx_step_attn_db(int signed_db) {
    std::lock_guard<std::mutex> guard(cc_lock_);
    if (prn == nullptr)
        return;

    int wire_value;
    switch (hpsdrModel) {
        case HPSDRModel::HERMESLITE:
        case HPSDRModel::HPSDR:  // HL2-class
            // Operator-axis -28..+31 → wire-encoded (31 - x); store
            // 6-bit (case 11 uses 6-bit + 0x40; case 4 uses 5-bit
            // truncation from the same field).
            wire_value = (31 - signed_db) & 0x3F;
            break;
        default:
            // ANAN / Orion / RedPitaya: raw value 0..31 → wire 0..31
            // (non-HL2 families).  ASSERT until tester hardware is
            // available per §3.6 family-parameterized Option A.
            assert(false && "non-HL2 TX step ATT encoding not yet "
                            "implemented — needs operator bench "
                            "verification");
            wire_value = signed_db & 0x3F;  // unreachable; compiler-quiet
            break;
    }
    prn->adc[0].tx_step_attn = wire_value;
}

// §4b-2.5 — RX step attenuator setter (no inversion, any family).
void FrameComposer::set_rx_step_attn_db(int signed_db, int adc_idx) {
    std::lock_guard<std::mutex> guard(cc_lock_);
    if (prn == nullptr)
        return;
    if (adc_idx < 0 || adc_idx >= kMaxAdc)
        return;
    prn->adc[adc_idx].rx_step_attn = signed_db & 0x3F;
}

// =================== §4a.3 case 0 ====================================
//
// Source: networkproto1.c:948-970, HL2 dispatch.

void FrameComposer::compose_case_0([[maybe_unused]] unsigned char& C0,
                                   unsigned char& C1,
                                   unsigned char& C2, unsigned char& C3,
                                   unsigned char& C4) {
    // Defensive: §4a-scope is WIRE-INERT; compose_case_0 should
    // never run with null prn / prbpfilter, but assert defensively
    // so a future wire-up that forgets to allocate them fails LOUD.
    assert(prn != nullptr);
    assert(prbpfilter != nullptr);

    // C0 is unchanged for case 0 — it carries only the XmitBit
    // base set in the caller (no `C0 |= addr` here; addr 0 leaves
    // bits 7..1 zero).  Parameter is `[[maybe_unused]]` to express
    // this in code; reference's case-0 body @ networkproto1.c:948-
    // 970 also has no C0 write.

    // C1 — sample-rate code (networkproto1.c:949)
    C1 = static_cast<unsigned char>(SampleRateIn2Bits & 3);

    // C2 — CW EER flag (bit 0) + OC pins shifted into bits 7..1
    // (networkproto1.c:950)
    C2 = static_cast<unsigned char>(
            (prn->cw.eer & 1)
          | ((prn->oc_output << 1) & 0xFE));

    // C3 — six OR'd bitfields then a three-way conditional on the
    // RX-input bits (networkproto1.c:951-959)
    C3 = static_cast<unsigned char>(
            (prbpfilter->_10_dB_Atten & 1)
          | ((prbpfilter->_20_dB_Atten << 1) & 0b00000010)
          | ((prn->rx[0].preamp        << 2) & 0b00000100)
          | ((prn->adc[0].dither       << 3) & 0b00001000)
          | ((prn->adc[0].random       << 4) & 0b00010000)
          | ((prbpfilter->_Rx_1_Out    << 7) & 0b10000000));

    if (prbpfilter->_XVTR_Rx_In)
        C3 |= 0b01100000;
    else if (prbpfilter->_Rx_1_In)
        C3 |= 0b00100000;
    else if (prbpfilter->_Rx_2_In)
        C3 |= 0b01000000;

    // C4 — antenna-select 3-way + duplex bit + nddc-1<<3 +
    // diversity<<7 (networkproto1.c:961-969)
    if (prbpfilter->_ANT_3 == 1)
        C4 = 0b10;
    else if (prbpfilter->_ANT_2 == 1)
        C4 = 0b01;
    else
        C4 = 0b0;
    C4 |= static_cast<unsigned char>(0b00000100);              // duplex bit
    C4 |= static_cast<unsigned char>((nddc - 1) << 3);          // DDC count
    C4 |= static_cast<unsigned char>((P1_en_diversity) << 7);   // diversity lock
}

// =================== §4a.4 case 2 ====================================
//
// Source: networkproto1.c:982-993, HL2 dispatch.
// DDC0 is always RX1 frequency, except in the nddc=2 (Hermes II)
// PS-on TX state where it carries TX frequency.  On HL2 nddc=4 the
// nddc==2 branch is a structural no-op (preserved verbatim).

void FrameComposer::compose_case_2(unsigned char& C0, unsigned char& C1,
                                   unsigned char& C2, unsigned char& C3,
                                   unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 4;  // addr 2: C0 |= (addr << 1) → 0x04

    int ddc_freq;
    if ((nddc == 2) && (XmitBit == 1) && (prn->puresignal_run))
        ddc_freq = prn->tx[0].frequency;
    else
        ddc_freq = prn->rx[0].frequency;

    C1 = static_cast<unsigned char>((ddc_freq >> 24) & 0xff);
    C2 = static_cast<unsigned char>((ddc_freq >> 16) & 0xff);
    C3 = static_cast<unsigned char>((ddc_freq >>  8) & 0xff);
    C4 = static_cast<unsigned char>((ddc_freq      ) & 0xff);
}

// =================== §4a.5 case 3 ====================================
//
// Source: networkproto1.c:995-1010, HL2 dispatch.
// Three-way conditional preserved verbatim:
//   (1) Hermes-II nddc=2 + PS + TX → DDC1 = TX freq;
//   (2) Orion / ANAN P1 nddc=5 → DDC1 = RX1 freq;
//   (3) default HL2 nddc=4 → DDC1 = RX2 freq.

void FrameComposer::compose_case_3(unsigned char& C0, unsigned char& C1,
                                   unsigned char& C2, unsigned char& C3,
                                   unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 6;  // addr 3: C0 |= (addr << 1) → 0x06

    int ddc_freq;
    if ((nddc == 2) && (XmitBit == 1) && (prn->puresignal_run))
        ddc_freq = prn->tx[0].frequency;
    else if (nddc == 5)
        ddc_freq = prn->rx[0].frequency;
    else
        ddc_freq = prn->rx[1].frequency;  // default HL2 nddc=4 path

    C1 = static_cast<unsigned char>((ddc_freq >> 24) & 0xff);
    C2 = static_cast<unsigned char>((ddc_freq >> 16) & 0xff);
    C3 = static_cast<unsigned char>((ddc_freq >>  8) & 0xff);
    C4 = static_cast<unsigned char>((ddc_freq      ) & 0xff);
}

// =================== §4b-1.1 case 1 ==================================
//
// Source: networkproto1.c:974-980, HL2 dispatch.
// TX VFO — DDC2/3 mirror it on HL2 nddc=4 (see §4c case 6).

void FrameComposer::compose_case_1(unsigned char& C0, unsigned char& C1,
                                   unsigned char& C2, unsigned char& C3,
                                   unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 2;  // addr 1: C0 |= (addr << 1) → 0x02

    C1 = static_cast<unsigned char>((prn->tx[0].frequency >> 24) & 0xff);
    C2 = static_cast<unsigned char>((prn->tx[0].frequency >> 16) & 0xff);
    C3 = static_cast<unsigned char>((prn->tx[0].frequency >>  8) & 0xff);
    C4 = static_cast<unsigned char>((prn->tx[0].frequency      ) & 0xff);
}

// =================== §4b-1.2 case 4 ==================================
//
// Source: networkproto1.c:1015-1021, HL2 dispatch.
// ADC assignments (`P1_adc_cntrl`) + TX step attenuator narrow 5-bit
// form.  The wider 6-bit MOX-gated form lives in case 11 (§4b-2) —
// both read the same `prn->adc[0].tx_step_attn` field.  No `31 - x`
// inversion at this layer; the inversion is the setter's job
// (`set_tx_step_attn_db`, lands with §4b-2).
//
// Note on C2: the literal mask `0b0011111111` is 10 bits but the
// destination is `unsigned char` (8 bits) — the upper 2 bits of
// the mask are structurally unreachable.  Preserved verbatim per
// Rule 24 (don't "clean up" reference quirks).

void FrameComposer::compose_case_4(unsigned char& C0, unsigned char& C1,
                                   unsigned char& C2, unsigned char& C3,
                                   unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 0x1c;  // addr 14 (0x0e in reference comment): C0 |= 0x0e << 1 = 0x1c

    C1 = static_cast<unsigned char>(P1_adc_cntrl & 0xFF);
    C2 = static_cast<unsigned char>((P1_adc_cntrl >> 8) & 0b0011111111);  // verbatim 10-bit mask
    C3 = static_cast<unsigned char>(prn->adc[0].tx_step_attn & 0b00011111);
    C4 = 0;
}

// =================== §4b-1.3 case 13 =================================
//
// Source: networkproto1.c:1127-1133, HL2 dispatch.
// CW enable bitfield + sidetone level + RF delay.

void FrameComposer::compose_case_13(unsigned char& C0, unsigned char& C1,
                                    unsigned char& C2, unsigned char& C3,
                                    unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 0x1e;  // addr 15: C0 |= 0x0f << 1 = 0x1e

    // `prn->cw.cw_enable` is a 1-bit bitfield in `_cw.mode_control`
    // (§1.5).  Bitfield → integer assignment yields 0 or 1.
    C1 = static_cast<unsigned char>(prn->cw.cw_enable);
    C2 = static_cast<unsigned char>(prn->cw.sidetone_level);
    C3 = static_cast<unsigned char>(prn->cw.rf_delay);
    C4 = 0;
}

// =================== §4b-1.4 case 14 =================================
//
// Source: networkproto1.c:1135-1141, HL2 dispatch.
// CW hang_delay split 10-bit across C1/C2; CW sidetone_freq split
// 12-bit across C3/C4.

void FrameComposer::compose_case_14(unsigned char& C0, unsigned char& C1,
                                    unsigned char& C2, unsigned char& C3,
                                    unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 0x20;  // addr 16: C0 |= 0x10 << 1 = 0x20

    // 10-bit hang_delay: upper 8 in C1, lower 2 in C2
    C1 = static_cast<unsigned char>((prn->cw.hang_delay >> 2) & 0b11111111);
    C2 = static_cast<unsigned char>(prn->cw.hang_delay        & 0b00000011);

    // 12-bit sidetone_freq: upper 8 in C3, lower 4 in C4
    C3 = static_cast<unsigned char>((prn->cw.sidetone_freq >> 4) & 0b11111111);
    C4 = static_cast<unsigned char>(prn->cw.sidetone_freq        & 0b00001111);
}

// =================== §4b-1.5 case 15 =================================
//
// Source: networkproto1.c:1143-1149, HL2 dispatch.
// EER PWM min/max — each 10-bit, split across two bytes.

void FrameComposer::compose_case_15(unsigned char& C0, unsigned char& C1,
                                    unsigned char& C2, unsigned char& C3,
                                    unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 0x22;  // addr 17: C0 |= 0x11 << 1 = 0x22

    // 10-bit epwm_min: upper 8 in C1, lower 2 in C2
    C1 = static_cast<unsigned char>((prn->tx[0].epwm_min >> 2) & 0b11111111);
    C2 = static_cast<unsigned char>(prn->tx[0].epwm_min        & 0b00000011);

    // 10-bit epwm_max: upper 8 in C3, lower 2 in C4
    C3 = static_cast<unsigned char>((prn->tx[0].epwm_max >> 2) & 0b11111111);
    C4 = static_cast<unsigned char>(prn->tx[0].epwm_max        & 0b00000011);
}

// =================== §4b-1.6 case 17 =================================
//
// Source: networkproto1.c:1162-1168, HL2 dispatch.
// HL2 TX-latency register — the "0x2e" enhancement.  5-bit PTT hang
// in C3, 7-bit TX latency (ms) in C4.

void FrameComposer::compose_case_17(unsigned char& C0, unsigned char& C1,
                                    unsigned char& C2, unsigned char& C3,
                                    unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 0x2e;  // addr 23: C0 |= 0x17 << 1 = 0x2e

    C1 = 0;
    C2 = 0;
    C3 = static_cast<unsigned char>(prn->tx[0].ptt_hang   & 0b00011111);  // 5-bit
    C4 = static_cast<unsigned char>(prn->tx[0].tx_latency & 0b01111111);  // 7-bit
}

// =================== §4b-1.7 case 18 =================================
//
// Source: networkproto1.c:1170-1176, HL2 dispatch.
// HL2 safety bit — when non-zero the gateware auto-resets on host
// disconnect.  Per §15.26 history, defaults to 0 to avoid the wedge
// defect where a clean Lyra stop triggered gateware reset.

void FrameComposer::compose_case_18(unsigned char& C0, unsigned char& C1,
                                    unsigned char& C2, unsigned char& C3,
                                    unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 0x74;  // addr 58: C0 |= 0x3a << 1 = 0x74

    C1 = 0;
    C2 = 0;
    C3 = 0;
    C4 = static_cast<unsigned char>(prn->reset_on_disconnect);
}

// =================== §4b-2.1 case 10 =================================
//
// Source: networkproto1.c:1076-1089, HL2 dispatch.
// Drive level + Apollo PA / filter / tuner / ATU bits + mic_boost
// + line_in + per-band HPF/LPF + legacy PA bit.

void FrameComposer::compose_case_10(unsigned char& C0, unsigned char& C1,
                                    unsigned char& C2, unsigned char& C3,
                                    unsigned char& C4) {
    assert(prn != nullptr);
    assert(prbpfilter != nullptr);

    C0 |= 0x12;  // addr 9: C0 |= 0x09 << 1 = 0x12

    // C1 — drive level (operator-axis; SWR correction applied at
    // Radio-layer caller, NOT here).  Source: :1078.
    C1 = static_cast<unsigned char>(prn->tx[0].drive_level);

    // C2 — mic_boost (bit 0) + line_in (bit 1) + Apollo globals
    // OR'd inline + forced bit 6 (0x40), masked to 7 bits.  Source:
    // :1079-1080.  The Apollo globals hold pre-shifted bit-values
    // (or zero) per the reference convention.
    C2 = static_cast<unsigned char>(
            ((prn->mic.mic_boost & 1)
          | ((prn->mic.line_in & 1) << 1)
          | ApolloFilt
          | ApolloTuner
          | ApolloATU
          | ApolloFiltSelect
          | 0b01000000) & 0x7f);

    // C3 — bit 0-4 = 5 HPF bands; bit 5 = Bypass; bit 6 = 6M_preamp;
    // bit 7 = legacy `prn->tx[0].pa`.  Source: :1081-1084.
    C3 = static_cast<unsigned char>(
            (prbpfilter->_13MHz_HPF  & 1)
          | ((prbpfilter->_20MHz_HPF & 1) << 1)
          | ((prbpfilter->_9_5MHz_HPF & 1) << 2)
          | ((prbpfilter->_6_5MHz_HPF & 1) << 3)
          | ((prbpfilter->_1_5MHz_HPF & 1) << 4)
          | ((prbpfilter->_Bypass     & 1) << 5)
          | ((prbpfilter->_6M_preamp  & 1) << 6)
          | ((prn->tx[0].pa           & 1) << 7));

    // C4 — 7 per-band LPF bits, bit 7 unused.  Source: :1085-1088.
    C4 = static_cast<unsigned char>(
            (prbpfilter->_30_20_LPF  & 1)
          | ((prbpfilter->_60_40_LPF & 1) << 1)
          | ((prbpfilter->_80_LPF    & 1) << 2)
          | ((prbpfilter->_160_LPF   & 1) << 3)
          | ((prbpfilter->_6_LPF     & 1) << 4)
          | ((prbpfilter->_12_10_LPF & 1) << 5)
          | ((prbpfilter->_17_15_LPF & 1) << 6));
}

// =================== §4b-2.2 case 11 =================================
//
// Source: networkproto1.c:1091-1103, HL2 dispatch.
// Preamps (4 bits, incl. the `rx[0].preamp << 3` reference quirk
// preserved verbatim) + mic_trs/bias/ptt + line_in_gain +
// puresignal_run + user_dig_out + MOX-gated 6-bit step ATT + 0x40
// enable.
//
// C4 MOX-gated is the §15.26-history load-bearing RX-ADC protection
// mechanism — during TX, the operator-policy layer ensures
// `prn->adc[0].tx_step_attn` is set to the protective value
// (Task #114 ATT-on-TX policy).

void FrameComposer::compose_case_11(unsigned char& C0, unsigned char& C1,
                                    unsigned char& C2, unsigned char& C3,
                                    unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 0x14;  // addr 10: C0 |= 0x0a << 1 = 0x14

    // C1 — bits 0/1/2 = rx[0/1/2].preamp; bit 3 = rx[0].preamp
    // AGAIN (reference quirk preserved per Rule 24); bits 4/5/6 =
    // mic_trs / mic_bias / mic_ptt.  Source: :1093-1096.
    C1 = static_cast<unsigned char>(
            (prn->rx[0].preamp & 1)
          | ((prn->rx[1].preamp & 1) << 1)
          | ((prn->rx[2].preamp & 1) << 2)
          | ((prn->rx[0].preamp & 1) << 3)   // verbatim duplicate
          | ((prn->mic.mic_trs  & 1) << 4)
          | ((prn->mic.mic_bias & 1) << 5)
          | ((prn->mic.mic_ptt  & 1) << 6));

    // C2 — line_in_gain (5-bit) + puresignal_run bit at 6.  Source:
    // :1097.
    C2 = static_cast<unsigned char>(
            (prn->mic.line_in_gain & 0b00011111)
          | ((prn->puresignal_run & 1) << 6));

    // C3 — user_dig_out 4-bit.  Source: :1098.
    C3 = static_cast<unsigned char>(prn->user_dig_out & 0b00001111);

    // C4 — MOX-gated 6-bit step ATT + 0x40 enable bit.  Source:
    // :1099-1102.  This is the §15.26-load-bearing RX-ADC
    // protection mechanism.
    if (XmitBit) {
        C4 = static_cast<unsigned char>(
                (prn->adc[0].tx_step_attn & 0b00111111) | 0b01000000);
    } else {
        C4 = static_cast<unsigned char>(
                (prn->adc[0].rx_step_attn & 0b00111111) | 0b01000000);
    }
}

// =================== §4b-2.3 case 12 =================================
//
// Source: networkproto1.c:1105-1123, HL2 dispatch.
// ADC1/ADC2 step ATT (XmitBit-force on ADC1) + CW keyer config.
// Two reference quirks preserved verbatim per Rule 24:
//   - No explicit `& 0x1F` mask on `adc[1].rx_step_attn` RX branch
//   - No `& 1` mask on `strict_spacing` (harmless — 1-bit bitfield)

void FrameComposer::compose_case_12(unsigned char& C0, unsigned char& C1,
                                    unsigned char& C2, unsigned char& C3,
                                    unsigned char& C4) {
    assert(prn != nullptr);

    C0 |= 0x16;  // addr 11: C0 |= 0x0b << 1 = 0x16

    // C1 — XmitBit force-31 on ADC1 ATT, then OR 0x20 enable bit.
    // RX branch does NOT mask `adc[1].rx_step_attn` — reference
    // quirk preserved.  Source: :1107-1111.
    if (XmitBit) {
        C1 = 0x1F;
    } else {
        C1 = static_cast<unsigned char>(prn->adc[1].rx_step_attn);
    }
    C1 |= 0b00100000;  // 0x20 enable bit

    // C2 — ADC2 5-bit step ATT + 0x20 enable + rev_paddle bit 6.
    // Source: :1112-1113.
    C2 = static_cast<unsigned char>(
            (prn->adc[2].rx_step_attn & 0b00011111)
          | 0b00100000
          | ((prn->cw.rev_paddle & 1) << 6));

    // CWMode 3-way conditional.  Source: :1115-1120.
    unsigned char CWMode;
    if (prn->cw.iambic == 0)
        CWMode = 0b00000000;
    else if (prn->cw.mode_b == 0)
        CWMode = 0b01000000;
    else
        CWMode = 0b10000000;

    // C3 — keyer_speed (6-bit) + CWMode (bits 6-7).  Source: :1121.
    C3 = static_cast<unsigned char>(
            (prn->cw.keyer_speed & 0b00111111) | CWMode);

    // C4 — keyer_weight (7-bit) + strict_spacing at bit 7.
    // `strict_spacing` has no `& 1` mask in the reference; harmless
    // because it's a 1-bit bitfield.  Source: :1122.
    C4 = static_cast<unsigned char>(
            (prn->cw.keyer_weight & 0b01111111)
          | ((prn->cw.strict_spacing) << 7));
}

// =================== §4b-2.4 case 16 =================================
//
// Source: networkproto1.c:1151-1160, HL2 dispatch.
// BPF2 (Alex1 secondary band-pass filter board) HPF/Bypass/preamp
// bits read from `prbpfilter2` + xvtr_enable + puresignal_run bit.
//
// The `_rx2_gnd << 7` has NO `& 1` mask in the reference — quirk
// preserved per Rule 24 (harmless because `_rx2_gnd` is a 1-bit
// bitfield in §2 RbpFilter2).

void FrameComposer::compose_case_16(unsigned char& C0, unsigned char& C1,
                                    unsigned char& C2, unsigned char& C3,
                                    unsigned char& C4) {
    assert(prn != nullptr);
    assert(prbpfilter2 != nullptr);

    C0 |= 0x24;  // addr 18: C0 |= 0x12 << 1 = 0x24

    // C1 — 5 Alex1 HPF bands + Bypass + 6M_preamp + `_rx2_gnd` at
    // bit 7 (no `& 1` mask preserved verbatim).  Source: :1153-1156.
    C1 = static_cast<unsigned char>(
            (prbpfilter2->_13MHz_HPF  & 1)
          | ((prbpfilter2->_20MHz_HPF & 1) << 1)
          | ((prbpfilter2->_9_5MHz_HPF & 1) << 2)
          | ((prbpfilter2->_6_5MHz_HPF & 1) << 3)
          | ((prbpfilter2->_1_5MHz_HPF & 1) << 4)
          | ((prbpfilter2->_Bypass     & 1) << 5)
          | ((prbpfilter2->_6M_preamp  & 1) << 6)
          | ((prbpfilter2->_rx2_gnd          ) << 7));  // no `& 1` per source

    // C2 — xvtr_enable (bit 0) + puresignal_run (bit 6).  Source:
    // :1157.
    C2 = static_cast<unsigned char>(
            (xvtr_enable & 1)
          | ((prn->puresignal_run & 1) << 6));

    // C3 / C4 — unused on case 16.  Source: :1158-1159.
    C3 = 0;
    C4 = 0;
}

// =================== §4a.1 main scheduler ============================
//
// Source mirror: networkproto1.c::WriteMainLoop_HL2 lines 869-1191.
// Structural elements (per §4a.1 row table):
//   - Two USB frames per call (outer txframe loop)
//   - Three sync bytes 0x7f at [0..2] of each USB frame
//   - MOX-edge jump (nddc=2 only — no-op on HL2)
//   - C0 base init = XmitBit (MOX bit at bit 0)
//   - I2C-transaction overlay (overrides C0..C4 when queued)
//   - Switch dispatch (cases 0-18 compile-time present per Q2)
//   - Cursor advance INSIDE the else branch (no advance on I2C
//     overlay frames)
//   - Post-switch packet packing into txbptr[3..7]

void FrameComposer::write_main_loop_hl2(char* txbptr_base) {
    std::lock_guard<std::mutex> guard(cc_lock_);

    // Reference defensively assumes prn / prbpfilter are non-null;
    // the §10.2 component split says the wire-layer initializer
    // allocates them before write_main_loop_hl2 is invoked.
    assert(prn != nullptr);
    assert(prbpfilter != nullptr);

    unsigned char C0 = 0, C1 = 0, C2 = 0, C3 = 0, C4 = 0;

    // Outer loop: 2 USB frames per UDP datagram (networkproto1.c:878)
    for (int txframe = 0; txframe < 2; ++txframe) {
        char* txbptr = txbptr_base + 512 * txframe;

        // Sync bytes (networkproto1.c:881-883)
        txbptr[0] = 0x7f;
        txbptr[1] = 0x7f;
        txbptr[2] = 0x7f;

        // MOX-edge jump (networkproto1.c:886-891).  On HL2 nddc=4
        // the `if (nddc == 2)` is FALSE so out_control_idx_ stays
        // put — but previous_tx_bit_ still updates.  Preserved
        // verbatim including the no-op-on-HL2 behavior so ANAN
        // tester hardware (nddc=2 Hermes II) gets the same logic
        // when its branch lands.
        if (XmitBit != previous_tx_bit_) {
            if (nddc == 2)
                out_control_idx_ = 2;
            previous_tx_bit_ = XmitBit;
        }

        // C0 base init — MOX bit goes to C0 bit 0
        // (networkproto1.c:896).  Per-case `C0 |= (addr << 1)`
        // OR's the address bits ABOVE bit 0.
        C0 = static_cast<unsigned char>(XmitBit);
        C1 = 0;
        C2 = 0;
        C3 = 0;
        C4 = 0;

        // I2C-transaction overlay (HL2-only).  Lines 898-943.
        // First leg: decrement delay if non-zero.
        if (0 != prn->i2c.delay) {
            prn->i2c.delay--;
        }

        // Second leg: pre-decrement delay AND check the queue.
        // Preserved verbatim — the double decrement is the
        // reference's countdown pattern.  When the queue has data
        // AND delay is at-or-below-zero, fire an I2C transaction
        // and reset delay to 5.
        if ((0 >= --prn->i2c.delay) &&
            (prn->i2c.in_index != prn->i2c.out_index)) {
            prn->i2c.delay = 5;

            // I2C queue ring-buffer next-index walk (HL2 has
            // MAX_I2C_QUEUE = 32 slots; kMaxI2cQueue mirrors).
            unsigned char next = prn->i2c.out_index + 1 >= kMaxI2cQueue
                                     ? 0
                                     : prn->i2c.out_index + 1;

            // C0 — I2C addr 0x3c (bus 0) or 0x3d (bus 1) shifted
            // into bits[7:1], plus ctrl_request → bit 7 (which is
            // the "stop" bit on the wire).
            if (0 == prn->i2c.i2c_queue[next].bus) {
                C0 |= static_cast<unsigned char>((0x3c << 1)
                          | (prn->i2c.ctrl_request << 7));
            } else {
                C0 |= static_cast<unsigned char>((0x3d << 1)
                          | (prn->i2c.ctrl_request << 7));
            }

            // C2 — I2C target address.  If MSB set (7-bit notation
            // shifted), normalize by right-shift; then OR 0x80
            // (Stop request).
            unsigned char address = prn->i2c.i2c_queue[next].address;
            if (0x7f < address) {
                address = address >> 1;
            }
            C2 = static_cast<unsigned char>(0x80 | address);

            // C1 — read=0x07 / write=0x06 sub-command
            if (prn->i2c.ctrl_read) {
                C1 = 0x07;
            } else {
                C1 = 0x06;
            }

            // C3 / C4 — payload control byte + write data
            C3 = prn->i2c.i2c_queue[next].control;
            C4 = prn->i2c.i2c_queue[next].write_data;

            // Advance queue out-index
            prn->i2c.out_index = next;

            // NB: I2C-overlay frames do NOT advance out_control_idx_.
            // The else-branch below does that on non-I2C frames.
        } else {
            // Normal switch dispatch (networkproto1.c:946-1178).
            // All 19 cases compile-time present per Q2 eager.
            // §4a-scope: cases 0 / 2 / 3 implemented.  Cases 1
            // (TX VFO) and 4-18 are `assert(false)` placeholders
            // until §4b / §4c populate.
            switch (out_control_idx_) {
                case 0:
                    compose_case_0(C0, C1, C2, C3, C4);
                    break;

                case 1:  // §4b-1.1 — TX VFO frame 0x02
                    compose_case_1(C0, C1, C2, C3, C4);
                    break;

                case 2:
                    compose_case_2(C0, C1, C2, C3, C4);
                    break;

                case 3:
                    compose_case_3(C0, C1, C2, C3, C4);
                    break;

                case 4:  // §4b-1.2 — ADC assignments + TX ATT narrow
                    compose_case_4(C0, C1, C2, C3, C4);
                    break;

                case 5: case 6: case 7: case 8: case 9:
                    // §4c — RX-mirror / ANAN-only cases.  HL2
                    // uses cases 5/6 as TX-mirror DDC writes; 7-9
                    // are ANAN-class extra DDCs.
                    assert(false && "cases 5-9 not yet implemented "
                                    "— see §4c");
                    break;

                case 10:  // §4b-2.1 — drive level + Apollo + mic + HPF/LPF + PA
                    compose_case_10(C0, C1, C2, C3, C4);
                    break;

                case 11:  // §4b-2.2 — preamps + mic + MOX-gated step ATT
                    compose_case_11(C0, C1, C2, C3, C4);
                    break;

                case 12:  // §4b-2.3 — adc[1]/adc[2] step ATT + CW keyer
                    compose_case_12(C0, C1, C2, C3, C4);
                    break;

                case 13:  // §4b-1.3 — CW enable + sidetone + rf_delay
                    compose_case_13(C0, C1, C2, C3, C4);
                    break;

                case 14:  // §4b-1.4 — CW hang_delay + sidetone_freq
                    compose_case_14(C0, C1, C2, C3, C4);
                    break;

                case 15:  // §4b-1.5 — EER PWM min/max
                    compose_case_15(C0, C1, C2, C3, C4);
                    break;

                case 16:  // §4b-2.4 — BPF2 (Alex1) + xvtr_enable + puresignal
                    compose_case_16(C0, C1, C2, C3, C4);
                    break;

                case 17:  // §4b-1.6 — HL2 TX-latency + PTT-hang
                    compose_case_17(C0, C1, C2, C3, C4);
                    break;

                case 18:  // §4b-1.7 — reset_on_disconnect
                    compose_case_18(C0, C1, C2, C3, C4);
                    break;
            }

            // Cursor advance — networkproto1.c:1180-1183.  Stays
            // INSIDE the else-branch so I2C-overlay frames do NOT
            // advance the round-robin.
            if (out_control_idx_ < 18)
                out_control_idx_++;
            else
                out_control_idx_ = 0;
        }

        // Post-switch packet packing — networkproto1.c:1186-1190.
        txbptr[3] = static_cast<char>(C0);
        txbptr[4] = static_cast<char>(C1);
        txbptr[5] = static_cast<char>(C2);
        txbptr[6] = static_cast<char>(C3);
        txbptr[7] = static_cast<char>(C4);
    }

    // The LRIQ memcpy + MetisWriteFrame + ReleaseSemaphore calls
    // at networkproto1.c:1194-1200 are EP2-thread concerns — they
    // land in `Ep2SendThread` (§7) per the §10.2 component split.
}

}  // namespace lyra::wire

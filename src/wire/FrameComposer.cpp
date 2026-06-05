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

                case 1:
                    // TX VFO 0x02 — §4b scope.  This case CANNOT
                    // fire during §4a wire-inert operation because
                    // write_main_loop_hl2 is not called yet; the
                    // assert is a tripwire if anyone wires it in
                    // before §4b lands.
                    assert(false && "case 1 (TX VFO frame 0x02) "
                                    "not yet implemented — see §4b");
                    break;

                case 2:
                    compose_case_2(C0, C1, C2, C3, C4);
                    break;

                case 3:
                    compose_case_3(C0, C1, C2, C3, C4);
                    break;

                case 4: case 5: case 6: case 7: case 8: case 9:
                case 10: case 11: case 12: case 13: case 14:
                case 15: case 16: case 17: case 18:
                    // §4b (TX att / drive / PA / mic / LNA / HL2
                    // TX-latency / reset_on_disconnect) + §4c (PS
                    // / wideband / ANAN-only RX freqs).
                    assert(false && "case 4-18 not yet implemented "
                                    "— see §4b / §4c");
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

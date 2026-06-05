// Lyra — Alex filter-board control words.
//
// Mirrors the reference's `_rbpfilter` and `_rbpfilter2` structs
// (ChannelMaster/network.h:293-389).  Two 32-bit packed bitfields
// driving the Alex filter-board signals: BPF / LPF selection,
// T/R relay, antenna mux, RX1/RX2 routing, attenuators, preamp,
// front-panel LEDs.  Same byte boundaries, three position
// differences between Alex0 and Alex1 per §2.3 of the parity
// checkpoint.
//
// Per Phase 2 step 13 sign-off (§2 of docs/architecture/
// PARITY_CHECKPOINTS.md, signed 2026-06-05):
//   - PascalCase typedef names (`RbpFilter`/`RbpFilter2`)
//   - Pointer-name globals (`prbpfilter`/`prbpfilter2`) verbatim
//     from the reference for cross-reference grep parity
//   - Plain-C copy semantics matching the reference (no Lyra-
//     native `= delete` safety additions)
//   - `enable` initialized to the Alex0/Alex1 id per
//     `netInterface.c:1729`/`:1733`
//
// Setter implementations land later with the UI/protocol-config
// plumbing (the `SetTRRelay`, `SetAtten`, `SetAlexAntennas`,
// `SetAlexHPF`, `SetAlex2HPF`, `SetRX2GroundOnTX`, `SetAlexLPF`
// surfaces enumerated in §2.4).  Readers (the bit-emit logic in
// case-0 / case-10 / case-16 of `FrameComposer`) land with
// their own checkpoint.
//
// WIRE-INERT: built but not wired into HL2Stream until §10.3
// step 14.
//
// Reference: ChannelMaster/network.h:293-389.

#pragma once

namespace lyra::wire {

// Alex0 filter-board control word (network.h:293-340 / _rbpfilter).
//
// Bit map (LSB to MSB):
//   Byte 0 [bits  0- 7]: rx_yellow_led + HPFs + 6M preamp
//   Byte 1 [bits  8-15]: RX antenna routing + bypass + attenuators
//                        + rx_red_led
//   Byte 2 [bits 16-23]: trx_status + tx_yellow_led + LPFs
//   Byte 3 [bits 24-31]: shared RX/TX antenna mux + T/R relay +
//                        tx_red_led + more LPFs
#pragma pack(push, 1)
#pragma warning(push)
#pragma warning(disable: 4201)  // nonstandard: nameless struct/union
                                // — Q4 byte-for-byte parity with
                                //   the reference's _rbpfilter
                                //   layout (network.h:297-337).
class RbpFilter {
public:
    RbpFilter();
    ~RbpFilter();

    // Alex0 id (set to 1 per netInterface.c:1729).  Forms the
    // wire-side enable mask alongside RbpFilter2::enable
    // (network.c:893 ORs the two enables into packetbuf[59]).
    int enable{1};

    union {
        unsigned bpfilter{0};
        struct {
            // Byte 0 (bits 0-7).
            unsigned char _rx_yellow_led : 1;  // bit 00
            unsigned char _13MHz_HPF     : 1;  // bit 01
            unsigned char _20MHz_HPF     : 1;  // bit 02
            unsigned char _6M_preamp     : 1;  // bit 03
            unsigned char _9_5MHz_HPF    : 1;  // bit 04
            unsigned char _6_5MHz_HPF    : 1;  // bit 05
            unsigned char _1_5MHz_HPF    : 1;  // bit 06
            unsigned char                : 1;  // bit 07 unused

            // Byte 1 (bits 8-15).
            unsigned char _XVTR_Rx_In    : 1;  // bit 08
            unsigned char _Rx_2_In       : 1;  // bit 09  EXT1
            unsigned char _Rx_1_In       : 1;  // bit 10  EXT2
            unsigned char _Rx_1_Out      : 1;  // bit 11  K36 RL17
            unsigned char _Bypass        : 1;  // bit 12
            unsigned char _20_dB_Atten   : 1;  // bit 13
            unsigned char _10_dB_Atten   : 1;  // bit 14
                                               //         (RX MASTER IN SEL RL22)
            unsigned char _rx_red_led    : 1;  // bit 15

            // Byte 2 (bits 16-23).
            unsigned char                : 1;  // bit 16 unused
            unsigned char                : 1;  // bit 17 unused
            unsigned char _trx_status    : 1;  // bit 18
            unsigned char _tx_yellow_led : 1;  // bit 19
            unsigned char _30_20_LPF     : 1;  // bit 20
            unsigned char _60_40_LPF     : 1;  // bit 21
            unsigned char _80_LPF        : 1;  // bit 22
            unsigned char _160_LPF       : 1;  // bit 23

            // Byte 3 (bits 24-31).
            unsigned char _ANT_1         : 1;  // bit 24
            unsigned char _ANT_2         : 1;  // bit 25
            unsigned char _ANT_3         : 1;  // bit 26
            unsigned char _TR_Relay      : 1;  // bit 27
            unsigned char _tx_red_led    : 1;  // bit 28
            unsigned char _6_LPF         : 1;  // bit 29
            unsigned char _12_10_LPF     : 1;  // bit 30
            unsigned char _17_15_LPF     : 1;  // bit 31
        };
    };
};
#pragma warning(pop)
#pragma pack(pop)

// Alex1 filter-board control word (network.h:342-389 / _rbpfilter2).
//
// Identical byte boundaries to RbpFilter.  Bits 0-7 + 16-23
// (rx_yellow_led + HPFs + 6M_preamp + trx_status + tx_yellow_led
// + LPFs) and bits 27-31 (TR_Relay + tx_red_led + more LPFs)
// MATCH RbpFilter — bits 16-23 + 27-31 are kept in sync from
// Alex0 via the setter copy at netInterface.c:489-490 and
// :695-696.  Three positions diverge from Alex0:
//   - bit  8: `_rx2_gnd`        (Alex1) vs `_XVTR_Rx_In` (Alex0)
//   - bits 9-11, 13-14: reserved (Alex1) vs RX-ant routing +
//     attenuators (Alex0)
//   - bits 24-26: `_TXANT_1..3` (Alex1, independent TX antenna
//                  mux) vs `_ANT_1..3` (Alex0, RX/TX-shared)
#pragma pack(push, 1)
#pragma warning(push)
#pragma warning(disable: 4201)
class RbpFilter2 {
public:
    RbpFilter2();
    ~RbpFilter2();

    // Alex1 id (set to 2 per netInterface.c:1733).
    int enable{2};

    union {
        unsigned bpfilter{0};
        struct {
            // Byte 0 (bits 0-7) — IDENTICAL to RbpFilter byte 0.
            unsigned char _rx_yellow_led : 1;  // bit 00
            unsigned char _13MHz_HPF     : 1;  // bit 01
            unsigned char _20MHz_HPF     : 1;  // bit 02
            unsigned char _6M_preamp     : 1;  // bit 03
            unsigned char _9_5MHz_HPF    : 1;  // bit 04
            unsigned char _6_5MHz_HPF    : 1;  // bit 05
            unsigned char _1_5MHz_HPF    : 1;  // bit 06
            unsigned char                : 1;  // bit 07 unused

            // Byte 1 (bits 8-15) — DIVERGES from RbpFilter.
            unsigned char _rx2_gnd       : 1;  // bit 08
            unsigned char                : 1;  // bit 09 unused
            unsigned char                : 1;  // bit 10 unused
            unsigned char                : 1;  // bit 11 unused
            unsigned char _Bypass        : 1;  // bit 12
            unsigned char                : 1;  // bit 13 unused
            unsigned char                : 1;  // bit 14 unused
            unsigned char _rx_red_led    : 1;  // bit 15

            // Byte 2 (bits 16-23) — IDENTICAL to RbpFilter byte 2.
            unsigned char                : 1;  // bit 16 unused
            unsigned char                : 1;  // bit 17 unused
            unsigned char _trx_status    : 1;  // bit 18
            unsigned char _tx_yellow_led : 1;  // bit 19
            unsigned char _30_20_LPF     : 1;  // bit 20
            unsigned char _60_40_LPF     : 1;  // bit 21
            unsigned char _80_LPF        : 1;  // bit 22
            unsigned char _160_LPF       : 1;  // bit 23

            // Byte 3 (bits 24-31) — DIVERGES from RbpFilter on
            // bits 24-26 (TX-only antenna mux, independent from
            // Alex0's RX/TX-shared mux).
            unsigned char _TXANT_1       : 1;  // bit 24
            unsigned char _TXANT_2       : 1;  // bit 25
            unsigned char _TXANT_3       : 1;  // bit 26
            unsigned char _TR_Relay      : 1;  // bit 27
            unsigned char _tx_red_led    : 1;  // bit 28
            unsigned char _6_LPF         : 1;  // bit 29
            unsigned char _12_10_LPF     : 1;  // bit 30
            unsigned char _17_15_LPF     : 1;  // bit 31
        };
    };
};
#pragma warning(pop)
#pragma pack(pop)

// Global instance pointers (network.h:340 + :389).
//
// Names mirror the reference VERBATIM per the operator naming
// directive (2026-06-05) — cross-reference grep parity with
// the reference source.  Assigned at HL2 session start by the
// wire-layer initializer (Phase 2 wire-up); stay nullptr until
// then.
extern RbpFilter*  prbpfilter;
extern RbpFilter2* prbpfilter2;

}  // namespace lyra::wire

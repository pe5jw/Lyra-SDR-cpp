# Parity Checkpoints

Per Rule 23 (locked 2026-06-05).  Each entry is a written side-by-side
comparison between the reference and Lyra-cpp, produced BEFORE the code
for that component lands.  Operator reviews row-by-row; sign-off
authorizes the commit.

**Format per row:** *Aspect / Reference (file:line) / Lyra (proposed or
shipped)*.  Verdict bar below each table, one of:

- ✅ **PARITY** — byte-for-byte / behavior-for-behavior match
- ⚠ **ACCEPTABLE DEVIATION** — different form, identical effect (C → C++23
  idiom translations).  Reason stated.
- 🔴 **OPERATOR-APPROVED DEVIATION** — substantive behavior change.
  Requires explicit operator sign-off in the row before code lands.

**Reference root:** operator-local path, set per machine.  All
file:line citations below are relative to the reference source
tree (e.g. `network.h:56-291`, `networkproto1.c:614-1267`).

---

## §1. `RadioNet` — radio-network state container

**Scope.** The C++ class `lyra::wire::RadioNet` (`src/wire/RadioNet.{h,cpp}`)
mirrors the reference's `_radionet` struct (typedef `radionet`, pointer
`RADIONET`, instance `prn`) at `ChannelMaster/network.h:56-291`.

This is the master state container for a host↔radio session.  Per the
locked operator decisions on the five scope questions (2026-06-05):

| # | Question | Locked |
|---|---|---|
| 1 | Mix WRITE + READ state in one struct? | Yes (mirror `_radionet`) |
| 2 | Family-parameterized vs HL2-only? | Family (ANAN-ready, mirror `HPSDRModel`) |
| 3 | Array sizes for `adc[]` / `rx[]` / `tx[]`? | Reference maxima (3 / 12 / 3) |
| 4 | Bit-packed control bytes? | C-style union-of-bitfield-struct + `#pragma pack(1)` |
| 5 | Synchronization primitives? | Six `std::mutex` embedded (Option A, reference-faithful) |

The §1 entry below tests EVERY field of `_radionet` against the locked
answers and against the §10.2 component split signed 2026-06-04.

### §1.1 Excluded networking infrastructure (acknowledged §10.2 split)

`_radionet` co-locates state with networking machinery (sockets, thread
handles, event handles, buffer pointers, sequence counters).  Per
`docs/TX_ARCHITECTURAL_MAPPING.md §10.2`, the networking machinery
lives in dedicated wire-layer components (`Ep6RecvThread`,
`Ep2SendThread`, `OutboundRing`), NOT in `RadioNet`.  `RadioNet`
carries the **state portion only**.

| Aspect | Reference (file:line) | Lyra |
|---|---|---|
| RX/TX buffer pointers | `network.h:60-66` `double** RxBuff; double* RxReadBufp; double* TxReadBufp; unsigned char* ReadBufp; char* OutBufp; double* outLRbufp; double* outIQbufp;` | Lives in `wire/Ep6RecvThread` (RxBuff/ReadBufp), `wire/Ep2SendThread` (TxReadBufp/OutBufp), `wire/OutboundRing` (outLRbufp/outIQbufp) |
| Thread / semaphore handles | `network.h:88-97` `HANDLE hReadThreadMain, hReadThreadInitSem, hWriteThreadMain, hWriteThreadInitSem, hsendLRSem, hsendIQSem, hsendEventHandles[2], hobbuffsRun[2], hKeepAliveThread, hTimer;` | Lives in respective threads (`Ep6RecvThread`, `Ep2SendThread`) — each owns its own `std::thread` + condition variables; HL2 P1 has no separate keep-alive thread (`KeepAliveThread` is dead code in the reference HL2 path) |
| Win32 waitable-timer | `network.h:98` `LARGE_INTEGER liDueTime;` | Lives in `wire/Ep2SendThread` (MMCSS Pro Audio @2 + drift-corrected high-resolution waitable timer per §3 design) |
| WSA event objects | `network.h:105-106` `WSAEVENT hDataEvent; WSANETWORKEVENTS wsaProcessEvents;` | Lives in `wire/Ep6RecvThread` (Lyra uses `recvfrom` with timeout, NOT WSAEventSelect — Lyra-native socket pattern; equivalent semantics) |
| Sequence counters (RX-side, IN `_radionet`) | `network.h:86-87` `unsigned int cc_seq_no, cc_seq_err;` | Lives in `wire/Ep6RecvThread` (per-direction seq tracking is a wire-layer concern, not radio state) |
| Sequence counter (TX-side, NOT in `_radionet`) | `networkproto1.c:30` `unsigned int MetisOutBoundSeqNum;` — separate file-scope global, NOT inside the `_radionet` struct | Lives in `wire/MetisFrame.cpp` TU-scope `g_metis_out_seq_num` per §6-B (sign-off 2026-06-06) — direct mirror of the reference's file-scope global, callable from both `Ep2SendThread::run_loop` AND `ForceCandC::prime` exactly as the reference's `MetisWriteFrame` is callable from `sendProtocol1Samples` + `ForceCandCFrames`.  §1.1 was originally drafted only for `_radionet`-resident networking infrastructure; the TX-side seq counter is NOT in `_radionet` and was incorrectly extended into §1.1's scope at §6 Q3 sign-off — corrected here. |
| Networking config ports | `network.h:58-59` `int p2_custom_port_base; int base_outbound_port;` | Lives in `wire/Ep6RecvThread` / `wire/Ep2SendThread` constructors (passed in from HL2 discovery, not session state) |

**VERDICT (ORIGINAL — superseded 2026-06-06):** 🔴
**OPERATOR-APPROVED DEVIATION** — these fields are PRESENT in
`_radionet` but EXCLUDED from `RadioNet` per the §10.2
component split signed 2026-06-04.  The §10.2 mapping doc
names the exact destination wire-layer component for each.
No state-bearing field is dropped — every field re-homes to a
wire-layer component that owns it.

**VERDICT (SUPERSEDED — §1-C Stage 4 sign-off 2026-06-06):**
✅ **PARITY** — §1-C reverted the §1.1 exclusion under
operator directive "Fix everything to reference — §1.1
included.  No patching."  All `_radionet` fields now live in
`RadioNet` as members exactly per the reference.  Wire-layer
components read from `prn->...`.  See the §1-C entry at the
end of this document for the full sweep + Stage 4E amendment
table.  The original 2026-06-04 sign-off cell above is
preserved as historical audit artifact per the Rule 2
amendment.

---

### §1.2 Top-level state scalars

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| `run` | `network.h:68` `int run;` | `int run;` |
| `wdt` | `network.h:69` `int wdt;` (watchdog tick) | `int wdt;` |
| `sendHighPriority` | `network.h:70` `int sendHighPriority;` | `int sendHighPriority;` |
| `num_adc` / `num_dac` | `network.h:71-72` `int num_adc; int num_dac;` | `int num_adc; int num_dac;` |
| `ptt_in` | `network.h:73` `int ptt_in;` (EP6 telemetry — HW PTT) | `int ptt_in;` |
| `dot_in` | `network.h:74` `int dot_in;` (EP6 telemetry — CW key dot) | `int dot_in;` |
| `dash_in` | `network.h:75` `int dash_in;` (EP6 telemetry — CW key dash) | `int dash_in;` |
| `pll_locked` | `network.h:76` `int pll_locked;` | `int pll_locked;` |
| `oc_output` | `network.h:77` `int oc_output;` (OC pins → C2 of case-0 frame) | `int oc_output;` |
| `oc_output_extras` | `network.h:78` `int oc_output_extras;` | `int oc_output_extras;` |
| `supply_volts` | `network.h:79` `int supply_volts;` (EP6 telemetry — case 0x18 C3:C4) | `int supply_volts;` |
| `user_adc0..3` | `network.h:80-83` `int user_adc0; int user_adc1; int user_adc2; int user_adc3;` (EP6 telemetry — AIN3/4/5/6) | `int user_adc0; int user_adc1; int user_adc2; int user_adc3;` |
| `user_dig_in` | `network.h:84` `int user_dig_in;` (EP6 telemetry — case 0x00 C1[4:1]) | `int user_dig_in;` |
| `user_dig_out` | `network.h:85` `int user_dig_out;` (case-11 C3 low nibble) | `int user_dig_out;` |
| `hardware_LEDs` | `network.h:108` `int hardware_LEDs;` | `int hardware_LEDs;` |
| `reset_on_disconnect` | `network.h:109` `int reset_on_disconnect;` (case-18 C4) | `int reset_on_disconnect;` |
| `swap_audio_channels` | `network.h:110` `int swap_audio_channels;` | `int swap_audio_channels;` |
| `puresignal_run` | `network.h:151` `int puresignal_run;` (case-11 C2 bit 6 + case-16 C2 bit 6) | `int puresignal_run;` |
| `lr_audio_swap` | `network.h:163` `int lr_audio_swap;` (firmware-bug workaround) | `int lr_audio_swap;` |
| `CATPort` | `network.h:166` `int CATPort;` | `int CATPort;` |

**VERDICT:** ✅ **PARITY** — each field declared with the same type
(`int`) and name as the reference.  Naming preserved verbatim (no
PascalCase rewrite on scalars — keeps grep against the reference
working when debugging).

---

### §1.3 Wideband scalars (HL2-irrelevant; included for ANAN-ready parity)

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| `wb_base_dispid` | `network.h:155` `int wb_base_dispid;` | `int wb_base_dispid;` |
| `wb_samples_per_packet` | `network.h:156` `int wb_samples_per_packet;` | `int wb_samples_per_packet;` |
| `wb_sample_size` | `network.h:157` `int wb_sample_size;` | `int wb_sample_size;` |
| `wb_update_rate` | `network.h:158` `int wb_update_rate;` | `int wb_update_rate;` |
| `wb_packets_per_frame` | `network.h:159` `int wb_packets_per_frame;` | `int wb_packets_per_frame;` |
| `wb_enable` | `network.h:160` `volatile long wb_enable;` | `std::atomic<long> wb_enable;` (⚠ — `volatile long` in C is the wrong way to spell "atomic" in C++23; `std::atomic` is the idiom-correct equivalent on the same surface) |

**VERDICT:** ⚠ **ACCEPTABLE DEVIATION** on `wb_enable` only
(C `volatile long` → C++23 `std::atomic<long>`, same semantics
correctly expressed); ✅ **PARITY** on the remaining five.  These
fields are inert on HL2 P1 but present for ANAN/P2 wideband
support (Q2 ANAN-ready lock).

---

### §1.4 Sub-struct `_i2c` — HL2 I²C queue

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Sub-struct nesting | `network.h:113-148` `struct _i2c { ... } i2c;` | `struct I2cState { ... } i2c;` (PascalCase the inner type name; field stays `i2c`) |
| Control byte union | `network.h:115-130` `#pragma pack(push,1) union { unsigned char i2c_control; struct { unsigned char ctrl_read:1, ctrl_stop:1, ctrl_request:1, ctrl_error:1, ctrl_read_available:1, :1, :1, :1; }; }; #pragma pack(pop)` | Same union + bitfield + `#pragma pack(1)` verbatim, byte-for-byte (Q4 lock) |
| Queue array | `network.h:132-139` `#pragma pack(push,1) struct { unsigned char bus; unsigned char address; unsigned char control; unsigned char write_data; } i2c_queue[MAX_I2C_QUEUE=32]; #pragma pack(pop)` | Same packed struct + same array size (`MAX_I2C_QUEUE = 32`) |
| Queue indices | `network.h:141-143` `unsigned char in_index; unsigned char out_index; signed char delay;` | Same three fields, same types |
| Readback state | `network.h:145-146` `unsigned char returned_address; unsigned char read_data[4];` | Same |

**VERDICT:** ✅ **PARITY** for the union/bitfield/queue layout; ⚠
**ACCEPTABLE DEVIATION** only on the inner type name (`_i2c` →
`I2cState`) per Lyra-native PascalCase convention for nested type
names.  Field name and instance name (`i2c`) preserved verbatim.

---

### §1.5 Sub-struct `_cw` — CW configuration

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Sub-struct | `network.h:185-210` `struct _cw { ... } cw;` | `struct CwConfig { ... } cw;` |
| Scalars | `network.h:187-193` `int sidetone_level; int sidetone_freq; int keyer_speed; int keyer_weight; int hang_delay; int rf_delay; int edge_length;` | Same 7 fields, same types |
| Mode-control union | `network.h:194-209` `#pragma pack(push,1) union { unsigned char mode_control; struct { unsigned char eer:1, cw_enable:1, rev_paddle:1, iambic:1, sidetone:1, mode_b:1, strict_spacing:1, break_in:1; }; }; #pragma pack(pop)` | Same union + bitfields + `#pragma pack(1)` verbatim (Q4) |

**VERDICT:** ✅ **PARITY** on layout; ⚠ **ACCEPTABLE DEVIATION** on
inner type name only.

---

### §1.6 Sub-struct `_mic` — Mic configuration

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Sub-struct | `network.h:212-232` `struct _mic { ... } mic;` | `struct MicConfig { ... } mic;` |
| Line-in gain | `network.h:214` `int line_in_gain;` | `int line_in_gain;` |
| Mic-control union | `network.h:215-230` `#pragma pack(push,1) union { unsigned char mic_control; struct { unsigned char line_in:1, mic_boost:1, mic_ptt:1, mic_trs:1, mic_bias:1, mic_xlr:1, :1, :1; }; }; #pragma pack(pop)` | Same union + bitfields + `#pragma pack(1)` verbatim |
| Samples-per-packet | `network.h:231` `int spp;` (I-samples per network packet) | `int spp;` |

**VERDICT:** ✅ **PARITY** on layout; ⚠ **ACCEPTABLE DEVIATION** on
inner type name only.

---

### §1.7 Sub-struct array `_adc adc[MAX_ADC=3]`

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Sub-struct + array | `network.h:168-183` `struct _adc { ... } adc[MAX_ADC];` with `MAX_ADC=3` (`network.h:33`) | `struct AdcState { ... } adc[MAX_ADC];` with `static constexpr int MAX_ADC = 3;` |
| `id` / `rx_step_attn` / `tx_step_attn` | `network.h:170-172` `int id; int rx_step_attn; int tx_step_attn;` | Same |
| ADC overload state | `network.h:173-174` `int previous_adc_overload; int adc_overload;` | Same |
| Dither / random | `network.h:175-176` `int dither; int random;` | Same |
| Wideband per-ADC | `network.h:178-182` `int wb_seqnum; int wb_state; double* wb_buff; uint16_t max_magnitude; uint16_t max_magnitude_at_overload;` | Same; `wb_buff` (a `double*`) is a buffer pointer — see §1.1 deviation but kept INSIDE `RadioNet` because it's per-ADC state, not a wire-layer producer/consumer ring (allocated by the radio layer at session start, lifecycle bound to the struct) |

**VERDICT:** ✅ **PARITY** on layout, ⚠ **ACCEPTABLE DEVIATION** on
inner type name.  Array size **`MAX_ADC=3`** preserved per Q3 lock
(HL2 uses adc[0] only; adc[1..2] inert but allocated for ANAN).

---

### §1.8 Sub-struct array `_rx rx[MAX_RX_STREAMS=12]`

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Sub-struct + array | `network.h:234-256` `struct _rx { ... } rx[MAX_RX_STREAMS];` with `MAX_RX_STREAMS=12` (`network.h:34`) | `struct RxState { ... } rx[MAX_RX_STREAMS];` |
| Per-RX scalars | `network.h:236-243` `int id; int rx_adc; int frequency; int enable; int sync; int sampling_rate; int bit_depth; int preamp;` | Same 8 fields |
| Sequence counters | `network.h:244-246` `unsigned rx_in_seq_no; unsigned rx_in_seq_err; unsigned rx_out_seq_no;` | Same 3 — these are per-stream telemetry counters tracked alongside the stream config; kept INSIDE `RadioNet` (the §1.1 exclusion is for global socket-level counters; per-stream counters are stream state) |
| Time + bits | `network.h:247-248` `time_t time_stamp; unsigned bits_per_sample;` | Same; `time_t` → `std::time_t` |
| Samples-per-packet | `network.h:249` `int spp;` | Same |
| Seq-error ring buffer + linked-list snapshots | `network.h:250-255` `int rx_in_seq_delta[MAX_IN_SEQ_LOG]; int rx_in_seq_delta_index; _seqLogSnapshot_t* snapshots_head; _seqLogSnapshot_t* snapshots_tail; int snapshot_length; _seqLogSnapshot_t* snapshot;` with `MAX_IN_SEQ_LOG=40` (`network.h:43`) | Same — port the `_seqLogSnapshot` linked list (`network.h:46-54`) as `struct SeqLogSnapshot { ... }` with PascalCase; otherwise verbatim |

**VERDICT:** ✅ **PARITY** on layout + array size + seq-log linked
list; ⚠ **ACCEPTABLE DEVIATION** on inner type names (`_rx`
→ `RxState`, `_seqLogSnapshot_t` → `SeqLogSnapshot`).  Array size
**`MAX_RX_STREAMS=12`** preserved per Q3 lock (HL2 uses
rx[0..3]; rx[4..11] inert).

---

### §1.9 Sub-struct array `_tx tx[MAX_TX_STREAMS=3]`

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Sub-struct + array | `network.h:258-282` `struct _tx { ... } tx[MAX_TX_STREAMS];` with `MAX_TX_STREAMS=3` (`network.h:35`) | `struct TxState { ... } tx[MAX_TX_STREAMS];` |
| Frequency + rate | `network.h:260-262` `int id; int frequency; int sampling_rate;` | Same |
| CW state bits | `network.h:263-266` `int cwx; int cwx_ptt; int dash; int dot;` (`cwx_ptt` is the MI0BOT HL2 enhancement) | Same |
| PTT + power | `network.h:267-269` `int ptt_out; int drive_level; int exciter_power;` | Same |
| Fwd / rev / phase | `network.h:270-272` `int fwd_power; int rev_power; int phase_shift;` | Same |
| EPWM + PA | `network.h:273-275` `int epwm_max; int epwm_min; int pa;` | Same |
| TX latency + PTT hang | `network.h:276-277` `int tx_latency; int ptt_hang;` (MI0BOT HL2 enhancements; case-17 register) | Same |
| Sequence counters | `network.h:278-280` `unsigned mic_in_seq_no; unsigned mic_in_seq_err; unsigned mic_out_seq_no;` | Same — per-stream counters kept INSIDE per §1.8 same reasoning |
| Samples-per-packet | `network.h:281` `int spp;` | Same |

**VERDICT:** ✅ **PARITY** on layout + every field; ⚠ **ACCEPTABLE
DEVIATION** on inner type name only.  Array size
**`MAX_TX_STREAMS=3`** preserved per Q3 lock (HL2 uses tx[0]
only; tx[1..2] inert).

---

### §1.10 Sub-struct array `_audio audio[MAX_AUDIO_STREAMS=2]`

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Sub-struct + array | `network.h:284-287` `struct _audio { int spp; } audio[MAX_AUDIO_STREAMS];` with `MAX_AUDIO_STREAMS=2` (`network.h:36`) | `struct AudioConfig { int spp; } audio[MAX_AUDIO_STREAMS];` |

**VERDICT:** ✅ **PARITY** on layout; ⚠ **ACCEPTABLE DEVIATION** on
inner type name only.

---

### §1.11 Embedded synchronization primitives (Q5 — Option A)

Per the locked Q5 = Option A: six `std::mutex` members embedded in
`RadioNet`, mirroring the reference's six `CRITICAL_SECTION`s.  Live
usage source-verified yesterday (`udpOUT` `network.c:1248,1283`;
`rcvpkt` `network.c:478,490,626`; `sndpkt` `network.c:1387,1392`;
`seqErrors` 6 sites; `rcvpktp1` `networkproto1.c:153,166,196,212`).
`sendOUT` is declared + init'd + destroyed but never locked in the
reference tree — included here for byte-for-byte parity per Rule 1.

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| `udpOUT` (UDP `sendto()` serializer) | `network.h:99` `CRITICAL_SECTION udpOUT;`; init `netInterface.c:1742`; used `network.c:1248,1283` | `std::mutex udpOUT;` |
| `sendOUT` (declared dead in reference) | `network.h:100` `CRITICAL_SECTION sendOUT;`; init `netInterface.c:1743`; **never locked** | `std::mutex sendOUT;` — included for parity per Rule 1; comment notes the reference declares-but-doesn't-use; eligible for retirement post-Phase-2 if a future audit confirms still dead |
| `rcvpkt` (P2 RX packet serializer) | `network.h:101` `CRITICAL_SECTION rcvpkt;`; init `netInterface.c:1744`; used `network.c:478,490,626` | `std::mutex rcvpkt;` |
| `sndpkt` (P2 send-side serializer) | `network.h:102` `CRITICAL_SECTION sndpkt;`; init `netInterface.c:1745`; used `network.c:1387,1392` | `std::mutex sndpkt;` |
| `seqErrors` (seq-error log mutator) | `network.h:103` `CRITICAL_SECTION seqErrors;`; init `netInterface.c:1747`; used 6 sites across `network.c` + `netInterface.c` | `std::mutex seqErrors;` |
| `rcvpktp1` (P1 RX serializer) | `network.h:104` `CRITICAL_SECTION rcvpktp1;`; init `netInterface.c:1746`; used `networkproto1.c:153,166,196,212` | `std::mutex rcvpktp1;` |

**VERDICT:** ✅ **PARITY** on count + names + roles; ⚠ **ACCEPTABLE
DEVIATION** on the type (`CRITICAL_SECTION` → `std::mutex` is the
C → C++23 idiom translation; same semantics, init in
constructor via default-init, destruction automatic via RAII —
no explicit `InitializeCriticalSection` / `DeleteCriticalSection`
needed).  Threads reach into the live global `RadioNet` instance
and lock the relevant member at the same call sites the reference
does (the §10.2 thread components — `Ep6RecvThread`,
`Ep2SendThread`, etc. — each take a pointer/reference to the
shared `RadioNet`).

**Lock-order note for future Phase 2 work:** lock-order is NOT
defined by the reference (it has no documented order); to prevent
deadlock if any thread ever takes more than one of these mutexes,
declare the order in this row at Phase-2-fill time as an
additional ⚠ ACCEPTABLE DEVIATION + comment block (no equivalent
existed in the reference; Lyra-native safety addition).

---

### §1.12 Global instance + typedef

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Type definition | `network.h:56-289` `typedef struct CACHE_ALIGN _radionet { ... } radionet, *RADIONET;` | `class RadioNet { ... };` |
| Cache alignment | `CACHE_ALIGN` = `__declspec(align(16))` (`network.h:38`) | `alignas(16) class RadioNet { ... };` (C++23 spelling, same width) |
| Global instance | `network.h:291` `RADIONET prn;` (a global pointer) | `extern RadioNet* prn;` declared in header; `RadioNet* prn = nullptr;` defined in `.cpp`; assigned at HL2 session start (Phase 2 wire-up).  Name mirrors the reference VERBATIM — critical for cross-reference grep discipline (the reference uses `prn->*` in hundreds of sites across `networkproto1.c`, `network.c`, `cmaster.c`; future PureSignal port reads `prn->*` extensively, byte-identical names enable line-by-line study). |

**VERDICT:** ✅ **PARITY** on access pattern + global instance
name (`prn` verbatim); ⚠ **ACCEPTABLE DEVIATION** on:
(a) `struct + typedef` → `class` (C++23 idiom; same memory
layout); (b) cache-align spelling (`__declspec(align(16))` →
`alignas(16)`, same width).

**Amendment 2026-06-05 (operator-tightened naming directive):**
the initial §1.12 draft proposed `g_radioNet` as a Lyra-native
global name.  Operator directive (2026-06-05): "never deviate
from reference naming unless absolutely necessary" — the
cross-reference grep concern (PureSignal/calcc/iqc will read
`prn->*` across hundreds of sites; symbol mismatch creates
permanent cognitive friction for every port).  Reverted in
the shipped `RadioNet.{h,cpp}` to `prn` matching the reference
verbatim.  Going forward: NO Lyra-native renames on
reference symbols without an explicit operator-discussed
necessity.

**§1.12 — `= delete` lines on copy/move:** the initial draft
included explicit `RadioNet(const RadioNet&) = delete;` etc.
as Lyra-native safety hardening.  Operator directive
(2026-06-05): match reference posture — no Lyra-native code
beyond what parity requires.  The `std::mutex` members (§1.11)
already make `RadioNet` non-copyable by language rules, so
removing the explicit deletes does NOT change behavior; only
removes the Lyra-native commentary lines.  Removed in the
shipped `RadioNet.h` per Rule 1 + Rule 5 strict reading
(no safety additions beyond reference parity).

---

### §1.13 Out of scope for §1 (deferred to later sections)

These appear in `network.h` adjacent to `_radionet` but are SEPARATE
structs / globals, each warranting their own §N entry:

- `_rbpfilter` + `_rbpfilter2` (`network.h:293-389`) — the 32-bit packed
  BPF/LPF/relay/LED filter-board bitfields.  Out of `_radionet`,
  sibling globals (`prbpfilter`, `prbpfilter2`).  **Deferred to §2.**
- Loose globals (`network.h:413-507`): `XmitBit`, `ControlBytesIn[5]`,
  `HaveSync`, `ADC_cntrl1`/`ADC_cntrl2`, `nreceivers`, `xvtr_enable`,
  `atu_tune`, `audioamp_enable`, `Alex*LPFMask`/`Alex*HPFMask`,
  `RevPower`/`FwdPower`, `ApolloFilt`/`ApolloFiltSelect`/`ApolloTuner`/
  `ApolloATU`, `WSAinitialized`, `listenSock`, `RemotePort`,
  `SampleRateIn2Bits`, `mic_decimation_factor`, `HPSDRModel`,
  `RadioProtocol`.  **Deferred to §3** (per `docs/TX_ARCHITECTURAL_MAPPING.md`
  §10.2 these split across `DispatchState`, `Capabilities`,
  `FrameComposer`, and family-enum scaffolding).

---

### §1 — Overall verdict

| Section | Verdict |
|---|---|
| §1.1 networking exclusion | 🔴 OPERATOR-APPROVED DEVIATION (§10.2 split) |
| §1.2 top-level scalars | ✅ PARITY |
| §1.3 wideband scalars | ✅ PARITY (one ⚠ on `wb_enable` atomic) |
| §1.4 i2c sub-struct | ✅ PARITY (⚠ inner type name) |
| §1.5 cw sub-struct | ✅ PARITY (⚠ inner type name) |
| §1.6 mic sub-struct | ✅ PARITY (⚠ inner type name) |
| §1.7 adc[3] | ✅ PARITY (⚠ inner type name) |
| §1.8 rx[12] | ✅ PARITY (⚠ inner type names) |
| §1.9 tx[3] | ✅ PARITY (⚠ inner type name) |
| §1.10 audio[2] | ✅ PARITY (⚠ inner type name) |
| §1.11 mutexes | ✅ PARITY (⚠ `CRITICAL_SECTION` → `std::mutex`) |
| §1.12 global + alignment | ✅ PARITY (`prn` verbatim, ⚠ only on `class` keyword + `alignas` spelling — C++23 idiom translations) |

**Substantive deviation requiring sign-off:** §1.1 (networking
infrastructure exclusion).  Already pre-approved by the operator's
§10.2 sign-off (2026-06-04); this checkpoint re-states + concretely
enumerates which fields move where.

**No other 🔴 items in §1.**

---

**OPERATOR SIGN-OFF:**

- [x] §1 reviewed, all rows verdicts confirmed
- [x] §1.1 networking-exclusion deviation accepted (re-confirmation
      of the 2026-06-04 §10.2 sign-off, enumerated)
- [x] §1.11 mutex lock-order deferral acknowledged (locked at
      Phase-2-fill time, not in this checkpoint)
- [x] Authorized to ship the Phase-2 `RadioNet` populated header
      matching this checkpoint

Signed: **N8SDR (Rick Langford)** Date: **2026-06-05**

---

## §2. `RbpFilter` / `RbpFilter2` — Alex filter-board control words

**Scope.** Two C++ classes `lyra::wire::RbpFilter` and
`lyra::wire::RbpFilter2` (`src/wire/RbpFilter.{h,cpp}`) mirror the
reference's `_rbpfilter` and `_rbpfilter2` struct + typedef pair at
`ChannelMaster/network.h:293-389`.  These are the Alex filter-board
control words — 32-bit packed bitfields driving BPF/LPF selection,
T/R relay, antenna mux, RX1/RX2 routing, attenuators, preamp,
front-panel LEDs.  Two distinct types because three bit positions
have different meanings between Alex0 and Alex1.

### §2.1 Architectural placement (acknowledged deviation from §1)

The reference declares these as **sibling globals** to `prn` (not
members of `radionet`).  Two separate typedefs with distinct
struct tags (`_rbpfilter`, `_rbpfilter2`) because the layouts
differ.  Lyra mirrors this:

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Sibling struct shape (not inside `radionet`) | `network.h:293-389` — two distinct typedefs, NOT members of `_radionet` | `class RbpFilter { ... };` + `class RbpFilter2 { ... };` declared in `src/wire/RbpFilter.{h,cpp}` (sibling to `RadioNet`, NOT a member of it) |
| Global instance pointers | `network.h:340` `RBPFILTER prbpfilter;` + `:389` `RBPFILTER2 prbpfilter2;` | `extern RbpFilter*  prbpfilter;` + `extern RbpFilter2* prbpfilter2;` declared in header; defined `nullptr` in `.cpp`; assigned at HL2 session start.  **Pointer names mirror the reference VERBATIM** (operator naming directive 2026-06-05 — reference symbols are preserved for grep parity with the reference source; PureSignal port reads `prbpfilter->*` extensively).  Only the type names PascalCase per §1 convention. |
| Lifecycle | `netInterface.c:1727-1733` `malloc0` + init `bpfilter=0`, `enable=1`/`enable=2`; `:1807-1808` `_aligned_free` | Constructor sets `bpfilter=0` + `enable` to the matching id (1 vs 2); destruction automatic via RAII when the wire-layer init releases ownership |
| Coupling to `RadioNet` | Read together by the frame composers (`WriteMainLoop_HL2` reads both `prn->*` and `prbpfilter->*` per case) — separate sources, fanned in at the use site | `FrameComposer` (later §) takes pointers to BOTH `RadioNet` and `RbpFilter`/`RbpFilter2` — same fan-in pattern |

**VERDICT:** ✅ **PARITY** on shape (sibling structs, NOT members
of RadioNet; pointer names `prbpfilter`/`prbpfilter2` verbatim);
⚠ **ACCEPTABLE DEVIATION** on the typedef names only (PascalCase
per the Q4 convention established in §1 — `rbpfilter` →
`RbpFilter`, `rbpfilter2` → `RbpFilter2`; reference's `_` prefix
on the struct tag dropped per C++23 idiom).

---

### §2.2 `RbpFilter` (Alex0) — 32-bit bitfield

`network.h:293-340 / _rbpfilter`.  Four byte rows (bits 0-7,
8-15, 16-23, 24-31).  Each bit drives a specific Alex0 filter-
board signal.

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Master alignment + pack | `network.h:293` `#pragma pack(push, 1)` ... `:339 #pragma pack(pop)` | Same `#pragma pack(push, 1)` / `pop` (Q4 byte-for-byte parity); `#pragma warning(disable: 4201)` for the anonymous union to keep the `/W4` build clean |
| `enable` | `network.h:296` `int enable;` (set to 1 at init per `netInterface.c:1729`) | `int enable{1};` (default-init to the Alex0 id; matches reference's init line) |
| Union: dword view + bitfield struct view | `network.h:297-337` `union { unsigned bpfilter; struct { ... 32 bits ... }; };` | Same union: `unsigned bpfilter{0};` first member (default-zeros the dword), anonymous struct second member with the 32-bit declaration verbatim |
| Byte 0 (bits 0-7) | `network.h:300-308` `_rx_yellow_led:1, _13MHz_HPF:1, _20MHz_HPF:1, _6M_preamp:1, _9_5MHz_HPF:1, _6_5MHz_HPF:1, _1_5MHz_HPF:1, :1;` (bit 7 unused) | Same 8 bitfield declarations, same order, same names (underscore-prefix preserved per Rule 1 cross-reference grep) |
| Byte 1 (bits 8-15) | `network.h:310-317` `_XVTR_Rx_In:1, _Rx_2_In:1 (EXT1), _Rx_1_In:1 (EXT2), _Rx_1_Out:1 (K36 RL17), _Bypass:1, _20_dB_Atten:1, _10_dB_Atten:1 (RX MASTER IN SEL RL22), _rx_red_led:1;` | Same 8 bitfield declarations, comments preserved verbatim (the `RL17`/`RL22` relay designators are the HL2 schematic refs — operator-facing for hardware debug) |
| Byte 2 (bits 16-23) | `network.h:319-326` `:1, :1, _trx_status:1, _tx_yellow_led:1, _30_20_LPF:1, _60_40_LPF:1, _80_LPF:1, _160_LPF:1;` | Same 8 bitfield declarations (bits 16-17 unused) |
| Byte 3 (bits 24-31) | `network.h:328-335` `_ANT_1:1, _ANT_2:1, _ANT_3:1, _TR_Relay:1, _tx_red_led:1, _6_LPF:1, _12_10_LPF:1, _17_15_LPF:1;` | Same 8 bitfield declarations |

**VERDICT:** ✅ **PARITY** on bit layout byte-for-byte; ⚠
**ACCEPTABLE DEVIATION** on the outer typedef name only
(`rbpfilter` → `RbpFilter`).  The 32 individual bitfield names
are preserved verbatim — they are the schematic reference
points and are operator-visible in hardware debug.

---

### §2.3 `RbpFilter2` (Alex1) — 32-bit bitfield, three position differences vs Alex0

`network.h:342-389 / _rbpfilter2`.  Same total bit count (32),
same byte boundaries, three positions diverge.

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Master alignment + pack | `network.h:342` `#pragma pack(push, 1)` ... `:388 #pragma pack(pop)` | Same |
| `enable` | `network.h:345` `int enable;` (set to 2 at init per `netInterface.c:1733`) | `int enable{2};` |
| Byte 0 (bits 0-7) | `network.h:350-357` — IDENTICAL to RbpFilter byte 0 (HPF + 6M preamp) | Same |
| Byte 1 (bits 8-15) DIVERGES | `network.h:359-366` `_rx2_gnd:1, :1, :1, :1, _Bypass:1, :1, :1, _rx_red_led:1;` (bit 8 = `_rx2_gnd`, NOT `_XVTR_Rx_In`; bits 9-11, 13-14 reserved) | Same 8 bitfield declarations; bit 8 declared as `_rx2_gnd` (the Alex1-specific RX2-GND-on-TX bit per `netInterface.c:659-661`) |
| Byte 2 (bits 16-23) | `network.h:368-375` — IDENTICAL to RbpFilter byte 2 (trx_status + tx_yellow_led + LPFs) | Same |
| Byte 3 (bits 24-31) DIVERGES | `network.h:377-384` `_TXANT_1:1, _TXANT_2:1, _TXANT_3:1, _TR_Relay:1, _tx_red_led:1, _6_LPF:1, _12_10_LPF:1, _17_15_LPF:1;` (bits 24-26 are `_TXANT_*` instead of `_ANT_*`) | Same 8 bitfield declarations; bits 24-26 named `_TXANT_1/2/3` (Alex1's TX-antenna mux is independent from Alex0's RX/TX-shared antenna mux) |
| Bit-inherit relationship vs RbpFilter | `netInterface.c:489-490` and `:695-696`: `prbpfilter2->bpfilter &= 0x0700ffff; prbpfilter2->bpfilter |= prbpfilter->bpfilter & 0xf8ff0000;` — bits 16-23 + 27-31 INHERIT from Alex0 (trx_status, tx_yellow_led, LPFs, TR_Relay, tx_red_led, 6_LPF, 12_10_LPF, 17_15_LPF); bits 0-15 + 24-26 are Alex1-independent | Replicated VERBATIM in the Lyra-native `SetAlexAntennas` / `SetAlexLPF` setters when those land (later §, with the UI-driven config plumbing).  The bit-inherit relationship is data-flow, not type-structure — both classes stay independent at the type level; the copy logic lives in setters. |

**VERDICT:** ✅ **PARITY** on bit layout byte-for-byte; ⚠
**ACCEPTABLE DEVIATION** on the outer typedef name only
(`rbpfilter2` → `RbpFilter2`).  Bit-inherit data-flow recorded
here for Phase-2 setter implementation; not part of the type
definition.

---

### §2.4 Use-site enumeration (informational, no code in §2)

Recorded so Phase 2 implementers know where the setters and
readers land.

**Setters (Phase 2 wiring — later §):**

| Reference symbol | File:line | Writes |
|---|---|---|
| `SetTRRelay(bit)` | `netInterface.c:376-381` | `prbpfilter->_TR_Relay`, both structs' `_trx_status` |
| `SetAtten(bits)` | `netInterface.c:425-428` | `prbpfilter->_20_dB_Atten`, `_10_dB_Atten` |
| `SetAlexAntennas(rx_out, rx_only_ant, trx_ant)` | `netInterface.c:465-495` | `prbpfilter->_Rx_1_Out`, `_10_dB_Atten`, `_Rx_1_In`, `_Rx_2_In`, `_XVTR_Rx_In`, `_ANT_1/2/3` + the bit-inherit copy + `prbpfilter2->_TXANT_1/2/3` |
| `SetAlexHPF(bits)` | `netInterface.c:609-615` | `prbpfilter->` HPF + Bypass + 6M_preamp (7 bits) |
| `SetAlex2HPF(bits)` | `netInterface.c:642-648` | `prbpfilter2->` same 7 bits |
| `SetRX2GroundOnTX(bits)` | `netInterface.c:659-661` | `prbpfilter2->_rx2_gnd` |
| `SetAlexLPF(bits)` w/ alex2 path | `netInterface.c:695-719` | Both structs' LPF bits + bit-inherit copy |

**Readers (Phase 2 `FrameComposer` — separate §):**

| Reference case | File:line | Reads |
|---|---|---|
| `WriteMainLoop_HL2` case 0 | `networkproto1.c:951-966` | `prbpfilter->_10_dB_Atten`, `_20_dB_Atten`, `_Rx_1_Out`, `_XVTR_Rx_In`, `_Rx_1_In`, `_Rx_2_In`, `_ANT_3`, `_ANT_2` |
| `WriteMainLoop_HL2` case 10 | `networkproto1.c:1081-1088` | `prbpfilter->` HPFs + Bypass + 6M_preamp + LPFs (14 bits) |
| `WriteMainLoop_HL2` case 16 | `networkproto1.c:1153-1156` | `prbpfilter2->` HPFs + Bypass + 6M_preamp + `_rx2_gnd` |
| Generic non-HL2 paths | `networkproto1.c:622-637`, `:752-759`, `:828-831` | Same read patterns (HL2 path is a fork) |
| P2 framer | `network.c:1028-1037` | Full `bpfilter` dword (32 bits) packed into bytes 1428-1435 |

**VERDICT:** Informational — no Lyra code declared in §2 for the
setters or readers.  Setters land when the UI/protocol-config
plumbing arrives (later §); readers land in `FrameComposer`
(its own §).  This enumeration is the audit trail so a future
implementer knows the full surface.

---

### §2.5 Global instance pointers + lifecycle

Same pattern as §1.12 RadioNet (post-correction):

| Aspect | Reference (file:line) | Lyra (proposed) |
|---|---|---|
| Alex0 global | `network.h:340` `RBPFILTER prbpfilter;` (declared); `netInterface.c:1727-1729` allocated at init | `extern RbpFilter* prbpfilter;` declared in header; `RbpFilter* prbpfilter = nullptr;` defined in `.cpp`; assigned by wire-layer init at HL2 session start (Phase 2 wire-up).  Name verbatim from the reference. |
| Alex1 global | `network.h:389` `RBPFILTER2 prbpfilter2;` (declared); `netInterface.c:1731-1733` allocated at init | `extern RbpFilter2* prbpfilter2;` same pattern. Name verbatim from the reference. |
| Lifetime | `netInterface.c:1807-1808` `_aligned_free` at session teardown | Wire-layer teardown releases ownership; destruction automatic via RAII |
| Copy / move semantics | Reference is plain C struct (trivially copyable; never copied by any caller) | Lyra C++ class: trivially copyable by default.  NO explicit `= delete` — Rule 1 reference parity, no Lyra-native safety addition (operator directive 2026-06-05: drop safety additions not present in the reference; if a real bug ever surfaces from accidental copying, revisit with operator sign-off then). |

**VERDICT:** ✅ **PARITY** on access pattern + global pointer
names verbatim from the reference + copy/move semantics (plain
struct, trivially copyable).  No ⚠ items.

**Amendment 2026-06-05 (operator directive — naming + safety
discipline):** the initial §2.5 draft proposed `g_rbpFilter1`/
`g_rbpFilter2` Lyra-native naming AND explicit `= delete`
non-copyable Lyra-native safety addition.  BOTH dropped per
the tightened operator directive (cross-reference grep parity;
no Lyra-native safety additions without explicit operator
discussion).  Names reverted verbatim; copy semantics match
the reference's plain-C posture.

---

### §2.6 Out of scope for §2 (deferred)

- **Setter implementations** (`SetTRRelay`, `SetAtten`, etc.)
  land with the UI/protocol-config plumbing — they're operator-
  driven actions, not state.  Each setter lands in the
  appropriate Lyra wire-layer or UI component when those §N
  entries arrive.
- **Reader implementations** (the case-by-case bit-emit logic
  in `FrameComposer`) — its own §N entry.  §2.4 enumerates
  the read patterns; the composer's checkpoint will table them
  byte-by-byte.
- **Bit-inherit copy logic** (`netInterface.c:489-490` and
  `:695-696`) — lives in the `SetAlexAntennas` / `SetAlexLPF`
  setters' Lyra-native equivalents; not part of the data
  structure declaration.

---

### §2 — Overall verdict

| Section | Verdict |
|---|---|
| §2.1 architectural placement | ✅ PARITY + ⚠ inner typedef names |
| §2.2 RbpFilter byte-for-byte | ✅ PARITY (32 bits, 4 byte rows) |
| §2.3 RbpFilter2 byte-for-byte | ✅ PARITY (32 bits, 3 position differences from RbpFilter all preserved) |
| §2.4 use-site enumeration | Informational only — no code in §2 |
| §2.5 global pointers + lifecycle | ✅ PARITY — pointer names + copy/move semantics verbatim |

**No 🔴 items in §2.**  The §10.2 component split keeps these as
sibling structs in the `wire/` layer per the locked architecture;
no new operator-approved deviation needed beyond the §1.1
networking-exclusion sign-off (which already established the
wire-layer sibling pattern).

---

**OPERATOR SIGN-OFF:**

- [x] §2 reviewed, all rows verdicts confirmed
- [x] §2.3 bit-inherit copy logic deferral acknowledged (lands
      with the setter implementations, not in the type
      definition)
- [x] §2.5 reference-verbatim naming + plain-C copy semantics
      confirmed (no Lyra-native additions)
- [x] Authorized to ship the populated `RbpFilter`/`RbpFilter2`
      header + CMakeLists.txt update matching this checkpoint

Signed: **N8SDR (Rick Langford)** Date: **2026-06-05**

---

*Last updated: 2026-06-05 — §3 HPSDR family + protocol enums + dispatch-relevant globals SIGNED.*

---

## §3. HPSDR family + protocol enums + dispatch-relevant globals

**Scope.** Three enums (`HPSDRHW`, `HPSDRModel`, `RadioProtocol`)
declared verbatim from `network.h:446-483`, plus the three runtime-
mutable globals the reference uses for wire dispatch (`XmitBit`,
the `HPSDRModel` variable shadow, the `RadioProtocol` variable
shadow).

**Architecture lock (operator-confirmed 2026-06-05).**

| Decision | Locked answer |
|---|---|
| Family inclusion now or easy-add-later? | **Option A** — type-level all-families now (all enum values verbatim); behavior-level HL2-only (ANAN/Atlas/Saturn paths land when tester hardware arrives — assert-on-hit until then) |
| All enum values verbatim from reference? | **YES** — `HPSDRHW` 9-value + `HPSDRModel` 16-value + `RadioProtocol` 2-value all declared, ANAN/Atlas/Saturn entries inert in v0.2 |
| Group axes into a Lyra-native `DispatchState` struct? | **NO** — operator directive 2026-06-05: do as the reference does.  Scattered globals at use sites, no struct wrapper.  Eliminates sync hazard, preserves grep parity 100%. |
| Where do the globals live? | **`RadioNet.h`** — matches reference placement (`network.h` holds the RadioNet equivalent + these enums + globals all in one file) |

### §3.1 `HPSDRHW` enum (gateware-architecture family)

`network.h:446-457`.  9 values; declared but **not used in the
reference wire dispatch** (the runtime dispatch uses `HPSDRModel`,
see §3.2).  Kept here for type-level parity per Q2 / Option A.

| Aspect | Reference (file:line) | Lyra |
|---|---|---|
| Enum type | `network.h:446-457` `enum HPSDRHW { ... };` | `enum class HPSDRHW { ... };` — C++23 idiom (scoped enum prevents implicit conversions) |
| Atlas | `network.h:448` `Atlas = 0` | `Atlas = 0` |
| Hermes | `:449` `Hermes = 1` | `Hermes = 1` |
| HermesII | `:450` `HermesII = 2` | `HermesII = 2` |
| Angelia | `:451` `Angelia = 3` | `Angelia = 3` |
| Orion | `:452` `Orion = 4` | `Orion = 4` |
| OrionMKII | `:453` `OrionMKII = 5` | `OrionMKII = 5` |
| HermesLite | `:454` `HermesLite = 6` | `HermesLite = 6` |
| Saturn | `:455` `Saturn = 10` | `Saturn = 10` |
| SaturnMKII | `:456` `SaturnMKII = 11` | `SaturnMKII = 11` |

Note: the reference's enum has a GAP between value 6 (HermesLite)
and 10 (Saturn) — values 7-9 are reserved.  Preserved verbatim.

**VERDICT:** ✅ **PARITY** on names + numeric values; ⚠
**ACCEPTABLE DEVIATION** on `enum class` (scoped) vs C `enum`
(unscoped) — C++23 idiom, same memory layout (default underlying
type `int`).

---

### §3.2 `HPSDRModel` enum (marketed-model identifier — the active dispatch axis)

`network.h:459-477`.  16 values.  This is the enum the reference
actually dispatches on (`==` comparisons across the wire-layer
sources — HL2-class vs ANAN-class branching).

| Aspect | Reference (file:line) | Lyra |
|---|---|---|
| Enum type + variable shadow | `network.h:459-477` `enum _HPSDRModel { ... } HPSDRModel;` — declares enum type AND a global variable of that type with the same name (C-style shadow) | `enum class HPSDRModel { ... };` (C++ requires the variable to be a distinct declaration with a non-colliding name — see §3.4 for the variable rename) |
| `HPSDRModel_HPSDR` (value 0) | `network.h:461` | `HPSDR = 0` (drop the `HPSDRModel_` prefix — redundant under `enum class`; the reference needed it because the enum was unscoped) |
| `HERMES` (value 1) | `:462` | `HERMES = 1` |
| `ANAN10` (value 2) | `:463` | `ANAN10 = 2` |
| `ANAN10E` (value 3) | `:464` | `ANAN10E = 3` |
| `ANAN100` (value 4) | `:465` | `ANAN100 = 4` |
| `ANAN100B` (value 5) | `:466` | `ANAN100B = 5` |
| `ANAN100D` (value 6) | `:467` | `ANAN100D = 6` |
| `ANAN200D` (value 7) | `:468` | `ANAN200D = 7` |
| `ORIONMKII` (value 8) | `:469` | `ORIONMKII = 8` |
| `ANAN7000D` (value 9) | `:470` | `ANAN7000D = 9` |
| `ANAN8000D` (value 10) | `:471` | `ANAN8000D = 10` |
| `ANAN_G2` (value 11) | `:472` | `ANAN_G2 = 11` |
| `ANAN_G2_1K` (value 12) | `:473` | `ANAN_G2_1K = 12` |
| `ANVELINAPRO3` (value 13) | `:474` | `ANVELINAPRO3 = 13` |
| `HERMESLITE` (value 14) | `:475` | `HERMESLITE = 14` |
| `REDPITAYA` (value 15) | `:476` | `REDPITAYA = 15` |

**VERDICT:** ✅ **PARITY** on numeric values + count (16) + all
identifier roots; ⚠ **ACCEPTABLE DEVIATION** on `enum class` vs
C unscoped enum (same memory layout) and dropped `HPSDRModel_`
prefix (redundant under `enum class HPSDRModel`).  Cross-reference
grep on the value identifier is preserved (`HPSDRModel::HERMESLITE`
in Lyra matches the reference's `HERMESLITE` token) — the surface
that matters for PureSignal port work.

---

### §3.3 `RadioProtocol` enum

`network.h:479-483`.  2 values, runtime-mutable variable.

| Aspect | Reference (file:line) | Lyra |
|---|---|---|
| Enum type + variable shadow | `network.h:479-483` `enum _RadioProtocol { USB = 0, ETH = 1 } RadioProtocol;` | `enum class RadioProtocol { USB = 0, ETH = 1 };` (variable rename per §3.4 — same pattern as `HPSDRModel`) |
| `USB` (value 0) | `:481` `USB = 0,  // Protocol USB (P1)` | `USB = 0` |
| `ETH` (value 1) | `:482` `ETH = 1   // Protocol ETH (P2)` | `ETH = 1` |

**VERDICT:** ✅ **PARITY** on names + values; ⚠ **ACCEPTABLE
DEVIATION** on `enum class` vs C unscoped enum (same idiom
translation as §3.1 / §3.2).

---

### §3.4 Dispatch-relevant runtime globals (no struct wrapper)

Three reference-verbatim globals consumed at use sites by the
wire-send thread (MOX-bit emission), the DDC routing matrix
(per-(mox, ps_armed, hw) selection), and the family / wire-
protocol dispatch.  No struct wrapper — the reference reads these
as scattered globals at use sites and Lyra does the same.

**Rule 24 correction (2026-06-05).** The original draft of this
row asserted `XmitBit` was `volatile long`.  Source-line read at
`network.h:413` + `:505` shows the reference type is plain `int`
(no `volatile`, no `long`).  The prior shipped declaration
`std::atomic<long> XmitBit` was a doubly-wrong memory-vs-source
error (wrong width + wrong synchronization wrapper).  This row
is corrected to verbatim `int` matching the reference.

| Global | Reference (file:line) | Lyra |
|---|---|---|
| MOX bit | `network.h:413` `int XmitBit;` (declared again at `:505` — benign reference defect; Lyra declares once) | `extern int XmitBit;` — type + name verbatim; no synchronization wrapper (reference posture preserved) |
| Active radio model | `network.h:459-477` `enum _HPSDRModel { ... } HPSDRModel;` — variable named `HPSDRModel` shadowing the enum type | `extern HPSDRModel hpsdrModel;` — variable named distinctly from the enum type (C++ does not permit the C-style same-name shadow); the role + reads-at-use-site behavior preserved |
| Active wire protocol | `network.h:479-483` `enum _RadioProtocol { ... } RadioProtocol;` | `extern RadioProtocol radioProtocol;` — same rename pattern |

**Note on the variable rename.** The reference shadows the enum
type with a variable of the same name (`HPSDRModel HPSDRModel;`).
C allows this in the tag-name namespace; C++ does not.  Two
minimal-deviation options were considered:
- **Rename variable (chosen):** `HPSDRModel hpsdrModel` — variable
  is lowerCamelCase, enum is PascalCase.  Cross-reference grep on
  the enum type name still works.
- **Rename enum type:** would break grep parity on the enum value
  identifiers (`HERMESLITE`, etc.) — worse trade-off.

**Synchronization.** The reference treats `XmitBit` + the model
+ protocol globals as plain reads/writes (no mutex).  Reads happen
from multiple threads (wire writer, EP6 reader); writes happen
from the FSM (XmitBit) or once at session open (model + protocol).
Lyra mirrors this — plain reads on x86_64 are the operational
guarantee, matching the reference's posture.  If a future bench
discovers a race, atomic wrappers can be added with explicit
operator sign-off then — NOT a preemptive safety addition per the
2026-06-05 directive.

**VERDICT (post-Rule-24 correction):** ✅ **PARITY** on all
three globals' types + names (`XmitBit` is `int` verbatim;
plain reads, no lock); ⚠ **ACCEPTABLE DEVIATION** on the
variable rename for the two enum-name shadows only.

---

### §3.4 supplement — additional dispatch globals (added 2026-06-05 per Rule 24)

`network.h:501-506` declares additional per-radio-class static
parameters that the C&C round-robin scheduler reads at use sites.
Missed in the original §3.4 draft; added here as a §3.4-supplement
under the same locked architecture (no struct wrapper, scattered
reads at use sites, no synchronization wrapper).  Original §3.5 /
§3.6 / §3.7 section numbering preserved unchanged — sign-off block
references at the end of §3 remain valid.

| Global | Reference (file:line) | Lyra | Default |
|---|---|---|---|
| `nddc` | `network.h:504` `int nddc;` — per-family DDC count (HL2/HL2+ = 4; Hermes II = 2; ANAN G2 = 4; ANAN 7000DLE = 7).  Read at case-0 `(nddc - 1) << 3`, the MOX-edge `if (nddc == 2)` jump (HL2 = no-op), and the PS/Orion overrides in cases 2 / 3 / 5. | `extern int nddc;` — type + name verbatim | `4` (HL2; per-family init at session open overwrites) |
| `SampleRateIn2Bits` | `network.h:506` `unsigned char SampleRateIn2Bits;` — outbound 2-bit sample-rate code (48k=0, 96k=1, 192k=2, 384k=3).  Written into case-0 C1. | `extern unsigned char SampleRateIn2Bits;` — verbatim | `0` = 48k; operator rate setter writes per session |
| `P1_en_diversity` | `network.h:501` `int P1_en_diversity;` — diversity-enabled flag.  When non-zero, RX1+RX2 VFOs lock (case-0 C4 bit 7). | `extern int P1_en_diversity;` — verbatim | `0` (HL2 has no diversity feature) |

**Synchronization.** Same posture as §3.4 — reference reads
these as plain globals (no mutex); set once per session by per-
family init code or rare operator setter; word-sized reads on
x86_64 are the operational guarantee.  No atomic wrapper.

**Other reference globals deferred to later §N entries** (not §3
scope — they're consumed by later components):
- `P1_adc_cntrl` (`network.h:503`) — case-4 C1/C2 ADC routing (TX);
  lands with §4b (FrameComposer TX cases).
- `PreviousTXBit` (`networkproto1.c:29`) — scheduler-internal
  edge-detect state; lives inside FrameComposer (§4a.1), NOT a
  cross-cutting global.
- `mic_decimation_factor`, `mic_decimation_count` (`network.h:507-
  508`) — mic-input thread state; lands with the mic-source
  component's §N entry.
- `FPGAReadBufp`, `FPGAWriteBufp` (`network.h:498-499`) — wire-
  layer buffer pointers; land with `Ep2SendThread` + `Ep6RecvThread`
  §N entries per the §10.2 component split.

**VERDICT:** ✅ **PARITY** on all three names + types; ZERO
deviations.

---

### §3.5 `ps_armed` and `rx2_enabled` — NOT new globals

In the prior (rejected) draft these appeared as struct fields.
With no struct, they are NOT new symbols:

- **`ps_armed`** — already lives at `prn->puresignal_run`
  (RadioNet §1.2).  Reference reads it directly from
  `prn->puresignal_run` at every dispatch site.  Lyra does the
  same.  No new global.
- **`rx2_enabled`** — the reference has no dedicated global;
  derived from `prn->nddc>=4` + per-rx enable checks.  Lyra does
  the same — derived expression at use sites.  No new global.

**VERDICT:** ✅ **PARITY** — both axes read from existing
reference globals at use sites.

---

### §3.6 Behavior-level scope (Q2 Option A)

**Type surface:** all enum values declared verbatim — covers
HL2/HL2+, all ANAN variants, Atlas, Hermes, Orion, Saturn,
AnvelinaPro3, RedPitaya, both protocols.

**Behavior:** HL2/HL2+ paths shipped first.  ANAN/Atlas/Saturn
dispatch branches in `FrameComposer` / `DdcMap` / future
components land as:

```cpp
switch (hpsdrModel) {
    case HPSDRModel::HERMESLITE:
    case HPSDRModel::HPSDR:  // (HL2-class)
        // ... HL2 behavior, fully implemented ...
        break;

    default:
        // ANAN / Atlas / Saturn / etc. — type-level supported,
        // behavior pending tester hardware.
        assert(false && "model not yet implemented — needs "
                        "operator bench verification");
}
```

Same pattern for `radioProtocol`: P1 (USB) shipped; P2 (ETH)
branch is `assert(false)` until ANAN-G2 / 7000DLE tester arrives.

**This is the Option-A discipline operationalized.** The asserts
fail LOUD if an unsupported family / protocol is accidentally
selected (e.g. via misconfigured discovery), preventing silent
mis-dispatch.  When ANAN tester hardware arrives, the operator-
empirical bench loop kicks in per the locked methodology — write
behavior against actual hardware, NOT against agent inference.

---

### §3.7 Out of scope for §3 (deferred to later sections)

- **Per-family static data** (nddc, has_onboard_codec, default
  audio path, tx_attenuator_range, puresignal_requires_mod,
  cwx_ptt_bit_position, pa_enable_uses_apollo_i2c, TR-sequencing
  defaults, etc.) — lives INLINE at each consumer's use site,
  matching the reference's posture (per-family `switch
  (hpsdrModel)` branches at every consumer, magic numbers
  hardcoded inline with source citations).  NOT a Lyra-native
  `Capabilities` struct — earlier draft proposed one; operator
  locked "do as the reference does, scattered branches" on
  2026-06-05 for consistency with §3.  Each consumer's §N
  checkpoint documents the per-family branches it carries.
- **Per-family DDC routing matrix** — same locked decision as
  §3-DispatchState + §4-Capabilities (operator 2026-06-05): the
  reference's `MetisReadThreadMainLoop_HL2:544-558` does this
  as an INLINE `switch (nddc)` block inside the EP6 read loop.
  Lyra does the same — no separate `DdcMap` class.  Per-DDC
  routing lands inline inside §5 `Ep6RecvThread`'s read-loop
  body.  The original `DdcMap` deferral is superseded — same
  pattern as the §3 DispatchState and §4 Capabilities classes
  the operator rejected on 2026-06-05.
- **`Saturn`-specific gateware quirks**, **ANVELINAPRO3
  end_frame=17 case**, etc. — behavior-level work, lands when
  the relevant tester hardware arrives (assert-on-hit until then
  per §3.6).

---

### §3 — Overall verdict

| Section | Verdict |
|---|---|
| §3.1 HPSDRHW enum | ✅ PARITY + ⚠ `enum class` idiom |
| §3.2 HPSDRModel enum | ✅ PARITY + ⚠ `enum class` + dropped redundant prefix |
| §3.3 RadioProtocol enum | ✅ PARITY + ⚠ `enum class` idiom |
| §3.4 Dispatch globals (`XmitBit`, `hpsdrModel`, `radioProtocol`) | ✅ PARITY + ⚠ variable rename on the two enum-shadow vars |
| §3.5 `ps_armed` / `rx2_enabled` (no new symbols) | ✅ PARITY (read from `prn` / derived at use sites) |
| §3.6 Behavior scope | Type-level all-families per Q2 / Option A locked |

**ZERO 🔴 OPERATOR-APPROVED DEVIATIONS.** Reference-verbatim
posture fully preserved.

---

**OPERATOR SIGN-OFF:**

- [x] §3 reviewed, all rows verdicts confirmed
- [x] §3.1 / §3.2 / §3.3 enum-class idiom accepted (verbatim
      values + names; C++23 scoping)
- [x] §3.4 plain-global access (no atomic/mutex wrapper on
      `hpsdrModel` / `radioProtocol`) accepted — matches
      reference posture; `XmitBit` uses `std::atomic<long>`
      per the locked §1.3 `volatile long` idiom translation;
      revisit if a real race surfaces on bench
- [x] §3.5 `ps_armed` / `rx2_enabled` stay read-from-existing-
      symbols at use sites (no new globals introduced)
- [x] §3.6 behavior-level HL2-only with `assert(false)` placeholders
      on unsupported family/protocol branches accepted
- [x] Authorized to ship the 3 enums + 3 extern variable
      declarations into `RadioNet.h` (no separate
      `DispatchState.h`; that empty skeleton is removed)
      matching this checkpoint

Signed: N8SDR        Date: 2026-06-05

---

*Last updated: 2026-06-05 — §3 body reconciled to match shipped code (`f3ccc51`); reference-name leaks scrubbed per Rule 2.*

---

## §4a. `FrameComposer` — round-robin scheduler + RX-essential cases

**Scope.** The C&C round-robin scheduler structure + the three
RX-essential cases on HL2 (case 0 frame `0x00` general settings;
case 2 frame `0x04` RX1 / DDC0; case 3 frame `0x06` RX2 / DDC1).
Case 1 frame `0x02` is **TX VFO** and belongs in **§4b**; cases
4-18 (TX att, drive, PA, mic, LNA, HL2 TX-latency, reset-on-
disconnect, PS, ANAN-only RX5/RX6 + per-DDC rates) land in **§4b**
and **§4c**.

**Source mirror.** `networkproto1.c::WriteMainLoop_HL2` lines
869-1201, HL2 / HL2+ dispatch.  Generic-P1 (older ANAN / Hermes /
Orion) and P2 branches land when tester hardware arrives, per
§3.7 — until then non-HL2 `case`-internal paths `assert(false
&& "model not yet implemented — needs operator bench verification")`.

**Locked architecture (Q1–Q5).** Operator-confirmed 2026-06-05;
post-Rule-24 source-verified rewrite.

| # | Question | Locked answer |
|---|---|---|
| Q1 | Scheduler structure | **Switch-style per-case dispatch, byte-identical to the reference.**  Lyra implements `compose_and_emit(uint8_t* txbptr)` as a C++ `switch (out_control_idx_)` whose cases each write into `C0..C4` local variables exactly as the reference does (`networkproto1.c:946-1178`).  No data-driven cycle list; no payload map; no helper functions.  ZERO deviation from reference's switch structure. |
| Q2 | Case population | **Eager.**  All 19 cases (0-18) are compile-time present in the switch, matching the reference's all-cases-in-switch posture.  No lazy slot registration. |
| Q3 | Per-family branches | **Inline `switch (hpsdrModel)` at each case site** — magic numbers literal, HL2 path implemented, non-HL2 paths `assert(false)`.  No `Capabilities` struct; no helper functions.  Matches the reference's per-case `if (HPSDRModel == ...)` discipline. |
| Q4 | Synchronization | One `std::mutex cc_lock_` member of `FrameComposer` ↔ reference's `_cc_lock`.  Held during BOTH composition (setter writes to `prn->*` fields are protected by the relevant component's lock, NOT `cc_lock_`) AND the per-frame compose-and-emit walk (the switch body reads from many globals; the lock is held for the duration of the case body + the post-switch `txbptr[3..7] = C0..C4` writes).  Lock-order acyclic per §15.26 W1.3/W1.4 lessons. |
| Q5 | Cursor advance | **Once per USB frame emit**, identical to the reference (`networkproto1.c:1180-1183`): `if (out_control_idx_ < 18) out_control_idx_++; else out_control_idx_ = 0;`.  Two USB frames per UDP datagram; the `for (txframe = 0; txframe < 2; txframe++)` outer loop in the reference produces two C&C-header advances per datagram. |

---

### §4a.1 Round-robin scheduler structure (source: `networkproto1.c:869-1191`)

| Aspect | Reference (file:line) | Lyra |
|---|---|---|
| Function signature | `void WriteMainLoop_HL2(char* bufp)` — `:869` | `void FrameComposer::write_main_loop_hl2(char* bufp);` — name + signature verbatim |
| Per-call USB-frame count | Outer `for (txframe = 0; txframe < 2; txframe++)` — produces 2 USB frames per UDP datagram (`:878`) | Same — `for (int txframe = 0; txframe < 2; ++txframe)` |
| Per-USB-frame sync bytes | `txbptr[0] = 0x7f; txbptr[1] = 0x7f; txbptr[2] = 0x7f;` (`:881-883`) | Same — three `0x7f` sync bytes at offsets 0/1/2 |
| C0 base initialization | `C0 = (unsigned char)XmitBit;` (`:896`) — the MOX bit is bit 0 of C0; the per-case `C0 \|= <addr<<1>` OR's the address bits ABOVE bit 0 | Same — `unsigned char C0 = static_cast<unsigned char>(XmitBit);` BEFORE the per-case switch dispatch |
| Cursor state | `out_control_idx` global (`:27`); `PreviousTXBit` global (`:29`) for MOX-edge detection | Per-FrameComposer member: `int out_control_idx_ = 0;` and `int previous_tx_bit_ = 0;`.  These are scheduler-internal — NOT cross-cutting globals like the §3.4 / §3.5 set — so they live inside FrameComposer, not as extern declarations. |
| MOX-edge jump (Hermes II only) | `if (XmitBit != PreviousTXBit) { if (nddc == 2) out_control_idx = 2; PreviousTXBit = XmitBit; }` (`:886-891`) — when MOX changes AND nddc=2 (Hermes II), jump the cursor to case 2 so the next emit is RX1/DDC0 (the Hermes II nddc=2 PS path needs DDC0 retuned to TX freq).  On HL2 (nddc=4) the inner `if (nddc == 2)` is FALSE so this is a no-op for the MOX-edge — but `PreviousTXBit = XmitBit;` still updates. | Same — preserved verbatim including the no-op-on-HL2 behavior.  Code reads `if (XmitBit != previous_tx_bit_) { if (nddc == 2) out_control_idx_ = 2; previous_tx_bit_ = XmitBit; }`. |
| I2C-transaction overlay (HL2-only) | Lines `:898-943`.  If `prn->i2c.delay` has expired AND `prn->i2c.in_index != prn->i2c.out_index` (a queued I2C transaction is ready), the C0-C4 bytes are OVERRIDDEN with I2C addressing/data bytes and the switch case body does NOT run for this USB frame.  Increments `prn->i2c.out_index`. | Same — preserved verbatim.  The I2C overlay block runs FIRST inside the per-USB-frame loop; only on `else` does control fall through to the switch dispatch (`:944-1178`). |
| Switch dispatch | `switch (out_control_idx) { case 0: ... case 18: ... }` (`:946-1178`).  19 cases. | Same — `switch (out_control_idx_)` with 19 cases compile-time present.  Per Q2 eager. |
| Post-switch packet packing | `txbptr[3] = C0; txbptr[4] = C1; txbptr[5] = C2; txbptr[6] = C3; txbptr[7] = C4;` (`:1186-1190`) — writes the 5 C-bytes into offsets 3-7 of the current USB frame buffer | Same — verbatim per-byte assignment |
| Cursor advance | `if (out_control_idx < 18) out_control_idx++; else out_control_idx = 0;` (`:1180-1183`) — INSIDE the `else` (non-I2C) branch, so a USB frame that emitted I2C bytes does NOT advance the cursor | Same — preserved exactly, including the no-advance-on-I2C-overlay behavior |
| Post-loop wire transmit | `memcpy(FPGAWriteBufp + 8, bufp, 8 * 63); memcpy(FPGAWriteBufp + 520, bufp + 504, 8 * 63); MetisWriteFrame(0x02, FPGAWriteBufp); ReleaseSemaphore(prn->hobbuffsRun[0], 1, 0); ReleaseSemaphore(prn->hobbuffsRun[1], 1, 0);` (`:1194-1200`) | Memcpy + wire-send are wire-thread concerns; they live in `Ep2SendThread` (§7).  FrameComposer's `write_main_loop_hl2` ONLY produces the C&C bytes + sync header into the caller-provided `txbptr`; the EP2 thread handles the LRIQ memcpy + the `sendto`. |

**VERDICT:** ✅ **PARITY** — switch structure, cursor variable +
edge-detect variable, two-USB-frame loop, sync bytes, C0 base
init, MOX-edge jump, I2C overlay, post-switch packet packing,
cursor advance — all verbatim from `networkproto1.c:869-1191`.
The `MetisWriteFrame` + semaphore releases are split into
`Ep2SendThread` per the §10.2 component decomposition (no
behavioral deviation; wire bytes identical).

---

### §4a.2 Mutex discipline

| Aspect | Reference | Lyra |
|---|---|---|
| Lock object | `_cc_lock` (`CRITICAL_SECTION` declared in `_radionet`; lock is process-global) | One `std::mutex cc_lock_` member of `FrameComposer` (process-lifetime singleton) |
| Held during | The case bodies that read setter-mutable state.  The reference acquires the lock implicitly via the C runtime's lock posture; the setters that write the read state acquire it explicitly. | Held during the entire `write_main_loop_hl2(...)` call AND during all setter methods (`set_rx_freq`, `set_sample_rate_code`, `set_oc_output`, etc.) that mutate state the switch reads. |
| Lock-order invariant | Reference does not document; Lyra empirical (§15.26 W1.3 / W1.4 lessons) — no nested acquisition of any other wire-layer lock from inside `cc_lock_` | Same discipline — `cc_lock_` is leaf in the lock-order partial order.  Setters must not call other wire-layer methods that would take a second lock |

**VERDICT:** ✅ **PARITY** on scope + held-during; ⚠
**ACCEPTABLE DEVIATION** on the C → C++23 lock idiom
(`CRITICAL_SECTION` → `std::mutex`, same as §1.11).

---

### §4a.3 Case 0 — frame `0x00` — general settings (source: `networkproto1.c:948-970`)

The longest case + the only one that reads BOTH `prn->*` AND
`prbpfilter->*` (the §1 + §2 globals working together).
Byte-by-byte verbatim from source:

| Byte | Reference (HL2 branch) | Lyra |
|---|---|---|
| C0 (post C0 base init) | No `C0 \|= ...` for case 0 — keeps `C0 = XmitBit` from line 896 | Same — case 0 leaves C0 = XmitBit (no addr OR'd in; addr 0 means bits stay 0) |
| C1 | `C1 = (SampleRateIn2Bits & 3);` (`:949`) — read of the §3.5 `SampleRateIn2Bits` global, masked to bits[1:0] | Same — `C1 = (SampleRateIn2Bits & 3);` |
| C2 | `C2 = (prn->cw.eer & 1) \| ((prn->oc_output << 1) & 0xFE);` (`:950`) — bit 0 = CW-EER flag from `prn->cw.eer` (the §1.5 mode_control bitfield); bits[7:1] = OC pins from `prn->oc_output` left-shifted by 1 | Same — `C2 = (prn->cw.eer & 1) \| ((prn->oc_output << 1) & 0xFE);` |
| C3 | `C3 = (prbpfilter->_10_dB_Atten & 1) \| ((prbpfilter->_20_dB_Atten << 1) & 2) \| ((prn->rx[0].preamp << 2) & 0b00000100) \| ((prn->adc[0].dither << 3) & 0b00001000) \| ((prn->adc[0].random << 4) & 0b00010000) \| ((prbpfilter->_Rx_1_Out << 7) & 0b10000000);` (`:951-953`) — six OR'd bitfields from `prbpfilter` + `prn` THEN conditionally `if (_XVTR_Rx_In) C3 \|= 0b01100000; else if (_Rx_1_In) C3 \|= 0b00100000; else if (_Rx_2_In) C3 \|= 0b01000000;` (`:954-959`) | Same — preserved verbatim including the three-way XVTR/Rx_1/Rx_2 conditional |
| C4 | 3-way conditional first: `if (_ANT_3) C4 = 0b10; else if (_ANT_2) C4 = 0b01; else C4 = 0;` (`:961-966`).  Then OR'd with three things: `C4 \|= 0b00000100;` (duplex bit, `:967`); `C4 \|= (nddc - 1) << 3;` (DDC count, `:968`); `C4 \|= (P1_en_diversity) << 7;` (diversity lock, `:969`) | Same — preserved verbatim.  HL2 with `nddc = 4`, `P1_en_diversity = 0`, `_ANT_2 = _ANT_3 = 0` → C4 = `0 \| 0x04 \| (3<<3) \| 0` = `0x04 \| 0x18` = `0x1C` (the §15.26 locked main-loop value) |

**Cross-reference: priming-path C4 value.**  When `ForceCandCFrames`
(§8) emits the priming burst of frame 0x00, the same composer
runs but `prn->reset_on_disconnect` and other state may not be
populated yet.  The C4 value is still `(0x04) \| ((nddc-1) << 3)
\| (P1_en_diversity << 7)` — for HL2 with `nddc = 4` and
`P1_en_diversity = 0` that's still `0x1C`.  The §15.26 history
mentioning "priming-path C4 = 0x18" appears to refer to a
DIFFERENT priming variant — to be source-verified when §8
ForceCandC is drafted; not in §4a scope.

**VERDICT:** ✅ **PARITY** — every bit-position + every input
source verbatim from `:948-970`.  HL2 family is the only branch
implemented; non-HL2 dispatch lands at consumer §N entries.

---

### §4a.4 Case 2 — frame `0x04` — RX1 VFO / DDC0 (source: `networkproto1.c:982-993`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 4;` (`:983`) → `C0 = XmitBit \| 0x04` | Same |
| ddc_freq selection (HL2 / nddc=4 path) | Lines `:984-988`: `if ((nddc == 2) && (XmitBit == 1) && (prn->puresignal_run)) ddc_freq = prn->tx[0].frequency; else ddc_freq = prn->rx[0].frequency;` — on HL2 (nddc=4) the `nddc == 2` is FALSE so `ddc_freq = prn->rx[0].frequency` always | Same — preserved verbatim including the nddc=2-only override (no-op on HL2) |
| C1 / C2 / C3 / C4 | `C1 = (ddc_freq >> 24) & 0xff; C2 = (ddc_freq >> 16) & 0xff; C3 = (ddc_freq >> 8) & 0xff; C4 = (ddc_freq) & 0xff;` (`:989-992`) — big-endian 32-bit freq, MSByte first | Same — verbatim 4-byte BE encoding |

**VERDICT:** ✅ **PARITY** — every byte verbatim from `:982-993`.

---

### §4a.5 Case 3 — frame `0x06` — RX2 VFO / DDC1 (source: `networkproto1.c:995-1010`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 6;` (`:996`) → `C0 = XmitBit \| 0x06` | Same |
| ddc_freq selection — 3-way conditional | Lines `:1000-1005`: `if ((nddc == 2) && (XmitBit == 1) && (prn->puresignal_run)) ddc_freq = prn->tx[0].frequency;` else `if (nddc == 5) ddc_freq = prn->rx[0].frequency;` else `ddc_freq = prn->rx[1].frequency;` — three cases: (1) Hermes-II nddc=2 PS-on TX state → DDC1 carries TX freq; (2) Orion / ANAN P1 nddc=5 → DDC1 carries RX1 freq (Orion uses DDC2 for RX2); (3) default HL2 nddc=4 → DDC1 carries RX2 freq from `prn->rx[1].frequency` | Same — preserved verbatim, all three branches present.  HL2 takes path (3). |
| C1 / C2 / C3 / C4 | `C1 = (ddc_freq >> 24) & 0xff; C2 = (ddc_freq >> 16) & 0xff; C3 = (ddc_freq >> 8) & 0xff; C4 = (ddc_freq) & 0xff;` (`:1006-1009`) | Same — verbatim 4-byte BE encoding |

**VERDICT:** ✅ **PARITY** — every byte verbatim from `:995-1010`,
including all three nddc-conditional branches.

---

### §4a.6 Non-§4a cases — placeholder (assert-false for HL2-bench until later §)

The §4a scope is cases 0 / 2 / 3 only.  Cases 1, 4-18 land in
§4b (TX cases) and §4c (PS + ANAN-only).  In the §4a code commit,
the switch will have all 19 cases compile-time present (per Q2
eager), but the cases NOT yet in §4a scope will `assert(false &&
"case N not yet implemented — see §4b/§4c")`.  This is the same
posture as the family-branch defaults (per Q3) — strict reference-
parity at compile time, behavior implemented in scope-locked
phases.  HL2 RX-only operation exercises ONLY cases 0 / 2 / 3 in
practice (the round-robin still iterates through all 19, so the
asserts WILL fire on every fourth-or-later USB frame from §4a
onwards) — the §4a code therefore CANNOT be wired into the EP2
writer thread until §4b lands.  This is honored by keeping
FrameComposer WIRE-INERT until §7 `Ep2SendThread` ships.

**VERDICT:** ✅ Architecture-correct — eager case presence per Q2;
behavior-locked phasing per the §4a/4b/4c split.

---

### §4a — Overall verdict

| Section | Verdict |
|---|---|
| Q1–Q5 architecture lock | Operator review pending |
| §4a.1 Scheduler structure (switch, cursor, MOX-edge jump, I2C overlay, post-switch packing) | ✅ PARITY — verbatim from `networkproto1.c:869-1191` |
| §4a.2 Mutex discipline | ✅ PARITY on scope; ⚠ `CRITICAL_SECTION` → `std::mutex` idiom |
| §4a.3 Case 0 (frame 0x00) | ✅ PARITY — every bit-position verbatim from `:948-970` |
| §4a.4 Case 2 (frame 0x04 RX1/DDC0) | ✅ PARITY — verbatim from `:982-993` |
| §4a.5 Case 3 (frame 0x06 RX2/DDC1) | ✅ PARITY — verbatim from `:995-1010`, all 3 nddc-conditional branches preserved |
| §4a.6 Out-of-scope cases (1, 4-18) | ✅ Architecture-correct; `assert(false)` placeholders, eager presence |

**ZERO 🔴 OPERATOR-APPROVED DEVIATIONS.** ZERO ⚠ ACCEPTABLE
DEVIATIONS beyond the §1.11-locked `std::mutex` idiom.  Wire bytes
byte-identical to the reference for all §4a-scope cases.

---

**OPERATOR SIGN-OFF:**

- [x] §4a architecture lock Q1–Q5 reviewed + confirmed (switch
      style, eager presence, inline family branches, single
      mutex, once-per-frame advance)
- [x] §4a.1 scheduler structure — reference posture preserved
      verbatim including the MOX-edge jump, I2C overlay, and the
      no-advance-on-I2C cursor behavior
- [x] §4a.2 mutex idiom accepted (`std::mutex` ↔ `CRITICAL_SECTION`,
      same as §1.11)
- [x] §4a.3 case 0 — every C-byte bit-position verbatim from
      `networkproto1.c:948-970`
- [x] §4a.4 case 2 — verbatim from `:982-993`
- [x] §4a.5 case 3 — verbatim from `:995-1010` including all
      three nddc-conditional branches preserved
- [x] §4a.6 out-of-scope cases land as `assert(false)` placeholders
      with eager compile-time presence; FrameComposer stays
      WIRE-INERT until §7 `Ep2SendThread` ships
- [x] Authorized to populate `src/wire/FrameComposer.{h,cpp}`
      matching this checkpoint (scheduler + cases 0/2/3; cases 1
      and 4-18 as `assert(false)` placeholders)

Signed: N8SDR        Date: 2026-06-05

---

*Last updated: 2026-06-05 — §4a FrameComposer signed + populated (commit lands separately); Rule-24-verified verbatim from `networkproto1.c:869-1191` cases 0 / 2 / 3 + scheduler structure (MOX-edge jump + I2C overlay + post-switch packet packing).*

---

## §4b-1. `FrameComposer` — simple TX cases (1, 4, 13, 14, 15, 17, 18)

**Scope.** Seven TX-relevant cases that are structurally simple
(single-field writes, no Apollo bits, no MOX-gated step ATT, no
per-band HPF/LPF bit unpacking).  Cases 10 / 11 / 12 / 16 (the
§15.26-history TX-heavyweight cluster) land in **§4b-2**; cases
5 / 6 / 7 / 8 / 9 (RX-mirror / ANAN-only) land in **§4c**.

Source mirror: `networkproto1.c::WriteMainLoop_HL2` lines
974-980 (case 1) + 1015-1021 (case 4) + 1127-1149 (cases 13-15)
+ 1162-1176 (cases 17-18).  HL2 / HL2+ dispatch.

**§3 supplement — one more global surfaced by §4b-1:**

`network.h:503` declares `int P1_adc_cntrl;` — read by case 4
as a per-radio-class ADC routing register.  Same pattern as the
§3.5 supplemental globals (added 2026-06-05 per Rule 24).  Add
to `RadioNet.h` extern declarations and `RadioNet.cpp`
definitions in the §4b-1 code commit.

| Global | Reference (file:line) | Lyra | Default |
|---|---|---|---|
| `P1_adc_cntrl` | `network.h:503` `int P1_adc_cntrl;` — per-family ADC-to-DDC routing control word; case 4 writes the low 8 bits to C1, bits 8-9 to C2.  HL2 / HL2+ uses ADC0 for all DDCs (`P1_adc_cntrl = 0` works); per-family init at session start overwrites for ANAN models. | `extern int P1_adc_cntrl;` — type + name verbatim | `0` (HL2 / HL2+; per-family init overrides) |

---

### §4b-1.1 Case 1 — frame `0x02` — TX VFO (source: `networkproto1.c:974-980`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 2;` (`:975`) → `C0 = XmitBit \| 0x02` | Same |
| C1 / C2 / C3 / C4 | `C1 = (prn->tx[0].frequency >> 24) & 0xff;` … `C4 = (prn->tx[0].frequency) & 0xff;` (`:976-979`) — big-endian 32-bit, MSByte first | Same — verbatim 4-byte BE encoding from `prn->tx[0].frequency` |

**Setter.** `set_tx_freq(int freq_hz)` writes `prn->tx[0].frequency`
under `cc_lock_`.  The reference has no setter; operator code
writes directly to `prn->tx[0].frequency`.  Lyra's setter ensures
the write is sequenced with the per-frame compose-and-emit walk.

**VERDICT:** ✅ **PARITY** — verbatim from `:974-980`.

---

### §4b-1.2 Case 4 — frame `0x1c` — ADC assignments + TX step attenuator (source: `networkproto1.c:1015-1021`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x1c;` (`:1016`) → `C0 = XmitBit \| 0x1c` | Same |
| C1 | `C1 = P1_adc_cntrl & 0xFF;` (`:1017`) — low 8 bits of the ADC-control word | Same |
| C2 | `C2 = (P1_adc_cntrl >> 8) & 0b0011111111;` (`:1018`) — bits 8-15 of the ADC-control word; the literal mask `0b0011111111` is 10 bits but the destination is `unsigned char` (8 bits), so the mask is effectively `& 0xFF`.  Reference quirk preserved verbatim. | Same — mask preserved verbatim including the wider-than-destination literal |
| C3 | `C3 = prn->adc[0].tx_step_attn & 0b00011111;` (`:1019`) — 5-bit TX step attenuator (frame 0x1c is the narrow form; the wider 6-bit MOX-gated form is in case 11 / frame 0x14, §4b-2) | Same.  No `31 - x` inversion at the COMPOSER layer — the inversion happens at the SETTER (`set_tx_step_attn_db`, lands with §4b-2 since the setter is shared between case 4 and case 11).  Until §4b-2 ships its setter, `prn->adc[0].tx_step_attn` stays at its zero-initialized value; this case will emit `C3 = 0`. |
| C4 | `C4 = 0;` (`:1020`) | Same |

**Setter discipline.** Setter `set_tx_step_attn_db(int signed_db)`
deferred to §4b-2 — it lives WITH case 11's MOX-gated 6-bit form
because both case 4 and case 11 read the same `prn->adc[0].tx_step_attn`
field.  Defining the setter in §4b-2 ensures the discipline (signed
operator-axis input → `(31 - signed_db)` inversion → store) is
locked alongside the full MOX-gate context.

**VERDICT:** ✅ **PARITY** — verbatim from `:1015-1021`.

---

### §4b-1.3 Case 13 — frame `0x1e` — CW enable + sidetone level + RF delay (source: `networkproto1.c:1127-1133`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x1e;` (`:1128`) → `C0 = XmitBit \| 0x1e` | Same |
| C1 | `C1 = prn->cw.cw_enable;` (`:1129`) — the §1.5 `_cw.mode_control` bitfield bit 1.  Bitfield → `unsigned char` assignment preserves value (0 or 1) | Same |
| C2 | `C2 = prn->cw.sidetone_level;` (`:1130`) — int field from §1.5 | Same |
| C3 | `C3 = prn->cw.rf_delay;` (`:1131`) — int field from §1.5 | Same |
| C4 | `C4 = 0;` (`:1132`) | Same |

**VERDICT:** ✅ **PARITY** — verbatim from `:1127-1133`.

---

### §4b-1.4 Case 14 — frame `0x20` — CW hang_delay + sidetone_freq (source: `networkproto1.c:1135-1141`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x20;` (`:1136`) → `C0 = XmitBit \| 0x20` | Same |
| C1 | `C1 = (prn->cw.hang_delay >> 2) & 0b11111111;` (`:1137`) — upper 8 bits of 10-bit hang_delay | Same |
| C2 | `C2 = (prn->cw.hang_delay & 0b00000011);` (`:1138`) — lower 2 bits of hang_delay | Same |
| C3 | `C3 = (prn->cw.sidetone_freq >> 4) & 0b11111111;` (`:1139`) — upper 8 bits of 12-bit sidetone_freq | Same |
| C4 | `C4 = (prn->cw.sidetone_freq) & 0b00001111;` (`:1140`) — lower 4 bits of sidetone_freq | Same |

**VERDICT:** ✅ **PARITY** — verbatim from `:1135-1141`.

---

### §4b-1.5 Case 15 — frame `0x22` — EER PWM min / max (source: `networkproto1.c:1143-1149`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x22;` (`:1144`) → `C0 = XmitBit \| 0x22` | Same |
| C1 | `C1 = (prn->tx[0].epwm_min >> 2) & 0b11111111;` (`:1145`) — upper 8 bits of 10-bit epwm_min | Same |
| C2 | `C2 = (prn->tx[0].epwm_min & 0b00000011);` (`:1146`) — lower 2 bits | Same |
| C3 | `C3 = (prn->tx[0].epwm_max >> 2) & 0b11111111;` (`:1147`) — upper 8 bits of 10-bit epwm_max | Same |
| C4 | `C4 = (prn->tx[0].epwm_max & 0b00000011);` (`:1148`) — lower 2 bits | Same |

**VERDICT:** ✅ **PARITY** — verbatim from `:1143-1149`.

---

### §4b-1.6 Case 17 — frame `0x2e` — HL2 TX latency + PTT hang (source: `networkproto1.c:1162-1168`)

This is the HL2-enhancement case carrying `prn->tx[0].tx_latency`
and `prn->tx[0].ptt_hang` — the per-§15.26 "0x2e" register that
the prior project's CLAUDE.md §15.7 / §15.26 history tuned for
audio-path latency.  Default values from per-family init.

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x2e;` (`:1163`) → `C0 = XmitBit \| 0x2e` | Same |
| C1 | `C1 = 0;` (`:1164`) | Same |
| C2 | `C2 = 0;` (`:1165`) | Same |
| C3 | `C3 = (prn->tx[0].ptt_hang & 0b00011111);` (`:1166`) — 5-bit PTT hang | Same |
| C4 | `C4 = (prn->tx[0].tx_latency & 0b01111111);` (`:1167`) — 7-bit TX latency | Same |

**VERDICT:** ✅ **PARITY** — verbatim from `:1162-1168`.

---

### §4b-1.7 Case 18 — frame `0x74` — reset on disconnect (source: `networkproto1.c:1170-1176`)

The HL2 safety mechanism — when this bit is set, the HL2 gateware
auto-resets if the host disconnects (e.g. Lyra crashes during TX,
gateware drops the carrier).  The §15.26 "wedge defect" history
documented that this bit defaulting to `1` on a Lyra-stop CAUSED
gateware to interpret clean stop as a disconnect and wedge the
next session — corrected via the §15.26-resolved default-`0`
posture.  Default `0` preserved here per the locked posture; the
field is operator-settable via a setter that lands with §4b-2 (the
TX-policy setter cluster).

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x74;` (`:1171`) → `C0 = XmitBit \| 0x74` | Same |
| C1 | `C1 = 0;` (`:1172`) | Same |
| C2 | `C2 = 0;` (`:1173`) | Same |
| C3 | `C3 = 0;` (`:1174`) | Same |
| C4 | `C4 = prn->reset_on_disconnect;` (`:1175`) | Same |

**VERDICT:** ✅ **PARITY** — verbatim from `:1170-1176`.

---

### §4b-1.8 New setter — `set_tx_freq(int freq_hz)`

Single new setter introduced by §4b-1 (case 1 above).  Pattern
matches §4a's `set_rx_freq`:

```cpp
void FrameComposer::set_tx_freq(int freq_hz) {
    std::lock_guard<std::mutex> guard(cc_lock_);
    if (prn == nullptr) return;
    prn->tx[0].frequency = freq_hz;
    // No payload caching — case 1 reads prn->tx[0].frequency
    // directly at compose time.
}
```

**Other setters deferred:** `set_tx_step_attn_db`, the CW config
setters, the TX-latency / PTT-hang setters, `set_reset_on_disconnect`
— all land with §4b-2 where the policy disciplines (MOX-gate,
force-on-keydown, default-OFF safety) get verified together.

---

### §4b-1 — Overall verdict

| Section | Verdict |
|---|---|
| §3 supplement — `P1_adc_cntrl` added | ✅ PARITY (verbatim `network.h:503`) |
| §4b-1.1 case 1 (TX VFO) | ✅ PARITY verbatim from `:974-980` |
| §4b-1.2 case 4 (TX step attenuator narrow) | ✅ PARITY verbatim from `:1015-1021` |
| §4b-1.3 case 13 (CW enable + sidetone level + rf_delay) | ✅ PARITY verbatim from `:1127-1133` |
| §4b-1.4 case 14 (CW hang_delay + sidetone_freq) | ✅ PARITY verbatim from `:1135-1141` |
| §4b-1.5 case 15 (EER PWM min/max) | ✅ PARITY verbatim from `:1143-1149` |
| §4b-1.6 case 17 (HL2 TX latency + PTT hang) | ✅ PARITY verbatim from `:1162-1168` |
| §4b-1.7 case 18 (reset on disconnect) | ✅ PARITY verbatim from `:1170-1176` |
| §4b-1.8 new setter `set_tx_freq` | ✅ Pattern match with §4a `set_rx_freq` |

**ZERO 🔴 OPERATOR-APPROVED DEVIATIONS. ZERO ⚠ new ACCEPTABLE
DEVIATIONS** beyond what's already locked.  Wire bytes verbatim
for all 7 cases.

---

### §4b-1 — Rule 24 circle-back verification

Per the operator directive 2026-06-05: each checkpoint gets a
second-pass source-line-by-source-line verification before any
code lands.  The above tables WERE drafted from open source
reads (Rule 24 in flight), but the circle-back will re-verify
each row by re-opening the cited lines after operator review
and before commit.

**Circle-back questions to answer before commit:**

1. Re-open `networkproto1.c:974-980` and confirm case 1 body
   verbatim matches §4b-1.1 row table.
2. Re-open `:1015-1021` and confirm the `0b0011111111` 10-bit
   mask quirk in C2 is preserved (this is the kind of thing a
   well-meaning C++ port would "clean up" to `0xFF` — Rule 24
   says don't).
3. Re-open `:1127-1133` and confirm `prn->cw.cw_enable` is the
   bitfield (not a separate int).
4. Re-open `:1135-1149` and confirm the bit-position splits for
   hang_delay / sidetone_freq / epwm_min / epwm_max are exactly
   `(>> 2) & 0xff` + `& 0x03` for the 10-bit splits and `(>> 4) &
   0xff` + `& 0x0f` for the 12-bit sidetone_freq split.
5. Re-open `:1162-1168` and confirm case 17's 5-bit ptt_hang +
   7-bit tx_latency widths.
6. Re-open `:1170-1176` and confirm case 18 reads `prn->reset_on_
   disconnect` directly into C4 (no width mask — implicit `& 0xFF`
   from the int-to-char assignment).
7. Re-open `network.h:503` and confirm `P1_adc_cntrl` is `int`.

---

**OPERATOR SIGN-OFF:**

- [x] §3 supplement — `P1_adc_cntrl` accepted as additional
      reference global (`network.h:503`)
- [x] §4b-1.1 case 1 (TX VFO frame `0x02`) — verbatim from
      `:974-980`
- [x] §4b-1.2 case 4 (TX step attenuator narrow, frame `0x1c`)
      — verbatim from `:1015-1021` including the 10-bit mask
      quirk in C2 preserved as-is
- [x] §4b-1.3 case 13 (CW enable + sidetone_level + rf_delay,
      frame `0x1e`) — verbatim from `:1127-1133`
- [x] §4b-1.4 case 14 (CW hang_delay + sidetone_freq, frame
      `0x20`) — verbatim from `:1135-1141`
- [x] §4b-1.5 case 15 (EER PWM min/max, frame `0x22`) —
      verbatim from `:1143-1149`
- [x] §4b-1.6 case 17 (HL2 TX latency + PTT hang, frame `0x2e`)
      — verbatim from `:1162-1168`
- [x] §4b-1.7 case 18 (reset_on_disconnect, frame `0x74`) —
      verbatim from `:1170-1176`
- [x] §4b-1.8 setter `set_tx_freq` matches §4a `set_rx_freq`
      pattern
- [x] Circle-back Rule 24 re-verify executed by implementor
      against the seven source citations above before commit
- [x] Authorized to populate `src/wire/FrameComposer.{h,cpp}`
      with the 7 case bodies + the new setter + add the
      `P1_adc_cntrl` extern to `RadioNet.h` / definition to
      `RadioNet.cpp`.  Cases 10 / 11 / 12 / 16 (§4b-2) and
      cases 5-9 (§4c) remain `assert(false)` placeholders.

Signed: N8SDR        Date: 2026-06-05

---

*Last updated: 2026-06-05 — §4b-1 simple TX cases signed + Rule-24 circle-back verified clean; populate commit lands separately.*

---

## §4b-2. `FrameComposer` — TX heavyweight cases (10, 11, 12, 16)

**Scope.** Four TX-relevant cases that form the §15.26-history
careful-verification cluster:
- Case 10 frame `0x12` — drive level + Apollo PA bits + mic/line
  + per-band HPF/LPF + `prn->tx[0].pa` bit
- Case 11 frame `0x14` — per-RX preamps + mic_trs/bias/ptt +
  line_in_gain + `puresignal_run` + **MOX-gated step ATT**
- Case 12 frame `0x16` — adc[1]/adc[2] step ATT (XmitBit-force
  on adc[1]) + CW keyer config (iambic / mode_b / keyer_speed /
  keyer_weight / strict_spacing)
- Case 16 frame `0x24` — BPF2 (Alex1 HPFs from `prbpfilter2`) +
  `xvtr_enable` + `puresignal_run` bit

Source mirror: `networkproto1.c::WriteMainLoop_HL2` lines
1076-1089 (case 10), 1091-1103 (case 11), 1105-1123 (case 12),
1151-1160 (case 16).

**§3 supplement — five more globals surfaced by §4b-2:**

`network.h:419` declares `int xvtr_enable;` (case 16); `:435-438`
declare four Apollo PA bit globals consumed inline in case 10's
C2 OR-chain.  Same supplement pattern as §3.4 / §3.5 (added per
Rule 24 once discovered).

| Global | Reference (file:line) | Lyra | Default |
|---|---|---|---|
| `xvtr_enable` | `network.h:419` `int xvtr_enable;` | `extern int xvtr_enable;` | `0` (HL2; no transverter) |
| `ApolloFilt` | `network.h:435` `int ApolloFilt;` | `extern int ApolloFilt;` | `0` (HL2 with no Apollo mod); set per-family at session start when Apollo PA mod is installed |
| `ApolloFiltSelect` | `network.h:436` `int ApolloFiltSelect;` | `extern int ApolloFiltSelect;` | `0` |
| `ApolloTuner` | `network.h:437` `int ApolloTuner;` | `extern int ApolloTuner;` | `0` (HL2 with no Apollo mod); set when Apollo tuner installed |
| `ApolloATU` | `network.h:438` `int ApolloATU;` | `extern int ApolloATU;` | `0` |

**Apollo bits — important note.** In the reference, these are
PRE-SHIFTED flag bits (each carries its bit-position value when
non-zero, not just `1`).  Case 10 OR's them inline into C2.
Per the §15.26 PART C history the HL2 PA-enable on Apollo-modded
gateware uses `ApolloTuner = 0x08` + `ApolloFilt = 0x04` (case
10 C2 bit 3 + bit 2).  The reference's setter (`EnableApolloTuner`
etc.) writes these bit values directly into the globals.  Lyra
preserves the reference pattern: the globals hold the bit-value-
or-zero, NOT a boolean.

---

### §4b-2.1 Case 10 — frame `0x12` — drive level + Apollo + mic + HPF/LPF (source: `networkproto1.c:1076-1089`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x12;` (`:1077`) → `C0 = XmitBit \| 0x12` | Same |
| C1 | `C1 = prn->tx[0].drive_level;` (`:1078`) — pre-SWR-adjusted drive level (operator-facing setter applies SWR correction BEFORE writing this field) | Same — verbatim read of `prn->tx[0].drive_level` |
| C2 | `C2 = ((prn->mic.mic_boost & 1) \| ((prn->mic.line_in & 1) << 1) \| ApolloFilt \| ApolloTuner \| ApolloATU \| ApolloFiltSelect \| 0b01000000) & 0x7f;` (`:1079-1080`) — bit 0 = mic_boost, bit 1 = line_in, Apollo bits OR'd in inline (each global holds its bit-position-or-zero), bit 6 forced ON (`0b01000000`), then masked to 7 bits (bit 7 reserved) | Same — verbatim OR-chain with the bit-6-forced-ON + final `& 0x7f` |
| C3 | `C3 = (prbpfilter->_13MHz_HPF & 1) \| ((prbpfilter->_20MHz_HPF & 1) << 1) \| ((prbpfilter->_9_5MHz_HPF & 1) << 2) \| ((prbpfilter->_6_5MHz_HPF & 1) << 3) \| ((prbpfilter->_1_5MHz_HPF & 1) << 4) \| ((prbpfilter->_Bypass & 1) << 5) \| ((prbpfilter->_6M_preamp & 1) << 6) \| ((prn->tx[0].pa & 1) << 7);` (`:1081-1084`) — bits 0-4 are 5 actual HPF band-select bits (13 / 20 / 9.5 / 6.5 / 1.5 MHz); bit 5 is `_Bypass`; bit 6 is `_6M_preamp`; bit 7 is `prn->tx[0].pa` | Same — verbatim 8-bit OR-chain |
| C4 | `C4 = (prbpfilter->_30_20_LPF & 1) \| ((prbpfilter->_60_40_LPF & 1) << 1) \| ((prbpfilter->_80_LPF & 1) << 2) \| ((prbpfilter->_160_LPF & 1) << 3) \| ((prbpfilter->_6_LPF & 1) << 4) \| ((prbpfilter->_12_10_LPF & 1) << 5) \| ((prbpfilter->_17_15_LPF & 1) << 6);` (`:1085-1088`) — seven per-band LPF bits, bit 7 unused | Same — verbatim 7-bit OR-chain |

**Note on C2 mask `& 0x7f`.** The OR-chain produces a value
that could in principle have bit 7 set (e.g. if an Apollo global
held a value with bit 7).  The trailing `& 0x7f` clears bit 7
unconditionally.  Preserved verbatim.

**VERDICT:** ✅ **PARITY** — verbatim from `:1076-1089`.

---

### §4b-2.2 Case 11 — frame `0x14` — preamps + mic + MOX-gated step ATT (source: `networkproto1.c:1091-1103`)

The MOX-gated step ATT in C4 is the §15.26-history load-bearing
RX-ADC protection mechanism.  When `XmitBit` is set, C4 carries
`prn->adc[0].tx_step_attn` (the operator's TX-axis attenuator);
when not transmitting, C4 carries `prn->adc[0].rx_step_attn`
(operator's RX-axis attenuator).  Both with 6-bit width and the
`0x40` enable bit set.

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x14;` (`:1092`) → `C0 = XmitBit \| 0x14` | Same |
| C1 | `C1 = (prn->rx[0].preamp & 1) \| ((prn->rx[1].preamp & 1) << 1) \| ((prn->rx[2].preamp & 1) << 2) \| ((prn->rx[0].preamp & 1) << 3) \| ((prn->mic.mic_trs & 1) << 4) \| ((prn->mic.mic_bias & 1) << 5) \| ((prn->mic.mic_ptt & 1) << 6);` (`:1093-1096`) — bit 3 reads `rx[0].preamp` AGAIN (after bit 0 already did) — reference quirk preserved verbatim; possibly intentional (bit 3 = ADC0-shared preamp linked to RX[0]) or possibly typo for `rx[3].preamp`; do not "fix" | Same — verbatim including the `rx[0].preamp << 3` duplicate |
| C2 | `C2 = (prn->mic.line_in_gain & 0b00011111) \| ((prn->puresignal_run & 1) << 6);` (`:1097`) — 5-bit line_in_gain + puresignal_run bit at 6 | Same |
| C3 | `C3 = prn->user_dig_out & 0b00001111;` (`:1098`) — 4-bit user_dig_out | Same |
| C4 | MOX-gated (`:1099-1102`): `if (XmitBit) C4 = (prn->adc[0].tx_step_attn & 0b00111111) \| 0b01000000;` else `C4 = (prn->adc[0].rx_step_attn & 0b00111111) \| 0b01000000;` — 6-bit step ATT + `0x40` enable bit, sourced per MOX state | Same — verbatim MOX-gated branch with 6-bit mask + 0x40 OR |

**Reference quirk preserved:** the `rx[0].preamp << 3` at bit 3
of C1 is the same field as bit 0.  Either intentional (a
hardware-level preamp shared across the ADC0 RXes) or a typo
that's been on the wire for years.  Rule 24 says preserve as-is.

**VERDICT:** ✅ **PARITY** — verbatim from `:1091-1103`.

---

### §4b-2.3 Case 12 — frame `0x16` — adc[1]/adc[2] step ATT + CW keyer (source: `networkproto1.c:1105-1123`)

Multi-ADC step attenuator for ANAN-class radios (HL2 has 1 ADC,
so `adc[1]`/`adc[2]` fields are inert on HL2 but case 12 still
emits them).  Plus CW keyer config (iambic mode + speed + weight
+ strict spacing).  C1 has XmitBit-forced 0x1F for `adc[1]`
during TX.

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x16;` (`:1106`) → `C0 = XmitBit \| 0x16` | Same |
| C1 | `if (XmitBit) C1 = 0x1F; else C1 = (prn->adc[1].rx_step_attn);` THEN `C1 \|= 0b00100000;` (`:1107-1111`) — XmitBit forces `adc[1]` to max ATT (0x1F = 31) during TX; otherwise emit the operator's rx_step_attn for ADC1; then OR in the `0x20` enable bit.  **Reference quirk preserved:** the RX branch has NO explicit `& 0x1F` mask on `adc[1].rx_step_attn` — the reference relies on the field being already-5-bit; if it overflows, bit 5+ leakage would conflict with the subsequent `\| 0x20`.  Verbatim per Rule 24; do not add a mask. | Same — verbatim MOX-force then OR-enable pattern (no implicit mask added) |
| C2 | `C2 = (prn->adc[2].rx_step_attn & 0b00011111) \| 0b00100000 \| ((prn->cw.rev_paddle & 1) << 6);` (`:1112-1113`) — 5-bit `adc[2].rx_step_attn` + 0x20 enable bit + CW rev_paddle at bit 6 | Same |
| CWMode computation | Lines `:1115-1120`: `if (prn->cw.iambic == 0) CWMode = 0b00000000; else if (prn->cw.mode_b == 0) CWMode = 0b01000000; else CWMode = 0b10000000;` — 3-way conditional encoding iambic + mode_b into 2 bits of CWMode | Same — verbatim 3-way conditional |
| C3 | `C3 = (prn->cw.keyer_speed & 0b00111111) \| CWMode;` (`:1121`) — 6-bit keyer_speed + 2-bit CWMode at bits 6-7 | Same |
| C4 | `C4 = (prn->cw.keyer_weight & 0b01111111) \| ((prn->cw.strict_spacing) << 7);` (`:1122`) — 7-bit keyer_weight + strict_spacing bit at 7.  **Reference quirk preserved:** no `& 1` mask on `strict_spacing`.  Harmless because `strict_spacing` is a 1-bit bitfield (§1.5 `_cw.mode_control`), so the unmasked `<< 7` always yields 0 or 0x80.  Preserved verbatim per Rule 24. | Same — no mask added on strict_spacing |

**VERDICT:** ✅ **PARITY** — verbatim from `:1105-1123`.

---

### §4b-2.4 Case 16 — frame `0x24` — BPF2 (Alex1 HPFs) + xvtr_enable + puresignal_run (source: `networkproto1.c:1151-1160`)

Alex1 secondary band-pass filter board — same HPF layout as
case 10 C3 but reading from `prbpfilter2` (the §2 Alex1 struct)
instead of `prbpfilter`.

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x24;` (`:1152`) → `C0 = XmitBit \| 0x24` | Same |
| C1 | `C1 = (prbpfilter2->_13MHz_HPF & 1) \| ((prbpfilter2->_20MHz_HPF & 1) << 1) \| ((prbpfilter2->_9_5MHz_HPF & 1) << 2) \| ((prbpfilter2->_6_5MHz_HPF & 1) << 3) \| ((prbpfilter2->_1_5MHz_HPF & 1) << 4) \| ((prbpfilter2->_Bypass & 1) << 5) \| ((prbpfilter2->_6M_preamp & 1) << 6) \| ((prbpfilter2->_rx2_gnd) << 7);` (`:1153-1156`) — six Alex1 HPF bits + 6M preamp + `_rx2_gnd` at bit 7 (note: NOT `& 1` on `_rx2_gnd` per the reference — preserved verbatim) | Same — verbatim 8-bit OR-chain including the `_rx2_gnd` without `& 1` mask quirk |
| C2 | `C2 = (xvtr_enable & 1) \| ((prn->puresignal_run & 1) << 6);` (`:1157`) — bit 0 = xvtr_enable, bit 6 = puresignal_run | Same |
| C3 | `C3 = 0;` (`:1158`) | Same |
| C4 | `C4 = 0;` (`:1159`) | Same |

**Reference quirk preserved:** the `_rx2_gnd << 7` is the only
bit in the Alex1 HPF byte that lacks an explicit `& 1` mask.
Functional effect: if `_rx2_gnd` is ever multi-bit (it shouldn't
be — it's a 1-bit bitfield from §2), the shift would propagate
those bits into C2/C3 of the result.  Reference behavior, preserved
as-is.

**VERDICT:** ✅ **PARITY** — verbatim from `:1151-1160`.

---

### §4b-2.5 Setters introduced by §4b-2

The setter discipline established in §4a (each FrameComposer
setter takes `cc_lock_`, writes the relevant `prn->*` field,
relies on the per-frame switch reading the field at compose time)
extends to §4b-2.  Four new setters:

| Setter | Writes | Reference encoding | Per-row notes |
|---|---|---|---|
| `set_drive_level(int level)` | `prn->tx[0].drive_level = level;` | Reference operator-facing setter applies SWR correction at the call site BEFORE writing this field (`SetOutputPowerFactor`); composer just emits the field verbatim | Lyra-level SWR correction is operator-policy (deferred); §4b-2 setter takes the raw level |
| `set_pa_on(bool on)` | `prn->tx[0].pa = on ? 1 : 0;` | Case 10 C3 bit 7 reads `prn->tx[0].pa & 1` | Boolean → 0/1; case 10 reads via the wire |
| `set_tx_step_attn_db(int signed_db)` | HL2: `prn->adc[0].tx_step_attn = (31 - signed_db) & 0x3F;`  Non-HL2: `prn->adc[0].tx_step_attn = signed_db & 0x3F;` | Source-verified `console.cs:10657-10663`: the `(31 - x)` inversion is **HL2-FAMILY-SPECIFIC** — only when `HardwareSpecific.Model == HPSDRModel.HERMESLITE`.  ANAN / Orion / RedPitaya use the raw operator value.  The setter must branch on `hpsdrModel == HPSDRModel::HERMESLITE` and apply the inversion only on the HL2 branch (with non-HL2 paths `assert(false)` per §3.6 until tester hardware arrives). | This is the §15.26 history's load-bearing TX-att encoding.  6-bit storage holds the encoded value; case 4 emits 5-bit truncated form, case 11 emits 6-bit + `0x40` enable.  See "PureSignal + panadapter entanglements" note below for the subsystems that share this actuator. |
| `set_rx_step_attn_db(int signed_db, int adc_idx = 0)` | `prn->adc[adc_idx].rx_step_attn = signed_db & 0x3F;` (no inversion for RX on any family) | Case 11 emits `adc[0].rx_step_attn` for RX path; cases 12 C1/C2 emit `adc[1]`/`adc[2].rx_step_attn` | Operator-axis dB matches wire encoding for RX (no inversion); 6-bit clamp |

**PureSignal + panadapter entanglements of `prn->adc[0].tx_step_attn`
(source-verified `console.cs`):**

The TX-att field is a **shared actuator** with four subsystems
writing it.  Single-writer discipline is critical.  These are
ALL deferred to operator-policy work outside §4b-2 — listed here
so the design ledger has the dependency map:

1. **Manual ATT-on-TX** (`SetupForm.ATTOnTX` setter `console.cs:
   10645+`).  Writes operator-set value (with HL2 inversion).
2. **QSK keydown-force-31** (`console.cs:13078`).  When CW QSK
   fires keydown: save current state + force ATTOnTX=true +
   force value=31.  On keyup (line 13089): restore.  Keydown/keyup
   are Ptt-FSM concerns in Lyra.
3. **PureSignal calibration auto-attenuator** (the v0.3 PS state
   machine).  PS writes the field during PS-on-TX state as part
   of its calibration loop.
4. **"Force 31 when PS-A off" safety** (`chkForceATTwhenPSAoff`,
   per the operator screenshot from the §15.26 work).  When PS
   is off, force ATTOnTX value to 31 for max RX-ADC protection.

**`Display.TXAttenuatorOffset` — panadapter / waterfall display
compensation** (`console.cs:10662 + 10665 + 19169 + 19174`):

When ATT-on-TX engages, the local RX ADC sees the transmitted
signal attenuated by N dB.  Without compensation, the panadapter
shows the signal as N dB lower than reality.  The reference
sets `Display.TXAttenuatorOffset = _tx_attenuator_data` (the
operator-axis dB, **not** the wire-encoded value) so the
panadapter/waterfall renderer can ADD BACK the attenuation when
rendering TX-state samples.  When ATT-on-TX is OFF, the offset
resets to 0.

**Lyra equivalent (deferred to §4b-2-followup operator-policy
commit):** add a Radio-layer signal `tx_attenuator_offset_db`
that the panadapter widget subscribes to.  The setter sequence:
- `set_tx_step_attn_db(signed_db)` writes the wire field (§4b-2)
- `set_att_on_tx(bool enable)` policy gate updates the offset
  signal and writes 0 to wire when disabled
- Lyra panadapter / waterfall widget reads the offset signal to
  compensate display levels during TX state (lands with the
  TX-state panadapter port; mirrors §15.29 / §15.30 work)

**Other deferred operator-policy items** (not reference-parity,
not §4b-2 scope):

- ATT-on-TX `m_bATTonTX` policy gate (when OFF, setter writes 0
  to wire) — Settings → TX checkbox; default per operator's
  recorded preference (the screenshot shows operator runs
  ATTOnTX=true with value=31)
- ATT-on-TX QSK keydown-force / keyup-restore — Ptt-FSM
- ATT-on-TX "Force 31 when PS-A off" + "AutoAttTXWhenNotInPS"
  policies — Settings → TX, PS-aware
- PS auto-attenuator state machine — v0.3 PureSignal scope
- Apollo bit setters — `ApolloFilt`/`ApolloTuner`/`ApolloATU`/
  `ApolloFiltSelect` are per-family init values, set once at
  session start; Lyra defaults all to 0 (PA OFF — the §15.26-
  locked safety default; PA-ON discipline lands as a separate
  operator-opt-in commit per §15.26 PART C)
- CW keyer config setters (iambic, mode_b, keyer_speed,
  keyer_weight, sidetone, rf_delay, hang_delay, strict_spacing,
  cw_enable, rev_paddle) — operator-UI plumbing, lands with the
  Settings → CW panel
- `xvtr_enable` setter — transverter operator UI, lands with the
  TX bandwidth / profile work
- mic_boost / line_in / mic_trs / mic_bias / mic_ptt / line_in_gain
  / user_dig_out setters — operator-UI plumbing
- `set_pa_on(bool)` policy gating (Settings → TX "Enable PA"
  default-OFF, force-off on session start, etc.) — operator-
  facing safety policy; FrameComposer setter just writes the
  ApolloTuner / ApolloFilt globals

---

### §4b-2 — Overall verdict (pending Rule 24 circle-back)

| Section | Verdict |
|---|---|
| §3 supplement — 5 new globals (xvtr_enable + 4 Apollo) | ✅ PARITY (verbatim `network.h:419, 435-438`) |
| §4b-2.1 case 10 (frame `0x12`) | ✅ PARITY verbatim from `:1076-1089` |
| §4b-2.2 case 11 (frame `0x14`) | ✅ PARITY verbatim from `:1091-1103` incl. `rx[0].preamp << 3` quirk preserved + MOX-gated step ATT |
| §4b-2.3 case 12 (frame `0x16`) | ✅ PARITY verbatim from `:1105-1123` incl. XmitBit-force `0x1F` for adc[1] + CW keyer 3-way CWMode |
| §4b-2.4 case 16 (frame `0x24`) | ✅ PARITY verbatim from `:1151-1160` incl. `_rx2_gnd << 7` without `& 1` mask quirk preserved |
| §4b-2.5 new setters (4 of them) | ✅ Pattern matches §4a / §4b-1 setters; `set_tx_step_attn_db` uses the §15.26-documented `(31 - signed_db)` encoding |

**ZERO 🔴 OPERATOR-APPROVED DEVIATIONS. ZERO ⚠ new ACCEPTABLE
DEVIATIONS** beyond what's already locked.  All 4 case bodies
verbatim from source.

---

### §4b-2 — Rule 24 circle-back items

Before sign-off + commit, the following will be re-verified by
re-opening the cited lines (Rule 24 discipline):

1. `networkproto1.c:1076-1089` — case 10: confirm C2 OR-chain
   bit-position of each Apollo global (preserve the literal
   `ApolloFilt \| ApolloTuner \| ApolloATU \| ApolloFiltSelect`
   ordering — operator may verify each global's bit-position
   intent from `netInterface.c` Apollo setters in a follow-up,
   but the case body just OR's them as-given).
2. `:1091-1103` — case 11: re-confirm the `rx[0].preamp << 3`
   quirk + the 6-bit `0x3F` mask + the `0x40` enable bit on
   both MOX branches.
3. `:1105-1123` — case 12: re-confirm the `XmitBit ? 0x1F :
   adc[1].rx_step_attn` 3-step pattern (assign then OR `0x20`),
   the `adc[2]` 5-bit mask + `0x20` enable + rev_paddle bit, and
   the CWMode 3-way conditional encoding into bits 6-7.
4. `:1151-1160` — case 16: re-confirm the `_rx2_gnd << 7`
   without `& 1` and the C2 `xvtr_enable` + `puresignal_run<<6`.
5. `network.h:419, 435-438` — re-confirm the 5 new globals are
   all `int`.

---

**OPERATOR SIGN-OFF:**

- [x] §3 supplement — 5 new globals (`xvtr_enable`,
      `ApolloFilt`, `ApolloFiltSelect`, `ApolloTuner`,
      `ApolloATU`) accepted, all `int`, all default `0`
- [x] §4b-2.1 case 10 (frame `0x12`) — verbatim from
      `:1076-1089` including the Apollo OR-chain + `& 0x7f`
      final mask + the precise C3 bit layout (5 HPFs + Bypass
      + 6M_preamp + tx_pa) + 7-bit LPF byte
- [x] §4b-2.2 case 11 (frame `0x14`) — verbatim from
      `:1091-1103` including the `rx[0].preamp << 3` reference
      quirk preserved + the MOX-gated 6-bit step ATT + `0x40`
      enable bit
- [x] §4b-2.3 case 12 (frame `0x16`) — verbatim from
      `:1105-1123` including the XmitBit-force-`0x1F` for adc[1]
      + no-`& 0x1F`-mask on adc[1] RX branch + no-`& 1`-mask on
      strict_spacing (both reference quirks preserved) +
      CW keyer 3-way CWMode encoding
- [x] §4b-2.4 case 16 (frame `0x24`) — verbatim from
      `:1151-1160` including the `_rx2_gnd << 7` no-mask quirk
- [x] §4b-2.5 setters — `set_tx_step_attn_db` per-family
      branched (HL2 `(31 - signed_db) & 0x3F`; non-HL2 raw +
      `assert(false)` placeholder), `set_rx_step_attn_db`
      no-inversion, `set_drive_level`, `set_pa_on` (legacy
      C3 bit 7; Apollo bit policy deferred to Task #114)
- [x] Operator-policy work deferred to Task #114 — ATT-on-TX
      m_bATTonTX gate, QSK keydown-force/keyup-restore,
      Display.TXAttenuatorOffset panadapter compensation,
      AutoAttTXWhenNotInPS, "Force 31 when PS-A off",
      PA-enable Settings → TX safety, Apollo bit per-family
      init, CW/mic/line operator-UI plumbing, PS auto-
      attenuator (v0.3 PureSignal subsystem)
- [x] Circle-back Rule 24 re-verified TWICE by implementor
      against the five source spans (`:1076-1089`, `:1091-
      1103`, `:1105-1123`, `:1151-1160`, `network.h:419 +
      :435-438`); two independent passes both clean
- [x] Authorized to populate `src/wire/FrameComposer.{h,cpp}`
      with the 4 case bodies + 4 setters + add the 5 new
      `extern` globals to `RadioNet.h` / definitions to
      `RadioNet.cpp`.  Cases 5-9 (§4c) remain `assert(false)`
      placeholders.  With §4b-2 populated, the only remaining
      `assert(false)` cases are 5-9 (§4c); FrameComposer is one
      checkpoint away from fully callable.

Signed: N8SDR        Date: 2026-06-05

---

*Last updated: 2026-06-05 — §4b-2 TX heavyweight cases signed + 2× Rule-24 circle-back verified clean; populate commit lands separately.*

---

## §4c. `FrameComposer` — RX-mirror / ANAN-only cases (5, 6, 7, 8, 9)

**Scope.** The last 5 cases of FrameComposer's 19-case round-robin.
All 5 are structurally simple — a single 32-bit big-endian
frequency write to C1-C4 — differing only in WHICH source
field they read (per-family conditional or fixed source).

| Case | Frame | DDC | HL2 (nddc=4) behavior | Source field |
|---|---|---|---|---|
| 5 | `0x08` | DDC2 | TX-mirror (nddc=5 Orion would use rx[3]) | `if (nddc == 5) prn->rx[3].frequency else prn->tx[0].frequency` |
| 6 | `0x0a` | DDC3 | TX-mirror always | `prn->tx[0].frequency` always |
| 7 | `0x0c` | DDC4 | TX-mirror (Orion2 PS-feedback use) | `prn->tx[0].frequency` always |
| 8 | `0x0e` | DDC5 | Unused on HL2; emits rx[0] freq as benign default | `prn->rx[0].frequency` always |
| 9 | `0x10` | DDC6 | Unused on HL2; emits rx[0] freq as benign default | `prn->rx[0].frequency` always |

Source mirror: `networkproto1.c::WriteMainLoop_HL2` lines
1023-1034 (case 5), 1036-1044 (case 6), 1046-1054 (case 7),
1056-1064 (case 8), 1066-1074 (case 9).  Bytes C1-C4 are
identical big-endian 32-bit splits in all 5 cases; only `C0 |=`
constant + ddc_freq source vary.

**No new globals.**  All sources already in `prn->*` per §1.
**No new setters.**  `set_tx_freq` (§4b-1) and `set_rx_freq`
(§4a) cover all source fields; case 5's nddc=5 path reads
`prn->rx[3].frequency` which `set_rx_freq(3, hz)` writes.

**HL2 behavior summary:** on HL2 (nddc=4), cases 5/6/7 all emit
`prn->tx[0].frequency` (TX mirror — the HL2 gateware uses
DDC2/DDC3/DDC4 to track TX freq for PS feedback purposes per
CLAUDE.md §3.1).  Cases 8/9 emit `prn->rx[0].frequency` (RX1
freq as a benign default — DDC5/DDC6 are not used on HL2 but
the reference emits a real freq anyway to avoid garbage on the
wire).

---

### §4c.1 Case 5 — frame `0x08` — RX3 VFO (DDC2) (source: `networkproto1.c:1023-1034`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 8;` (`:1024`) → `C0 = XmitBit \| 0x08` | Same |
| ddc_freq selection | `if (nddc == 5) ddc_freq = prn->rx[3].frequency; else ddc_freq = prn->tx[0].frequency;` (`:1026-1029`) — Orion (nddc=5) → DDC2 carries RX2 freq stored at `rx[3]`; HL2 / Hermes (nddc=2/4) → DDC2 carries TX freq | Same — preserved verbatim including the **`rx[3]` indexing** for Orion (reference comment says "DDC2 is RX2 frequency" but array index is 3; reference quirk preserved — likely a per-family RX-index convention where Orion's "RX2" is stored at array slot 3 for some hardware-driven reason; Rule 24 says don't reinterpret) |
| C1 / C2 / C3 / C4 | Big-endian 32-bit `ddc_freq` (`:1030-1033`) | Same — `freq_be32(ddc_freq)` 4-byte BE encoding |

**VERDICT:** ✅ **PARITY** — verbatim from `:1023-1034`.

---

### §4c.2 Case 6 — frame `0x0a` — RX4 VFO (DDC3) (source: `networkproto1.c:1036-1044`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x0a;` (`:1037`) | Same |
| ddc_freq | `ddc_freq = prn->tx[0].frequency;` (`:1039`) — DDC3 is always TX frequency on all families | Same — unconditional read of `prn->tx[0].frequency` |
| C1 / C2 / C3 / C4 | Big-endian 32-bit (`:1040-1043`) | Same — 4-byte BE encoding |

**VERDICT:** ✅ **PARITY** — verbatim from `:1036-1044`.

---

### §4c.3 Case 7 — frame `0x0c` — RX5 VFO (DDC4) (source: `networkproto1.c:1046-1054`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x0c;` (`:1047`) | Same |
| ddc_freq | `ddc_freq = prn->tx[0].frequency;` (`:1049`) — DDC4 is TX-frequency for Orion2 PS feedback; for other families "not used, so make TX always" per the reference comment | Same — unconditional read of `prn->tx[0].frequency` |
| C1 / C2 / C3 / C4 | Big-endian 32-bit (`:1050-1053`) | Same |

**VERDICT:** ✅ **PARITY** — verbatim from `:1046-1054`.

---

### §4c.4 Case 8 — frame `0x0e` — RX6 VFO (DDC5) (source: `networkproto1.c:1056-1064`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x0e;` (`:1057`) | Same |
| ddc_freq | `ddc_freq = prn->rx[0].frequency;` (`:1059`) — DDC5 is "not used" per the reference comment; reference emits `rx[0].frequency` as the benign default (a real freq, not garbage) | Same — unconditional read of `prn->rx[0].frequency` |
| C1 / C2 / C3 / C4 | Big-endian 32-bit (`:1060-1063`) | Same |

**VERDICT:** ✅ **PARITY** — verbatim from `:1056-1064`.

---

### §4c.5 Case 9 — frame `0x10` — RX7 VFO (DDC6) (source: `networkproto1.c:1066-1074`)

| Byte | Reference | Lyra |
|---|---|---|
| C0 | `C0 \|= 0x10;` (`:1067`) | Same |
| ddc_freq | `ddc_freq = prn->rx[0].frequency;` (`:1069`) — DDC6 is "not used" per the reference comment; reference emits `rx[0].frequency` as the benign default | Same — unconditional read of `prn->rx[0].frequency` |
| C1 / C2 / C3 / C4 | Big-endian 32-bit (`:1070-1073`) | Same |

**VERDICT:** ✅ **PARITY** — verbatim from `:1066-1074`.

---

### §4c — Overall verdict

| Section | Verdict |
|---|---|
| §4c.1 case 5 (frame `0x08`) | ✅ PARITY verbatim from `:1023-1034`; nddc=5 conditional + Orion `rx[3]` indexing quirk preserved |
| §4c.2 case 6 (frame `0x0a`) | ✅ PARITY verbatim from `:1036-1044`; always-TX |
| §4c.3 case 7 (frame `0x0c`) | ✅ PARITY verbatim from `:1046-1054`; always-TX |
| §4c.4 case 8 (frame `0x0e`) | ✅ PARITY verbatim from `:1056-1064`; always-rx[0] benign default |
| §4c.5 case 9 (frame `0x10`) | ✅ PARITY verbatim from `:1066-1074`; always-rx[0] benign default |

**ZERO 🔴 OPERATOR-APPROVED DEVIATIONS. ZERO new ⚠ ACCEPTABLE
DEVIATIONS** beyond what's already locked.  Wire bytes verbatim
for all 5 cases.

**No new globals, no new setters.**  `set_tx_freq` (§4b-1) and
`set_rx_freq` (§4a — extended to support `rx_idx=3` for case 5's
Orion path) cover all source fields.

**With §4c populated, all 19 FrameComposer cases are populated.**
The `assert(false)` placeholders for cases 5-9 get replaced with
the new compose helpers.  FrameComposer becomes fully callable
end-to-end.  Wire-INERT discipline still applies because no
caller invokes `write_main_loop_hl2` yet — that hookup lands
with §7 Ep2SendThread.

---

### §4c — Rule 24 circle-back items

Re-verify before commit:

1. `:1023-1034` case 5: confirm `nddc == 5` conditional + `rx[3]` array index in the Orion path
2. `:1036-1044` case 6: always `tx[0].frequency`
3. `:1046-1054` case 7: always `tx[0].frequency`
4. `:1056-1064` case 8: always `rx[0].frequency`
5. `:1066-1074` case 9: always `rx[0].frequency`
6. All five cases: `C0 |= <constant>` value matches (0x08, 0x0a, 0x0c, 0x0e, 0x10 respectively) + the 4-byte BE encoding pattern matches §4a / §4b-1 (`(ddc_freq >> N) & 0xff`)

---

**OPERATOR SIGN-OFF:**

- [x] §4c.1 case 5 (frame `0x08` RX3/DDC2) — verbatim from
      `:1023-1034` including the nddc=5 conditional that reads
      `prn->rx[3].frequency` (Orion path) and the default
      `prn->tx[0].frequency` (HL2 / Hermes path)
- [x] §4c.2 case 6 (frame `0x0a` RX4/DDC3) — verbatim from
      `:1036-1044`; unconditional TX-freq
- [x] §4c.3 case 7 (frame `0x0c` RX5/DDC4) — verbatim from
      `:1046-1054`; unconditional TX-freq
- [x] §4c.4 case 8 (frame `0x0e` RX6/DDC5) — verbatim from
      `:1056-1064`; unconditional `rx[0].frequency` benign default
- [x] §4c.5 case 9 (frame `0x10` RX7/DDC6) — verbatim from
      `:1066-1074`; unconditional `rx[0].frequency` benign default
- [x] Circle-back Rule 24 re-verify executed before commit
      (all 5 cases verified verbatim against source — zero defects)
- [x] Authorized to populate `src/wire/FrameComposer.{h,cpp}`
      with 5 new compose helpers + update the switch to call
      them instead of asserting.  With §4c populated, ALL 19
      cases are filled — FrameComposer becomes fully callable
      end-to-end (still wire-inert because no caller hooks it
      up until §7 Ep2SendThread).

Signed: N8SDR        Date: 2026-06-05

---

*Last updated: 2026-06-05 — §4c FrameComposer RX-mirror cases signed + Rule-24 circle-back verified clean; populate commit lands separately.  All 19 FrameComposer cases populated post-commit.*

---

## §5. `Ep6RecvThread` + `Router` — EP6 datagram receive loop + dispatch primitives

**Scope.** The EP6 receive thread: UDP `recvfrom` loop, watchdog,
per-USB-frame parse (sync-bytes + C0..C4 extraction), I2C readback,
EP6 telemetry decode (5 status-class switch), per-DDC IQ unpacking,
**inline `switch (nddc)` DDC routing** (the former §5 DdcMap home —
collapsed inline per the locked 2026-06-05 discipline), mic
decimation + harvest, and the `Router` (`xrouter` + `twist`)
Lyra-native dispatch primitives.

Source mirror: `networkproto1.c::MetisReadThreadMainLoop_HL2`
(lines 422-586) + `networkproto1.c::MetisReadDirect` (lines
141-168+) + `networkproto1.c::twist` (lines 263-274) + `router.c::
xrouter` (line 71+).  HL2 / HL2+ dispatch; ANAN-class branches
(nddc=2 Hermes-II, nddc=5 Orion) are present in the inline switch
verbatim from source.

**Locked architecture (Q1–Q5).**  Operator-confirmed 2026-06-05;
all answers default to "do as the reference does" per the locked
discipline.

| # | Question | Locked answer |
|---|---|---|
| Q1 | Read-loop structure | **Switch-style read loop matching the reference verbatim.**  Lyra implements `Ep6RecvThread::run_loop()` as a near-line-for-line port of `MetisReadThreadMainLoop_HL2`.  Outer `while (io_keep_running)` + WSA-equivalent socket wait + per-USB-frame for-loop + per-DDC iq-unpack + DDC-routing switch + mic harvest.  All structural elements verbatim. |
| Q2 | `Router` as separate file? | **Yes — `src/wire/Router.{h,cpp}`** as a separate Lyra component, matching the reference's `router.c` / `router.h` file separation.  Implements `xrouter()` + `twist()` as free functions in `lyra::wire::` namespace.  Per the locked discipline (reference has the dispatch primitives in their own file, Lyra does the same). |
| Q3 | xrouter implementation completeness | **Full reference clone** — including the runtime-configurable callback tables + control word + multi-stream-per-port dispatch.  Per the locked discipline (mirror the reference's complexity even if v0.0.x HL2-RX-only doesn't exercise all paths today).  Pays back when v0.3 PS work re-routes DDC outputs via the control-word mechanism without infrastructure rebuild. |
| Q4 | Where do `RxBuff` / `TxReadBufp` / `ControlBytesIn` / thread-state live? | **Inside `Ep6RecvThread` as members** — honoring the signed §1.1 networking-infrastructure exclusion from `RadioNet`.  `RxBuff[nddc][]` is per-DDC IQ sample staging; `TxReadBufp[]` is mic sample staging; `ControlBytesIn[5]` is the per-frame C0..C4 buffer.  Same pattern as FrameComposer's `out_control_idx_` / `previous_tx_bit_` (scheduler-internal state). |
| Q5 | Synchronization | One `std::mutex recv_lock_` member of `Ep6RecvThread` ↔ reference's `prn->rcvpktp1` (the P1 receive critical section from §1.11).  Lock-order acyclic per §15.26 W1.3 / W1.4 lessons. |

**§3 supplement — additional globals surfaced by §5:**

| Global | Reference (file:line) | Lyra | Default |
|---|---|---|---|
| `mic_decimation_factor` | `network.h:507` `int mic_decimation_factor;` — divisor for mic-sample harvest cadence (mic comes from EP6 at the full 48 kHz IQ rate; per-family decimator drops to operator-target rate) | `extern int mic_decimation_factor;` | `1` (no decimation; per-family init or operator rate-set overwrites) |
| `mic_decimation_count` | `network.h:508` `int mic_decimation_count;` — running counter for the decimator state | `extern int mic_decimation_count;` | `0` |

---

### §5.1 Thread lifecycle + UDP recvfrom + watchdog (source: `networkproto1.c:422-463`)

The outer read loop's structural elements.

| Aspect | Reference | Lyra |
|---|---|---|
| Function entry | `void MetisReadThreadMainLoop_HL2(void)` (`:422`) | `void Ep6RecvThread::run_loop()` — runs on the spawned `std::thread`; MMCSS Pro-Audio priority set in the thread-start wrapper |
| Initial state | `mic_decimation_count = 0; SeqError = 0;` (`:424-425`) | Same — reset both at loop entry |
| Buffer allocations | `FPGAReadBufp = calloc(1024, 1); FPGAWriteBufp = calloc(1024, 1);` (`:427-428`) | `fpga_read_buf_.resize(1024); fpga_write_buf_.resize(1024);` (`std::vector<unsigned char>` members; same 1024-byte size matching the EP6 datagram + sync overhead) |
| Priming | `ForceCandCFrame(3);` (`:430`) — 3-frame priming burst before the main loop | Same — invokes `ForceCandC::prime(3)` (§7 component; for §5 this is a stub call) |
| Event setup | `prn->hDataEvent = WSACreateEvent(); WSAEventSelect(listenSock, prn->hDataEvent, FD_READ);` (`:433-434`) | Lyra-native equivalent — Linux/cross-platform `recvfrom` with `MSG_DONTWAIT` or `poll`-based; Windows-specific `WSAEventSelect` semantics are an OS-platform deviation acceptable per C → C++23 cross-platform port |
| Main loop | `while (io_keep_running != 0)` (`:439`) | `while (io_keep_running_)` — same; `io_keep_running_` is an `std::atomic<int>` Ep6RecvThread member (instead of file-scope global) |
| Watchdog | `WSAWaitForMultipleEvents(1, &prn->hDataEvent, FALSE, prn->wdt ? 3000 : WSA_INFINITE, FALSE);` (`:443`).  On timeout: `HaveSync = 0; destroy_pro(prop); prop = NULL; continue;` (`:446-449`) | Same — 3000 ms timeout when `prn->wdt` is set, infinite wait otherwise; on timeout clear `have_sync_` flag + invoke `destroy_pro_stub_()` placeholder + continue.  `destroy_pro` is sync-discovery cleanup; Lyra stubs until discovery component lands. |
| Network-event enumeration | `WSAEnumNetworkEvents(...); if (events & FD_READ)` (`:453-454`) | Lyra-native `recvfrom` (no need for explicit event enumeration; `poll` or `recvfrom`'s own success indicates data ready) |
| Read | `MetisReadDirect(FPGAReadBufp);` (`:463`) — UDP `recvfrom` wrapper with seq-number tracking | `metis_read_direct(fpga_read_buf_.data())` Lyra-native equivalent (sub-§5.2) |

**Reference quirk preserved:** the buffers allocated by `calloc(1024, 1)` are 1024 bytes — the EP6 datagram is 1032 bytes (8 header + 2 × 512 USB frames), so the reference's allocation is technically 8 bytes short.  In practice the trailing 8 bytes are the metis/sync header that gets consumed by `MetisReadDirect` before the body is copied into `FPGAReadBufp`.  Lyra uses 1024 bytes verbatim per Rule 24.

**VERDICT:** ✅ **PARITY** — structural elements verbatim from `:422-463`; ⚠ **ACCEPTABLE DEVIATION** on the Windows-specific WSA event machinery → cross-platform `recvfrom`/`poll` (idiom translation, same semantic effect).

---

### §5.2 EP6 frame validation + C0..C4 extraction (source: `:470-476`)

| Aspect | Reference | Lyra |
|---|---|---|
| Per-USB-frame loop | `for (frame = 0; frame < 2; frame++) { bptr = FPGAReadBufp + 512 * frame; ...` (`:470-472`) | Same — outer for-loop, 2 USB frames per UDP datagram |
| Sync-bytes validation | `if ((bptr[0] == 0x7f) && (bptr[1] == 0x7f) && (bptr[2] == 0x7f))` (`:473`) | Same — all three `0x7f` bytes required at offsets [0..2]; if invalid, the entire USB frame is silently dropped (matches reference behavior — no error logging on sync-byte mismatch) |
| C0..C4 extraction | `for (cb = 0; cb < 5; cb++) ControlBytesIn[cb] = bptr[cb + 3];` (`:475-476`) — copies 5 bytes from offsets [3..7] into the `ControlBytesIn[5]` buffer | Same — verbatim 5-byte copy into `control_bytes_in_[5]` member |

**VERDICT:** ✅ **PARITY** — verbatim from `:470-476`.

---

### §5.3 I2C readback path (source: `:478-493`)

When C0 bit 7 is set, the EP6 frame is an I2C-readback response (not a normal status frame).  Stores 4 bytes of read data into `prn->i2c.read_data` and sets the `ctrl_read_available` flag.  Reference defect / nuance: when the returned address is `0x3f` (i.e. the gateware's "no response" indicator), the read-error flag is set instead.

| Aspect | Reference | Lyra |
|---|---|---|
| Trigger | `if (ControlBytesIn[0] & 0x80)` (`:478`) | Same |
| Error-address check | `if (0x3f == prn->i2c.returned_address) { prn->i2c.ctrl_error = 1; }` (`:480-483`) | Same — verbatim |
| Normal readback | `prn->i2c.read_data[0..3] = ControlBytesIn[1..4]; prn->i2c.ctrl_read_available = 1;` (`:486-491`) | Same |

**VERDICT:** ✅ **PARITY** — verbatim from `:478-493`.

---

### §5.4 EP6 telemetry decode — 5 status-class switch (source: `:494-525`)

The normal-status path (C0 bit 7 clear).  Extracts PTT/dot/dash, then dispatches a `switch (C0 & 0xf8)` for the 5 telemetry-class subdivisions.

**This is the §15.26-history PA-current / PA-volts / supply-volts
slot-mapping territory** — the operator's PRIOR-PROJECT CORRECTION-3
work established the correct slot map on his HL2+ AK4951 gateware.
Per Rule 24 source-read here: the slot map IS the one the
reference declares; the prior project's empirical-bench finding
that "PA-current reads n/a on this gateware variant" may STILL
apply (the gateware doesn't emit `user_adc1` on this rev) — that's
a HARDWARE finding, NOT a source-decode finding.  Lyra emits the
exact reference decode; what the gateware delivers in each slot is
an empirical-bench concern.

| C0 mask | Reference (file:line) | Lyra |
|---|---|---|
| PTT / dot / dash always extracted | `prn->ptt_in = ControlBytesIn[0] & 0x1; prn->dash_in = (ControlBytesIn[0] << 1) & 0x1; prn->dot_in = (ControlBytesIn[0] << 2) & 0x1;` (`:496-498`) — **note the `<< 1` / `<< 2` are LEFT shifts then `& 0x1` mask = always 0** (reference defect; the intent was probably `>> 1` / `>> 2` to extract bits 1 and 2.  Preserved verbatim per Rule 24 — Lyra emits the same broken decode the reference does.  Operator-policy work in Task #114 can layer a Lyra-native "actually decode bits 1 and 2 correctly" wrapper if dot/dash via EP6 telemetry is ever wanted; but the locked discipline is "do as the reference does, including bugs.") | Same — verbatim left-shift-then-mask |
| `0x00` — primary status | `prn->adc[0].adc_overload = ControlBytesIn[1] & 0x01; prn->user_dig_in = ((ControlBytesIn[1] >> 1) & 0xf);` (`:501-504`) — ADC0 overload + user_dig_in 4-bit field | Same |
| `0x08` — power telemetry I | `prn->tx[0].exciter_power = (CB[1] << 8 \| CB[2]) & 0xffff;` (AIN5 drive power); `prn->tx[0].fwd_power = (CB[3] << 8 \| CB[4]) & 0xffff;` (AIN1 PA coupler).  Plus `PeakFwdPower(prn->tx[0].fwd_power)` (`:506-508`) | Same — verbatim 16-bit MSB-first decode for both fields; `peak_fwd_power_callback_(fwd_power)` is a Lyra-native sink callback (operator-policy hookup deferred to Task #114) |
| `0x10` — power telemetry II + PA volts | `prn->tx[0].rev_power = (CB[1] << 8 \| CB[2]) & 0xffff;` (AIN2 PA reverse power); `prn->user_adc0 = (CB[3] << 8 \| CB[4]) & 0xffff;` (**AIN3 MKII PA Volts**).  Plus `PeakRevPower(prn->tx[0].rev_power)` (`:510-513`) | Same — verbatim 16-bit decode; `peak_rev_power_callback_` deferred per Task #114 |
| `0x18` — PA amps + supply volts | `prn->user_adc1 = (CB[1] << 8 \| CB[2]) & 0xffff;` (**AIN4 MKII PA Amps** — the §15.26-corrected slot); `prn->supply_volts = (CB[3] << 8 \| CB[4]) & 0xffff;` (AIN6 Hermes Volts) (`:516-517`) | Same — preserves the §15.26 source-verified slot map.  Empirical: the operator's HL2+ ak4951v4 gateware may NOT emit `user_adc1` on this slot per the prior project's bench finding — that's a hardware-variant concern, NOT a source-decode concern.  Lyra emits the verbatim decode; what's IN the slot is a bench question for the operator at first-RF time. |
| `0x20` — multi-ADC overload | `prn->adc[0].adc_overload = CB[1] & 1; prn->adc[1].adc_overload = (CB[2] & 1) << 1; prn->adc[2].adc_overload = (CB[3] & 1) << 2;` (`:519-523`) — note the **per-ADC shift** (ADC1 gets bit-1 position, ADC2 gets bit-2) | Same — verbatim shift pattern |

**Reference defects preserved verbatim per Rule 24:**
1. **`dash_in` / `dot_in` left-shift bug** at `:497-498` — `(CB[0] << 1) & 0x1` always = 0 (left-shift, then mask bit 0 — but the shift moves the source bit OUT of bit 0).  Likely intended `>> 1` / `>> 2`.  Reference has it wrong; Lyra preserves verbatim.  Comment in Lyra code flags it explicitly.
2. **§15.26 empirical hardware variance** — on the operator's HL2+ ak4951v4, `user_adc1` (PA amps) may not appear at the `0x18` C1:C2 slot per prior-project bench finding.  This is a HARDWARE concern (which gateware variant emits which data); the SOURCE decode in §5.4 is correct against the reference.  Empirical-bench at first-RF time will confirm whether the gateware delivers in the documented slots.

**VERDICT:** ✅ **PARITY** — every byte / every shift / every slot verbatim from `:494-525`; two reference defects preserved verbatim per Rule 24 (dash/dot shift bug, PA-amps slot empirical-variance).

---

### §5.5 Per-DDC IQ unpacking (source: `:527-542`)

| Aspect | Reference | Lyra |
|---|---|---|
| Samples-per-record | `spr = 504 / (6 * nddc + 2);` (`:527`) — the EP6 USB-frame body is 504 bytes (after the 8-byte sync+C0..C4 prefix); each sample slot is `6 * nddc + 2` bytes (6 bytes per DDC for the IQ pair + 2 mic bytes); spr is how many slots fit | Same — `int spr = 504 / (6 * nddc + 2);` |
| Per-DDC × per-sample loop | `for (iddc = 0; iddc < nddc; iddc++) { for (isample = 0; isample < spr; isample++) { ... } }` (`:528-541`) | Same — nested loop |
| Sample byte offset | `int k = 8 + isample * (6 * nddc + 2) + iddc * 6;` (`:532`) — `8` = sync(3) + C0..C4(5); per-sample stride = `6 * nddc + 2`; per-DDC offset within a sample slot = `iddc * 6` | Same — verbatim offset arithmetic |
| I-sample decode | `RxBuff[iddc][2*isample+0] = const_1_div_2147483648_ * (double)(bptr[k+0] << 24 \| bptr[k+1] << 16 \| bptr[k+2] << 8);` (`:533-536`) — 24-bit BE signed sample left-shifted into the high bits of an int (so the sign bit aligns at position 31), then divided by 2^31 to normalize to [-1.0, +1.0) | Same — `const_1_div_2147483648_` is a Lyra `constexpr double k_i32_to_unit = 1.0 / 2147483648.0;` (reference-verbatim value `1.0 / 2147483648.0` = `2^-31`) |
| Q-sample decode | Same pattern at offsets `k+3..k+5`, stored at `RxBuff[iddc][2*isample+1]` (`:537-540`) | Same |

**Reference idiom preserved:** the `(byte << 24) | (byte << 16) | (byte << 8)` pattern places the 24-bit signed sample in the high 24 bits of an `int`, leaving the low 8 bits as 0.  When the int is cast to `double` and divided by 2^31, the result is the normalized signed sample.  This is a clean way to handle the sign-extension of 24-bit values in C — Lyra preserves verbatim.

**VERDICT:** ✅ **PARITY** — verbatim from `:527-542`.

---

### §5.6 DDC routing — inline `switch (nddc)` (source: `:544-559`)

The former §5 DdcMap home — collapsed inline per the locked
2026-06-05 "do as the reference does" discipline.

| `nddc` | Reference | Lyra |
|---|---|---|
| 2 (Hermes II) | `twist(spr, 0, 1, 0);` (`:547`) — DDC0+DDC1 twist-paired → consumer 0 | Same |
| 4 (HL2 / HL2+) | `xrouter(0, 0, 0, spr, prn->RxBuff[0]); twist(spr, 2, 3, 1); xrouter(0, 0, 2, spr, prn->RxBuff[1]);` (`:550-552`) — DDC0 → consumer 0 (RX1); DDC2+DDC3 twist-paired → consumer 1 (PS feedback path); DDC1 → consumer 2 (RX2) | Same — verbatim 3-call sequence |
| 5 (Orion / ANAN P1 5-DDC) | `twist(spr, 0, 1, 0); twist(spr, 3, 4, 1); xrouter(0, 0, 2, spr, prn->RxBuff[2]);` (`:555-557`) | Same |

**Note on consumer-slot semantics:** "consumer 0" = host channel 0 = RX1; "consumer 2" = host channel 2 = RX2; "consumer 1" = the twist-paired output (PS feedback path on HL2 nddc=4, diversity-sync on Hermes II nddc=2 + ANAN P1 nddc=5).  This routing convention is locked across families.

**VERDICT:** ✅ **PARITY** — verbatim from `:544-559`.

---

### §5.7 Mic decimation + harvest + delivery (source: `:560-579`)

| Aspect | Reference | Lyra |
|---|---|---|
| Decimation state | `mic_decimation_count` runs per-sample; when it hits `mic_decimation_factor`, emit a mic sample + reset count (`:566-577`) | Same |
| Per-sample mic byte offset | `int k = 8 + nddc * 6 + isamp * (2 + nddc * 6);` (`:564`) — per-sample stride is `2 + nddc * 6` (2 mic + nddc IQ); offset into the mic slot is `8 + nddc * 6` (skip sync+C0..C4+per-DDC IQ) | Same |
| Mic sample decode | `TxReadBufp[2*mic_sample_count+0] = const_1_div_2147483648_ * (double)(bptr[k+0] << 24 \| bptr[k+1] << 16);` (`:570-572`) — note: only TWO bytes consumed (16-bit mic, NOT 24-bit), shifted into the high 16 bits | Same — verbatim |
| Q channel zeroed | `TxReadBufp[2*mic_sample_count+1] = 0.0;` (`:573`) — mic is real, not complex; Q always zero | Same |
| Inbound delivery | `Inbound(inid(1, 0), mic_sample_count, prn->TxReadBufp);` (`:579`) — `inid(1, 0)` = stream-class 1 (mic), instance 0 | Lyra-native `inbound_callback_(mic_sample_count, tx_read_buf_.data())` — sink callback set by operator code at session start.  The reference's `Inbound()` is a channel-master buffer push; Lyra's callback is a thinner equivalent (no channel-master scaffolding) |

**VERDICT:** ✅ **PARITY** on every byte; ⚠ **ACCEPTABLE DEVIATION** on the `Inbound`/channel-master scaffolding → Lyra-native callback (the channel-master system is reference-specific and not present in Lyra; equivalent function via sink callback).

---

### §5.8 `Router` — `xrouter` + `twist` Lyra-native (source: `networkproto1.c::twist:263-274` + `router.c::xrouter:71+`)

New file: `src/wire/Router.{h,cpp}`.  Matches the reference's
`router.{c,h}` separation per the locked discipline.

**`twist(int nsamples, int stream0, int stream1, int source)`**
— interleaves I/Q from two per-DDC `RxBuff[]` staging buffers
into a paired-IQ output, then dispatches via `xrouter()`.  Source:
`networkproto1.c:263-274`.

| Aspect | Reference | Lyra |
|---|---|---|
| Interleave loop | `for (i=0, j=0; i < 2*nsamples; i+=2, j+=4) { RxReadBufp[j+0..1] = RxBuff[stream0][i+0..1]; RxReadBufp[j+2..3] = RxBuff[stream1][i+0..1]; }` (`:266-272`) | Same — verbatim 4-element interleave pattern |
| Forward to xrouter | `xrouter(0, 0, source, 2 * nsamples, prn->RxReadBufp);` (`:273`) | Same — `xrouter(nullptr, 0, source, 2 * nsamples, rx_read_buf_)` |

**`xrouter(void* ptr, int id, int source, int nsamples, double* data)`**
— callback-table-driven dispatch primitive.  Source: `router.c:71+`.

| Aspect | Reference | Lyra |
|---|---|---|
| Router selection | `if (ptr == 0) a = prouter[id]; else a = (ROUTER)ptr;` (`:75-76`) — if no explicit router pointer, look up by `id` in the global `prouter[]` array | Same — Lyra's `Router` class has static `instances[]` array indexed by `id`; `nullptr` arg = use default instance |
| Control word | `ctrl = _InterlockedAnd(&(a->controlword), 0xffffffff);` (`:78`) — atomic read of the runtime control word (used for MOX-PS state-product re-routing in v0.3) | `ctrl = control_word_.load();` (`std::atomic<int>`) |
| Source-port check | `bport = source; if (bport < a->sources) { ... }` (`:80-82`) | Same |
| Per-port callback dispatch | `for (i = 0; i < a->ncalls; i++) { switch (a->function[bport][i][ctrl]) { case 1: Inbound(a->callid[bport][i][ctrl], nsamples, data); break; ... } }` (`:84+`) — multi-callback dispatch per port per control word | Same structure — `for` loop over `n_calls_` with per-callback `function_[port][i][ctrl]` dispatch |
| `Inbound()` call | Reference uses channel-master `Inbound()` to push samples into a CM buffer | Lyra-native sink-callback interface (each consumer registers a `std::function<void(int n, const double*)>` callback; xrouter invokes the registered callback) |

**Setter surface for §5 / Router** — needed for HL2 RX-only wire-inert state:
- `Router::register_consumer(int port, std::function<void(int, const double*)> cb)` — operator/host code registers a callback for a given consumer slot at session start.
- `Router::set_control_word(int ctrl)` — operator/host code can re-route consumers via control word changes (v0.3 PS scope).  For §5 wire-inert state, single fixed control word.

**VERDICT:** ✅ **PARITY** on the structural pattern; ⚠ **ACCEPTABLE DEVIATION** on the C-callback-array storage → `std::function` (C++23 idiom translation, same semantic effect).  Wire bytes / dispatch sequence identical.

---

### §5 — Overall verdict

| Section | Verdict |
|---|---|
| Q1–Q5 architecture lock | Operator review pending |
| §5.1 Thread lifecycle + UDP recvfrom + watchdog | ✅ PARITY structural; ⚠ Windows-WSA → cross-platform recvfrom idiom translation |
| §5.2 Frame validation + C0..C4 extraction | ✅ PARITY verbatim from `:470-476` |
| §5.3 I2C readback path | ✅ PARITY verbatim from `:478-493` |
| §5.4 EP6 telemetry decode (5 C0-class switch) | ✅ PARITY verbatim from `:494-525`; two reference defects preserved verbatim per Rule 24 (dash/dot shift, PA-amps slot empirical-variance) |
| §5.5 Per-DDC IQ unpacking | ✅ PARITY verbatim from `:527-542` |
| §5.6 DDC routing inline switch (former §5 DdcMap) | ✅ PARITY verbatim from `:544-559` |
| §5.7 Mic decimation + harvest + delivery | ✅ PARITY on bytes; ⚠ Inbound/channel-master → Lyra-native callback |
| §5.8 Router (xrouter + twist) | ✅ PARITY on structural pattern; ⚠ C callback-array → `std::function` idiom |

**ZERO 🔴 OPERATOR-APPROVED DEVIATIONS.**  ⚠ ACCEPTABLE
DEVIATIONS are limited to C → C++23 idiom translations (the
Windows-WSA → cross-platform recvfrom, channel-master `Inbound`
→ Lyra-native callback, C callback-array → `std::function`).

---

### §5 — Rule 24 circle-back items

Re-verify before commit:

1. `networkproto1.c:422-463` — thread setup + watchdog structure
2. `:470-476` — frame validation + C0..C4 extraction
3. `:478-493` — I2C readback path + 0x3f error case
4. `:494-525` — telemetry decode (5 C0-class switch); pay extra attention to the dash/dot shift bug + the 0x18 slot map
5. `:527-542` — IQ unpacking with 24-bit BE shift pattern
6. `:544-559` — DDC routing switch with per-family branches
7. `:560-579` — mic decimation + Inbound call
8. `:263-274` — twist body verbatim
9. `router.c:71+` — xrouter body (control word + callback dispatch)
10. `network.h:507-508` — mic_decimation_factor / mic_decimation_count types (both `int`)

---

**OPERATOR SIGN-OFF:**

- [x] §5 architecture lock Q1–Q5 reviewed + confirmed
- [x] §3 supplement — 2 new globals (`mic_decimation_factor`,
      `mic_decimation_count`) accepted
- [x] §5.1 thread lifecycle — verbatim structural elements
      from `:422-463`; Windows-WSA → cross-platform recvfrom
      idiom acceptable
- [x] §5.2 frame validation + C0..C4 extraction — verbatim
      from `:470-476`
- [x] §5.3 I2C readback path — verbatim from `:478-493`
- [x] §5.4 EP6 telemetry decode — verbatim from `:494-525`
      including BOTH reference defects preserved per Rule 24
      ("do as Thetis does, including bugs"): the dash/dot
      left-shift bug + the PA-amps slot empirical-variance
- [x] §5.5 per-DDC IQ unpacking — verbatim 24-bit BE shift
      pattern + 2^-31 normalization
- [x] §5.6 DDC routing inline switch — verbatim from `:544-559`
- [x] §5.7 mic decimation + harvest — verbatim from `:560-579`;
      Inbound → Lyra-native callback acceptable
- [x] §5.8 Router (`xrouter` + `twist`) — separate file
      `src/wire/Router.{h,cpp}` matching reference's
      router.{c,h}
- [x] Circle-back Rule 24 re-verify executed (zero defects on
      source-decode side; 2 reference defects flagged +
      preserved verbatim per discipline)
- [x] Authorized to populate

Signed: N8SDR        Date: 2026-06-05

---

## §5-A Parity correction sweep — 9 fixes vs `c8baa63` (audit-driven)

Two-agent post-§5 audit (parity-vs-reference + rule-compliance) ran
against commit `c8baa63`.  Rule audit PASS.  Parity audit returned
FAIL on 3 items + 3 NOTEs.  Re-read of `networkproto1.c:422-586`
verbatim surfaced 6 more items the audit had not isolated, including
one CRITICAL RX-audio bug.  Operator directive: "fix all to match
reference, including the NOTEs unless we lack hardware to test."
All 9 fixes shipped together below; this entry records the audit
trail.

### Fixes applied (all reference-source-verified, Rule 24)

| # | Item | Severity | Source-line |
|---|------|----------|-------------|
| 1 | **IQ unpack scale** — top-24 packing (`<<24, <<16, <<8`) + `/2^31` divisor (effective `/2^23`); was bottom-24 + `/2^31` (effective `/2^31`) → all RX 48 dB too quiet | 🔴 CRITICAL | `:533-540` |
| 2 | Mic sink emits doubles as IQ-pairs (I=mic, Q=0); was raw int16 | drift | `:570-573, 579` |
| 3 | Mic decimation logic (count++/==factor/reset+harvest); was ignored | drift | `:566-577` |
| 4 | `twist()` → `xrouter()` passes `2 * nsamples`; was `nsamples` | drift | `:273` |
| 5 | Dispatch cadence per-USB-frame (not per-datagram); twist/xrouter + mic harvest fire inside the per-frame body | drift | `:470-580` |
| 6 | dash_in / dot_in REVERTED to literal `(cc[0] << N) & 0x1` (always-zero reference defect, preserved per Rule 24); was deliberate `& 0x02` / `& 0x04` "fix" | drift | `:497-498` |
| 7 | 5-case telemetry switch on `cc[0] & 0xf8` populates RADIONET shadows: case 0x00 (adc[0].overload + user_dig_in), case 0x08 (tx[0].exciter_power + tx[0].fwd_power), case 0x10 (tx[0].rev_power + user_adc0), case 0x18 (user_adc1 + supply_volts), case 0x20 (adc[0..2].overload).  `adc[0].adc_overload` moved INSIDE cases 0x00 + 0x20 (was unconditional) | drift | `:499-524` |
| 8 | Dynamic per-frame layout — `stride = 6*nddc + 2`, `spr = 504 / stride` computed from `nddc`; was hard-coded `26` / `19` (nddc=4 only) | drift | `:527, 532, 562` |
| 9 | `mic_decimation_factor` default `0` (matches reference BSS-init); was `1`.  HL2 operating point sets `1` via per-family setter (lands with Phase 2 wire-up) | drift | `network.h:507` |

### Deviation FIXMEs (deferred to Task #114 TX-policy plumbing)

| Item | Note |
|------|------|
| `PeakFwdPower()` / `PeakRevPower()` side-effect helpers | Reference calls these as external functions after writing fwd/rev power to maintain peak-meter state.  Raw `tx[0].fwd_power` / `tx[0].rev_power` are populated; peak-tracking helper lands with Task #114.  FIXME comments inline at cases 0x08 + 0x10. |

### Lyra-native additions (acceptable, signed off)

| Item | Note |
|------|------|
| `Ep6TelemetrySink` raw-bytes forwarding | Additive convenience layer fired AFTER the reference-faithful RADIONET shadow writes.  Operator/host consumers that want raw payload bytes (independent of the RADIONET path) register a sink.  No impact on wire or RADIONET state.  Signed as an acceptable Lyra-native addition. |
| Shared `tx_read_bufp_` scratch buffer | Reference's `RADIONET prn` holds two separate pointers `RxReadBufp` + `TxReadBufp` (`network.h:291`); Lyra uses ONE buffer reused sequentially within each USB-frame body — twist writes (xrouter consumes inline), then mic harvest writes (mic_sink consumes inline).  Benign because sinks consume synchronously before the next step overwrites; sized to the larger of the two layouts (`4 * kMaxSprPerFrame` doubles).  Documented inline at `Ep6RecvThread.h:194-201`.  Signed as an acceptable Lyra-native addition (§1.1 buffer-ownership-in-EP6-thread). |
| Wrap-aware sequence-error counter | Reference tracks seq externally via `MetisReadDirect()`; Lyra's `seq != last + 1` wrap-aware compare in `process_datagram()` is an additive diagnostic counter, read via `sequence_errors()`.  Documented inline at `Ep6RecvThread.h:174-178`.  Signed as an acceptable Lyra-native addition. |

### Architectural cleanup

- `set_ddc_sink()` REMOVED from `Ep6RecvThread`.  The reference uses
  `xrouter()` as the SOLE per-DDC dispatch path; Lyra's earlier
  duplicate `ddc_sinks_[]` was an unsigned parallel path.  Consumers
  now register exclusively on `Router::instance(id)` per the §5.8
  signed-off idiom translation.

### Sign-off

- [x] Source-verified line-by-line vs `networkproto1.c:422-586` +
      `:263-274` (twist) + `network.h:507-508` (decimation globals)
- [x] All 9 fixes applied; build green (`lyra.exe` re-linked, 0
      compile warnings, 0 errors)
- [x] Reference defects (dash_in/dot_in left-shift; adc_overload
      single-frame assignment) preserved verbatim per Rule 24
- [x] No new safety-relevant defaults; §4b-2 PA-OFF posture intact

Signed: N8SDR        Date: 2026-06-05

---

*Last updated: 2026-06-05 — §5-A parity correction sweep — 9 fixes vs `c8baa63` shipped (including the IQ-scale critical bug).  Single-commit parity correction on top of §5; §6 unblocked.*

---

## §6. `Ep2SendThread` + `OutboundRing` — EP2 datagram send loop + outbound framing primitives

**Scope.** The EP2 send thread: semaphore-driven outbound pump that
waits on a paired (LR-audio + TX-IQ) buffer-ready signal, applies
the per-sample TX-side transforms (MOX-edge IQ zeroing, optional
EER mode overwrite of LR with IQ, optional L/R audio channel swap,
float → int16 round-to-nearest quantization, HL2 4-bit / non-HL2
3-bit CW state-bit overlay on TX I-sample LSBs when CW enabled),
calls into the §4 `FrameComposer` to produce the 2-USB-frame
outbound buffer (504 bytes C&C + 504 bytes LRIQ per frame), and
hands the 1024-byte composed payload to the EP2 frame writer
(`MetisWriteFrame`-equivalent) which prepends the 8-byte HPSDR
header + 4-byte BE outbound sequence number and calls `sendto`.
Also covers the `OutboundRing` LR-audio + TX-IQ buffer plumbing
that audio-mixer / TX-DSP threads write into and the send thread
reads from.

Source mirror: `networkproto1.c::sendProtocol1Samples` (lines
1204-1267) + `networkproto1.c::MetisWriteFrame` (lines 216-237) +
the `MetisOutBoundSeqNum` global + the `prn->hsendEventHandles[2]`
+ `prn->outLRbufp` + `prn->outIQbufp` + `prn->hobbuffsRun[2]`
semaphore/buffer state.  HL2 / HL2+ EP2 send; ANAN-class branches
(non-HERMESLITE) call `WriteMainLoop` instead of `WriteMainLoop_HL2`
but the surrounding `sendProtocol1Samples` body is family-shared.

**Locked architecture (Q1–Q5 — PROPOSED, awaiting operator
sign-off).**  Per the standing 2026-06-05 directive "reference =
make Lyra the same" all answers default to verbatim reference
mirror.

| # | Question | Proposed answer (operator review) |
|---|---|---|
| Q1 | Send-loop structure | **Switch-style send loop matching the reference verbatim.**  `Ep2SendThread::run_loop()` as a near-line-for-line port of `sendProtocol1Samples` (`networkproto1.c:1204-1267`).  Outer `while (io_keep_running)` + semaphore wait on both LR + IQ buffers ready + MOX-edge zeroing branch + optional EER overwrite + optional L/R swap + per-sample int16 quantization with CW state-bit overlay + call into `FrameComposer::compose_pair()` (the §4 emit path) → call into `metis_write_frame(0x02, buf)`. |
| Q2 | `OutboundRing` as separate file? | **Yes — `src/wire/OutboundRing.{h,cpp}`** — matches reference's logical separation (`outLRbufp` + `outIQbufp` + `hsendEventHandles[2]` are a coherent buffer-pair-with-semaphore unit; reference holds them on `RADIONET` but they are unambiguously networking-infrastructure state per §1.1 RadioNet exclusion).  Lyra-native: one ring class holds the two paired buffers + the two-element semaphore + producer-side `push_lr()`/`push_iq()` setters + consumer-side `wait_pair_ready()` blocking method.  Outboundring header already present as empty skeleton in CMakeLists; populate in this commit. |
| Q3 | `MetisWriteFrame` placement | **Free function `metis_write_frame(int endpoint, const uint8_t* payload, std::size_t payload_bytes)` in `src/wire/Ep2SendThread.cpp` anonymous namespace** — matches the reference's free-function shape (`networkproto1.c:216`).  Owns the 8-byte HPSDR header build (`0xEF 0xFE 0x01 endpoint` + 4-byte BE seqnum) + `MetisOutBoundSeqNum` increment + `sendto`/`sendPacket` call.  Outbound seqnum is a class member (`out_seq_num_`) — not a global — per the §1.1 networking-infrastructure-stays-out-of-RadioNet discipline.  ✅ accepted Lyra-native deviation. |
| Q4 | Where do `outLRbufp` / `outIQbufp` / `OutBufp` / outbound seqnum live? | **Inside `OutboundRing` (LR + IQ buffers + their semaphore) + inside `Ep2SendThread` (composed outbound buffer + seqnum)** — honors the signed §1.1 networking-infrastructure exclusion from `RadioNet`.  `prn->outLRbufp` (sized for 126 LR audio samples) + `prn->outIQbufp` (sized for 126 TX IQ samples) live as `std::vector<double>` members on `OutboundRing`.  `prn->OutBufp` (the 504-byte composed LRIQ output buffer, written to by `sendProtocol1Samples`, read by `WriteMainLoop_HL2`) lives as a member of `Ep2SendThread` (`out_buf_`).  Same pattern as §5 (`RxBuff`/`TxReadBufp`/`ControlBytesIn`). |
| Q5 | Synchronization | One `std::mutex send_lock_` member of `Ep2SendThread` ↔ reference's `prn->sndpktp1` (the P1 send critical section, sibling of the §5 `prn->rcvpktp1`).  Held across the `metis_write_frame` `sendto` call to prevent overlapping sends on the same socket.  Lock-order acyclic per §15.26 lessons.  The `OutboundRing` semaphore (LR-ready + IQ-ready paired wait) uses `std::counting_semaphore<1>` × 2 (matches reference's two `HANDLE` semaphores in `hsendEventHandles[2]`). |

**§3 supplement — additional globals surfaced by §6:**

| Global | Reference (file:line) | Lyra | Default |
|---|---|---|---|
| `MetisOutBoundSeqNum` | `network.h` (referenced by `networkproto1.c:221, 231` as `unsigned int`) — running outbound seq counter, incremented per emitted EP2 datagram | (NOT a global) — class member `Ep2SendThread::out_seq_num_` per Q3.  Lyra-native deviation; reference's global counter is the only consumer (no cross-file reads) so encapsulation is safe. | `0` (incremented to 1 on first emit; matches reference behavior at first `++MetisOutBoundSeqNum`) |
| `XmitBit` | `network.h:413` (already surfaced in §3) — read at `:1222` (`prn->run && XmitBit`) and `:1227` (`if (!XmitBit) memset(outIQbufp, 0, ...);`) | already present | `0` (unchanged) |
| `prn->swap_audio_channels` | `network.h` — read at `:1231` for the optional L/R channel-swap loop | RADIONET field, add to `AudioConfig` sub-struct (if not already present) | `0` (no swap) |
| `prn->cw.cw_enable` + `prn->tx[0].{cwx_ptt, dot, dash, cwx}` | `network.h::CwConfig` + `network.h::TxState` — read at `:1247-1256` for the HL2 4-bit / non-HL2 3-bit CW state-bit overlay on TX I-sample LSBs | already present in `RadioNet.h` (CwConfig + TxState) | `0` (CW off; preserves bare-SSB-only behavior) |
| `pcm->xmtr[0].peer->run` | reference's channel-master state — read at `:1222` for the EER mode gate | NOT applicable to Lyra (no channel-master); the EER branch is a deferred §6 deviation — see §6.4 below.  Operator-policy plumbing per Task #114. | (n/a) |

---

### §6.1 Thread lifecycle + MMCSS Pro-Audio priority (source: `networkproto1.c:1204-1216`)

| Aspect | Reference | Lyra |
|---|---|---|
| Function entry | `DWORD WINAPI sendProtocol1Samples(LPVOID n)` (`:1204`) | `void Ep2SendThread::run_loop()` — runs on a spawned `std::thread`; thread start sets MMCSS Pro-Audio priority via `AvSetMmThreadCharacteristicsW` (Windows-only; no-op elsewhere) |
| MMCSS priority | `AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex); AvSetMmThreadPriority(hTask, 2);` (`:1207-1208`); fallback `SetThreadPriority(..., THREAD_PRIORITY_HIGHEST)` (`:1209`) | Same — MMCSS Pro-Audio + priority 2 via `AvSetMmThreadPriority(handle, AVRT_PRIORITY_NORMAL+2)` (Windows only); on non-Windows, no-op (cross-platform port acceptable deviation, signed §6.7) |
| Local state | `double *pbuffs[2]; pbuffs[0]=prn->outLRbufp; pbuffs[1]=prn->outIQbufp;` (`:1214-1216`) | Same — local pointer pair captured from `OutboundRing` members at loop entry |
| Loop entry | `while (io_keep_running != 0)` (`:1218`) | `while (io_keep_running_)` — `std::atomic<int>` member |

**VERDICT:** ✅ **PARITY** — structural elements verbatim; ⚠ **ACCEPTABLE DEVIATION** on the Windows-only MMCSS calls being conditional under `#ifdef _WIN32` (cross-platform).

---

### §6.2 Paired-buffer semaphore wait (source: `networkproto1.c:1220`)

| Aspect | Reference | Lyra |
|---|---|---|
| Wait | `WaitForMultipleObjects(2, prn->hsendEventHandles, TRUE, INFINITE);` (`:1220`) — waits for BOTH LR + IQ buffers to signal ready (`TRUE` = wait-all semantic) | `OutboundRing::wait_pair_ready()` — acquires both `std::counting_semaphore<1>` members in lockstep; blocks until both producers (audio mixer + TX-DSP) have released their respective semaphores |
| Semaphore release (producer side) | `ReleaseSemaphore(prn->hobbuffsRun[0], 1, 0);` (`:1199` in `WriteMainLoop_HL2`) — done at the END of frame emission to signal "send-side has consumed; producer free to refill" | NOT in §6 scope — producer-side release is part of FrameComposer's emit path; the producer-side `notify_consumed()` API will land with the Phase 2 wire-up step that connects FrameComposer's emit to OutboundRing's free-list signaling.  For §6 wire-inert, the semaphore-pair is signaled by stub helpers in unit tests. |

**VERDICT:** ✅ **PARITY** — `std::counting_semaphore<1>` × 2 acquired in lockstep matches the wait-all `WaitForMultipleObjects` semantic; ⚠ **DEFERRED** on the producer-side release wiring (lands with Phase 2 step 14).

---

### §6.3 MOX-edge IQ zeroing (source: `networkproto1.c:1227`)

| Aspect | Reference | Lyra |
|---|---|---|
| MOX-off branch | `if (!XmitBit) memset(prn->outIQbufp, 0, sizeof(complex) * 126);` (`:1227`) — zero the IQ buffer when not transmitting | Same — `if (!XmitBit) { std::fill(out_iq_buf_.begin(), out_iq_buf_.begin() + 2 * 126, 0.0); }` |

**Reference safety property preserved:** even if the audio-mixer / TX-DSP producer pushes non-zero IQ samples into the buffer while MOX is low (e.g., due to a race between PTT-release and the producer's next process_block), the EP2 send thread zeros the IQ buffer in the wire path BEFORE quantization → no RF leak on PTT-release race.  ⚠ **CRITICAL safety property** — preserve verbatim.

**VERDICT:** ✅ **PARITY** — verbatim from `:1227`; safety property preserved.

---

### §6.4 EER mode LR-overwrite-by-IQ (source: `networkproto1.c:1222-1226`)

| Aspect | Reference | Lyra |
|---|---|---|
| EER branch | `if (pcm->xmtr[0].peer->run && XmitBit) { memcpy(prn->outLRbufp, prn->outIQbufp + 256, sizeof(complex) * 126); }` (`:1222-1226`) — when in EER/ETR mode and transmitting, overwrite the LR audio with delayed IQ data (EER = Envelope Elimination & Restoration; ETR = Envelope Tracking) | **DEFERRED** to Task #114 TX-policy plumbing — Lyra has no channel-master equivalent of `pcm->xmtr[0].peer->run`; EER mode is a v0.3+ TX feature that requires upstream WDSP TX-channel + envelope-tracking implementation.  For §6, the branch is stubbed with a `// FIXME (Task #114): EER mode requires WDSP TX channel state` comment + a hardcoded `if (false &&` guard that disables the branch.  When EER lands, the guard flips to the real condition. |

**VERDICT:** ⚠ **DEFERRED DEVIATION** — operator-approved per the locked Q1-Q5 sign-off; branch shape preserved with stubbed condition.  Marked 🔴 OPERATOR-APPROVED DEVIATION pending Task #114.

---

### §6.5 Optional L/R audio channel swap (source: `networkproto1.c:1231-1239`)

| Aspect | Reference | Lyra |
|---|---|---|
| Swap branch | `if (prn->swap_audio_channels) { for (i = 0; i < 4 * 63; i += 2) { swap = pbuffs[0][i+0]; pbuffs[0][i+0] = pbuffs[0][i+1]; pbuffs[0][i+1] = swap; } }` (`:1231-1239`) — when operator-configured, swap L ↔ R audio in-place to compensate for hardware-firmware variants | Same — read `prn->swap_audio_channels` (RADIONET shadow set by operator-config), perform in-place swap on the 504-byte LR buffer (252 doubles = 4*63 LR pairs).  Verbatim loop bounds + index math. |

**VERDICT:** ✅ **PARITY** — verbatim from `:1231-1239`.

---

### §6.6 Float → int16 quantization with round-to-nearest (source: `networkproto1.c:1241-1259`)

| Aspect | Reference | Lyra |
|---|---|---|
| Outer loop | `for (i = 0; i < 2 * 63; i++)` — 126 sample-slots per buffer pair | Same — `for (int i = 0; i < 2 * 63; ++i)` |
| Middle loop | `for (j = 0; j < 2; j++)` — j=0 = LR pair, j=1 = IQ pair | Same |
| Inner loop | `for (k = 0; k < 2; k++)` — k=0 = first component (L or I), k=1 = second component (R or Q) | Same |
| Quantization | `temp = pbuffs[j][i*2+k] >= 0.0 ? (short)floor(pbuffs[j][i*2+k] * 32767.0 + 0.5) : (short)ceil(pbuffs[j][i*2+k] * 32767.0 - 0.5);` (`:1245-1246`) — symmetric round-to-nearest with floor/ceil split on sign | Same — `int16_t temp = (v >= 0.0) ? static_cast<int16_t>(std::floor(v * 32767.0 + 0.5)) : static_cast<int16_t>(std::ceil(v * 32767.0 - 0.5));` |
| BE pack to OutBufp | `prn->OutBufp[8*i + 4*j + 2*k + 0] = (char)((temp >> 8) & 0xff); prn->OutBufp[8*i + 4*j + 2*k + 1] = (char)(temp & 0xff);` (`:1257-1258`) — 8 bytes per sample-slot = L_hi L_lo R_hi R_lo I_hi I_lo Q_hi Q_lo | Same — verbatim offset math; output to `out_buf_[8*i + 4*j + 2*k + {0,1}]` |

**Reference quirk preserved (Rule 24):** the quantization uses `(short)floor(...)` cast (truncation toward zero of a positive double; potential UB on value > INT16_MAX before cast).  Lyra preserves the same behavior via `static_cast<int16_t>(...)` (truncating-cast in C++23 is implementation-defined but identical on every two's-complement platform); add `assert(v >= -1.0 && v <= 1.0)` debug guard.

**VERDICT:** ✅ **PARITY** — verbatim quantization + BE pack from `:1241-1259`.

---

### §6.7 CW state-bit overlay on TX I-sample LSBs (source: `networkproto1.c:1247-1256`)

| Aspect | Reference | Lyra |
|---|---|---|
| Gate | `if (prn->cw.cw_enable && j == 1)` (`:1247`) — only when CW enabled, only on the IQ pair (j=1) | Same |
| HL2 branch | `if (HPSDRModel == HPSDRModel_HERMESLITE) temp = (prn->tx[0].cwx_ptt << 3 \| prn->tx[0].dot << 2 \| prn->tx[0].dash << 1 \| prn->tx[0].cwx) & 0b00001111;` (`:1248-1252`) — HL2 has 4 CW state bits (cwx_ptt at bit 3; dot at 2; dash at 1; cwx at 0); replace `temp` with the 4-bit state word | Same — gated on `hpsdrModel == HPSDRModel::HERMESLITE`; verbatim shift + OR + mask |
| Non-HL2 branch | `else temp = (prn->tx[0].dot << 2 \| prn->tx[0].dash << 1 \| prn->tx[0].cwx) & 0b00000111;` (`:1253-1256`) — ANAN class has 3 CW state bits (dot at 2; dash at 1; cwx at 0); no cwx_ptt bit | Same |

**Critical reference behavior:** when CW is enabled, the TX I-sample LSBs are OVERWRITTEN with the CW state word — the actual SSB / digital-mode modulator output is replaced by CW state on the wire.  This is intentional per the HL2 protocol; consumers (CW keyer + HL2 gateware) coordinate to interpret the I-LSB as CW state during CW transmit.  Preserved verbatim per Rule 24.

**VERDICT:** ✅ **PARITY** — verbatim from `:1247-1256`; HL2 4-bit / non-HL2 3-bit branch preserved.

---

### §6.8 `WriteMainLoop_HL2` dispatch + buffer copy (source: `networkproto1.c:1261-1264, 1193-1200`)

| Aspect | Reference | Lyra |
|---|---|---|
| Per-family branch | `if (HPSDRModel == HPSDRModel_HERMESLITE) WriteMainLoop_HL2(prn->OutBufp); else WriteMainLoop(prn->OutBufp);` (`:1261-1264`) | Same — `if (hpsdrModel == HPSDRModel::HERMESLITE) frame_composer_->compose_hl2(out_buf_.data()); else frame_composer_->compose_generic(out_buf_.data());` (the §4 FrameComposer's two emit paths) |
| LRIQ memcpy into composed buffer | `memcpy(FPGAWriteBufp + 8, bufp, 8 * 63); memcpy(FPGAWriteBufp + 520, bufp + 504, 8 * 63);` (`:1193-1195`) — copy 504 bytes of LRIQ into each of the 2 USB frames (after the 8-byte sync+C&C header) | Already in §4 FrameComposer (`compose_hl2` writes the C&C header at offsets [0..7] + [512..519] and the LRIQ payload at offsets [8..511] + [520..1023]) |
| Submit | `MetisWriteFrame(0x02, FPGAWriteBufp);` (`:1198`) — endpoint 0x02 = EP2 outbound | `metis_write_frame(0x02, fpga_write_buf_.data(), 1024)` — see §6.9 |
| Producer-side release | `ReleaseSemaphore(prn->hobbuffsRun[0], 1, 0); ReleaseSemaphore(prn->hobbuffsRun[1], 1, 0);` (`:1199-1200`) — signal both LR + IQ producers "buffer consumed, free to refill" | `outbound_ring_->notify_consumed_pair()` — Lyra-native, paired release on both semaphores |

**VERDICT:** ✅ **PARITY** — verbatim dispatch + LRIQ memcpy (handled by §4 FrameComposer) + producer-side release.

---

### §6.9 `MetisWriteFrame` — 8-byte HPSDR header + seqnum + sendto (source: `networkproto1.c:216-237`)

| Aspect | Reference | Lyra |
|---|---|---|
| Function signature | `int MetisWriteFrame(int endpoint, char* bufp)` (`:216`) — endpoint ∈ {0x02 EP2, 2 priming-CC} | `int metis_write_frame(int endpoint, const uint8_t* payload, std::size_t payload_bytes)` — free function in `Ep2SendThread.cpp` anonymous namespace |
| Outbound datagram size | `unsigned char framebuf[1032]` (`:218-220`) — 8-byte header + 1024-byte payload | Same — `std::array<uint8_t, 1032> framebuf{};` |
| HPSDR sync | `framebuf[0]=0xef; framebuf[1]=0xfe; framebuf[2]=01; framebuf[3]=endpoint;` (`:223-226`) | Same — verbatim 4-byte sync prefix |
| Outbound seqnum (BE) | `unsigned char* p = (unsigned char*)&MetisOutBoundSeqNum; framebuf[4]=p[3]; framebuf[5]=p[2]; framebuf[6]=p[1]; framebuf[7]=p[0]; ++MetisOutBoundSeqNum;` (`:221, 227-231`) — manual byte-swap of native uint32 to BE wire order, post-increment | Same — `framebuf[4]=(out_seq_num_ >> 24) & 0xff; framebuf[5]=(out_seq_num_ >> 16) & 0xff; framebuf[6]=(out_seq_num_ >> 8) & 0xff; framebuf[7]=out_seq_num_ & 0xff; ++out_seq_num_;` (explicit BE-pack via shifts — well-defined under C++23) |
| Payload copy | `memcpy(outpacket.framebuf + 8, bufp, 1024);` (`:232`) | Same — `std::memcpy(framebuf.data() + 8, payload, 1024);` |
| sendto | `result = sendPacket(listenSock, (char*)&outpacket, 1024 + 8, prn->base_outbound_port);` (`:234`) — `sendPacket` is a wrapper around `sendto` with the configured destination port | Same — `::sendto(socket_fd_, framebuf.data(), 1032, 0, dest_addr, dest_addrlen)` — Lyra holds the destination address as a member (set at thread start, alongside `socket_fd_`) |
| Return | `result = sendPacket(...); result -= 8; return result;` (`:234-236`) — returns payload bytes sent (or negative error) | Same — `return result < 0 ? result : result - 8;` |

**Reference quirk preserved (Rule 24):** outbound seqnum is post-incremented (first emit ships seq=0, second ships seq=1, ...) and wraps at uint32 max (~136 years at 380 fps).  Lyra mirrors verbatim.

**VERDICT:** ✅ **PARITY** — verbatim header + seqnum + payload + sendto; Lyra-native `out_seq_num_` encapsulation per Q3 acceptable deviation.

---

### §6.10 `OutboundRing` — LR + IQ buffer-pair + paired semaphore (Lyra-native, source-mirror of `prn->outLRbufp` + `prn->outIQbufp` + `prn->hsendEventHandles[2]` + `prn->hobbuffsRun[2]`)

Lyra-native class with reference-mirror state:

| Member | Reference equivalent | Notes |
|---|---|---|
| `std::vector<double> lr_buf_` (252 doubles = 4 × 63 LR pairs) | `prn->outLRbufp` | Producer = audio-mixer; consumer = `Ep2SendThread` |
| `std::vector<double> iq_buf_` (252 doubles = 4 × 63 IQ pairs) | `prn->outIQbufp` | Producer = TX-DSP worker; consumer = `Ep2SendThread` |
| `std::counting_semaphore<1> lr_ready_` | `prn->hsendEventHandles[0]` | Released by producer when LR buffer filled |
| `std::counting_semaphore<1> iq_ready_` | `prn->hsendEventHandles[1]` | Released by producer when IQ buffer filled |
| `std::counting_semaphore<1> lr_consumed_` | `prn->hobbuffsRun[0]` | Released by consumer when LR buffer drained |
| `std::counting_semaphore<1> iq_consumed_` | `prn->hobbuffsRun[1]` | Released by consumer when IQ buffer drained |
| `void push_lr(const double* src, int n)` | producer-side memcpy + `ReleaseSemaphore(hsendEventHandles[0])` | Producer API |
| `void push_iq(const double* src, int n)` | producer-side memcpy + `ReleaseSemaphore(hsendEventHandles[1])` | Producer API |
| `void wait_pair_ready()` | `WaitForMultipleObjects(2, hsendEventHandles, TRUE, INFINITE)` | Consumer-blocking — waits for BOTH semaphores |
| `void notify_consumed_pair()` | `ReleaseSemaphore(hobbuffsRun[0]); ReleaseSemaphore(hobbuffsRun[1]);` | Consumer-release — signals both producers free to refill |

**VERDICT:** ✅ **PARITY** — buffer sizing + semaphore-pair semantics + producer/consumer API verbatim from reference.  Lives in its own file (`src/wire/OutboundRing.{h,cpp}`) per Q2 + §1.1 networking-buffer exclusion.

---

### §6 sign-off

- [x] Q1 — switch-style send loop matching the reference verbatim
- [x] Q2 — `OutboundRing` as separate `src/wire/OutboundRing.{h,cpp}` file
- [x] Q3 — `metis_write_frame` as free function in `Ep2SendThread.cpp` anon namespace; `out_seq_num_` encapsulated as member (Lyra-native deviation)
- [x] Q4 — `outLRbufp` / `outIQbufp` inside `OutboundRing`; `OutBufp` inside `Ep2SendThread`
- [x] Q5 — `std::mutex send_lock_` + paired `std::counting_semaphore<1>` × 2 for OutboundRing
- [x] §3 supplement globals added: `swap_audio_channels` field on `AudioConfig` if not present
- [x] §6.4 EER mode deferred to Task #114 with stub + `// FIXME` marker (operator-approved deferred deviation)
- [x] §6.6 quantization + §6.7 CW state-bit overlay preserved verbatim per Rule 24
- [x] §6.9 outbound seqnum post-increment + BE pack preserved verbatim
- [x] 4-item verbatim cross-check vs reference (§6.3 / §6.6 / §6.7 / §6.9) confirmed pre-sign-off
- [x] Authorized to populate

Signed: N8SDR        Date: 2026-06-05

---

## §6-A Parity correction sweep — 6 fixes vs `4c580a1` (audit-driven)

Two-agent post-§6 audit (parity-vs-reference + rule-compliance)
ran against commit `4c580a1`.  Rule audit PASS (1 cosmetic note
re §6.9 inline `// Rule 24` token).  Parity audit returned FAIL
on 2 drift items + 4 NOTEs.  Operator directive (consistent with
§5-A precedent): "fix all to match reference."  All 6 fixes
shipped together below; this entry records the audit trail.

### Fixes applied (all reference-source-verified, Rule 24)

| # | Item | Severity | Source-line |
|---|------|----------|-------------|
| 1 | **§6.1 MMCSS priority** — `AVRT_PRIORITY_CRITICAL` (=2 per Windows SDK avrt.h enum); was `AVRT_PRIORITY_HIGH` (=1 = one tier lower than reference) | drift | `:1208` |
| 2 | **§6.8 dispatch** — per-family branch `if (hpsdrModel == HERMESLITE) write_main_loop_hl2 else …`; was unconditional HL2 call.  Non-HL2 path = zero-fill placeholder with FIXME (Task #114 / no ANAN hardware to test; full `WriteMainLoop_generic` port lands when hardware arrives).  Branch shape preserved per reference | drift | `:1261-1264` |
| 3 | **§6.7 CW bit-source shifts** — REVERTED preemptive `& 0x01` masks on each bit-source value before shift; reference does NOT apply them.  In practice bool-like fields make the two forms equivalent, but byte-equivalence requires the verbatim reference idiom (final mask `& 0x0F` / `& 0x07` cleans up the result) | drift | `:1248-1256` |
| 4 | **§6.7 CW state capture** — REVERTED once-per-pair capture to per-sample reads of `prn->cw.cw_enable` + `prn->tx[0].{cwx_ptt, dot, dash, cwx}` + `HPSDRModel` inside the innermost loop, matching the reference's 504-reads-per-datagram cadence verbatim.  CW keyer edge timing now observes the same Δt window as the reference | drift | `:1247-1259` |
| 5 | **`OutboundRing::unblock()` UB guard** — REMOVED `lr_ready_.release()` + `iq_ready_.release()` calls.  C++20 `std::binary_semaphore::release()` is UB when the semaphore is already at max count (1); a producer-just-released-but-consumer-not-yet-acquired race would trigger it.  Shutdown latency falls back to the polling loop's 100 ms upper bound — well under the operator-perceptible budget | safety | n/a (C++20 [thread.sema.cnt]) |
| 6 | **§6.9 inline `// Rule 24` token** — added the explicit "preserved verbatim per Rule 24" comment at the seqnum BE-pack site (consistent with §6.3 / §6.6 / §6.7 inline-token convention).  Cosmetic; structural Rule 24 coverage was already complete via the file-header block | doc cosmetic | n/a |

### Lyra-native additions retained (signed, unchanged by §6-A)

| Item | Note |
|------|------|
| `Ep2SendThread::out_seq_num_` encapsulated as `atomic<uint32_t>` | Q3 sign-off — replaces reference's `MetisOutBoundSeqNum` global per §1.1 networking-infrastructure exclusion |
| `OutboundRing` as separate `src/wire/OutboundRing.{h,cpp}` file | Q2 sign-off |
| `Ep2SendThread::send_lock_` mutex around `metis_write_frame` | Q5 sign-off — mirrors `prn->sndpktp1` send-side critical section |
| Polling sequential `try_acquire_for` semaphore wait (vs `WaitForMultipleObjects` wait-all) | C++20 idiom translation; 100 ms shutdown poll cadence; wait-all semantic preserved |
| Bounded 5 s `try_acquire_for` on producer push | §15.21 wedged-writer-audit-class safety belt; reference is unbounded |

### Deferred FIXMEs (Task #114 TX-policy plumbing)

| Item | Note |
|------|------|
| `WriteMainLoop_generic` for non-HL2 dispatch (§6.8 else-branch) | Pending ANAN P1 hardware availability for bench-verification |
| EER mode LR-overwrite-by-IQ (§6.4) | Pending WDSP TX channel state Lyra does not have today |
| `PeakFwdPower()` / `PeakRevPower()` helpers (§5 case 0x08/0x10) | Pending TX-policy plumbing |

### Sign-off

- [x] Source-verified line-by-line vs `networkproto1.c:1204-1267` +
      `:216-237` + `:1193-1200`
- [x] All 6 fixes applied; build green (`lyra.exe` re-linked, 0
      compile warnings, 0 errors)
- [x] Reference defects (floor/ceil sign-split quantization;
      CW state-bit overlay overwriting TX I-LSBs; outbound
      seqnum post-increment) preserved verbatim per Rule 24
- [x] No new safety-relevant defaults; §4b-2 PA-OFF posture +
      §5-A `mic_decimation_factor=0` intact
- [x] §6.7 CW bit-source shifts no longer pre-masked
      (preserved reference defect parity if multi-bit values
      ever appear)
- [x] §6.7 CW state capture per-sample (matches reference
      504-reads/datagram cadence)
- [x] `OutboundRing::unblock()` UB-free (C++20 binary_semaphore
      compliance)

Signed: N8SDR        Date: 2026-06-05

---

## §6-B Parity correction sweep — TU-scope networking-globals fix (+ §7 ForceCandC enabling)

Operator directive 2026-06-06: "FIX it so it is like the reference,
NO PATCHING, it must be fixed to perform like the reference does
period."  §6 Q3 ("`out_seq_num_` encapsulated as `Ep2SendThread`
member") and §6 Q5 ("`Ep2SendThread::send_lock_` mutex") signed
2026-06-05 are revisited under the standing "do as reference"
rule.

**Root finding (Rule 24 source re-read):** `MetisOutBoundSeqNum`
(`networkproto1.c:30`), `MetisWriteFrame` (`:216-237`), and
`listenSock` (file-scope) are **NOT** inside the reference's
`_radionet` struct — they are separate TU-scope file-scope
globals.  §1.1's "networking infrastructure exclusion" was
correctly drafted for `_radionet`-resident fields (RxBuff,
hsendEventHandles, hobbuffsRun, etc.), but was incorrectly
extended in §6 Q3 to cover the wire-emit primitive's own TU
globals.  §1.1 stands for `_radionet` fields; the §6 Q3/Q5
encapsulations of `MetisOutBoundSeqNum` and the added
`send_lock_` are corrected here to mirror the reference's
actual TU-scope free-function + globals + no-lock structure.

§1.1 amendment landed in this commit:  the "Sequence counters"
row was split into RX-side (which IS in `_radionet`, stays in
`wire/Ep6RecvThread`) and TX-side (which is NOT in `_radionet`,
moves to `wire/MetisFrame.cpp` TU-scope).

### Fixes applied (all reference-source-verified, Rule 24)

| # | Item | Severity | Source-line |
|---|------|----------|-------------|
| 1 | **Hoist `metis_write_frame()` to `wire/MetisFrame.{h,cpp}`** — was a free function in `Ep2SendThread.cpp`'s anonymous namespace, callable only by §6.  Now a TU-scope free function in its own translation unit, callable by §6 (`Ep2SendThread::run_loop`) AND §7 (`ForceCandC::prime` + `prime_pass`) — direct mirror of reference's `MetisWriteFrame(int endpoint, char* bufp)` at `:216-237`, callable from any site in `networkproto1.c` | structural | `:216-237` |
| 2 | **TU-scope `g_metis_out_seq_num`** — `static std::uint32_t g_metis_out_seq_num{0};` at file scope in `wire/MetisFrame.cpp`.  Replaces `Ep2SendThread::out_seq_num_` member.  Direct mirror of reference's `unsigned int MetisOutBoundSeqNum;` at `:30` (file-scope global in `networkproto1.c`).  Post-increment + BE-pack preserved verbatim from §6.9 reference-source-line.  Type is plain `uint32_t` (not `std::atomic`) — reference is plain `unsigned int`; concurrency safety rests on TEMPORAL SEPARATION (priming completes before main-send spins up), exactly mirroring the reference | structural | `:30, :221-231` |
| 3 | **TU-scope `g_metis_socket_fd` + `g_metis_dest_addr` + `g_metis_dest_addrlen`** — file-scope statics in `wire/MetisFrame.cpp`.  Bound once at session-open (and idempotently re-bound by `Ep2SendThread::start()` for backward compatibility) via `metis_wire_bind(int socket_fd, const sockaddr* dest, std::size_t dest_len)` setter.  Replaces `Ep2SendThread::socket_fd_` / `dest_addr_` / `dest_addrlen_` members.  Direct mirror of reference's `listenSock` (file-scope) + `prn->base_outbound_port` access pattern at `:234` | structural | `:234` (sendPacket call site) |
| 4 | **Drop `Ep2SendThread::send_lock_` mutex** — REMOVED.  Reference has NO lock around `MetisWriteFrame` calls (priming + main-loop send are temporally disjoint by design: priming completes synchronously before the send-thread spins up its producer/consumer loop).  Lyra preserves the same temporal property (`ForceCandC::prime()` runs synchronously on the session-open thread BEFORE `Ep2SendThread::start()` is called), so the §6 Q5 mutex was over-engineering with no reference counterpart.  Removed | structural | n/a (reference has none) |
| 5 | **Drop `Ep2SendThread::out_seq_num_` member + `out_seq_num()` accessor** — REMOVED.  Replaced by fix #2's TU-scope global; diagnostic readers call `lyra::wire::metis_out_seq_num()` directly.  Reference's seq counter is a file-scope global, not a struct member | structural | `:30, :221, :231` |
| 6 | **§1.1 row "Sequence counters" amendment** — the §1.1 row pointing seq counters to "Ep6RecvThread / Ep2SendThread members" was correct for the RX-side `cc_seq_no` / `cc_seq_err` (which ARE inside `prn->_radionet` at `network.h:86-87`) but incorrect for the TX-side `MetisOutBoundSeqNum` (which is NOT in `_radionet` — it's a separate file-scope global at `:30`).  Amended §1.1 to split the row: RX seq stays in `Ep6RecvThread` per §1.1; TX seq moves to `wire/MetisFrame.cpp` TU-scope per §6-B | doc clarification | `network.h:86-87` vs `networkproto1.c:30` |

### §6 sign-off rows superseded by §6-B

| §6 row | Original status | §6-B status |
|---|---|---|
| Q3 — `out_seq_num_` as Ep2SendThread member | signed Lyra-native deviation | **SUPERSEDED** — TU-scope global mirrors reference |
| Q5 — `send_lock_` mutex | signed Lyra-native deviation | **SUPERSEDED** — no lock, temporal separation mirrors reference |
| §6-A retained-additions row "`out_seq_num_` encapsulated" | signed | **SUPERSEDED** by fix #2 |
| §6-A retained-additions row "`send_lock_` mutex" | signed | **SUPERSEDED** by fix #4 |
| §1.1 row "Sequence counters → wire/Ep6RecvThread" | signed | **AMENDED** — splits RX (stays) vs TX (moves to `wire/MetisFrame.cpp` TU) |

### §6-B sign-off

- [x] Source-verified line-by-line vs `networkproto1.c:30, 216-237`
- [x] `metis_write_frame()` body unchanged from §6.9 (header + seqnum + memcpy + sendto verbatim)
- [x] Reference seqnum post-increment + BE-pack idiom preserved
- [x] No lock — temporal separation contract documented in `wire/MetisFrame.h` header comment
- [x] §1.1 row amended in PARITY_CHECKPOINTS.md (single-line split: RX vs TX seq counter homes)
- [x] §7 ForceCandC consumes the same TU primitives via `metis_write_frame()` + the TU globals (no new wire-emit copy)
- [x] Build green; §5 + §6 send/receive parity unchanged (byte-identical wire output)
- [x] Authorized to populate (operator directive 2026-06-06 — "FIX it so it is like the reference, NO PATCHING")

Signed: N8SDR        Date: 2026-06-06

---

## §7. `ForceCandC` — startup C&C priming (source: `networkproto1.c:106-139`)

Sends 3 priming C&C frames per VFO (TX freq via case-2, RX1 freq
via case-4) at HL2Stream session-open, BEFORE `Ep2SendThread`
starts the main send loop.  Without this, post-priming RX freq
updates can be missed by HL2 gateware per CLAUDE.md §3.2 (the
duplex-bit-deferred-to-main-loop nuance).

Reference call site: `MetisReadThreadMainLoop_HL2` at
`networkproto1.c:430` invokes `ForceCandCFrame(3);` at the TOP of
the read loop, before the EP6 read loop begins consuming
datagrams.  Lyra's equivalent: `HL2Stream::session_open()` calls
`ForceCandC::prime(3, tx_freq, rx_freq)` synchronously, BEFORE
spinning `Ep2SendThread::start()` and `Ep6RecvThread::start()`
— wire-up lands in Phase 2 step 14 (next commit).

### §7.1 Buffer layout — 1024-byte zero-filled, two USB frames (source: `networkproto1.c:107-109`)

| Aspect | Reference | Lyra |
|---|---|---|
| Total buffer | `unsigned char buf[1024]; memset(buf, 0, sizeof(buf));` (`:107, :109`) | Same — `std::array<std::uint8_t, 1024> buf{};` (zero-initialized) |
| Per-USB-frame size | 512 bytes (frame 0 at 0..511; frame 1 at 512..1023) | Same |

**VERDICT:** ✅ **PARITY**

### §7.2 USB Frame 0 — fixed C&C header (source: `networkproto1.c:111-118`)

| Aspect | Reference | Lyra |
|---|---|---|
| `buf[0..2]` sync | `buf[0]=0x7f; buf[1]=0x7f; buf[2]=0x7f;` (`:111-113`) | Same — verbatim |
| `buf[3]` c0 | `buf[3]=0; /* c0 */` (`:114`) | Same — `buf[3]=0x00;` (frame 0 header byte) |
| `buf[4]` c1 | `buf[4]=(SampleRateIn2Bits & 3); /* c1 */` (`:115`) | Same — reads `lyra::wire::SampleRateIn2Bits & 0x03` |
| `buf[5]` c2 | `buf[5]=0; /* c2 */` (`:116`) | Same |
| `buf[6]` c3 | `buf[6]=0; /* c3 */` (`:117`) | Same |
| `buf[7]` c4 | `buf[7]=(nddc - 1) << 3;` (`:118`) — **no duplex bit set** (= 0x18 for HL2 nddc=4) | Same — reads `lyra::wire::nddc`, computes `(nddc - 1) << 3`.  **Per CLAUDE.md §3.2: priming explicitly does NOT set the duplex bit (c4 bit 2 = 0); main-loop frame-0 emission DOES set it (c4 = 0x1C).  Reference defect/quirk preserved verbatim per Rule 24** |

**VERDICT:** ✅ **PARITY** — incl. verbatim no-duplex-bit priming quirk

### §7.3 USB Frame 1 — VFO freq slot (source: `networkproto1.c:120-127`)

| Aspect | Reference | Lyra |
|---|---|---|
| `buf[512..514]` sync | `buf[512]=0x7f; buf[513]=0x7f; buf[514]=0x7f;` (`:120-122`) | Same — verbatim |
| `buf[515]` c0 | `buf[515]=c0; /* c0 */` (`:123`) — caller-supplied (2 = TX freq slot, 4 = RX1 freq slot) | Same — caller-supplied `int c0` parameter |
| `buf[516]` c1 (freq byte 0) | `buf[516]=(vfofreq >> 24) & 0xff;` (`:124`) | Same |
| `buf[517]` c2 (freq byte 1) | `buf[517]=(vfofreq >> 16) & 0xff;` (`:125`) | Same |
| `buf[518]` c3 (freq byte 2) | `buf[518]=(vfofreq >> 8) & 0xff;` (`:126`) | Same |
| `buf[519]` c4 (freq byte 3) | `buf[519]=vfofreq & 0xff;` (`:127`) | Same — BE-pack of 32-bit freq verbatim |

**VERDICT:** ✅ **PARITY**

### §7.4 Send loop — N frames via shared `metis_write_frame` (source: `networkproto1.c:129-131`)

| Aspect | Reference | Lyra |
|---|---|---|
| Loop | `for (i = 0; i < count; i++) { MetisWriteFrame(2, (char*)buf); }` (`:129-131`) | Same — `for (int i = 0; i < count; ++i) { metis_write_frame(0x02, buf.data()); }` |
| EP endpoint | `2` (EP2) | Same — `0x02` |
| Wire primitive | `MetisWriteFrame` — shared with `sendProtocol1Samples` + `WriteMainLoop_HL2`; consumes one `MetisOutBoundSeqNum` per call | Same — `metis_write_frame()` (per §6-B fix #1, TU-scope) shared with `Ep2SendThread::run_loop`; consumes one `g_metis_out_seq_num` per call (per §6-B fix #2) |

**VERDICT:** ✅ **PARITY** — shared wire primitive + shared TU-scope seq counter (per §6-B)

### §7.5 `ForceCandCFrame` priming sequence — TX freq + RX freq + sleeps (source: `networkproto1.c:134-139`)

| Aspect | Reference | Lyra |
|---|---|---|
| TX-freq priming pass | `ForceCandCFrames(count, 2, prn->tx[0].frequency);` (`:135`) | Same — `prime_pass(count, 2, tx_freq_hz);` (caller passes `prn->tx[0].frequency` per §7.6) |
| First sleep | `Sleep(10);` (`:136`) — 10 ms | Same — `std::this_thread::sleep_for(std::chrono::milliseconds(10));` (⚠ idiom translation — Win32 `Sleep` → C++23 portable equivalent; identical 10 ms suspension) |
| RX1-freq priming pass | `ForceCandCFrames(count, 4, prn->rx[0].frequency);` (`:137`) | Same — `prime_pass(count, 4, rx_freq_hz);` |
| Second sleep | `Sleep(10);` (`:138`) — 10 ms | Same — `std::this_thread::sleep_for(std::chrono::milliseconds(10));` |

**VERDICT:** ✅ **PARITY** — sequence + sleep timing verbatim

### §7.6 Call-site placement — HL2Stream session-open (source: `networkproto1.c:430`)

| Aspect | Reference | Lyra |
|---|---|---|
| Caller | `MetisReadThreadMainLoop_HL2()` (`:422`) | `HL2Stream::session_open()` — Phase 2 step 14 wire-up (next commit) |
| Position | Top of read loop function, BEFORE EP6 read loop begins (`:430`) | Top of session-open, AFTER socket bind + `metis_wire_bind()`, BEFORE `Ep6RecvThread::start()` AND BEFORE `Ep2SendThread::start()` — same temporal-separation contract preserves the §6-B no-lock invariant |
| Count argument | `ForceCandCFrame(3);` — 3 frames per pass × 2 passes = 6 datagrams + 20 ms of sleeps | Same — `force_candc_.prime(3, tx_freq, rx_freq);` |

**VERDICT:** ✅ **PARITY**

### Lyra-native additions (acceptable, signed off)

| Item | Note |
|------|------|
| `std::this_thread::sleep_for` in lieu of Win32 `Sleep` | ⚠ idiom translation — identical 10 ms suspension semantics; portable C++23 equivalent |
| `metis_wire_bind()` setter for TU-scope socket/dest globals | §6-B fix #3 — Lyra-native setter; reference initializes its `listenSock` and `prn->base_outbound_port` via the discovery/startup path |
| `ForceCandC::prime_pass()` exposed as a public method (in addition to `prime()`) | Lyra-native — mirrors the reference's two-function split (`ForceCandCFrames` + `ForceCandCFrame`) for symmetry; identical wire bytes per pass |

### §7 sign-off

- [x] Source-verified line-by-line vs `networkproto1.c:106-139, 430`
- [x] Buffer layout byte-for-byte verbatim (sync, c0..c4, BE-pack)
- [x] `c4 = (nddc - 1) << 3` priming form preserved — **no duplex bit set** (CLAUDE.md §3.2 quirk per Rule 24)
- [x] Two-pass + double-sleep sequence verbatim
- [x] Shared `metis_write_frame()` + `g_metis_out_seq_num` (per §6-B) — single seq stream across priming + main send
- [x] Synchronous on session-open thread, BEFORE Ep2SendThread / Ep6RecvThread start (temporal-separation contract preserved)
- [x] Build green
- [x] Authorized to populate (operator directive 2026-06-06)

Signed: N8SDR        Date: 2026-06-06

---

## §1-C Comprehensive correction sweep — full "do as reference, period" sweep across the TX wire layer

Operator directive 2026-06-06: "Fix everything to reference —
§1.1 included.  No patching.  Things must be fixed and work
like the reference does."

Triggered by the post-§6-B comprehensive 6-agent audit
(2026-06-06 mid-morning) which found §6-Q3/Q5-class signed
deviations across §1, §3, §5, §6 — most importantly that §1.1
itself (the signed 🔴 networking-infrastructure exclusion from
2026-06-04) was a structural deviation of the same class as
the §6 Q3/Q5 deviations §6-B reverted that same morning.
Under strict "do as reference, period," §1.1's exclusion of
`_radionet` networking fields from RadioNet was unjustified;
the §1-C sweep reverts it.

### Stages (shipped in committable increments, all build-clean)

| Stage | Commit | Scope |
|---|---|---|
| **§1-C Stage 1** | `0e2a375` | Small reverts: `wb_enable` `std::atomic<long>` → `volatile long` (matches reference `network.h:160`); OutboundRing bounded `try_acquire_for(5s)` producer push → unbounded `acquire()` (matches reference `Inbound`/`obbuffs` producer pattern); Ep2SendThread `datagrams_sent_`/`send_errors_` diagnostic counters dropped (no reference counterpart) |
| **§1-C Stage 2A** | `2b78d9e` | Router class → plain struct + free functions (`xrouter`/`register_sink`/`set_control_word`/`set_call_count`/`router_instance`) + file-scope `g_routers[]` array — direct mirror of reference's `ROUTER` typedef'd struct + `router.c` free functions + `prouter[MAX_EXT_ROUTER]` global at `router.c:29-30`.  §5.8 `std::function` callback retained (signed C↔C++23 idiom translation) |
| **§1-C Stage 3** | `d88ddd6` | OutboundRing wait-all via `std::condition_variable` + 4 `bool` flags + single mutex — replaces 4 `std::binary_semaphore`s + 100 ms polling.  Direct semantic mirror of reference's `WaitForMultipleObjects(2, hsendEventHandles, TRUE, INFINITE)` atomic wait-all at `networkproto1.c:1220`.  Removes 100-200 ms shutdown-latency floor + the §6-A binary_semaphore UB-guard workaround. |
| **§1-C Stage 4A** | `e157301` | §1.1 networking-infrastructure REVERT — additive field additions to RadioNet: buffer pointers (RxBuff, TxReadBufp, ReadBufp, OutBufp, outLRbufp, outIQbufp), RX seq counters (cc_seq_no, cc_seq_err), thread handles (hReadThreadMain, hWriteThreadMain, hKeepAliveThread), init semaphores (hReadThreadInitSem, hWriteThreadInitSem), outbound sync (cv_outbound + mu_outbound + 4 bool flags + outbound_stop — cv-collapsed mirror of reference's 4-HANDLE pattern), waitable timer (hTimer, liDueTime — Win32, HL2-inert), WSA event (hDataEvent, wsaProcessEvents — Win32), networking ports (p2_custom_port_base, base_outbound_port).  Wire-inert; consumers migrate in 4B/4C/4D. |
| **§1-C Stage 4B** | `26acea3` | Ep6RecvThread refactor: buffer members `rx_buff_[kMaxDdc]` / `tx_read_bufp_` / `control_bytes_in_[5]` / `last_seq_` / `seq_seen_` / `rx_datagrams_` / `seq_errors_` MIGRATED.  `_radionet` fields → `prn->RxBuff` / `prn->TxReadBufp`.  File-scope-globals (per reference) → TU-scope statics in Ep6RecvThread.cpp (`g_metis_last_recv_seq` / `g_seq_error` / `g_seq_seen` / `g_control_bytes_in[5]`).  Diagnostic counter `rx_datagrams_` dropped (Stage 1 §6.12 precedent).  Replaced `recv()` + timeout poll loop with `WSAEventSelect` + `WSAWaitForMultipleEvents` mechanism reading `prn->hDataEvent` + `prn->wsaProcessEvents` (folds in §5.10 audit fix). |
| **§1-C Stage 4B.1** | `f1031da` | Correction-of-correction (Rule 24 audit catch): Stage 4B mismapped the HL2 P1 EP6 raw receive buffer to `prn->ReadBufp` — but per reference, `prn->ReadBufp` (network.h:63) is the P2 inbound buffer (used at network.c:667+); HL2 P1 EP6 uses `FPGAReadBufp` (network.h:498) which is file-scope, NOT in `_radionet`.  Added TU-scope `g_fpga_read_bufp` in Ep6RecvThread.cpp as direct file-scope-global mirror; `prn->ReadBufp` stays declared in RadioNet for P2-family forward-compat parity (HL2-inert). |
| **§1-C Stage 4C** | `1d73bea` | Ep2SendThread refactor: `out_buf_` (1008 LRIQ-packed bytes) → `prn->OutBufp` (IS in `_radionet`, network.h:64).  `fpga_write_buf_` (1024 EP2 payload) → TU-scope `g_fpga_write_bufp` static (reference `FPGAWriteBufp` at network.h:499 is file-scope, NOT in `_radionet` — sister of g_fpga_read_bufp). |
| **§1-C Stage 4D** | `79fe4ec` | **§1.1 revert COMPLETE.**  OutboundRing class DISSOLVED into namespace-scope free functions (`outbound_init`, `outbound_push_lr`, `outbound_push_iq`, `outbound_wait_pair_ready`, `outbound_lr_buf` / `_mut`, `outbound_iq_buf` / `_mut`, `outbound_notify_consumed_pair`, `outbound_unblock`) operating on `prn->outLRbufp` / `prn->outIQbufp` / `prn->cv_outbound` / `prn->mu_outbound` / 4 bool flags / `prn->outbound_stop`.  Sister-pattern of Stage 2A's Router dissolution.  Constants `kSamplesPerDatagram` / `kDoublesPerBuffer` exposed at namespace scope as `kOutboundSamplesPerDatagram` / `kOutboundDoublesPerBuffer`.  Ep2SendThread loses `OutboundRing* ring_` member + signature param. |

### §6-Q3/Q5-class candidates resolved

| Audit-flagged item | Resolution stage | Status |
|---|---|---|
| §1.3 `std::atomic<long> wb_enable` (vs reference `volatile long`) | Stage 1 | ✅ FIXED |
| §5.9 `class Router` (vs reference free functions + array) | Stage 2A | ✅ FIXED |
| §5.10 `recv()` + timeout (vs reference `WSAEventSelect`) | Stage 4B | ✅ FIXED |
| §6.9 OutboundRing sequential `try_acquire_for(100ms)` polling (vs reference `WaitForMultipleObjects(TRUE)` wait-all) | Stage 3 | ✅ FIXED (cv-based wait-all) |
| §6.10 Bounded `try_acquire_for(5s)` producer push (vs reference unbounded) | Stage 1 | ✅ FIXED |
| §6.11 / §6.14 OutboundRing unblock() polling-shutdown UB-guard | Stage 3 | ✅ FIXED (cv notify_all subsumes) |
| §6.12 `datagrams_sent_` / `send_errors_` / `push_timeouts_*` diagnostic counters (no reference counterpart) | Stage 1 + 4B | ✅ FIXED |
| **§1.1 networking-infrastructure exclusion (THE BIG ONE)** | **Stages 4A-4D** | **✅ FIXED — see §1.1 amendment below** |

### Stage 4E §1.1 verdict amendment

The original §1.1 entry (signed 🔴 OPERATOR-APPROVED DEVIATION
2026-06-04) is now superseded.  Replacement verdict:

| Aspect | Reference | Lyra (post-§1-C) |
|---|---|---|
| `_radionet` fields | `network.h:58-106` (buffer pointers, thread/sem/event handles, RX seq counters, networking ports, WSA event, waitable timer) | All present in `RadioNet` class as members, identical field names, C↔C++23 idiom-translated types (`HANDLE` → `std::thread` / `std::counting_semaphore` for thread-class fields; `volatile long` → `volatile long` preserved verbatim; 4 reference HANDLE pairs collapsed to single `cv` + 4 bool flags per the §1-C Stage 3 design since C++20 `std::counting_semaphore` lacks wait-all primitive) |
| Reference file-scope globals (NOT in `_radionet`) | `networkproto1.c:26-30` (`SeqError`, `MetisLastRecvSeq`, `MetisOutBoundSeqNum`), `network.h:414` (`ControlBytesIn[5]`), `network.h:498-499` (`FPGAReadBufp`, `FPGAWriteBufp`), `listenSock` | TU-scope statics in the appropriate wire-layer .cpp file: `wire/MetisFrame.cpp` (g_metis_out_seq_num, g_metis_socket_fd + dest), `wire/Ep6RecvThread.cpp` (g_metis_last_recv_seq, g_seq_error, g_seq_seen, g_control_bytes_in, g_fpga_read_bufp), `wire/Ep2SendThread.cpp` (g_fpga_write_bufp) |
| Wire-layer access pattern | Reference free functions in `networkproto1.c` / `router.c` / `cmaster.c` dereference `prn->...` globals directly | Lyra wire-layer methods/free-functions dereference `prn->...` globals directly + read the TU-scope statics for the non-`_radionet` globals.  No more wire-layer class members duplicating reference state. |

**VERDICT:** ✅ **PARITY** — every reference state-bearing
field now lives at its correct reference home in Lyra (either
inside `RadioNet` for `_radionet`-resident fields, or as
TU-scope statics in the appropriate wire-layer .cpp for
file-scope globals).  The 2026-06-04 🔴 OPERATOR-APPROVED
DEVIATION verdict at the top of §1.1 is SUPERSEDED by this
amendment; the structural deviation it documented no longer
exists in the shipped code.

### §1-C sign-off

- [x] Source-verified line-by-line vs `network.h:58-106` +
      `networkproto1.c:26-30, 422-586, 1204-1267` +
      `router.c:29-156`
- [x] All 8 audit-flagged §6-Q3/Q5-class candidates fixed
      (§1.3 wb_enable, §5.9 Router, §5.10 WSAEventSelect,
      §6.9 wait-all, §6.10 unbounded push, §6.11/6.14
      shutdown, §6.12 diag counters, §1.1 itself)
- [x] All `_radionet` fields now in RadioNet as members
- [x] All reference file-scope globals (NOT in `_radionet`)
      now as TU-scope statics in the correct wire-layer .cpp
- [x] Wire-layer class members for reference-state fields
      all removed (no duplicate copies; single source of
      truth)
- [x] §1.1 verdict amended from 🔴 to ✅ PARITY (above)
- [x] Build green throughout all 8 stages
- [x] Wire-inert — `HL2Stream::session_open` not yet calling
      anything (step 14 wire-up next)
- [x] Stage 4E ships this consolidated §1-C entry +
      supersedes §1.1 verdict

Signed: N8SDR        Date: 2026-06-06

---

## §14 Step 14 — Phase 2 wire-up into `HL2Stream::open()`

Operator-locked plan in `docs/architecture/STEP14_PLAN.md` (8 stages,
2 bench-critical commits).  Three operator corrections folded
2026-06-06: priming pass INSIDE `Ep6RecvThread::run_loop` per
reference order; chunk granularity = 2×19 samples per datagram per
reference per-USB-frame xrouter dispatch; single `MetisOutBoundSeqNum`
across priming + steady-state.  §5.8 Router callback shape verified
SAFE-AS-IS for PureSignal compatibility (signed C↔C++23 idiom
translation; reference `function[port][i][ctrl]` + `callid[port][i]
[ctrl]` collapse into one `std::function` closure preserving the
`port × call_idx × ctrl_word` table).  Standing rule: "do as
reference, period, NO PATCHING."

### §14 Stage 1 — wire-layer `create_rnet()` + bind + outbound_init (wire-INERT)

**CORRECTED 2026-06-06** after the operator audit caught the
initial Stage 1 commit (`9bf963e`) shipping two patches against
the standing "do as reference, period" rule:

1. **The singleton itself.** Initial Stage 1 added a `radio_net()`
   Meyers-singleton accessor returning `&static RadioNet instance`.
   Reference at `netInterface.c:1595` uses
   `prn = (RADIONET)malloc(sizeof(radionet));` — heap-allocation,
   no accessor.  Fix: replaced `radio_net()` with `create_rnet()`
   doing `prn = new RadioNet();` (C++ equivalent of malloc) +
   the full per-element scalar/sub-struct/buffer init.

2. **Buffer-init split.** §1-C-signed Lyra spread buffer
   allocation across three call sites (`outbound_init()` for
   outLRbufp/outIQbufp; `Ep6RecvThread::start()` for RxBuff /
   TxReadBufp; `Ep2SendThread::start()` for OutBufp).  Reference
   allocates ALL `_radionet` buffers in one `create_rnet()` at
   startup (`netInterface.c:1600-1608`).  Fix: consolidated all
   buffer allocation into `create_rnet()`; the per-thread sites
   now allocate only their TU-scope `g_fpga_read_bufp` /
   `g_fpga_write_bufp` (which the reference itself does at
   thread entry per `networkproto1.c:427-428`).

3. **Buffer sizes corrected.** `prn->OutBufp` was sized 1008
   bytes in Lyra (datagram-payload sized) vs reference's 1440
   bytes (`netInterface.c:1606`).  `prn->outLRbufp` / `outIQbufp`
   were 252 doubles in Lyra (one-datagram-stereo sized) vs
   reference's 1440 doubles (`netInterface.c:1607-1608`).  The
   1440-double outIQbufp size is load-bearing for EER mode
   (reference's `memcpy(prn->outLRbufp, prn->outIQbufp + 256,
   ...)` at `networkproto1.c:1225` reads past the 252-double
   front).  Fix: matched reference sizes in `create_rnet()`.

`HL2Stream::open()` now calls `lyra::wire::create_rnet();`
(idempotent — first call allocates + initializes, subsequent
open/close cycles return immediately) followed by
`lyra::wire::metis_wire_bind(socket_, destStorage_,
sizeof(sockaddr_in));` and `lyra::wire::outbound_init();`
(per-session sync-flag reset only — no buffer touch).

Symmetric `lyra::wire::metis_wire_bind(-1, nullptr, 0);`
teardown in `close()` after worker joins + socket close
clears the TU-scope bind so any stale post-close
`metis_write_frame()` fails fast on `sendto(-1)`.  `prn` and
its allocations persist for process lifetime (matches
reference's "create_rnet once, never free" posture).

**Reference provenance:**
- `create_rnet()` body (heap-allocate `_radionet` + all buffers
  + scalar/sub-struct/per-element init): `netInterface.c:1590-1763`.
- `prn` non-null contract before session-open body proceeds:
  `netInterface.c:40`.
- Per-session semaphore allocation (= Lyra-native bool-flag
  reset in `outbound_init`): `netInterface.c:68-71`.
- File-scope `listenSock` bind via `metis_wire_bind`: implicit
  at every `sendPacket(listenSock, ...)` site (`networkproto1.c:55,
  89, 234`).
- TU-scope `FPGAReadBufp` / `FPGAWriteBufp` allocated at thread
  entry (NOT in create_rnet): `networkproto1.c:427-428`.

**Wire-INERT:** `prn` now non-null with all reference-default
scalar values + reference-sized buffers; `metis_socket_fd()`
returns a valid fd; outbound sync flags initialized — but NO
new code path reads any of it yet.  `rxWorkerLoop` /
`txWorkerLoop` remain the live RX/TX path.  Build clean
(all 5 touched .cpp files recompiled), no new compile
warnings, Rule-2 grep clean across the 6 touched files.

**Files touched (Stage 1, corrected):**
- `src/hl2_stream.h` — added `std::uint32_t destStorage_[4]`
  private member (opaque sockaddr_in buffer; lifetime tied to
  HL2Stream object so the wire layer's caller-owned dest_addr
  pointer remains valid for the life of the EP2/EP6 threads).
- `src/hl2_stream.cpp` — added 3 wire-layer includes
  (`wire/RadioNet.h`, `wire/MetisFrame.h`, `wire/OutboundRing.h`),
  added the wire-layer init block in `open()` calling
  `create_rnet()` + `metis_wire_bind` + `outbound_init`, added
  the symmetric `metis_wire_bind(-1, nullptr, 0)` teardown in
  `close()`.
- `src/wire/RadioNet.h` — added `create_rnet()` declaration
  (replaces the initial `radio_net()` accessor).
- `src/wire/RadioNet.cpp` — added `create_rnet()` implementation
  mirroring `netInterface.c:1590-1763` verbatim: `prn = new
  RadioNet();`, all scalar/sub-struct init line-by-line cited,
  all buffer allocations (RxBuff/RxReadBufp/TxReadBufp/ReadBufp/
  OutBufp/outLRbufp/outIQbufp) at reference-faithful sizes.
- `src/wire/OutboundRing.cpp` — removed buffer-allocation from
  `outbound_init()`; now per-session sync-flag reset only
  (matches reference's `netInterface.c:68-71` semaphore-init
  posture, Lyra-native bool-flag equivalent).
- `src/wire/Ep6RecvThread.cpp` — removed `prn->RxBuff` +
  `prn->TxReadBufp` sizing from `start()`; `g_fpga_read_bufp`
  sizing stays (TU-scope mirror of reference's `FPGAReadBufp`
  allocated at thread entry, `networkproto1.c:427`).
- `src/wire/Ep2SendThread.cpp` — removed `prn->OutBufp` sizing
  from `start()`; `g_fpga_write_bufp` sizing stays (TU-scope
  mirror of reference's `FPGAWriteBufp`, `networkproto1.c:428`).

**VERDICT:** ⏳ **PARITY — pending bench gate**

**Bench gate:** build-clean (done) + 60s RX soak side-by-side
with HEAD comparing EP2 send rate + EP6 recv rate + seq-error
counter within ±0.1%.  Operator confirms on dummy-load bench;
no HL2 hardware interaction differs from HEAD (this stage is
wire-INERT, zero new wire bytes go out).

**Rollback risk:** small.  Allocation consolidated into one
new function + 3 sites simplified.  Revert path: drop
`create_rnet()` body, restore the buffer .assign() calls in
the three thread/init sites + the destStorage_ +
metis_wire_bind / outbound_init block in `open()`.

**Audit gap surfaced + corrected:** the original §1-C audit
verified field LOCATIONS (where state lives) and CALLBACK
SHAPES (dispatch semantics) but did NOT audit ALLOCATION
PATTERNS (heap vs static, where buffers get sized, single vs
multi call-site allocation).  This stage's correction
introduced an explicit pre-write reference grep gate for
every new function + every new allocation in future stages,
with file:line citation in the implementation comment.

Signed: _____         Date: __________

---

*Last updated: 2026-06-06 — §14 Stage 1 CORRECTED + SHIPPED.  Operator audit caught the initial Stage 1 commit (`9bf963e`) shipping a Meyers-singleton patch (`radio_net()` returning `&static RadioNet`) against the standing "do as reference, period" rule + a §6-Q-class miss in the §1-C audit (buffer allocation split across 3 sites vs reference's single `create_rnet()` allocator).  Correction commit replaces `radio_net()` with `create_rnet()` mirroring `netInterface.c:1590-1763` verbatim (heap-allocate via `new RadioNet()`, all scalar + sub-struct + buffer init in one place, reference-faithful buffer sizes 1440 bytes / 1440 doubles).  Buffer init removed from `outbound_init` / `Ep6RecvThread::start` / `Ep2SendThread::start` (now only the TU-scope `g_fpga_read_bufp` / `g_fpga_write_bufp` mirrors stay at thread entry, per `networkproto1.c:427-428`).  Methodology tightened: every new function + new allocation in future stages now requires a pre-write reference grep with file:line cited in the implementation comment.  Earlier today: §1-C comprehensive correction sweep COMPLETE (twelve commits across the morning + afternoon; §1.1 networking-infrastructure exclusion fully reverted; Router + OutboundRing dissolved to free functions; build clean throughout).*

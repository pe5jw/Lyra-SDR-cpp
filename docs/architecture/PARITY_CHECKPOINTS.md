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
| Sequence counters | `network.h:86-87` `unsigned int cc_seq_no, cc_seq_err;` | Lives in `wire/Ep6RecvThread` (per-direction seq tracking is a wire-layer concern, not radio state) |
| Networking config ports | `network.h:58-59` `int p2_custom_port_base; int base_outbound_port;` | Lives in `wire/Ep6RecvThread` / `wire/Ep2SendThread` constructors (passed in from HL2 discovery, not session state) |

**VERDICT:** 🔴 **OPERATOR-APPROVED DEVIATION** — these fields are
PRESENT in `_radionet` but EXCLUDED from `RadioNet` per the §10.2
component split signed 2026-06-04.  The §10.2 mapping doc names the
exact destination wire-layer component for each.  No state-bearing
field is dropped — every field re-homes to a wire-layer component
that owns it.

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
- **Per-family DDC routing matrix** (the per-(mox, ps_armed,
  hpsdrModel) dispatch function) — lands with `DdcMap`'s §N
  entry.
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

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

**OPERATOR SIGN-OFF (awaiting your review):**

- [ ] §4a architecture lock Q1–Q5 reviewed + confirmed (switch
      style, eager presence, inline family branches, single
      mutex, once-per-frame advance)
- [ ] §4a.1 scheduler structure — reference posture preserved
      verbatim including the MOX-edge jump, I2C overlay, and the
      no-advance-on-I2C cursor behavior
- [ ] §4a.2 mutex idiom accepted (`std::mutex` ↔ `CRITICAL_SECTION`,
      same as §1.11)
- [ ] §4a.3 case 0 — every C-byte bit-position verbatim from
      `networkproto1.c:948-970`
- [ ] §4a.4 case 2 — verbatim from `:982-993`
- [ ] §4a.5 case 3 — verbatim from `:995-1010` including all
      three nddc-conditional branches preserved
- [ ] §4a.6 out-of-scope cases land as `assert(false)` placeholders
      with eager compile-time presence; FrameComposer stays
      WIRE-INERT until §7 `Ep2SendThread` ships
- [ ] Authorized to populate `src/wire/FrameComposer.{h,cpp}`
      matching this checkpoint (scheduler + cases 0/2/3; cases 1
      and 4-18 as `assert(false)` placeholders)

Signed: _______________ Date: _______________

---

*Last updated: 2026-06-05 — §4a FrameComposer scheduler + RX-essential cases REWRITTEN per Rule 24 source-verification (post-2026-06-05 XmitBit-finding + scope corrections: case 1 is TX VFO not RX1, removed Q1/Q2 deviations).*

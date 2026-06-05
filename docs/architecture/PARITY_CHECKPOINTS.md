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

**Reference root:** `D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13\Project Files\Source\`.

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
| Type definition | `network.h:56-289` `typedef struct CACHE_ALIGN _radionet { ... } radionet, *RADIONET;` | `class RadioNet { ... };` (already shipped Phase 1 empty skeleton) |
| Cache alignment | `CACHE_ALIGN` = `__declspec(align(16))` (`network.h:38`) | `alignas(16) class RadioNet { ... };` (or `alignas(std::hardware_destructive_interference_size)` — C++17 idiom; pick the explicit `16` to match the reference verbatim) |
| Global instance | `network.h:291` `RADIONET prn;` (a global pointer) | `RadioNet* g_radioNet = nullptr;` initialized at HL2 session start (Phase 2 work); access via the global — same pattern as the reference (no singleton wrapper, no smart pointer; one process-lifetime instance owned by the wire-layer init) |

**VERDICT:** ✅ **PARITY** on access pattern (global pointer to one
instance, process-lifetime); ⚠ **ACCEPTABLE DEVIATION** on:
(a) `struct + typedef` → `class` (C++23 idiom; same memory
layout); (b) global instance name `prn` → `g_radioNet` (Lyra-
native naming; `prn` was the reference's terse pointer name,
not informative in a multi-radio future per Q2); (c) cache-align
spelling (`__declspec(align(16))` → `alignas(16)`, same width).

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
  §10.2 these split across `Hl2DispatchState`, `Hl2Capabilities`,
  `Hl2FrameComposer`, and family-enum scaffolding).

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
| §1.12 global + alignment | ✅ PARITY (⚠ Lyra-native naming) |

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

*Last updated: 2026-06-05 — §1 RadioNet draft for operator review.*

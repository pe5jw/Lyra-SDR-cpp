# lyra-cpp — Session Log

Running EOD log. Newest entry on top. Short rough-outline format.

---

## 2026-06-06 (Saturday LATE-AFTERNOON — §1-C FULLY COMPLETE + RE-AUDIT CLEAN)

**Branch:** `tx-rebuild` HEAD = `169f1c2`.  Build clean,
`lyra.exe` relinked at 11:10:09.  **16 commits shipped today**
+ TWO full multi-agent audit rounds.  §1-C arc COMPLETE
through Stage 4F.2 (FrameComposer dissolution).  Final
re-audit returned 37/37 PASS across 3 sections, zero remaining
§6-Q3/Q5-class candidates, zero forbidden tokens in src/wire/.

### What's reference-faithful now
- TX wire layer: 100% reference-faithful (all `_radionet`
  fields in RadioNet; all reference file-scope globals as
  TU-scope statics in correct .cpp; Router + OutboundRing
  + FrameComposer all dissolved into free functions matching
  reference; symmetric socket-binding via metis_wire_bind()
  in both Ep6RecvThread + Ep2SendThread).
- §1.1 verdict: 🔴 → ✅ PARITY (Stage 4E doc consolidation).

### What's NOT yet reference-faithful (tracked, NOT in §1-C)
- **Task #73** — pre-existing forbidden-token violations
  (Thetis / Console.cs / OpenHPSDR / PowerSDR) in 5 non-wire
  files: tci_server.{h,cpp}, mainwindow.cpp,
  hl2_stream.{h,cpp}, wdsp_native.h.  Doc-style cleanup,
  ~1 hour, no impact on shipped wire-layer paths.
- **Wire-INERT** — nothing in HL2Stream::open() calls the
  new wire-layer free functions yet (step 14 wire-up).
- **Task #114 deferrals** — EER mode, non-HL2
  WriteMainLoop_generic, PeakFwdPower/PeakRevPower helpers.
  Hardware-blocker (no ANAN hardware to bench-test).

### Step 14 scope (NOT trivial)
HL2Stream is 4203 lines total with its own monolithic
`rxWorkerLoop` + `txWorkerLoop` member methods at
`hl2_stream.cpp:921` + `:2437` (the OLD wire path).  Step 14
must REPLACE these (per Rule 7 "Delete TX, don't refactor")
with calls into the new wire-layer free functions:
1. `prn` global pointer assigned at session-open
2. `metis_wire_bind(socket, dest, dest_len)`
3. `outbound_init()` to size prn->outLRbufp/outIQbufp
4. `ForceCandC::prime(3, tx_freq, rx_freq)` synchronously
   per §7 temporal-separation contract
5. `Ep6RecvThread::start(socket_fd)` → spins RX thread
   reading from prn-> + WSAEventSelect
6. `Ep2SendThread::start(socket, dest, dest_len)` → spins
   TX thread; no more composer / ring params (Stage 4F.2)
7. Migrate HL2Stream's existing telemetry/mic/IQ callbacks
   to the new sink-based registration (Router::register_sink
   for IQ; Ep6RecvThread::set_*_sink for telemetry/mic/I2C)

**Plan-first commit (R6).**  Bench-critical (first time the
new wire layer actually emits datagrams to the radio).
Multi-hour, possibly multi-commit.  Wrong wire-up = broken
RX/TX on real HL2 hardware.

### Resume after step 14
Operator-bench verification of RX-side audio + telemetry on
real HL2+ hardware.  Then TX-side bench (CW/SSB + Palstar
power readings).  Then Task #73 cleanup (small, doc-only,
can happen anytime).

### Wire status
Wire-INERT.  All wire-layer components reference-faithful
but not connected to runtime.

---

## 2026-06-06 (Saturday AFTERNOON — §1.1 REVERT COMPLETE — historical)

**Branch:** `tx-rebuild` HEAD = `79fe4ec`.  Build clean,
`lyra.exe` relinked at 10:45:10.  **TWELVE commits shipped
today** + 6-agent comprehensive TX audit done.  ALL stages
of the §1-C sweep complete in code (§1.1 networking-
infrastructure exclusion fully reverted).  Only Stage 4E
(PARITY_CHECKPOINTS doc consolidation) remains before step 14
wire-up.

### Today's commits (newest first)
- `79fe4ec` §1-C Stage 4D — OutboundRing dissolves to free functions (§1.1 revert COMPLETE)
- `1d73bea` §1-C Stage 4C — Ep2SendThread reads from prn->OutBufp + g_fpga_write_bufp
- `f1031da` §1-C Stage 4B.1 — correct FPGAReadBufp scope (file-scope, not _radionet)
- `26acea3` §1-C Stage 4B — Ep6RecvThread reads from prn-> + WSAEventSelect
- `e157301` §1-C Stage 4A — add networking-infrastructure fields to RadioNet
- `d88ddd6` §1-C Stage 3 — OutboundRing wait-all via condition_variable
- `2b78d9e` §1-C Stage 2A — Router class → struct + free functions
- `0e2a375` §1-C Stage 1 — small reverts (wb_enable, push timeout, diag counters)
- `50b713b` §6-B nit — drop metis_write_frame null-guard
- `3bfd248` §6-B + §7 — TU-scope MetisFrame + ForceCandC populate

### §1.1 revert summary
All `_radionet` fields the original §1.1 exclusion (signed 🔴
2026-06-04) had punted to wire-layer class members now live in
RadioNet as members exactly per reference:
- Buffer pointers: `RxBuff`, `TxReadBufp`, `outLRbufp`,
  `outIQbufp`, `OutBufp` (+ `ReadBufp` declared P2-future-use)
- RX seq counters: `cc_seq_no`, `cc_seq_err` (P2-only;
  declared in RadioNet but HL2-inert)
- Thread handles: `hReadThreadMain`, `hWriteThreadMain`,
  `hKeepAliveThread` (std::thread types)
- Init semaphores: `hReadThreadInitSem`, `hWriteThreadInitSem`
  (std::counting_semaphore<1>)
- Outbound sync (cv-collapsed mirror of 4 reference HANDLE
  pairs): `cv_outbound` + `mu_outbound` + 4 bool flags +
  `outbound_stop`
- WSA event + waitable timer (Win32-only): `hDataEvent`,
  `wsaProcessEvents`, `hTimer`, `liDueTime`
- Networking config ports: `p2_custom_port_base`,
  `base_outbound_port`

Reference-FILE-SCOPE globals (NOT in `_radionet`) live as
TU-scope statics in their respective wire-layer .cpp files
per the §6-B precedent:
- `wire/MetisFrame.cpp`: `g_metis_out_seq_num` (was Ep2SendThread::out_seq_num_)
- `wire/Ep6RecvThread.cpp`: `g_metis_last_recv_seq`, `g_seq_error`, `g_seq_seen`, `g_control_bytes_in[5]`, `g_fpga_read_bufp`
- `wire/Ep2SendThread.cpp`: `g_fpga_write_bufp`

Wire-layer classes dissolved or reduced:
- `Router`: class → plain struct + free functions (Stage 2A)
- `OutboundRing`: class → namespace-scope free functions
  operating on `prn->...` (Stage 4D)
- `Ep6RecvThread`: still a class (thread orchestrator), but
  all data moved to `prn->...` or TU-scope statics
- `Ep2SendThread`: same — thread orchestrator, data moved out

### Remaining — Stage 4E (doc-only)
Consolidate the §1-C entry in `docs/architecture/PARITY_CHECKPOINTS.md`:
- Single §1-C entry covering Stages 1+2A+3+4A-D + the
  earlier §6-B + §7
- §1.1 verdict amended from 🔴 OPERATOR-APPROVED DEVIATION
  to ✅ PARITY (now genuinely byte/structure-faithful)
- Sign-off block + commit trailer

### Resume after Stage 4E
Phase 2 step 14 — wire `outbound_init()` + `metis_wire_bind()`
+ `ForceCandC::prime` + `Ep6RecvThread::start` + `Ep2SendThread::start`
into `HL2Stream::session_open()` in the correct temporal sequence.

### Wire status
Wire-INERT today.  All wire-layer components built but nothing
in `HL2Stream::session_open` calls them yet.

---

## 2026-06-06 (Saturday MID-DAY — STAGE 3 CHECKPOINT — historical)

**Branch:** `tx-rebuild` HEAD = `d88ddd6`.  Build clean, `lyra.exe`
relinked at 09:54:09.  Five commits shipped today + 6-agent
comprehensive TX audit done.  Stages 1+2A+3 of the §1-C sweep
DONE; Stages 2B+4 remain (bundled — §5.10 WSAEventSelect lives
in `_radionet` fields per reference, so it lands with §1.1
revert).

### Today's commits (newest first)
- `d88ddd6` §1-C Stage 3 — OutboundRing wait-all via condition_variable
- `2b78d9e` §1-C Stage 2A — Router class → struct + free functions
- `0e2a375` §1-C Stage 1 — small reverts (wb_enable, push timeout, diag counters)
- `50b713b` §6-B nit — drop metis_write_frame null-guard
- `3bfd248` §6-B + §7 — TU-scope MetisFrame + ForceCandC populate

### What got fixed to match reference (Stages 1+2A+3)
- §1.3: `std::atomic<long> wb_enable` → `volatile long` (matches `network.h:160`)
- §6.10: OutboundRing producer push — drop bounded 5s try_acquire_for + push_timeouts_*; unbounded `acquire()` mirroring reference Inbound/obbuffs
- §6.12: Ep2SendThread `datagrams_sent_`/`send_errors_` diagnostic counters dropped (no reference counterpart)
- §5.9: `class Router` → plain struct + free functions (`xrouter`/`register_sink`/`set_control_word`/`set_call_count`/`router_instance`) + file-scope `g_routers[]` array, mirroring reference `router.c` structure verbatim
- §6.9/6.11/6.14: OutboundRing wait-all via `std::condition_variable` + `bool` flags + single mutex (was 4 binary_semaphores + polling) — mirrors reference `WaitForMultipleObjects(2, hsendEventHandles, TRUE, INFINITE)` atomic wait-all, no polling, fully interruptible

### Remaining — Stage 4 (THE BIG ONE)
**§1.1 networking-infrastructure revert + §5.10 WSAEventSelect bundled.**

§1.1 (signed 🔴 2026-06-04) excludes from RadioNet a list of
fields that the reference puts INSIDE `_radionet`.  Under
strict "do as reference, period" §1.1 itself is a §6-Q3/Q5-
class candidate.  Stage 4 puts these fields back into RadioNet:
- Buffer pointers: `RxBuff`, `TxReadBufp`, `ReadBufp`, `OutBufp`, `outLRbufp`, `outIQbufp`
- Thread/sem/event handles: `hReadThreadMain`, `hsendEventHandles[2]`, `hobbuffsRun[2]`, etc.
- Waitable timer + WSA event: `liDueTime`, `hDataEvent`, `wsaProcessEvents`
- RX seq counters: `cc_seq_no`, `cc_seq_err`
- Networking ports: `p2_custom_port_base`, `base_outbound_port`

Then Ep6RecvThread + Ep2SendThread + OutboundRing read from
`prn->...` instead of their own members.  §5.10 (recv() →
WSAEventSelect) folds in because `hDataEvent` +
`wsaProcessEvents` are `_radionet` fields.

**Scope: multi-hour, multi-commit.**  Planned sub-stages:
- 4A: Add fields to RadioNet (additive, no behavior change)
- 4B: Ep6RecvThread refactor + WSAEventSelect mechanism
- 4C: Ep2SendThread refactor (reads buffers from RadioNet)
- 4D: OutboundRing — buffers + sync primitives move into
  RadioNet; class may dissolve into free functions
- 4E: PARITY_CHECKPOINTS §1-C entry finalized + §1.1 amendment

### Resume after Stage 4 completes
Phase 2 step 14 — wire `ForceCandC::prime` + `metis_wire_bind`
+ WSAEventSelect setup into `HL2Stream::session_open()` in the
correct temporal sequence.

### Wire status
Wire-INERT today.  All wire-layer components built but nothing
in `HL2Stream::session_open` calls them yet.

---

## 2026-06-06 (Saturday MID-DAY — AUDIT PAUSE BOOKMARK — historical)

**Branch:** `tx-rebuild` HEAD = `50b713b`.  Build clean, `lyra.exe`
relinked at 09:27:22.  All §6-B + §7 work shipped + audited
20/20 PASS this morning.

### Done this morning (2 commits, ALL like reference)
- `3bfd248` §6-B parity correction sweep + §7 ForceCandC populate
  - Hoisted `metis_write_frame` to `wire/MetisFrame.{h,cpp}` TU-scope
  - TU-scope `g_metis_out_seq_num` (was `Ep2SendThread::out_seq_num_`)
  - TU-scope socket/dest globals (were Ep2SendThread members)
  - DROPPED `Ep2SendThread::send_lock_` mutex (no reference counterpart)
  - §1.1 row split: RX seq stays / TX seq moves
  - `ForceCandC::prime` + `prime_pass` byte-for-byte from `networkproto1.c:106-139`
- `50b713b` §6-B nit — dropped `metis_write_frame` null-guard (no reference
  counterpart; audit-caught NOTE-D)
- Two parallel audits run: parity (20/10 PASS) + rule-compliance (10/10 PASS)

### Pause reason
Operator-requested **comprehensive TX audit** of ALL work since
the rebuild started (30 commits since `0348238` Phase 0 mapping
doc 2026-06-04).  Sections to audit against the reference:
- §1 RadioNet (`2171fb9`/`dcbf9d1`)
- §2 RbpFilter / RbpFilter2 (`228ffd8`)
- §3 HPSDR family + dispatch globals (`f3ccc51`/`55ae658`/`782e65d`)
- §4a/§4b-1/§4b-2/§4c FrameComposer 19-case dispatch
  (`93e1511`/`5fc4192`/`4446916`/`f06a796`)
- §5 / §5-A Ep6RecvThread + Router (`c8baa63`/`a6be425`/`b988177`/`6bbf449`)
- §6 / §6-A Ep2SendThread + OutboundRing (`4c580a1`/`89ab298`)
- §6-B + §7 already audited this morning (skipping re-audit)

### Resume point after audit
Resume here: **Phase 2 step 14 — wire-up `ForceCandC::prime` +
`metis_wire_bind` into `HL2Stream::session_open()`** in the
correct temporal sequence (bind → prime → Ep6 start → Ep2 start).

### Wire status
Wire-INERT today.  Nothing in `HL2Stream::session_open` calls
the new wire-layer components yet.  Step 14 (next commit after
audit closes) flips the wire on.

---

## 2026-06-05 (Friday EOD)

**Project:** lyra-cpp TX Wire-Layer Rebuild (Task #112) — branch `tx-rebuild`

### Done today
- **§5 EP6 Receive Path** — `Ep6RecvThread` + `Router` (`xrouter`/`twist`) populated (`c8baa63`)
- **§5-A 9-fix parity correction** (`a6be425`) including 🔴 CRITICAL RX-audio bug (IQ unpack scale 48 dB too quiet — caught before any consumer wired)
- **§5-A doc completeness** (`b988177`)
- **§6 EP2 Send Path** — `Ep2SendThread` + `OutboundRing` populated (`4c580a1`)
- **§6-A 6-fix parity correction** (`89ab298`) — MMCSS priority, per-family dispatch, CW bit-shift revert to verbatim, CW state per-sample reads, `binary_semaphore` UB guard, Rule 24 inline token
- 4 audit passes total (2 per §5 / §6) — all PASS after corrections

### Where we are
Branch `tx-rebuild` HEAD = `89ab298`. §5 + §6 of the wire-layer rebuild are **shipped + doubly-verified** (parity-audit + rule-audit + correction sweeps + re-audits, all clean). Wire-layer is still wire-inert — no Phase 1 code path instantiates the new components.

### Next up
**§7 — `ForceCandC` priming + ANY remaining wire-layer pieces.** Reference: `networkproto1.c::ForceCandCFrame` (lines 134-139) + `ForceCandCFrames` (lines 106-132). Small commit — just the 3-frame priming burst that runs at session start before the EP2 send loop begins.

After §7 completes the §1c-listed wire-layer skeletons, the next phase is **Phase 2 step 14 — wire-up**: instantiate Ep6RecvThread + Ep2SendThread + OutboundRing + Router + ForceCandC + FrameComposer into HL2Stream's session-open path.

### Pending / parked
- **Task #114** TX-policy plumbing — ATT-on-TX, panadapter offset, PA-enable safety, plus the §6 FIXMEs: `WriteMainLoop_generic` for non-HL2, EER mode (§6.4), `PeakFwdPower`/`PeakRevPower` helpers (§5 telemetry cases 0x08/0x10)
- **No hardware bench yet** — gateware-watchdog kill-test, real-antenna TX, foot-switch HW-PTT all parked until Phase 2 wire-up + verification
- **Don't push to GitHub** (standing rule)

### Commit summary (today)
```
89ab298 fix(wire): §6-A parity correction sweep — 6 fixes
4c580a1 feat(wire): populate §6 Ep2SendThread + OutboundRing
b988177 docs(parity): §5-A Lyra-native additions table — 2 audit-flagged rows
a6be425 fix(wire): §5-A parity correction sweep — 9 fixes
c8baa63 feat(wire): populate §5 Ep6RecvThread + Router
```

### Standing rules in effect
- Rule 24 "always verify against reference" — applied per commit, audit-verified
- 2026-06-05 directive "reference = make Lyra the same" — driving all Q-decisions
- §6 EER mode + non-HL2 dispatch + Peak power helpers all deferred to Task #114 with FIXMEs
- Operator hardware = HL2+/AK4951; no ANAN P1/P2 hardware available

---

*Session log started 2026-06-05.  Older entries (if any) below this line as they accumulate.*

# Lyra panadapter audit + "hybrid" concept locator (Task #44)

Companion to `panadapter_tx_audit_THETIS_2026-06-02.md`.  Inline
file:line audit produced 2026-06-02 to ground the Task #44
implementation plan in real shipped Lyra state — not aspirational
docs.

---

## A.  The "hybrid" concept the operator was recalling

**Found.**  Lives in `docs/refs/dual_rx/README.md:10`:

> ## Locked decision: Option A (Hybrid focused-RX) + EESDR active-VFO enhancements
>
> Not a Thetis clone, not pure EESDR — deliberately different, optimised
> for the HL2/HL2+ reality that "RX2" structurally maps to EESDR's
> SubRX (shared ADC + shared BPF passband) rather than to EESDR's true
> RX2 (independent main receiver).

This is the operator + working-group lock from 2026-05-11 — Rick
+ Brent + Timmy.  The PDF that captures the full design analysis
is `Lyra-v0.1-Phase3-UI-Candidates.pdf` (path noted at line 139),
also operator-locked.

**Crucially for Task #44, the hybrid spec explicitly addresses
the TX-state panadapter behavior** at `docs/refs/dual_rx/README.md:120-135`:

> ## Crossover with TX-state visual treatment
>
> The RED / gray TX indicator design here is the SAME visual language
> that drives the panadapter TX-state behavior (Task #44 area):
>
> * TX-state badge on the panadapter pane (top-right corner) =
>   RED accent same as the VFO-LED TX indicator.
> * Per-side selectivity: in SPLIT-on, the panadapter pane that's
>   showing the TX-target VFO gets the badge; the listening-only pane
>   doesn't.
> * Operator-set TX `SpectrumGridMin/Max` swaps in on MOX edge
>   (Thetis-faithful), independently of which VFO is the TX-target.
>
> When Task #44 lands (the operator is currently looking for the
> confirmation PDF that fixes its scope), this badge styling reuses
> the indicator visuals locked here.

**Three locked items for #44 directly from this passage:**

1. **TX SpectrumGridMin/Max swaps in on MOX edge — Thetis-faithful.**
   (Operator-set values, not adaptive auto-scale.  Matches the
   reference findings in the Thetis dossier exactly: fixed
   operator-tunable +20 / -80 dBFS defaults, swap in on keydown,
   restore RX on keyup.)
2. **Red TX-state badge on the panadapter pane top-right.**
   Same color/style as the dual_rx VFO-LED TX-indicator (RED).
3. **Per-side selectivity in SPLIT-on.**  When SPLIT lands (Phase
   3, NOT current v0.2.x scope), the badge belongs to whichever
   pane shows the TX-target VFO.

Other "hybrid" hits in the tree, for completeness:

* `src/qml/PlasmaBar.qml:6` — "fluid + stepped hybrid" segment
  overlay on the meter panel.  Not TX/RX-related.
* No other matches for `hybrid|TX scope|TX/RX overlay|dual-pane|
  split-pane|tri-state panadapter|red-on-air|TX panadapter|TX
  spectrum` in `src/`, `docs/`, or project memory.

---

## B.  Current Lyra panadapter behavior on MOX

**The panadapter is fully MOX-unaware.**

* `src/panadapter.cpp` — grep for `mox|MOX|moxActive|tx_active`
  returns **zero matches**.  No MOX-edge handler, no TX state
  tracking, no signal connection to `HL2Stream::moxActiveChanged`.
* `src/qml/PanadapterPanel.qml` — same: zero MOX/TX references
  in the QML side either.
* `src/panadapter.h:239-240` — the dB-range defaults:
  ```cpp
  double  dbMin_ = -130.0;
  double  dbMax_ =  -20.0;
  ```
  Single set of values for both RX and TX states.  Persisted via
  `prefs.h:29` `Q_PROPERTY(double dbMin READ dbMin WRITE setDbMin)`
  + a sibling for dbMax.
* `src/panadapter.h:241-242` — there's an `autoScale_` toggle +
  `AutoScaler autoScaler_` member already wired (for the
  operator's "auto-fit dB range to current band noise floor"
  option), but it's an RX-bound concept (the AutoScaler tracks
  spectrum noise floor over time).  Not used as a MOX-edge
  rescaler.

**What HAPPENS on MOX today** (traced from `radio`-side wiring):

* `src/main.cpp:368-376` wires the global `moxActiveChanged`
  signal — the comment notes "moxActiveChanged fires on the
  TR-settled edges only (post-mox_delay)" and the slot is a UI
  handler.
* `src/metermodel.cpp:218` — the MeterModel listens to the same
  signal + does the per-source auto-swap (RX_SMETER → PWR on
  keydown, swap back on keyup).  This is the established
  Lyra MOX-edge precedent.
* `src/hl2_stream.h:157` — the canonical state surface
  (`Q_PROPERTY(bool moxActive...)` + signal) the panadapter
  needs to subscribe to.

The panadapter, per its `metermodel.cpp:218` precedent, would
hook the same signal — but currently does not.  On the actual
MOX edge today:

* RX1 channel STOPS DSP (`_request_rx_channel(False)` per the
  §15.25 keydown discipline + `_tx_rx_muted=True` for audio
  muting per §15.26 PART B).
* No fresh RX spectrum frames arrive at the panadapter for the
  MOX duration.
* The panadapter therefore continues displaying whatever was
  in its analyzer buffer at the moment the RX channel stopped
  (effectively a frozen trace), at the operator's RX-side dB
  range (typically `-130..-20 dBm`).
* On keyup the RX channel restarts (per §15.26 PART B's
  Thetis-faithful "RX-DSP-stop-on-keydown / restart-after-
  ptt_out-settle" lock); fresh RX frames resume.
* Waterfall: same — paused (nothing scrolling because no new
  spectrum frames) but not deliberately frozen.

So today's TX-state UX is: **stuck-frozen RX panadapter** with
no visual indication that MOX is active and no scale adjustment
for any TX content (which the panadapter isn't even fed in the
first place — there's no Lyra-side `sip1` tap consumer yet).

---

## C.  Task #44 design intent + adjacent already-shipped TX visual rules

### C.1.  Task #44 scope (currently a one-liner)

The visible task description is:

> Panadapter TX-state rescale — match reference auto-rescale on
> MOX edge.

That's the entire surface.  No expanded design in
`tx1_ssb_design.md` beyond passing mentions:

* `docs/architecture/tx1_ssb_design.md:34` — historical reference:
  > old Python lyra is NOT a reference for TX-DSP / WDSP-TXA /
  > mic-input; it remains relevant ONLY for (a) TX panadapter
  > visual rule (red passband rectangle on TX-active) and (b)
  > 2RX + SUB/SPLIT UI design ideas.
* `docs/architecture/tx1_ssb_design.md:1864-1865` — the EQ panel
  doc says:
  > Integrated TX Spectrum Analyzer overlay (TX EQ ONLY — RX EQ
  > omits, the panadapter already serves that role)
  Implies the main panadapter IS expected to show TX content on
  TX (the EQ panel's spectrum analyzer is for the EQ widget
  itself, separately).
* `docs/architecture/tx1_ssb_design.md:2149, 2268` — both lines
  just repeat "#44 Panadapter TX-state rescale on MOX edge" on
  the TX backlog punch list.

The dual_rx hybrid passage (§A above) IS the locked design
spec.  No other.

### C.2.  Adjacent already-shipped TX visual rules

* **§15.9 "red-on-air" rule** — NOT shipped as a visual rule.
  Grep for `red-on-air|redOnAir|red.*passband|passband.*red`
  in `src/`:
  * `src/hl2_stream.cpp:2054` — comment in `onTxSafetyTimeout()`:
    > "UI red-on-air clears via the standard moxActiveChanged(false) edge."
  * `src/hl2_stream.h:509, 1209` — comments on the
    `moxActive` property: "Read by the UI red-on-air indicator."
  These are SCAFFOLDING for a future red-on-air implementation
  — they document the signal the UI WILL subscribe to.  No
  actual visual element (VFO LED color change, passband
  rectangle color change, etc.) currently flips on MOX.  The
  red-on-air rule is locked in CLAUDE.md §15.9 + the dual_rx
  README, but the visual implementation is unbuilt.
* **§15.6 SPLIT TX marker** — not applicable yet (SPLIT itself
  is Phase 3 / future).  The locked design (per dual_rx README
  walkthrough 2) calls for a red TX-indicator next to the
  VFO-B LED in SPLIT mode; that hasn't been built.
* **MeterModel MOX-edge auto-swap** — SHIPPED, working
  precedent at `metermodel.cpp:214-242`:
  ```cpp
  if (stream_) {
      connect(stream_, &lyra::ipc::HL2Stream::moxActiveChanged, this,
              [this](bool on) {
                  const Source target = on ? txSource_ : rxSource_;
                  if (target == source_) return;
                  source_ = target;
                  level_ = 0.0;
                  peak_  = 0.0;
                  // ... [reset per-source rendering state]
                  emit sourceChanged();
              });
  }
  ```
  Pattern: subscribe to `moxActiveChanged` in the ctor → on
  edge, swap a single "active value" between two operator-set
  preferences → reset cached state → emit change signal.  This
  is the exact pattern Task #44 wants for the panadapter dB
  range, with `(rxDbMin, rxDbMax)` ↔ `(txDbMin, txDbMax)` as
  the two preference pairs.

### C.3.  The Lyra TX-spectrum DATA path is also unbuilt

Important honest scope note: even with #44 done (y-axis swap),
**the panadapter has no TX-spectrum DATA source today**.

* `docs/architecture/tx_audio_path_reference.md:77` lists the
  TXA chain entry:
  > | 27 | `sip1` | siphon for TX panadapter (display tap) | run=1 |
* That `sip1` stage is configured in the WDSP TXA chain
  internally (WDSP's `TXA.c:586` runs the siphon every block)
  but Lyra has no consumer that reads from it and feeds it
  into the panadapter's spectrum engine.

So fully matching the reference (TX spectrum on the panadapter
during MOX) requires TWO pieces of work:

* **Phase 1 (= Task #44 as named):** y-axis rescale on MOX
  edge.  Operator-tunable TX dB range, swap on MOX edge,
  restore RX range on keyup.  Panadapter still shows the
  frozen RX trace during MOX (because RX channel is
  stopped) — but at the TX-appropriate scale, with the red
  TX badge visible.  Small commit; mirrors the MeterModel
  pattern.
* **Phase 2 (NOT in #44, future task — likely #44b or own
  task):** plumb the TXA `sip1` siphon into a TX-spectrum
  feed so the panadapter actually paints the post-TXA TX
  I/Q spectrum during MOX.  Requires: TxChannel sip1 reader,
  TX-side analyzer instance (or repurposing the RX analyzer
  with mode switching), MOX-edge source swap in the spectrum
  engine.  Bigger commit; involves the spectrum-engine pipeline
  in addition to the y-axis state.

The dual_rx README spec at line 130 — "Operator-set TX
SpectrumGridMin/Max swaps in on MOX edge (Thetis-faithful)" —
explicitly covers Phase 1 only.  The data-source swap is not
called out, so Phase 1 alone satisfies the locked design.

---

## Summary table

| Aspect | Current Lyra state | Locked design (dual_rx README §"Crossover") | Gap |
|---|---|---|---|
| Panadapter MOX awareness | None — zero MOX refs in panadapter.{h,cpp} or PanadapterPanel.qml | Subscribe to `moxActiveChanged` | **All of it** |
| TX-side dB range storage | None — single `dbMin_` / `dbMax_` for both states | Operator-set TX-side `dbMin_tx` / `dbMax_tx` | Add 2 Prefs + 2 Panadapter properties |
| MOX-edge y-axis swap | Doesn't happen | Swap in TX range on keydown, restore RX range on keyup | Add the connect() in panadapter.cpp ctor; mirror metermodel.cpp:218 |
| Default TX dB range | n/a | Reference default `+20 / -80 dBFS` | Match reference |
| TX-state badge (red, top-right) | Not present | Red badge matching VFO-LED TX-indicator color | Add a small Rectangle/Item in PanadapterPanel.qml gated on `Stream.moxActive` |
| Per-side selectivity in SPLIT | Not applicable (SPLIT itself unbuilt) | Badge belongs to TX-target VFO's pane | Defer to Phase 3 SPLIT work |
| Red-on-air visual rule (§15.9) | Scaffolding only — `moxActive` signal + comments | VFO LED + passband rectangle turn red on TX | Out of #44 scope (separate task; would benefit from same MOX subscription pattern) |
| TX-spectrum DATA on panadapter | Not fed — `sip1` configured in TXA but no consumer | Not explicitly required in #44 locked spec | Phase 2 follow-up, separate task |

---

## Recommended Task #44 implementation (Phase 1 only)

### Code changes

1. **`src/prefs.h` + `src/prefs.cpp`:**
   * Add `Q_PROPERTY(double txDbMin / txDbMax)` (defaults
     `-80.0` / `+20.0` dBFS — matches reference).
   * Persist under `panadapter/tx_db_min` and
     `panadapter/tx_db_max` QSettings keys.

2. **`src/panadapter.h` + `src/panadapter.cpp`:**
   * Pointer to `HL2Stream*` (passed at construction or via
     a setter; mirrors the MeterModel late-bound pattern).
   * Internal `(txDbMin_, txDbMax_)` mirror of the Prefs values.
   * MOX-edge handler in the ctor:
     ```cpp
     connect(stream_, &lyra::ipc::HL2Stream::moxActiveChanged, this,
             [this](bool on) {
                 if (on) {
                     // keydown: save RX range, swap in TX range
                     savedRxDbMin_ = dbMin_;
                     savedRxDbMax_ = dbMax_;
                     setDbMin(txDbMin_);
                     setDbMax(txDbMax_);
                 } else {
                     // keyup: restore RX range
                     setDbMin(savedRxDbMin_);
                     setDbMax(savedRxDbMax_);
                 }
             });
     ```
   * When `autoScale_` is on: bypass the swap (auto-fit takes
     priority — operator chose dynamic, don't fight it).

3. **`src/qml/PanadapterPanel.qml`:**
   * Small red TX-state badge (`Rectangle` with bg `#E14040`,
     2-3 px round corners, ~14 px tall, "TX" text) positioned
     top-right of the spectrum area.
   * Bound to `visible: Stream.moxActive` so it appears/
     disappears on the MOX edge.
   * Tooltip: "Transmitting" or "MOX active — TX dB range
     applied to scale."

4. **`src/settingsdialog.cpp` (Settings → Visuals):**
   * Two spin boxes "TX dB range (floor / ceiling)" with the
     Prefs binding.  Per the dual_rx spec: "operator-set"
     means operator-tunable.  Document the +20 / -80 default
     in the tooltip as Thetis-faithful and explain the swap
     posture ("These take effect during MOX; your RX range
     is restored on keyup").

### Scope check

* Matches the dual_rx README locked design at §"Crossover
  with TX-state visual treatment" exactly.
* Matches the Thetis dossier finding (operator-tunable fixed
  TX range, swap on edge, restore on keyup — same posture).
* Does NOT include the TX-spectrum data source swap (Phase 2,
  separate task, requires sip1 plumbing).
* Does NOT include the §15.9 broader red-on-air rule (VFO LED
  + passband red on TX) — those are separate visual items;
  this commit ships the badge ONLY, and naturally leaves room
  for the other red elements to land later sharing the same
  `Stream.moxActive` subscription pattern.
* SPLIT per-side selectivity correctly deferred to Phase 3
  when SPLIT itself ships.

### Bench gate

* On non-MOX (RX): panadapter renders normally with the
  operator's RX dB range, no TX badge visible.
* On MOX keydown: badge appears top-right; scale labels
  redraw with new (TX) range; trace re-positions on the
  new scale (currently shows the frozen RX trace at the new
  scale, which until Phase 2 will be empty / pinned at the
  new floor — acceptable Phase-1 cosmetic).
* On MOX keyup: badge disappears; scale labels redraw with
  RX range; RX channel restart resumes fresh trace per the
  §15.25/§15.26 keyup pattern.
* Operator can adjust TX dB range in Settings → Visuals
  while NOT in MOX, key into a dummy load, observe the new
  range; come out of MOX, see RX range restored.

---

## Provenance

* Tree audited: `Y:/Claude local/SDRProject/lyra-cpp/`
* Hybrid concept search: ripgrep across `docs/`, `src/`,
  project memory.  Only hit was `docs/refs/dual_rx/README.md`
  + non-relevant `PlasmaBar.qml` mention.
* Companion: `panadapter_tx_audit_THETIS_2026-06-02.md`
  (reference-side audit) — produced by parallel Explore agent
  earlier today.

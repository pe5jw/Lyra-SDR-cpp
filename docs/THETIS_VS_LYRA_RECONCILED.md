# Thetis vs Lyra-cpp — TX Chain Deviation Audit (RECONCILED)

**Snapshot:** 2026-06-04 morning
**Sources:** Overnight 3-lens audit (`THETIS_VS_LYRA_DEVIATIONS.md`) + fresh 3-lens audit (`THETIS_VS_LYRA_DEVIATIONS_FRESH.md`)
**Scope:** ENTIRE TX chain end-to-end. RX excluded.
**Operator directive:** "Do as Thetis does, no variation"
**Known live bug:** Lyra TX via TCI (MSHV FT8) radiates constant tone → zero PSKReporter spots

Both audits converged on most findings. This document reconciles the two —
keeping agreements at face value, calling out disagreements with the
evidence that resolves them, and surfacing new findings either audit
caught alone.

---

## ⚠ Critical disagreement RESOLVED

**Claim (fresh H-1):** PHROT is the FT8 prime suspect — "ON by default,
rotating 8-tone GFSK envelope on DIGU."

**Counter-claim (overnight TX-018):** PHROT is dormant — docstring claims
"ON at open" but `setPhrotOn()` is never called anywhere in src/.

**Evidence (Claude-verified grep):**

```
src/tx_channel.cpp:407   void TxChannel::setPhrotOn(bool on)     ← definition only
src/tx_channel.h:145     void setPhrotOn(bool on);               ← declaration only
src/tx_dsp_worker.h:147  setPhrotOn(bool on) { tx_.setPhrotOn(on); }  ← pass-through only
```

No caller anywhere. PHROT sits at WDSP create-time default = **OFF**.

**Verdict:** Overnight is correct. PHROT is **NOT** the FT8 root cause.
The fresh audit's H-1 framing is wrong on the "ON by default" premise.

**What remains valid from fresh H-1:** the underlying Mode-enum
limitation (USB/LSB only, DIGU silently mapped to USB) IS a real
deviation — same finding as overnight TX-009. Treat as MED-severity
forward-compat (the audit reframing).

---

## HIGH-severity deviations (both audits agree, both HIGH)

| ID | Title | Reference (file:line) | Lyra (file:line) | Impact | Fix |
|---|---|---|---|---|---|
| **R-H1** (TX-004 + H-2) | `kInSize=128` violates `getbuffsize(48000)=64` rule | `ChannelMaster\cmsetup.c:106-111` `getbuffsize(rate)=64*rate/48000` | `src/tx_channel.cpp:65` `kInSize=128`; `src/tx_dsp_worker.h:97` `kBlockSize=128`; `src/tci_server.h:242` `kTciTxBlockSamples=128` | §15.29 deviation that varied from reference at our rate; doubles fexchange0 frame quantum; possible FT8 symbol-boundary alignment issues | Revert all three constants to 64; centralize as one `TxChannel::kInSize` constant. Ring sized `8*kBlockSize` at `tx_dsp_worker.cpp:54` follows. |
| **R-H2** (MIC-001 + H-4) | TCI mic-source auto-flip mechanics | `Console/TCIServer.cs` handleTrx + `cmaster.SetTciSource(true)` route TCI to TXA | `src/tci_server.cpp:1287-1289` uses `wantTciAudio = useTciAudio \|\| (prefs_->micSource()=="tci")` — token gate already removed; `:1312-1313` auto-flips picker only when explicit `,tci` token sent | If operator's `prefs_->micSource()` value isn't "tci" at MSHV keydown, TCI audio bypass gate at `tx_dsp_worker.h:208` drops every sample → constant carrier symptom | Empirical first: at MSHV keydown, log `activeMicSource_` AND `prefs_->micSource()`. If mismatched, force `activeMicSource=Tci` for the keydown lifetime whenever any TCI client owns TX_AUDIO_STREAM (token-agnostic). |
| **R-H3** (FRESH-only, H-3) | EP2 case-11 (reg 0x14) C1/C2/C3 hard-zero | `networkproto1.c:1093-1098` C1=preamps+mic_trs/bias/ptt, C2=line_in_gain+puresignal_run<<6, C3=user_dig_out | `src/hl2_stream.cpp:2350-2360` only emits C4 (LNA gain / TX step-att); C1=C2=C3 default 0 | Forward-compat blocker for v0.3 PureSignal (puresignal_run on C2 bit-6). Foot-switch/electret-mic toggles silently broken today | Compose C1/C2/C3 from atomics: `mic.mic_trs + mic_bias + mic_ptt + line_in_gain + puresignal_run + user_dig_out`. Pair with case-4 cntrl1=0x08 PS routing as coherent commit. |
| **R-H4** (OVERNIGHT-only, FRESH-BOOT-STACK) | Fresh-install TX power deficit (~10-15 dB until QSettings dirties) | `Console/radio.cs:3017-3018` Leveler ON; `:2998-2999` Leveler decay 100 ms; `:2960` ALC decay 10 ms; UI defaults ALC max_gain=3.0, Leveler max_gain=15.0 | `src/tx_channel.cpp` `open()` pushes NONE of these — relies on WDSP create-defaults (Leveler run=0, max_gain=1.0, decay=500 ms) + QSettings autoload | Fresh installs / first-launches see ~10-15 dB lower TX until autoload populates. §15.27 fix addressed ALC LINEAR units but NOT the init-push. | Push reference defaults in `TxChannel::open()` init-setters: `SetTXALevelerSt(1)` + `SetTXALevelerTop(15.0)` + `SetTXALevelerDecay(100)` + `SetTXAALCMaxGain(3.0)` + `SetTXAALCDecay(10)` + `SetTXAALCSt(1)`. ~10 LOC. |
| **R-H5** (TX-009 + H-1 mode-enum part) | `TxChannel::Mode` enum lacks DIGU/DIGL/CW/AM/FM/SAM | `wdsp/TXA.c`: TXA_DIGU=10, TXA_DIGL=9 distinct enum values | `src/tx_channel.h:58` `enum class Mode { USB, LSB }` only. DIGU from TCI lands as USB; no digital-mode dispatch possible | Blocks per-mode bypass logic (PHROT/Leveler/EQ/comp for digital). Couples with v0.2.2 modulator work (#105/#106/#107). MED-severity reframe since PHROT is dormant. | Extend enum to USB/LSB/DIGU=10/DIGL=9/CWU/CWL/FM/AM/SAM/DSB. Wire `modeToWdsp()` to return real TXA_* ints. Optionally add per-mode bypass hooks for future. |

---

## MED-severity deviations (one or both audits, all sound)

| ID | Title | Reference (file:line) | Lyra (file:line) | Impact | Fix |
|---|---|---|---|---|---|
| **R-M1** (overnight TX-002+TX-018) | PHROT docstring-vs-code divergence | `wdsp/TXA.c:71-78` create_phrot(run=0); operator-only setter `Setup/setup.cs:18743-18749` | `src/tx_channel.h:30-31` docstring claims "PHROT on at open"; reality: `setPhrotOn()` never called. Docstring lies | Currently dormant by accident. If a future commit "fixes" the docstring by adding the setter call, FT8 phase coherence detonates | Rewrite docstring to match code (PHROT defaults OFF). Add Task #109 operator-tunable toggle when needed. |
| **R-M2** (overnight TX-003) | TXA dsp_rate=96k while mic/out=48k | HL2 path = 48/48/48 single-rate; rsmpin/rsmpout run=0 | `src/tx_channel.cpp:148-154` pins (48000, 96000, 48000). rsmpin.run=1 + rsmpout.run=1 — extra resampler stages | ~10-20 sample group delay + 2× TX hot-path CPU; downstream dsp_size constants doubled. Compounds with R-H1 kInSize. | Default `open()` to (48000, 48000, 48000); resamplers become no-ops. |
| **R-M3** (overnight TX-008) | Bandpass set BEFORE mode at TX channel open | `Console/radio.cs:2692/2738` SetTXAMode then SetTXABandpassFreqs | `src/tx_channel.cpp:102-132` `pushBandpassLocked` always pushes bandpass then mode | One-block stale-bp0 window on rate/mode change. Benign in steady state. | Reverse the two calls; SetTXAMode first. |
| **R-M4** (fresh M-4) | No per-mode TX bandpass memory | `Console/console.cs:8079-8118` `UpdateTXLowHighFilterForMode` applies per-mode `tx_filter_low_save[mode]/tx_filter_high_save[mode]` | `src/tx_channel.cpp:303-326` `setMode()` uses cached `opLow_/opHigh_`; no per-mode memory | Operator's SSB filter (e.g. 200-2800) carries into DIGU; FT8 tones above the SSB high-cutoff get filtered out. Real candidate for FT8 issues if operator's TX BW is set tight. | Add per-mode TX filter table in Prefs/TxChannel orchestrator; push saved (lo,hi) on setMode. |
| **R-M5** (fresh M-5) | Double-fade on MOX edges (MoxEdgeFade + WDSP tslewup/down) | WDSP TXA has `tslewup=0.010/tslewdown=0.010` channel-state slew | `src/mox_edge_fade.cpp` + `src/hl2_stream.cpp:2626` multiplies cos² per-sample at the EP2 packer **in addition to** WDSP slew | Net envelope = product of two cos² ramps. Reference has NO wire-layer envelope. Inconsistent power readings on short keyups. | Disable per-sample `fade_.advance()` during steady-state TX; rely on WDSP's slew alone. Operator decision needed (this was operator-asked-for click suppression originally). |
| **R-M6** (fresh M-6) | TX panadapter shares RX1 analyzer ID (kAnDisp=0) — reconfig race on MOX edge | Reference uses separate display ID for TXA sip1 feed | `src/wdsp_engine.cpp:442/466/470/473` reuses `kAnDisp=0`, reconfigs sample rate/detector/avg on every MOX edge | Stale-content flicker at MOX edges; blocks Task #96 RX2 which needs its own display ID. | Allocate `kAnDispTx=2`, call `SetAnalyzer` once per ID at init, flip only the producer on MOX. |
| **R-M7** (fresh M-7) | CHRONO fallback request = 480 vs reference 2400 | Reference uses client-advertised `m_txStreamAudioRequestedSamples` (typical 2400 = MSHV 50 ms block) | `src/tci_server.h:247` `kChronoFallbackRequestSamples = 480` | Fresh TCI connect before MSHV buffering hint arrives → Lyra asks for 480-sample fragments; MSHV may struggle. | Bump fallback to 2400 for closer reference parity. |
| **R-M8** (overnight ATT-on-TX-port) | §15.26 PART C `m_bATTonTX` policy port from Lyra-py not confirmed | `console.cs:30293-30327` forces step-att 31 during TX to protect RX ADC | Audit could not confirm port to lyra-cpp | Inconsistent A/B comparisons during FT8 bench; RX-ADC overload could pollute diagnosis | Audit `src/tx_channel.cpp` open()/start() + step-att composer; if missing, port the policy and add to keydown sequence. |
| **R-M9** (overnight TCI-003 / fresh M-3) | TUN handled as MOX — no separate tune-carrier path | `Console/TCIServer.cs:4343-4364` handleTune sets independent TUN state | `src/tci_server.cpp:1192-1249` TUNE branch calls `requestMoxFromTci(true)` like trx | MSHV TUNE button radiates whatever mic content is in ring (silence + noise) rather than clean carrier. Task #74 area. | Wire separate tune-carrier generator path with TUN drive % prefs. |
| **R-M10** (fresh M-2) | TUN tone is DC injection, not 1 kHz complex sinusoid | Reference TUN runs WDSP gen1 1 kHz post-bp0 in xtxa | `src/hl2_stream.cpp:2571` emits constant DC in I, Q=0 | Single LO spike on panadapter (matches operator's earlier "single bright vertical line" observation). Blocks v0.3 PureSignal calcc which expects swept tone. PA result fine. | Use `TxChannel::set_postgen` to drive WDSP gen1 at 1 kHz, or generate a 1 kHz complex sinusoid in the EP2 packer instead of DC. |
| **R-M11** (fresh M-8) | EP2 case-12 (reg 0x16) CW state bits not packed | `networkproto1.c:1247-1256` during CW + j==1, TX-I sample OVERWRITTEN with `(cwx_ptt<<3 \| dot<<2 \| dash<<1 \| cwx) & 0x0F` | `src/hl2_stream.cpp:2664-2672` no CW j==1 overwrite branch | v0.2.2 CW modulator will need structural packer change; flag now to reserve scope. | Add `cw_enable + dot/dash/cwx/cwx_ptt` atomics; reserve j==1 branch in EP2 pack loop. Wire-inert until cw_enable=true. |

---

## LOW-severity deviations

Both audits surface 25+ low-severity items — mostly cosmetic, documentation,
or forward-compat for v0.3 PureSignal / v0.4 multi-radio. See the raw
audit files (`THETIS_VS_LYRA_DEVIATIONS.md` and
`THETIS_VS_LYRA_DEVIATIONS_FRESH.md`) for the full lists. Examples:

- TX in_size constant duplicated in 3 places (fix with R-H1)
- HL2 mic path writes I=Q=mic vs reference I=mic, Q=0 (xpanel inselect=2
  masks Q today; latent if `SetTXAPanelSelect(1,3)` ever set)
- TCI modulations_list missing NFM
- EP2 cases 0/4/10 various optional bits hard-zero (forward-compat gaps,
  bare HL2+ inert)

---

## Refuted findings (audit trail)

- **Fresh H-1 PHROT-as-FT8-culprit:** REFUTED above. PHROT setter never
  invoked anywhere in src/; sits at WDSP default OFF. The Mode-enum
  half of H-1 stays valid as R-H5.
- **Overnight MIC-001 (original framing):** PARTIAL — current code at
  `tci_server.cpp:1287-1289` is more nuanced than the original framing
  suggested. Re-framed as R-H2 with empirical-verification gate.
- **Overnight WIRE-002 (dsp_size formula):** REFUTED by cross-validation.
  Reference TXA OpenChannel passes literal `dsp_size=4096`, not
  `getbuffsize(dsp_rate)`. Lyra matches reference here.
- **TX-DSP TX-004 (I=Q=mic on TCI path):** REFUTED — reference
  `cmaster.cs:5344-5351` `channels==1` case fills both I and Q with
  mono; Lyra matches. The deviation only applies to HL2 codec-mic
  path (L-3 in fresh, L-3 / TX-015 cleaned up in overnight).

---

## Methodology + caveats

Two independent 3-lens adversarial audits were run, each producing
~50 findings. This reconciled view:

- KEEPS findings both audits independently produced
- RESOLVES the one critical disagreement (PHROT) with Claude-verified
  grep evidence
- ADOPTS the fresh audit's new findings (R-H3, R-M4, R-M5, R-M6, R-M7,
  R-M10, R-M11) — none refuted by overnight
- ADOPTS the overnight audit's findings missed by fresh (R-H4
  fresh-boot stack, R-M2 dsp_rate=96k, R-M3 bandpass-before-mode,
  R-M8 ATT-on-TX port)

Caveats:
- **R-H2 (TCI mic-source routing)** needs operator-empirical
  verification BEFORE any code change. The cheapest test: at MSHV
  keydown, log `activeMicSource_` and `prefs_->micSource()`. If they
  disagree, fix is straightforward.
- **R-M8 (ATT-on-TX port)** could not be confirmed from cited sources
  in either audit — needs a fresh grep of `src/tx_channel.cpp`
  open()/start() and step-att composer.

---

## Recommended next move

**Smallest revertable diagnostic test (5 minutes, no code change):**

At MSHV-FT8 keydown, the operator confirms two things:

1. **Settings → TX → Mic Source** says **TCI** (operator already
   confirmed this earlier in the session)
2. Add a one-line debug log in `tci_server.cpp` at the trx handler
   entry: print `prefs_->micSource()` and `activeMicSource_` to
   stderr. Run one MSHV cycle. Check the log.

If the two values match ("tci" / Tci), R-H2 is clean and the FT8 root
cause is elsewhere. If they don't match, R-H2 fix is mechanical.

**Then the prime suspect ranking becomes:**

1. **R-H1 (kInSize 128→64 revert)** — the §15.29 deviation. Single
   commit, 3 constants + ring sizing, fully revertable. Bench gate:
   MSHV FT8 cycle, watch PSKReporter for spots.
2. **R-M4 (per-mode TX bandpass)** — verify operator's current TX BW.
   If 200-2700 SSB-voice, MSHV FT8 tones at 1690 Hz and 814 Hz should
   pass, but tones at >2700 Hz wouldn't. Operator-side workaround:
   widen TX BW to 200-3000 manually for DIGU.
3. **R-H4 (fresh-boot stack)** — push reference init-setters at TX
   channel open. Addresses voice power, not FT8 directly.
4. **R-H3 (EP2 case-11 bytes)** — important for HW PTT correctness +
   PureSignal v0.3 prereq, but unlikely to be FT8 root cause.

After FT8 unblocks, the rest of the MED items get tackled one at a
time per the locked methodology (smallest revertable step → operator
HL2 bench → next step).

---

*Generated 2026-06-04 morning from the reconciliation of two
independent 3-lens adversarial audits. Both raw audit outputs are
preserved at `docs/THETIS_VS_LYRA_DEVIATIONS.md` (overnight) and
`docs/THETIS_VS_LYRA_DEVIATIONS_FRESH.md` (this morning) for
archaeology.*

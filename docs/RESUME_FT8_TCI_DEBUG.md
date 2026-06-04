# Resume — FT8 / TCI TX broken (2026-06-03 EOD)

## The symptom

- Operator: working yesterday, broken today on FT8 via MSHV/TCI on 40m
- MOX engages from MSHV via TCI trx:1 ✓
- Palstar shows ~4.5 W RF ✓
- Lyra MIC bar reads −4.8 dBFS during TX ✓ (audio IS reaching TXA)
- Lyra ALC G: −4.0 dB GR during TX (some compression — see #108 note)
- **Lyra panadapter + waterfall during TX shows a single perfectly-vertical bright line — NOT the 8-tone FT8 cycling pattern**
- **Zero PSKReporter spots in 15+ min on 40m FT8 with 1069 active monitors**
- Operator decodes other stations fine (RX path healthy)
- Same MSHV/TCI setup works on Thetis-as-reference

## What's been ruled out

- ✗ Frequency mismatch (initial wrong-diagnosis by Claude — MSHV displays
  shifted freq during TX; that's MSHV's normal "show actual TX freq" behaviour)
- ✗ Leveler ON (operator confirmed OFF)
- ✗ Mic source mismatch (operator confirmed Mic Source = TCI)
- ✗ PHROT (setter exists but NO caller anywhere; sits at WDSP create-time
  default = OFF; today's commits don't touch it)
- ✗ TUN button armed (operator confirmed visually NOT lit; just MOX from
  MSHV)
- ✗ TCI internal consistency on 64→128 bump (kTciTxBlockSamples = 128
  matches kBlockSize = 128 matches kInSize = 128; TxRing scales with
  kBlockSize; accumulator reserve scales with kBlockSize)
- ✗ c4a0a80 TUN-drive orchestrator (provenance: operator-Claude
  collaboration fixing #77+#78 regressions from #74; orchestrator only
  adjusts drive level, NEVER touches `setTuneEnabled` — innocent)

## Prime suspect (the deviation from reference)

**§15.29's `in_size` 64→128 bump.** The comment at `tx_channel.cpp:181`
explicitly documents the reference's rule:

> *"the reference's getbuffsize() rule (in_size = 64 \* rate / 48000)"*

At our HL2+ AK4951 mic rate of **48 kHz**, that formula gives **64**, NOT
128. The 128 came from Claude misreading the operator's "Buffer Size =
128" screenshot (which was the reference at a higher internal rate, 64 ×
96k/48k = 128). At our 48 kHz the reference uses 64.

The §15.29 summary literally said:

> *"in_size 64→128 change (no visible effect). Made the change anyway per
> 'do as reference' directive but it didn't visibly change anything (as
> math predicted)."*

For SSB voice: invisible. For multi-tone FT8 with sub-block sample-timing
precision: most plausible vector for the "8 tones collapsed to 1 tone"
symptom.

This is exactly the kind of "varied from reference without explicit
acknowledgment" item the operator's standing directive was meant to
prevent. Owned by Claude.

## Proposed first move (operator-go-ahead-pending)

**REVERT `in_size` 128 → 64 across three files** (single line each):

1. `src/tx_channel.h` — `kInSize` 128 → 64
2. `src/tx_dsp_worker.h` — `kBlockSize` 128 → 64
3. `src/tci_server.h` — `kTciTxBlockSamples` 128 → 64

Keep ALL other §15.29 changes (Log Recursive averaging, FFT 32768,
pixout=1 waterfall separation, TX dB range) — those are display-only and
innocent.

Keep §15.27 ALC LINEAR fix — operator-confirmed working for voice.

Keep #108 TCI TX-in gain slider — operator-asked feature, unity-skip safe.

## Bench gate procedure (operator-empirical)

1. Apply the 3-file `in_size` revert
2. Rebuild (~30 sec)
3. Operator launches Lyra + MSHV, dial to 7.074 DIGU on 40m
4. MSHV fires one FT8 TX cycle (~12.6 sec)
5. Watch Lyra panadapter + waterfall:
   - If 8 distinct tones cycling visibly → in_size was the bug, ship the
     revert + update §15.29 to note the rate-aware rule
   - If still constant tone → in_size wasn't it, proceed to bisect
6. Check PSKReporter ~5 min after TX cycle for new spots:
   - If spots appear → confirmed fix
   - If still zero → revert was wrong; bisect

## If revert doesn't fix it — clean bisect plan

No more Claude-inference guesses. Manual one-at-a-time revert of today's
session work between FT8 bench attempts:

1. Revert §15.27 ALC LINEAR fix (`tx_channel.cpp` setAlcMaxGainLinear → previous form)
2. Revert #108 TCI TX-in gain (3 files: prefs.h/cpp, tci_server.h/cpp, settingsdialog.cpp)
3. If still broken → look at the actual `git log --since='2 days ago'`
   commits beyond today's session (c4a0a80, c9aeba4, 39b7c3e are RX/UI;
   anything else?)

The bisect is the operator-empirical answer if the in_size revert
doesn't land it.

## Code snapshot reference (current state)

- Branch: feature/v0.0.9.6-audio-foundation (or current — confirm with
  `git status`)
- HEAD before this session's uncommitted work: see `git log --oneline -5`
- This session shipped:
  - §15.27 Commits A+B
  - §15.28 Settings dialog reorg
  - §15.29 (panadapter rendering + the suspect in_size 64→128)
  - §15.30 (#86) waterfall TX dB range
  - #87 USER_GUIDE doc update
  - #108 TCI TX-in gain slider

## ⚠ Overnight audit running — READ THIS FIRST IN THE MORNING

Before going to bed, operator asked for a 3-lens adversarial audit
comparing Thetis vs Lyra-cpp on **TX DSP / TCI audio / TX wire path**
to find every deviation from the "do as Thetis does, no variation"
directive. Workflow launched in background:

  * **Run ID:** `wf_e00db3c1-933`
  * **3 lenses Round 1** (parallel): TX DSP chain, TCI audio paths,
    Wire path + spectrum — each independently scans Thetis source
    (`D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13\Project Files\Source\`)
    and Lyra source (`Y:\Claude local\SDRProject\lyra-cpp\src\`)
    with file:line citations
  * **3 cross-validation passes Round 2** (parallel): each lens reviews
    the other two's findings — AGREE / DISAGREE / EXTEND — and flags
    anything missed
  * **Synthesis**: reconciled deviation list compiled to a single
    operator-facing markdown document

**Expected output location:** `docs/THETIS_VS_LYRA_DEVIATIONS.md`
(Claude will save the final report there when the workflow notifies
completion).

**Process when you wake up:**

1. Read `docs/THETIS_VS_LYRA_DEVIATIONS.md` first — that's the deviation
   list with citations.
2. Use this file (`RESUME_FT8_TCI_DEBUG.md`) for the live FT8 symptom
   context — what we observed, what was ruled out, the prime
   suspect (`in_size` 64→128 reversion).
3. Decide which deviations you want fixed FIRST, in what order.
4. Ask Claude to ship them as discrete operator-approved fixes — one
   at a time, smallest revertable steps, bench between each.

The audit may or may not find the FT8 root cause directly (the
`in_size` 64→128 deviation is already on the prime-suspect list).
The audit's broader purpose: every "varied from reference without
acknowledgment" item gets caught and listed before any code changes.

## Resume command for tomorrow

> *"Read docs/THETIS_VS_LYRA_DEVIATIONS.md (the overnight audit
> output). Then read docs/RESUME_FT8_TCI_DEBUG.md for the live FT8
> bug context. I've reviewed the deviation list; let's start with
> [N] / fix all the HIGHs / etc."*

---

*Generated 2026-06-03 EOD. Operator going to bed; 3-lens audit
workflow running in background. Resume in the morning with the
audit output as the first read.*

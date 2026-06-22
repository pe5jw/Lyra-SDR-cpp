# RX2 / multi-receiver layout — design note

**Status:** DESIGN NOTE — captures the operator-discussed layout lean
(2026-06-21) so RX2 (#96–#101) is built deliberately, not defaulted. NO code.
RX2 is still pending; this records *how* to lay it out + the abstraction to
build in from the first commit so ANAN/Brick/Protocol-2 don't force a rewrite.

**Why this note:** RX2's task entries (#96–#101) lock the *control* model
(SUB, focus, per-RX vol, SWAP, SPLIT) but never decided the **panadapter
geometry**. That decision interacts with CTUNE (#174) and with the future
ANAN/Brick/P2 multi-radio arc, so it's worth getting right up front.

---

## 1. Operator usage philosophy (N8SDR, 2026-06-21)

- "I hardly ever use RX2." On HL2 the realistic use is a **monitoring panel** —
  park a watch on a second spot, glance/listen ("what's going on here"), and if
  it's worth it, **bring it to the main (RX1/TX1)** and operate there.
- That workflow is closed by a **SWAP / A↔B** button (already #99), not by a
  heavyweight second-receiver console.
- Usage genuinely varies by operator; this is the 80/20, not the only case.
  (Other real uses: split-pileup listening — really the SPLIT-TX case #99; and
  stereo/diversity-ish listening — one signal per ear.)

---

## 2. The HL2 hardware reality (shapes everything)

HL2 has **one ADC** (AD9866, direct-sampling all of HF). Nuance that matters:

- The two DDCs each have their **own NCO** and *can* tune independently across
  the sampled range — so RX2 is **not** strictly forced onto RX1's band by the
  protocol.
- What's shared is the **front end**: one LNA gain, one attenuator, and whatever
  band-pass / preselector filter is engaged. So an RX2 parked on a *different*
  band gets clobbered by RX1's engaged filter / can't have its own gain. **In
  practice RX2 lives in the same swath you're already looking at** — a
  shared-front-end limit, not a protocol limit.
- Consequence: on HL2, RX2 is a **second marker on the same spectrum**, not a
  reason for a second spectrum.

---

## 3. HL2 layout — SIMPLE (build this for RX2 now)

Single panadapter / waterfall, **two markers** (RX1 + RX2), plus:
- **SUB** toggle to bring RX2 up (stereo-split or mono-mix audio, #97);
- **per-RX volume / mute** so RX2 can ride quiet in the background (#98);
- **A→B / B→A / SWAP** — the "ooh, what's that → make it the main" closer (#99);
- focus model: second VFO LED + focus border + Ctrl+1/Ctrl+2 + middle-click
  focus swap (#98).

No second panadapter pane on HL2. This is the lightest thing that fully serves
the §1 monitoring + swap workflow, and it matches what the sibling Python-Lyra
project actually shipped (v0.1.0) and its later design preference (single pane +
markers over a split pane).

---

## 4. What changes with ANAN / Brick / Protocol 2

The §2/§3 "RX2 is in-band → one spectrum + markers" assumption **breaks** on
dual-ADC P2 radios, and "RX2" stops being the right concept:

1. **Dual ADC → true cross-band, independent front ends.** RX2 can be on 20m
   while RX1 is on 40m, both at full sensitivity → a single shared panadapter
   **cannot** show both → cross-band needs **per-receiver spectra**.
2. **N receivers, not "RX2."** P2 advertises DDC count at discovery (G2 ≈ 4,
   7000DLE ≈ 7). The model must generalize from "RX1 + RX2" to **a list of
   receivers**.
3. **Diversity** — phase-coherent two-antenna combine — is an ANAN feature with
   no HL2 equivalent (a *third* way two receivers relate: not stereo, not split,
   summed).
4. **Protocol 2 is a separate wire layer** (different framing, discovery,
   command structure, **per-DDC sample rates**). A `p2` / `p2_anan` module
   alongside the P1/HL2 wire path — the big lift, mostly *below* the UI, same
   clean-room-from-reference discipline as the TX rebuild.
5. **No onboard codec** → audio to the host (PC soundcard), a different sink
   path than the HL2 AK4951 jack.

---

## 5. The abstraction to build in NOW (so ANAN later ≠ rewrite)

Build HL2 RX2 as the §3 simple version, **but** structure it so the two
assumptions ANAN breaks are not hard-coded:

- **A `Receiver` abstraction** — per-receiver `freq / mode / filter /
  panadapter-source / audio-route`, NOT `rx1Foo` / `rx2Foo` twins. HL2
  instantiates 2; ANAN instantiates N from discovery.
- **A capabilities object the UI reads** — `nRx`, `independentBand`,
  `diversityCapable`, `audioPath`. The panadapter layer branches on
  `independentBand`:
  - `false` (HL2) → single pane + N markers;
  - `true` (ANAN/Brick) → per-receiver panes (stacked / tabbed).

What carries over **unchanged** (receiver-agnostic): the focus model, per-RX
vol/mute, A↔B / SWAP, SPLIT, and CTUNE. Only the **panadapter geometry** and
the **wire/audio plumbing** fork by capability.

Then adding ANAN = (a) the P2 wire module, (b) the capability struct flips, (c)
the panadapter gains the multi-pane render path. The control UI just works.

**Don't build** the multi-pane / P2 path now (no hardware, big arc). **Do
avoid** hard-coding "exactly 2 RX" and "RX2 == in-band marker on RX1's pano."

---

## 6. CTUNE (#174) interaction — reinforcing, not colliding

CTUNE is a **per-receiver** property: lock *that* receiver's DDC center, tune
the offset within *its* captured span. So:
- On HL2's single pane, RX1 + RX2 are just offset markers within the locked
  span — same mental model as CTUNE (locked center + offsets).
- On a dual-ADC cross-band rig, CTUNE works **per-pane** identically.

⇒ Building CTUNE **per-receiver, before RX2** means RX2 inherits the
center/offset machinery and CTUNE generalizes to ANAN with zero rework. The two
features reinforce each other. (This corrects the earlier overstated caveat that
RX2 "reworks the panadapter into split panes" — that geometry is an *open
decision*; under the §3 single-pane lean the CTUNE QML rework at RX2 is nearly
nil.)

---

## 7. Honest unknowns

- **Brick is TBD** — which Brick, and whether it speaks HPSDR P1 (→ P1/HL2
  path), P2 (→ ANAN path), or vendor-specific (→ a third module). Can't design
  precisely until pinned.
- **P2 + ANAN is a months-scale arc** (wire module + discovery + per-DDC rates +
  host-audio path + multi-pane UI) needing real hardware to bench. Nothing here
  shrinks that; the point is only to keep HL2 RX2 from painting it into a corner.

---

## 8. Decision summary

- **HL2 RX2 = single panadapter + two markers + SUB + per-RX vol + SWAP**
  (the §3 simple layout). Ship the 80/20.
- **Sit it on a `Receiver` + capabilities abstraction** (§5) so ANAN/Brick/P2
  is "add a wire module + flip the capability + add the multi-pane render," not
  a rewrite.
- **Capability-gate the second panadapter pane** — HL2 never shows it; ANAN/Brick
  light it up when `independentBand` is true.
- **Build CTUNE per-receiver, before RX2** (#174) so both features share the
  center/offset machinery.

*Operator-discussed 2026-06-21; geometry decision still the operator's call when
RX2 (#96–#101) actually starts — this records the reasoning while fresh.*

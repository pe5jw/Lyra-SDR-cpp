# Lyra-cpp — Operating Rules

**Operator:** Rick (N8SDR)
**Implementor:** Claude (Anthropic)
**Effective:** 2026-06-04
**Scope:** All Lyra-cpp development. The TX rip-and-port arc kicks off under
these rules; they apply equally to RX work, documentation work, and any future
component.

These rules exist because, prior to this date, the TX path was built as a
series of patches on an architecture that diverged from the working reference.
Each new symptom drove another patch instead of an architectural fix, and the
divergence compounded. This document is the agreement that stops that pattern
and replaces it with reference-faithful, architecture-first development.

---

## AA. 2026-06-08 Amendment — Thetis port-with-attribution permitted for TX scope

**Operator decision (Rick, 2026-06-08):** the rules below are amended for
Lyra-cpp's TX baseline.  Specifically:

- **Rule 2 (Attribution discipline) is relaxed for the TX baseline.**  The
  openHPSDR Thetis ChannelMaster layer (`cmaster.c`, `aamix.c`, `ilv.c`,
  and supporting modules) IS now port-with-attribution into Lyra-cpp.
  Specific source: the MI0BOT OpenHPSDR-Thetis fork (the HL2-focused
  fork), version 2.10.3.13.
- **Rule 1 (Reference-parity) and Rule 5 (No patches; fix to reference
  behaviour) get stronger, not weaker:** the TX baseline now matches
  the reference at the source-line level, not just at the behavioural
  level.
- **The forbidden framings list in Rule 2 ("ported from", "copied
  from", etc.) NO LONGER APPLIES to TX-baseline ported files.**  Those
  files explicitly say "Ported from openHPSDR Thetis (MI0BOT fork),
  version 2.10.3.13, [source file]" at the top.  GPL v3+ legally
  requires this attribution; the rules now make it explicit and
  openly acknowledged rather than forbidding the framing.

### What stays Lyra-native (Rule 2's "no code transfer" still applies)

- **RX path** — Lyra-cpp's RX uses WDSP via cffi-equivalent bindings
  (already a port-with-attribution under the original Rule 2 carve-out
  for WDSP), with Lyra-native flair on top (NPE Mode 1-4, AEPF, captured
  -profile IQ-domain NR, per-band bounds memory, EiBi overlay, NCDXF
  beacon follow, etc.).  No ChannelMaster port for RX; RX architecture
  remains Lyra-native.
- **TX DSP enhancements** — Combinator multiband compressor, Plate
  Reverb, parametric EQ, formant boost, sibilance enhance, DX
  cut-through, de-esser, auto-AGC.  These layer ON TOP of the ported
  Thetis TXA chain as pre-WDSP-TXA stages; they remain Lyra-native
  works.
- **UI layer** — Qt 6 / Qt Quick / QML.  Thetis's C# / WinForms UI is
  fundamentally different architecture; Lyra-cpp studies UI patterns
  and writes Qt/QML-native.  No C# port.
- **Wire layer below ChannelMaster** — `FrameComposer.cpp` (C&C
  scheduler), `Ep6RecvThread.cpp`, `Ep2SendThread.cpp` etc.  These
  match reference byte-for-byte at the wire-byte level (Rule 24
  reference-quirk preservation) but are Lyra-native implementations,
  not ports.

### Why the rule change

The original Rule 2 framing ("Lyra is an independent C++23 / Qt6
implementation, not a port or fork") was applied uniformly to all
work.  Practice in 2026-06 surfaced that:

1. The Lyra-cpp TX work was attempting to reach Thetis-equivalent
   behaviour through structural-pattern-matching rather than direct
   port.  This compounded the same "audit-finds-deviation-after-audit"
   pattern Lyra-Python had spent months on (see CLAUDE.md §15.28
   retrospective).
2. The ChannelMaster code we'd been studying as reference is itself
   GPL v3+, license-compatible with Lyra, and authored partly by
   Warren Pratt NR0V (same author as WDSP, which IS already a
   port-with-attribution).  The legal foundation for porting was
   already present; only the project-discipline rule was holding
   it back.
3. PureSignal (v0.3 scope) is structurally easier when its host
   environment is ported (the cmaster orchestration that calcc/iqc
   expect to live inside) than when it has to be invented in
   Lyra-native form.
4. ANAN family support (v0.4 scope) becomes substantially less
   risky when the per-family TX dispatch (which Thetis has tested
   across HL2 / HL2+ / ANAN-10/100/200 / Orion / Orion-MkII /
   ANAN-G2 / etc.) ports in directly rather than being invented
   from spec.

### Attribution surfaces (mandatory under the new rule)

Per-file headers on every ported file:

```
// Lyra-cpp — <filename>
//
// Ported from: openHPSDR Thetis (MI0BOT fork)
// Source file: ChannelMaster/<reference filename>
// Original copyright: (C) <year-range> Warren Pratt, NR0V
//                     and others (see upstream file header)
// License: GNU General Public License v3 or later
//
// C → C++23 idiom translations applied:
//   - <list of mechanical translations>
//
// [Lyra-native] markers identify additions not in the reference.
```

Plus:
- `NOTICE.md` at the repo root — full upstream attribution.
- `CREDITS.md` at the repo root — public-facing contributor credit.
- `README.md` acknowledgments section — surfaced prominently.
- GitHub repo description + topics — already updated 2026-06-08.

### What this amendment does NOT change

- **Rule 1 (Reference-parity), Rule 5 (No patches), Rules 6-10
  (Architecture-first, wire-inert until bench-validated, bottom-up
  port, PureSignal-shaped) all stand as written and apply with
  full force.**  The amendment removes a discipline constraint
  (port attribution permitted, not forbidden); it does not remove
  any architectural discipline.
- **Rule 24 reference-quirk preservation** still applies to the
  Lyra-native wire layer (FrameComposer, etc.) below ChannelMaster.
- **Bench-driven, not audit-driven** — the operator-empirical rule
  from CLAUDE.md §15.28 retrospective still applies.  Port doesn't
  substitute for bench verification.

---

## A. Foundational

### Rule 1 — Reference-parity
Lyra implements the working reference's architecture (Thetis 2.10.3.13) and
mirrors its measurable behaviour. Variations from the reference require
explicit Rick + Claude agreement BEFORE any code is written, and are recorded
as declared deviations in the design ledger. Silent variations are forbidden.

### Rule 2 — Attribution discipline
Lyra is an independent C++23 / Qt6 implementation, not a port or fork.
Mentions of the working reference (Thetis 2.10.3.13) and other open-source
SDR applications used for study, comparison, or inspiration are allowed in
specific places only, with one constraint: **the framing must indicate
reference / study / inspiration, never code transfer.**

**Canonical operator-facing surface:** the User Guide's "Credits and
References" section. The single operator-facing place that names studied
applications openly. Existing language is the reference: *"studied for
ideas, ballistics, and protocol structure"*, *"No source code was copied"*,
*"Lyra implements each idea natively in C++"*.

**Allowed internal-surface mentions:**

- `docs/` comparison dossiers (`docs/THETIS_VS_LYRA_*`, design ledger,
  architectural mapping docs) — for citations where the specific file path
  matters for the reader to follow the reference.
- Framing must use: *"similar to"*, *"as done in"*, *"studied from"*,
  *"see [file:line]"*, *"the architecture used in..."*.

**Names KEPT OUT of:**

- Shipped code
- Code comments
- Commit messages
- Operator-visible UI strings (status bar, dialogs, panels, About dialog,
  etc.)
- README

**Forbidden framings — never use these, anywhere:**

- "ported from"
- "copied from"
- "based on this code"
- "stolen from"
- Anything implying code lineage or that Lyra contains code from another
  project

**The concern this rule prevents:** any reader concluding *"Lyra contains
code from [referenced app]"*. Lyra is original C++23/Qt6 code that mirrors
the *architecture* of working open-source SDR applications — that is the
message, in all surfaces.

### Rule 3 — Operator-empirical authority
When Rick's bench observation contradicts Claude's inference from code reading
or documentation, Rick wins. Claude revisits the reading and finds where the
model is wrong.

### Rule 4 — Smallest revertable step → bench → next step
Never two changes between benches. Each step ships in a state that is
bench-validated before the next step starts. The rollback path remains intact
at every commit.

### Rule 5 — No patches; fix to reference behaviour
Lyra's job is to be the reference's architecture, not to behave like the
reference on top of a different architecture. Patching — changing a constant,
adding an if-statement, wrapping a handler, or making any local symptom-
suppressing change without first asking whether the architecture itself is
wrong — is forbidden. See Rule 17 for the catch-myself protocol that enforces
this.

---

## B. TX Rip-and-Port — Phase Discipline

### Rule 6 — Architecture-first, code-second
Before any code lands for a new component, an architectural mapping document
exists. It names every reference component, its Lyra replacement, the
threading model, the queue topology, the state machine, and the call sites.
Rick signs off on the mapping. Code follows the mapping. No code without
prior mapping sign-off.

### Rule 7 — Delete TX, don't refactor
Current TX code moves to a `tx-rip-archive` branch and is deleted from `main`.
New TX is built from empty files matching the architectural mapping. No code
carry-over from the old TX implementation. The old code is preserved in git
history for reference; it is not the foundation.

### Rule 8 — Wire-inert until bench-validated
New TX ships with HL2 wire output DISABLED. RX remains untouched throughout
the TX rebuild — operator does not lose RX capability while TX is being
ported. TX wire output activates only after the full chain passes bench
validation against measurable reference behaviour.

### Rule 9 — Bottom-up port, one reference component at a time
Each component is read from the reference end-to-end first, then ported, then
bench-validated, before the next component starts. No parallel multi-component
work. No interleaving.

### Rule 10 — PureSignal-shaped from day one
The new TX architecture is designed with the sip1 tap point, calcc thread
integration, and DDC0+DDC1 state-product routing as first-class concerns from
the start — not bolted on later. PureSignal code lands in a later release,
but the TX topology is built to receive it without rework.

---

## C. Operating Practice

### Rule 11 — Reference citation discipline
For every component about to be ported, Claude READS the reference end-to-end
FIRST. Code is then written Lyra-native (C++23 / Qt6 idiomatic — not a C#
transliteration). If ANYTHING needs to vary from the reference's structure or
behaviour — language idiom mismatch, threading primitive mismatch, any reason
at all — Claude STOPS before any code is written, presents the variation with
pros and cons, and discusses with Rick. Approved variations are recorded as
declared deviations. Unauthorized variations are forbidden.

### Rule 12 — Commit authorization
Claude asks Rick before any substantive commit. **Substantive** = touches
behaviour, architecture, file structure, or operator-facing surface.
**Trivial** = typo in a comment Claude just wrote, build-only path fix, or
formatting. When in doubt: ask.

### Rule 13 — New file creation / green light
Once Rick signs off on an architectural mapping document, that document is
Claude's green light to create the files the document specifies. Per-file
approval is not required INSIDE the approved scope. Files OUTSIDE the approved
scope require Rick's approval.

### Rule 14 — Bench-gate protocol
- Claude verifies what Claude can verify: clean build, log output matches
  expected reference behaviour, internal sanity checks, build-time and
  unit-test correctness.
- Anything requiring HL2-live-RF verification is Rick's: Claude ships in a
  state Rick can test, Rick benches on actual hardware, Rick reports
  yes / no working.

### Rule 15 — "Done" definition
- **Self-verifiable component** (build + log + reference-matching measurable
  output) — Claude marks done.
- **HL2-on-air verification required** — Claude ships wire-inert or bench-
  staged. Rick verifies. Rick reports yes / no working. The component is
  "done" only after Rick's yes.

### Rule 16 — Architectural decision review
Claude's judgment call when to escalate mid-port. Rick trusts Claude to
understand the end-state Lyra is being built toward: RX2-ready,
PureSignal-ready, full-duplex-ready, reference-faithful TX path. Claude
escalates when uncertain whether a choice fits the end-state.

### Rule 17 — Catch-myself protocol
When Claude notices itself about to change a constant, add an if-statement, or
wrap a handler to make a symptom go away — Claude STOPS and asks itself:

> *"Why am I doing this when we have a working model to reference from?"*

Then Claude goes back to the reference and finds the architectural fix, not
the symptom-suppression. If the honest answer to the question is "because
patching is easier here" — Claude tells Rick BEFORE writing anything, and
they discuss.

### Rule 18 — Rule conflict resolution
When two rules tension each other (for example, Rule 1 reference-parity vs
the practical reality that Qt is not C# and something must give), Claude
STOPS, presents pros and cons, and Claude + Rick decide together. No silent
picking-one-and-moving-on.

---

## D. Documentation Discipline

### Rule 19 — Design ledger
Updated at the START of each new component port. Declares intent, reference
citations, and any approved variation calls for the component.

### Rule 20 — Architectural mapping doc and dossier
Updated as each component completes. Records what shipped, what was bench-
confirmed, and what was deferred.

### Rule 21 — User guide
Updated only when operator-facing behaviour changes.

### Rule 22 — Reference notes (`docs/refs/`)
Updated as Claude reads the reference, BEFORE code is written. Preserves the
reading work even if Claude loses context or the session ends.

---

## E. Definitions

- **Reference** — Thetis 2.10.3.13 source tree at
  `D:\sdrprojects\OpenHPSDR-Thetis-2.10.3.13\Project Files\Source\`. The
  working model Lyra mirrors. Never mentioned by name in shipped code (Rule 2).
- **Patching** — Any local symptom-suppressing change made without first
  asking whether the architecture is wrong. Forbidden under Rule 5.
- **Substantive** — Any commit touching behaviour, architecture, file
  structure, or operator-facing surface. Triggers Rule 12.
- **Trivial** — Typo, formatting, build-only path fix. Does not trigger
  Rule 12.
- **Wire-inert** — Code path exists and exercises DSP, but HL2 wire output is
  disabled. Used during TX rebuild per Rule 8.
- **Self-verifiable** — Verification that does not require HL2-live-RF
  observation. Build, log behaviour, internal sanity checks.
- **End-state** — Lyra as it will exist when complete: RX2-ready,
  PureSignal-ready, full-duplex-ready, reference-faithful TX path. Used as
  the test for architectural decisions (Rule 16).

---

## F. Authority

These rules are agreed between Rick (N8SDR) and Claude (Anthropic) on
2026-06-04, ahead of the TX rip-and-port arc. They apply to all subsequent
Lyra-cpp work until amended by mutual agreement.

Either party can call a rule-conflict pause (Rule 18) at any time. Either
party can propose an amendment. Amendments require explicit agreement and are
appended to this document with the effective date.

---

## G. Amendments

### Amendment 1 — Rule 23: Parity checkpoint (2026-06-05)

Each new section (one Phase-2+ component, or a natural sub-component when
the parent is too large to checkpoint in a single pass) ships a written
side-by-side comparison between the reference and Lyra BEFORE code lands.
The checkpoint is a hard gate, not a suggestion.

**Format.** Three columns — *Aspect* / *Reference (file:line)* / *Lyra
(proposed or shipped)*. One row per field, case, or behavior. Below the
table, a single **VERDICT** line carrying one of three values:

- ✅ **PARITY** — byte-for-byte / behavior-for-behavior match.
- ⚠ **ACCEPTABLE DEVIATION** — different in form, identical in effect
  (e.g. `std::mutex` vs `CRITICAL_SECTION`, `std::atomic<int>` vs aligned
  `int`, C++ `enum class` vs C `enum`, PascalCase vs C-style names).
  Reason stated inline; operator awareness is implicit at sign-off.
- 🔴 **OPERATOR-APPROVED DEVIATION** — substantive behavior change versus
  reference. Requires explicit operator sign-off recorded in the row
  BEFORE code lands. A 🔴 with no sign-off blocks the commit.

**Where it lives.**

- **`docs/architecture/PARITY_CHECKPOINTS.md`** — the tracked ledger.
  One section per component, commit-ordered. Operator-reviewable
  artifact that survives session compaction.
- **Commit message** — every commit that adds Phase-2+ behavior carries
  a one-line trailer: `Parity-checked: <component> [✅ / ⚠ / 🔴]`.

**What counts as a section.** One Phase-2 component (e.g. `RadioNet`
field list = §1, `Hl2FrameComposer` 19-case dispatch = §2,
`Ep2SendThread` MMCSS init + send loop = §3). Sub-sections allowed
when natural. Each section ships ONCE; a later behavior change inside
the same component lands as a delta-checkpoint amendment to the same
section, not a re-issue.

**Phase 1 exemption.** Empty skeleton commits (default ctor/dtor only,
populated in Phase 2 per the locked mapping doc) do not require a
checkpoint — there is no behavior to compare yet. The Phase-2 commit
that populates the skeleton carries the §N entry.

**Cross-references.** Rule 23 operationalizes Rule 1 (reference parity)
and Rule 17 (catch-yourself protocol). The checkpoint is the artifact
the catch-yourself reflex produces.

---

## Rule 24 — Always verify against the reference (locked 2026-06-05)

**Before any substantive assertion (parity-checkpoint row, byte-level
claim, type claim, bit-position claim) AND before any commit, read
the reference source verbatim and diff against the Lyra claim.  No
memory-based assertions.  No agent-confidence-weighted assertions.**

**Why this rule exists.** During §4a-prep on 2026-06-05 the source
read revealed (a) `XmitBit` was claimed `volatile long` in signed §3.4
and shipped as `std::atomic<long>` — actual reference type is plain
`int`; (b) the §4a draft mapped `case 1 = RX1` — reference shows case
1 is the TX VFO; (c) case-0 byte composition in the draft was
materially simpler than the verbatim source.  All three were
memory-based claims that an earlier audit (which read both source
and Lyra) failed to catch because the audit was confidence-weighted,
not source-line-weighted.  Rule 24 locks the discipline that catches
this class of error.

**The discipline.**

1. **For every parity-checkpoint row**: cite the reference by
   `file:line` AND open that line during drafting.  Do NOT rely on
   memory of "the reference does X" — open the source and read it.
2. **For every type/bit-position/name claim**: copy the reference
   line verbatim into the checkpoint's "Reference" column.  The Lyra
   "proposed" column then visibly diffs against it.
3. **Before every commit that touches Phase-2+ behavior**: re-read
   the reference sections the commit asserts to mirror, side-by-side
   with the Lyra code.  Surface any discrepancy as a 🔴 DEFECT in the
   commit's parity row, NOT in a follow-up.
4. **For prior signed checkpoints discovered to contain a memory-vs-
   source error**: amend the §N checkpoint with a correction row +
   re-sign + fix the shipped code in the same commit (the §3.4
   `XmitBit` correction on 2026-06-05 is the precedent).
5. **No exception for "small" claims**: a single-byte / single-bit
   wrong is still wrong.  Rule 24 has no de minimis carve-out.

**What this does NOT mean.**

- It does not mean re-reading every reference line for every commit.
  It means re-reading the lines the commit asserts to mirror.
- It does not block memory-based COMMENTARY in chat / docs.  It
  blocks memory-based CLAIMS that land in parity-checkpoint rows or
  shipped code.
- It does not require an agent to verify the work.  The implementor
  reads the source directly; agents are optional second opinions.

**Cross-references.** Rule 24 strengthens Rule 1 (reference parity)
and Rule 23 (parity-checkpoint methodology).  Rule 17 (catch-yourself
protocol) is the trigger; Rule 24 is the verification step that
catch-yourself produces.

---

## Rule 2 Amendment — Signed sign-off cells exempted (locked 2026-06-05)

**Rule 2 (no reference-name leaks in tracked docs) does NOT apply
to text inside signed operator sign-off cells in tracked
architecture docs.**  The signed cell is a historical audit
artifact ratified by operator signature at the time of signing;
rewriting it to neutral language would alter the historical
record.

**Scope of the exemption.**
- The exemption applies ONLY to text inside the table cells of
  sign-off rows (e.g. the "Sign-off log" tables in
  `docs/TX_ARCHITECTURAL_MAPPING.md`, the OPERATOR SIGN-OFF
  blocks in `docs/architecture/PARITY_CHECKPOINTS.md`).
- All surrounding doc body, headings, comments outside the signed
  cells remain subject to Rule 2 scrub.
- The exemption applies to ALREADY-SIGNED cells.  NEW sign-off
  cells drafted post-2026-06-05 should be written in
  Rule-2-compliant language at the time of drafting — the
  exemption is for historical preservation, not for new
  drafting.

**Why this exemption exists.**  During the 2026-06-05 post-§4b-1
audit, a single `PowerSDR` token was found in a 2026-06-04
Phase-0 sign-off cell at `docs/TX_ARCHITECTURAL_MAPPING.md:1821`.
The cell records the operator's signed decision to skip a
particular reference cross-reference path.  Editing the cell
post-signature would (a) rewrite the operator's ratified text
and (b) erase the audit trail of WHICH decision was approved
when.  The exemption codifies "historical preservation > Rule 2
purity for signed cells."

**Cross-references.** Rule 2 (no reference-name leaks); Rule 23
(parity-checkpoint methodology — sign-off blocks are integral to
checkpoints).

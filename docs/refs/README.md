# Reference reading notes (`docs/refs/`)

Per Rule 22: notes are captured as Claude reads the reference, BEFORE
code is written. Preserves the reading work even if Claude loses
context or the session ends.

## What's in here

One file per reference component read. Each file:

- Names the reference source (file:line ranges)
- Records verbatim observations + structure (threading, queues, state
  machines, control flow)
- Cites the **specific file:line** the observation came from — every
  claim must be traceable back to a line of reference source
- Stays raw / unprocessed — no Lyra-mapping synthesis (that lives in
  `docs/TX_ARCHITECTURAL_MAPPING.md` §10)

## What's NOT in here

- Lyra implementation plans (those live in the mapping doc)
- Code (Phase 0 produces no code by definition — Rule 6)
- Operator-facing prose (these notes are for me / future-Claude /
  reviewing agents — `docs/help/USER_GUIDE.md` is operator-facing)

## Framing discipline (Rule 2)

These notes ARE allowed to name the reference (Thetis 2.10.3.13)
explicitly — `docs/` is one of the canonical surfaces per Rule 2. But
the framing rule still holds:

- Allowed: *"the reference's TX service thread is...", "as done in
  cmaster.c:line", "studied from TCIServer.cs"*
- Forbidden: *"ported from", "copied from", "based on this code"*

## Index

(Populated as files land here.)

*Started 2026-06-04 alongside Phase 0 of the TX rip-and-port arc.*

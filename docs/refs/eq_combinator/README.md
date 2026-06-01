# EQ + Combinator reference images (locked 2026-06-01)

Operator-curated visual references for Tasks #50 (TX 8-band parametric EQ),
#59 (RX 8-band parametric EQ — mirror of #50), and #51 (5-band Combinator).

These are reproductions kept here for stable in-tree provenance — the
authoritative locked spec lives in
`docs/architecture/tx1_ssb_design.md` §11.

## EQ layout reference (Tasks #50 + #59)

* **`sparkle-boost-eq-2048x811.jpg`** — operator-selected layout for the
  Lyra TX (and matching RX) Parametric EQ.  8 bands left-to-right with
  fixed role assignment (HPF / Low Shelf / 4× Bell / High Shelf / LPF),
  color-coded, drag-the-marker interactive curve, per-band Hz/dB/Q
  tile readouts.  Matches the EESDR3 manual TX Equalizer
  (`D:/sdrprojects/ExpertSDR3_User_Manual_ENG_DX.pdf`, p.89-90) verbatim.

## Combinator layout reference (Task #51)

The six Reaper FX3 "Dual Combinator" screenshots
(`Screenshot 2026-06-01 0603[06/30/46]_…04[07/19/56].jpg`) show the same
panel with one of the band-radio-buttons selected per shot (HIGH /
HI-MID / MID / LO-MID / LOW + bypass) — useful for understanding the
band-selector UX.  Locked Lyra spec uses the same 5-band L→R role
order with color-matched per-band meters at the bottom.

## Other panels in the same folder (NOT EQ — informational only)

* `Screenshot 2026-06-01 060607.jpg` — Behringer X-Air mixer EQ
  (4-band PEQ).  Operator considered this earlier; superseded by the
  EESDR3 8-band layout for the EQ work.  Retained for the channel-strip
  surround context.
* `Screenshot 2026-06-01 060732.jpg` — Behringer X-Air Compressor
  panel.  Not the locked Lyra compressor reference (Lyra TX compressor
  comes from WDSP `compress.c`), retained for future side-chain UX
  ideas (the Side Chain Filter + KeySrc/Self idiom may inform Lyra's
  later TX side-chain controls).

## Provenance note

Per the project's no-attribution rule: these reference names do NOT
appear in shipped code, code comments, commits, or operator-visible
UI strings.  Provenance lives only in this README and in
`tx1_ssb_design.md` §11.

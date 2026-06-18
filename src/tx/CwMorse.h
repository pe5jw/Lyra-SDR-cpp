// Lyra — CW morse engine (#105 CW-3a, host software keyer).
//
// Pure, hardware-free text → keying-element translation. The keyer
// thread (CwKeyer) consumes the element list and drives the wire
// CW key bits; this module owns only the morse table + PARIS timing
// + operator weighting. No Qt, no threads, no I/O — unit-testable in
// isolation.
//
// Timing (PARIS standard): one element unit = dit = 1200 / WPM ms;
// dah = 3·dit; intra-element gap = 1·dit; inter-character gap =
// 3·dit; inter-word gap = 7·dit. The reference CWX engine
// (`cwx.cs:407-410,469-525`) uses the same PARIS basis but does NOT
// apply weight; Lyra applies the operator `cwKeyerWeight` as a
// deliberate one-up (locked decision D, cw3_software_keyer_design.md
// §4). Weight scales the mark and the 1-dit intra-element gap around
// the 50 % neutral point (markScale = weight/50, spaceScale =
// (100-weight)/50); inter-character (3·dit) and inter-word (7·dit)
// gaps stay nominal so character/word rhythm is preserved.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lyra::tx {

// One keying element: hold the CW carrier on (mark) or off (space)
// for `durationMs` milliseconds. The keyer thread translates a `mark`
// to cwx=1 and a `space` to cwx=0 (cwx_ptt held across the whole
// list).
struct CwElement {
    bool key;          // true = mark (carrier on), false = space (off)
    int  durationMs;
};

// Translate ASCII `text` to a flat keying-element list at `wpm`
// (5..100) and `weightPct` (10..90, 50 = neutral). Letters are
// case-folded to upper; characters with no morse mapping are skipped
// (no spurious keying). Runs of spaces collapse to a single inter-
// word gap. The list begins with a mark and never has a trailing gap
// (the keyer owns PTT lead/tail timing, not this list).
std::vector<CwElement> cwTextToElements(const std::string& text,
                                        int wpm,
                                        int weightPct = 50);

// The dit unit in ms for a given WPM (PARIS): 1200 / wpm, clamped to
// a sane 5..100 WPM range. Exposed for the keyer's spacing-hang math
// and for tests.
int cwDitMs(int wpm) noexcept;

}  // namespace lyra::tx

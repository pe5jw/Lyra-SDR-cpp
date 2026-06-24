// Lyra — factory-default dock layout (operator N8SDR's curated arrangement).
//
// Captured via QMainWindow::saveState() (dock layout) + the panadapter/
// waterfall SplitView state, base64-embedded so BOTH a fresh install AND
// "Reset to default layout" come up exactly this way — no reliance on
// whatever happens to be in QSettings.
//
// To regenerate after the panel set changes: arrange the docks the way the
// build should ship (chip-summoned TX/RX/CW rack panels CLOSED), save it to
// a named layout slot via View -> Layouts -> "Save to slot N", then de-wrap
// the registry values and re-embed.  QSettings stores a QByteArray as
// REG_BINARY = UTF-16LE of the string "@ByteArray(" + Latin1(rawBytes) + ")".
// PowerShell to extract the raw saveState/split bytes from slot 1:
//
//   $r = (Get-ItemProperty 'HKCU:\Software\N8SDR\Lyra-cpp\layouts\1').windowState
//   $low = for ($i=0; $i -lt $r.Length; $i+=2) { $r[$i] }       # UTF-16LE low bytes
//   $raw = $low[11 .. ($low.Length-2)]                          # strip "@ByteArray(" + ")"
//   [Convert]::ToBase64String([byte[]]$raw)
//
// (repeat for .panadapterSplit).  Do NOT base64 the raw REG_BINARY directly —
// that embeds the "@ByteArray(" UTF-16 wrapper and restoreState() rejects it
// (the bug that left the prior factory default broken).

#pragma once

#include <QByteArray>

namespace lyra::ui {

inline QByteArray defaultWindowState() {
    return QByteArray::fromBase64(QByteArrayLiteral(
        "AAAA/wAAAAD9AAAAAgAAAAIAAAoAAAAFDfwBAAAAAfwAAAAAAAAKAAAAA/sA/////AIAAAAF"
        "/AAAADMAAACvAAAAFgD////8AQAAAAP7AAAACABiAGEAbgBkAQAAAAAAAANFAAAAdgD////7"
        "AAAADAB0AHUAbgBpAG4AZwEAAANJAAAENgAAAIUA////+wAAAAoAbQBlAHQAZQByAQAAB4MA"
        "AAJ9AAAAfAD////8AAAA5gAAAHYAAAAWAP////wBAAAAA/sAAAAKAGEAdQBkAGkAbwEAAAAA"
        "AAAE3gAAAHwA////+wAAABQAbQBvAGQAZQBmAGkAbAB0AGUAcgEAAATiAAACRAAAAIIA////"
        "+wAAAA4AZABpAHMAcABsAGEAeQEAAAcqAAAC1gAAAIYA/////AAAAWAAAABTAAAAUwD////8"
        "AQAAAAP7AAAABAB0AHgBAAAAAAAABI0AAABhAP////sAAAAQAHAAcgBvAGYAaQBsAGUAcwEA"
        "AASRAAAB1gAAAI0A////+wAAABYAcAByAG8AcABhAGcAYQB0AGkAbwBuAQAABmsAAAOVAAAD"
        "BQD////7AAAAFABwAGEAbgBhAGQAYQBwAHQAZQByAQAAAbcAAAOJAAAAFgD////7AAAAEAB0"
        "AHgAcwBwAGUAZQBjAGgCAAAKFAAAA5EAAAHOAAAByAAAAAMAAAAAAAAAAPwBAAAABvsAAAAI"
        "AHQAeABlAHECAAAMNQAAA3gAAANeAAABbvsAAAAYAHQAeABjAG8AbQBiAGkAbgBhAHQAbwBy"
        "AgAAD44AAAOSAAAB2AAAAdD7AAAADgB0AHgAcABsAGEAdABlAgAAEaYAAAOWAAABzgAAAVj7"
        "AAAAEgBjAHcAYwBvAG4AcwBvAGwAZQoAAApKAAACcAAAAc4AAAJ3+wAAAAgAcgB4AGUAcQIA"
        "AAWxAAABzAAAA14AAAFu+wAAABIAYwB3AGQAZQBjAG8AZABlAHICAAAOzQAAAfoAAAHYAAAC"
        "NQAACgAAAAAAAAAABAAAAAQAAAAIAAAACPwAAAABAAAAAgAAAAEAAAAYAG0AYQBpAG4AXwB0"
        "AG8AbwBsAGIAYQByAQAAAAD/////AAAAAAAAAAA="));
}

// The panadapter/waterfall vertical SplitView state that ships with the
// factory layout (applied via Prefs::setPanadapterSplit in applyDefaultLayout).
inline QByteArray defaultPanadapterSplit() {
    return QByteArray::fromBase64(QByteArrayLiteral(
        "gaJlaW5kZXgBb3ByZWZlcnJlZEhlaWdodPtAgGAAAAAAAA=="));
}

} // namespace lyra::ui

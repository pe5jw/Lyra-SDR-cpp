// Lyra — Mode + Filter dock panel.
//
// Mirrors old Lyra's MODE+FILTER panel (RX side): a Mode picker and an
// RX-bandwidth picker.  Bandwidth presets are per-mode (SSB / CW / AM /
// FM / DSB / DIG) and the chosen bandwidth is remembered per mode via
// the shared Prefs object, exactly like old Lyra.  Both controls drive
// WdspEngine.mode / .bandwidth, which push SetRXAMode + RXASetPassband
// using the sideband-correct passband convention.
//
// SCOPE: RX-only (this build has no TX yet) — TX BW, Rate switching and
// a CW-pitch control are deferred; CWU/CWL centre on a 600 Hz default.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitHeight: 50
    implicitWidth: 540
    color: "#101820"
    border.color: "#2a4a5a"

    // Mode list + per-mode bandwidth presets (Hz), from old Lyra.
    readonly property var modes: ["LSB", "USB", "CWL", "CWU",
                                  "DSB", "AM", "FM", "DIGU", "DIGL"]
    readonly property var bwSSB: [1500, 1800, 2100, 2400, 2700, 3000,
                                  3600, 4000, 6000, 8000, 10000]
    readonly property var bwCW:  [50, 100, 150, 250, 400, 500, 750, 1000]
    readonly property var bwDSB: [3000, 4000, 5000, 6000, 8000, 10000]
    readonly property var bwAM:  [3000, 4000, 6000, 8000, 10000, 12000]
    readonly property var bwFM:  [6000, 8000, 10000, 12000, 15000]
    readonly property var bwDIG: [1500, 2400, 3000, 3600, 4000, 6000]

    function presetsFor(mode) {
        if (mode === "LSB" || mode === "USB") return bwSSB
        if (mode === "CWL" || mode === "CWU") return bwCW
        if (mode === "DSB") return bwDSB
        if (mode === "AM")  return bwAM
        if (mode === "FM")  return bwFM
        if (mode === "DIGL" || mode === "DIGU") return bwDIG
        return bwSSB
    }
    function fmtBw(hz) {
        return hz >= 1000 ? (hz / 1000).toFixed(hz % 1000 ? 1 : 0) + " k"
                          : hz + " Hz"
    }
    // Nearest preset index for the current bandwidth (handles a stored
    // value that isn't an exact preset).
    function bwIndex(mode, bw) {
        var p = presetsFor(mode), best = 0
        for (var i = 1; i < p.length; ++i)
            if (Math.abs(p[i] - bw) < Math.abs(p[best] - bw)) best = i
        return best
    }

    // IQ sample rates (96/192/384 k — 48 k excluded, EP2 cadence).
    readonly property var rateVals: [96000, 192000, 384000]
    function rateIndex(r) {
        var i = rateVals.indexOf(r)
        return i < 0 ? 1 : i
    }
    // A rate switch touches three places: Prefs (persist), the HL2 wire
    // speed bits (Stream), and the WDSP channel + analyzer (Engine).
    function applyRate(r) {
        Prefs.sampleRate = r
        Stream.setSampleRate(r)
        WdspEngine.setSampleRate(r)
    }
    // Push the persisted rate to the wire + DSP at startup (before the
    // channel opens on Start, so it comes up at the right rate).
    Component.onCompleted: applyRate(Prefs.sampleRate)

    // Both controls are the single source of truth via Prefs; the engine
    // follows.  (Engine pushes SetRXAMode + RXASetPassband on change.)
    Binding { target: WdspEngine; property: "mode";      value: Prefs.mode }
    Binding { target: WdspEngine; property: "bandwidth"; value: Prefs.rxBandwidth }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 8

        Label { text: qsTr("Rate"); color: "#cccccc"; font.bold: true }
        ComboBox {
            id: rateCombo
            Layout.preferredWidth: 76
            model: ["96 k", "192 k", "384 k"]
            currentIndex: root.rateIndex(Prefs.sampleRate)
            onActivated: root.applyRate(root.rateVals[currentIndex])
        }

        Rectangle {   // divider
            Layout.preferredWidth: 1
            Layout.topMargin: 10; Layout.bottomMargin: 10
            Layout.fillHeight: true
            color: "#2a4a5a"
        }

        Label { text: qsTr("Mode"); color: "#cccccc"; font.bold: true }
        ComboBox {
            id: modeCombo
            Layout.preferredWidth: 84
            model: root.modes
            currentIndex: Math.max(0, root.modes.indexOf(Prefs.mode))
            onActivated: Prefs.mode = root.modes[currentIndex]
        }

        Rectangle {   // divider
            Layout.preferredWidth: 1
            Layout.topMargin: 10; Layout.bottomMargin: 10
            Layout.fillHeight: true
            color: "#2a4a5a"
        }

        Label { text: qsTr("RX BW"); color: "#cccccc"; font.bold: true }
        ComboBox {
            id: bwCombo
            Layout.preferredWidth: 90
            // Rebuilds when the mode changes (presets are per-mode).
            model: root.presetsFor(Prefs.mode).map(root.fmtBw)
            currentIndex: root.bwIndex(Prefs.mode, Prefs.rxBandwidth)
            onActivated: Prefs.rxBandwidth =
                root.presetsFor(Prefs.mode)[currentIndex]
        }

        Item { Layout.fillWidth: true }
    }
}

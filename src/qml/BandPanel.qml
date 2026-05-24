// Lyra — Band dock panel (HF/6m amateur band switching).
//
// One button per amateur band (160m … 6m), built from the shared C++
// band table (Bands context property) — a single source of truth, so
// the list never drifts from the protocol/DSP side.  Clicking a band
// tunes RX1 to that band's default frequency.
//
// Active-band highlight — modeled on old Lyra + Thetis (studied
// 2026-05-23), NOT a per-button live comparison:
//   * Old Lyra: an explicit _refresh_band_highlight() loops every
//     button and sets exactly one checked, driven by the freq-changed
//     signal (red-glow :checked style).
//   * Thetis: auto-exclusive RadioButtons-as-buttons — framework
//     guarantees one checked; the active one gets a distinct fill.
// Common principle: ONE mutually-exclusive "active" state, set
// IMPERATIVELY when the frequency changes — clear all, light one.
//
// We follow that here: a single `activeBand` index is recomputed in the
// onRx1FreqChanged handler (the signal reliably fires — the Tuning panel
// relies on it the same way) via Bands.indexForFreq().  Each button's
// look derives from `index === activeBand` through a CUSTOM background,
// so exactly one band lights and the previous one clears.  This avoids
// the earlier failure mode where binding `highlighted` directly to
// Stream.rx1FreqHz per-button left old buttons stuck lit.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitHeight: 44
    implicitWidth: 720
    color: "#101820"
    border.color: "#2a4a5a"

    // The one source of truth: index of the band that contains RX1's
    // frequency (-1 = none).  Set IMPERATIVELY — never a per-button
    // reactive read of Stream.rx1FreqHz (that proved unreliable here).
    property int activeBand: -1
    function refreshActiveBand() {
        // Imperative call (not a binding) — reading a C++ Q_INVOKABLE /
        // context property inside a function always works; it's only
        // BINDINGS through the context property that were flaky.
        root.activeBand = Bands.indexForFreq(Stream.rx1FreqHz)
    }
    Component.onCompleted: refreshActiveBand()
    Connections {
        target: Stream
        function onRx1FreqChanged() { root.refreshActiveBand() }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 4

        Label { text: qsTr("Band"); color: "#cccccc"; font.bold: true }

        Repeater {
            model: Bands.amateur()
            delegate: Button {
                id: bandBtn
                required property var modelData
                required property int index
                readonly property int bandHz: modelData.hz
                // Active state derives ONLY from the local activeBand
                // index — a local-property binding, which IS reliably
                // reactive (unlike the cross-context-property read).
                readonly property bool active: index === root.activeBand
                text: modelData.name
                // Same flat "chip" shape as the Solar band cells (the
                // operator-chosen look): compact translucent box, 1px
                // border, 40×20.  The Band panel keeps its OWN cyan-idle /
                // red-glow-active colour scheme.
                Layout.preferredWidth: 44
                Layout.preferredHeight: 22
                padding: 0
                focusPolicy: Qt.NoFocus
                // Return to the last frequency you were on in this band
                // (per-band memory); fall back to the band's default.
                onClicked: {
                    var f = BandMemory.freqFor(modelData.name)
                    Stream.setRx1FreqHz(f > 0 ? f : bandHz)
                }

                // Custom background — fully owned, so no Qt-style
                // highlight/hover state can leak.  Active band = the
                // old-Lyra red-glow; idle = the cyan theme on the shared
                // semi-translucent chip fill.
                background: Rectangle {
                    radius: 4
                    color: bandBtn.active ? "#260808"
                           : (bandBtn.down ? "#1d2b38" : "#b416202a")
                    border.width: bandBtn.active ? 2 : 1
                    // Bright cyan outline so the chip reads like the Solar
                    // cells' coloured outline (their look comes from a
                    // saturated border, not a dark one).  Active = red glow.
                    border.color: bandBtn.active ? "#ff3344"
                                  : (bandBtn.hovered ? "#8fdcff" : "#5ec8ff")
                }
                contentItem: Text {
                    text: bandBtn.text
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 13
                    font.bold: true
                    // Text matches the outline colour (cyan idle / amber
                    // active) — the same border==text chip styling Solar uses.
                    color: bandBtn.active ? "#ffcc88" : "#5ec8ff"
                }
            }
        }

        Item { Layout.fillWidth: true }
    }
}

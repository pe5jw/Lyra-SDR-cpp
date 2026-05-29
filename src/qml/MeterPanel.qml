// Lyra — Meter dock panel (RX signal-strength S-meter).
//
// Hosts one of two GPU-flavoured renderers — Horizon Arc (default) or
// Plasma Bar — both fed by the C++ `Meter` context property (MeterModel).
// A tiny Arc|Bar toggle in the corner flips the style live so the two
// can be compared without diving into Settings; the choice persists.
//
// Today the only source is the RX S-meter.  When TX + HL2 telemetry
// land, the same renderers gain PWR/SWR/ALC/MIC/PA/Temp sources with
// click-to-cycle — no inert placeholders now.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitWidth: 300
    implicitHeight: 200
    color: "#101820"
    border.color: "#2a4a5a"

    // ── Style toggle (top-right): Arc | Bar ──
    Row {
        id: styleToggle
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 6
        anchors.rightMargin: 8
        spacing: 0
        z: 10

        Repeater {
            model: [{ t: "Arc", s: 0 }, { t: "Bar", s: 1 }]
            delegate: Rectangle {
                width: 34; height: 18
                readonly property bool sel: Meter.style === modelData.s
                color: sel ? "#15435a" : "#0c1218"
                border.color: sel ? "#00e5ff" : "#2a4a5a"
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: modelData.t
                    color: sel ? "#00e5ff" : "#7790a0"
                    font.pixelSize: 10
                    font.bold: sel
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Meter.style = modelData.s
                }
            }
        }
    }

    // ── The active renderer ──
    Loader {
        id: face
        anchors.fill: parent
        anchors.margins: 6
        sourceComponent: Meter.style === 1 ? barFace : arcFace
    }

    Component { id: arcFace; HorizonArc {} }
    Component { id: barFace; PlasmaBar {} }

    // ── Click-to-cycle source picker (task #35 / FEATURES.md §6) ──
    // Click on the meter face cycles the source through the wired
    // options.  Setting Meter.source routes through MeterModel::
    // setSource which writes to the appropriate preference slot
    // (rxSource or txSource based on the live MOX state) — so the
    // click persists into the operator's pref for that state, not
    // a transient mode.
    //
    // The cycle list is constant for now: only the wired sources
    // appear (RX S-meter / PWR / SWR).  PA_CURRENT / PA_VOLTS / TEMP /
    // ALC / MIC / COMP get added when their MeterModel computes
    // land in follow-on commits.  The Arc|Bar toggle in the top-
    // right corner sits ABOVE this MouseArea (higher z) so clicks
    // there don't fall through to the source cycle.
    MouseArea {
        anchors.fill: face
        z: 1                              // below the styleToggle (z:10)
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton
        property var sourceCycle: [0, 1, 2]  // RX_SMETER, PWR, SWR
        onClicked: function(mouse) {
            var i = sourceCycle.indexOf(Meter.source)
            var next = sourceCycle[(i + 1) % sourceCycle.length]
            Meter.source = next
        }
        ToolTip.delay: 1500
        ToolTip.visible: containsMouse && !pressed
        ToolTip.text: qsTr("Click to cycle the meter source.  Sets your "
                           + "RX or TX preference (depending on whether "
                           + "the radio is keyed) — Settings → Meter "
                           + "also picks each one explicitly.")
        hoverEnabled: true
    }
}

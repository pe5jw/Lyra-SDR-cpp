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
}

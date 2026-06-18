// Lyra — CW console dock panel (#105 CW-3b).  Chip-launched, floats by
// default (audio-rack idiom): only operators who keyboard-send and/or
// decode CW open it.  Container built once to hold both halves —
//   - Send (CW-3b): WPM + a type-and-send field (Enter sends, Esc/Stop
//     aborts) calling Stream.sendCw / Stream.abortCw.  The host morse
//     keyer drives tx[0].cwx/cwx_ptt; the gateware keys the carrier +
//     HW sidetone.  No-op outside CW mode (cw_enable is CW-mode only).
//   - Decoder (CW-5): the reserved pane below lands the RX CW decoder.
//
// Bindings: Stream (HL2Stream) for sendCw/abortCw + cwKeyerSpeedWpm;
// WdspEngine.mode to gate/dim the send controls outside CWU/CWL.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitWidth: 420
    implicitHeight: collapsed ? 40 : (body.implicitHeight + 12)
    color: "#0d141b"
    border.color: "#2a4a5a"

    readonly property color cAccent: "#00e5ff"
    readonly property color cText:   "#cdd9e5"
    readonly property color cMuted:  "#8a9aac"

    property bool collapsed: false

    // CW send only does anything in CWU/CWL (cw_enable gate); dim + disable
    // the send controls + show a hint otherwise.
    readonly property bool cwActive: {
        var m = WdspEngine.mode.toUpperCase()
        return m === "CWU" || m === "CWL"
    }

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: 6
        spacing: 6

        // ── Header ──────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Label { text: qsTr("CW Console"); color: root.cAccent
                    font.bold: true; font.pixelSize: 14 }
            Label { text: qsTr("keyboard send + decode"); color: root.cMuted
                    font.pixelSize: 11 }
            Label {
                visible: !root.cwActive
                text: qsTr("• switch to CW to send")
                color: "#e0a030"; font.pixelSize: 11
            }
            Item { Layout.fillWidth: true }
            Button {
                implicitWidth: 28; implicitHeight: 22
                text: root.collapsed ? "▲" : "▼"
                onClicked: root.collapsed = !root.collapsed
                background: Rectangle { radius: 4; color: "#1f2a35"; border.color: "#3a5060" }
                contentItem: Text { text: parent.text; color: root.cAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter }
            }
        }

        // ── WPM ─────────────────────────────────────────────────────────
        RowLayout {
            visible: !root.collapsed
            Layout.fillWidth: true
            spacing: 8
            Label { text: qsTr("WPM"); color: root.cText; font.bold: true
                    font.pixelSize: 13; Layout.preferredWidth: 48 }
            Slider {
                id: wpm
                Layout.fillWidth: true
                from: 5; to: 60; stepSize: 1
                snapMode: Slider.SnapAlways
                value: Stream.cwKeyerSpeedWpm
                onMoved: Stream.cwKeyerSpeedWpm = value
            }
            Label {
                text: Math.round(Stream.cwKeyerSpeedWpm) + qsTr(" wpm")
                color: root.cText; font.family: "Consolas"; font.bold: true
                font.pixelSize: 13; Layout.preferredWidth: 64
                horizontalAlignment: Text.AlignRight
            }
        }

        // ── Send field ──────────────────────────────────────────────────
        RowLayout {
            visible: !root.collapsed
            Layout.fillWidth: true
            spacing: 6
            opacity: root.cwActive ? 1.0 : 0.5
            TextField {
                id: sendField
                Layout.fillWidth: true
                enabled: root.cwActive
                placeholderText: qsTr("type, Enter sends — Esc stops")
                color: root.cText
                font.family: "Consolas"; font.pixelSize: 14
                selectByMouse: true
                background: Rectangle {
                    radius: 4; color: "#0b141b"
                    border.color: sendField.activeFocus ? root.cAccent : "#2a4a5a"
                }
                onAccepted: {
                    if (text.length > 0) {
                        Stream.sendCw(text)
                        text = ""
                    }
                }
                Keys.onEscapePressed: Stream.abortCw()
            }
            Button {
                text: qsTr("Send")
                enabled: root.cwActive && sendField.text.length > 0
                onClicked: { Stream.sendCw(sendField.text); sendField.text = "" }
                background: Rectangle { radius: 4
                    color: enabled ? "#1c3a44" : "#16202a"
                    border.color: enabled ? root.cAccent : "#2a3a44" }
                contentItem: Text { text: parent.text
                    color: parent.enabled ? root.cAccent : root.cMuted
                    font.bold: true; font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter }
            }
            Button {
                text: qsTr("Stop")
                onClicked: Stream.abortCw()
                background: Rectangle { radius: 4; color: "#3a1c1c"
                    border.color: "#c0504d" }
                contentItem: Text { text: parent.text; color: "#ff8a80"
                    font.bold: true; font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter }
            }
        }

        // ── Decoder pane (reserved for CW-5) ────────────────────────────
        Rectangle {
            visible: !root.collapsed
            Layout.fillWidth: true
            Layout.preferredHeight: 96
            radius: 4
            color: "#0b141b"
            border.color: "#1c2a36"
            Label {
                anchors.centerIn: parent
                text: qsTr("RX decoder — coming with CW-5")
                color: root.cMuted; font.pixelSize: 12
            }
        }
    }
}

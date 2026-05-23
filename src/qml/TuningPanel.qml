// Lyra — Tuning dock panel (RX1 / DDC0).
//
// Mirrors old Lyra's RX1 tuning: a big LED-style VFO readout
// (FreqDisplay, ported from led_freq.py) you tune by clicking a digit
// and wheeling, plus a Step combo that sets the wheel resolution when
// you're not over a specific digit, plus double-click-to-type direct
// frequency entry.  All RX1 (DDC0) — drives Stream.setRx1FreqHz.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LyraUI

Rectangle {
    id: root
    implicitHeight: 74
    implicitWidth: 600
    color: "#101820"
    border.color: "#2a4a5a"

    // Local mirror of the tuned freq (signal-driven — a direct binding
    // to Stream.rx1FreqHz is unreliable in this QQuickWidget setup; same
    // lesson as the Band panel).
    property int centerHz: 0
    Component.onCompleted: centerHz = Stream.rx1FreqHz
    Connections {
        target: Stream
        function onRx1FreqChanged() { root.centerHz = Stream.rx1FreqHz }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 6
        anchors.bottomMargin: 6
        spacing: 10

        Label { text: qsTr("RX1"); color: "#cccccc"; font.bold: true }

        // LED VFO readout + the typed-entry overlay (double-click).
        Item {
            Layout.preferredWidth: 380
            Layout.fillHeight: true

            FreqDisplay {
                id: led
                anchors.fill: parent
                freqHz: root.centerHz
                externalStepHz: stepCombo.stepVals[stepCombo.currentIndex]
                onFreqEdited: (hz) => Stream.setRx1FreqHz(hz)
                onEditRequested: {
                    editField.text = (led.freqHz / 1.0e6).toFixed(6)
                    editField.visible = true
                    editField.selectAll()
                    editField.forceActiveFocus()
                }
            }
            // Direct-entry field — overlays the LED on double-click.
            // Accepts MHz decimal ("7.074"), Hz with separators
            // ("7.074.000"), or bare numbers (parseFreqInput decides).
            TextField {
                id: editField
                anchors.fill: parent
                visible: false
                horizontalAlignment: TextInput.AlignHCenter
                verticalAlignment: TextInput.AlignVCenter
                font.family: "Consolas"
                font.pixelSize: 22
                font.bold: true
                color: "#ffb000"
                background: Rectangle {
                    color: "#000000"
                    border.color: "#00d8ff"
                    border.width: 2
                    radius: 3
                }
                function commit() {
                    if (!visible) return
                    var hz = led.parseFreqInput(text)
                    visible = false
                    if (hz >= 0) Stream.setRx1FreqHz(hz)
                }
                onAccepted: commit()
                onActiveFocusChanged: if (!activeFocus) commit()
                Keys.onEscapePressed: visible = false
            }
        }

        Label { text: qsTr("Step"); color: "#cccccc"; font.bold: true }
        ComboBox {
            id: stepCombo
            Layout.preferredWidth: 84
            // Wheel-tune step when the cursor isn't on a specific digit.
            property var stepVals: [1, 10, 100, 1000, 5000, 10000]
            model: ["1 Hz", "10 Hz", "100 Hz", "1 kHz", "5 kHz", "10 kHz"]
            currentIndex: 3   // 1 kHz default
        }

        Item { Layout.fillWidth: true }
    }
}

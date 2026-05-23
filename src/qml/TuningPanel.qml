// Lyra — Tuning dock panel (RX1 / DDC0).
//
// Mirrors old Lyra's RX1 tuning: the Lyra logo, a big LED-style VFO
// readout (FreqDisplay) you tune by clicking a digit + wheeling, a Step
// combo for wheel resolution, double-click-to-type entry, and a CW
// Pitch control (shown in CW modes).
//
// CW carrier convention (old Lyra / Thetis): the LED shows the signal
// CARRIER (VFO); the hardware DDS is offset by ±pitch so the carrier
// lands in the pitch-centred filter.  VFO = DDS + WdspEngine.markerOffsetHz,
// so we display centerHz+offset and write (vfo − offset) to the wire.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LyraUI

Rectangle {
    id: root
    implicitHeight: 120
    implicitWidth: 680
    color: "#101820"
    border.color: "#2a4a5a"

    // Local mirror of the wire (DDS) freq — signal-driven (a direct
    // binding to Stream.rx1FreqHz is unreliable in this QQuickWidget).
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

        Image {
            source: "qrc:/qt/qml/Lyra/src/assets/logo/lyra-icon-256.png"
            Layout.fillHeight: true
            Layout.preferredWidth: height   // square, as tall as the row
            Layout.alignment: Qt.AlignVCenter
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
        }

        Label { text: qsTr("RX1"); color: "#cccccc"; font.bold: true }

        // LED VFO readout (shows the carrier) + typed-entry overlay.
        Item {
            Layout.preferredWidth: 380
            Layout.preferredHeight: 64
            Layout.maximumHeight: 66       // don't balloon with the logo
            Layout.alignment: Qt.AlignVCenter

            FreqDisplay {
                id: led
                anchors.fill: parent
                // VFO (carrier) = DDS + CW marker offset.
                freqHz: root.centerHz + WdspEngine.markerOffsetHz
                externalStepHz: stepCombo.stepVals[stepCombo.currentIndex]
                // Tune: convert the carrier back to the DDS wire freq.
                onFreqEdited: (hz) =>
                    Stream.setRx1FreqHz(hz - WdspEngine.markerOffsetHz)
                onEditRequested: {
                    editField.text = (led.freqHz / 1.0e6).toFixed(6)
                    editField.visible = true
                    editField.selectAll()
                    editField.forceActiveFocus()
                }
            }
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
                    var hz = led.parseFreqInput(text)   // entered carrier
                    visible = false
                    if (hz >= 0)
                        Stream.setRx1FreqHz(hz - WdspEngine.markerOffsetHz)
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
            property var stepVals: [1, 10, 100, 1000, 5000, 10000]
            model: ["1 Hz", "10 Hz", "100 Hz", "1 kHz", "5 kHz", "10 kHz"]
            currentIndex: 3   // 1 kHz default
        }

        // CW pitch — shown only in CW modes (old Lyra placed it here).
        Label {
            text: qsTr("Pitch")
            color: "#cccccc"; font.bold: true
            visible: Prefs.mode === "CWU" || Prefs.mode === "CWL"
        }
        SpinBox {
            id: pitchSpin
            visible: Prefs.mode === "CWU" || Prefs.mode === "CWL"
            Layout.preferredWidth: 110
            from: 200; to: 1500; stepSize: 10
            value: WdspEngine.cwPitchHz
            onValueModified: WdspEngine.setCwPitchHz(value)
        }

        Item { Layout.fillWidth: true }
    }
}

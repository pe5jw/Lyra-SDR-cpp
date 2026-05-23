// Lyra — Tuning dock panel (RX1 / DDC0 receive frequency).
//
// Hosted by a QQuickWidget inside a QDockWidget.  Root is a Rectangle
// (an Item) so the QQuickWidget can host it.  Context property Stream
// is provided by the host.  Always visible in the dock so the operator
// can set the frequency before a stream opens.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitHeight: 48
    implicitWidth: 600
    color: "#101820"
    border.color: "#2a4a5a"

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 8
        Label { text: qsTr("RX1"); color: "#cccccc"; font.bold: true }
        Button {
            text: qsTr("−10k")
            onClicked: Stream.setRx1FreqHz(Stream.rx1FreqHz - 10000)
        }
        Button {
            text: qsTr("−1k")
            onClicked: Stream.setRx1FreqHz(Stream.rx1FreqHz - 1000)
        }
        TextField {
            id: freqField
            Layout.preferredWidth: 150
            horizontalAlignment: TextInput.AlignHCenter
            font.family: "Consolas"
            font.pixelSize: 16
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            Component.onCompleted:
                text = (Stream.rx1FreqHz / 1.0e6).toFixed(6)
            onEditingFinished: {
                var mhz = parseFloat(text)
                if (!isNaN(mhz)) {
                    Stream.setRx1FreqHz(Math.round(mhz * 1.0e6))
                }
            }
        }
        Label { text: qsTr("MHz"); color: "#999" }
        Button {
            text: qsTr("+1k")
            onClicked: Stream.setRx1FreqHz(Stream.rx1FreqHz + 1000)
        }
        Button {
            text: qsTr("+10k")
            onClicked: Stream.setRx1FreqHz(Stream.rx1FreqHz + 10000)
        }
        // (Band switching lives in the Band panel; FT8/other spot
        // presets will come back via the Memory feature later.)
        Item { Layout.fillWidth: true }
    }
    // Keep the field in sync when freq changes via buttons / presets,
    // but never stomp the operator mid-edit.
    Connections {
        target: Stream
        function onRx1FreqChanged() {
            if (!freqField.activeFocus) {
                freqField.text = (Stream.rx1FreqHz / 1.0e6).toFixed(6)
            }
        }
    }
}

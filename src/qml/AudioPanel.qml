// Lyra — Audio dock panel (mute / volume / output device).
//
// Hosted by a QQuickWidget inside a QDockWidget.  Root is a Rectangle
// so the QQuickWidget can host it.  Context property WdspEngine is
// provided by the host.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitHeight: 46
    implicitWidth: 600
    // Match the shared panel theme (Tuning / Display / Band) — Audio
    // was the lone outlier on a flat grey (#161616/#333).
    color: "#101820"
    border.color: "#2a4a5a"

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 12
        Label {
            text: qsTr("Audio")
            color: "#cccccc"
            font.bold: true
        }
        Button {
            text: WdspEngine.muted ? qsTr("Unmute") : qsTr("Mute")
            onClicked: WdspEngine.setMuted(!WdspEngine.muted)
        }
        Label {
            text: WdspEngine.muted ? qsTr("MUTED") : qsTr("LIVE")
            color: WdspEngine.muted ? "#ffd07f" : "#7fff7f"
            font.bold: true
            font.family: "Consolas"
        }
        Label { text: qsTr("Vol"); color: "#999" }
        Slider {
            id: volSlider
            Layout.preferredWidth: 200
            from: 0.0
            to: 1.0
            value: WdspEngine.volume
            onMoved: WdspEngine.setVolume(value)
            // Mouse-wheel nudges volume ±0.02 (≈ fine steps over 0–1).
            WheelHandler {
                onWheel: (ev) => {
                    var nv = WdspEngine.volume
                             + (ev.angleDelta.y > 0 ? 0.02 : -0.02)
                    WdspEngine.setVolume(Math.max(0.0, Math.min(1.0, nv)))
                }
            }
        }
        Label {
            text: WdspEngine.volume <= 0.0
                  ? qsTr("-∞ dB")
                  : Math.round(WdspEngine.volumeDb) + qsTr(" dB")
            color: "#cccccc"
            font.family: "Consolas"
            Layout.preferredWidth: 56
        }
        Item { Layout.fillWidth: true }
        Label { text: qsTr("Out"); color: "#999" }
        ComboBox {
            Layout.preferredWidth: 300
            model: WdspEngine.audioOutputDevices()
            currentIndex: WdspEngine.audioDeviceIndex
            onActivated: WdspEngine.setAudioOutputDevice(currentIndex)
        }
    }
}

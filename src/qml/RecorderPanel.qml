// Lyra — Session Recorder panel (#201, RECORDER_DESIGN.md).
//
// Opt-in dockable tool window (View → Recorder; NOT in the default shipped
// layout).  A big REC / ⏹ button + live elapsed timer + a snapshot on/off
// toggle.  The heavy config (path, split, storage cap) and the Sessions list
// live on Settings → Recording (Stage 4); this panel is the one-press
// operate surface.  The always-visible "● REC" status-bar chip (MainWindow)
// is the independent "don't forget it's running" safety light.
//
// Bindings: Recorder (recording / elapsedMs / snapshotsOn / toggle()) —
// exposed as a context property by MainWindow::makeQuick.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitWidth: 320
    implicitHeight: 172
    color: "#0d141b"
    border.color: "#2a4a5a"

    readonly property color cAccent: "#00e5ff"
    readonly property color cText:   "#cdd9e5"
    readonly property color cMuted:  "#8a9aac"
    readonly property color cRed:    "#e67878"

    readonly property bool recording: Recorder ? Recorder.recording : false

    function hhmmss(ms) {
        var s = Math.floor((ms || 0) / 1000)
        var h = Math.floor(s / 3600)
        var m = Math.floor((s % 3600) / 60)
        var ss = s % 60
        function p(n) { return (n < 10 ? "0" : "") + n }
        return p(h) + ":" + p(m) + ":" + p(ss)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Text { text: "Session recorder"; color: root.cText
                   font.pixelSize: 15; font.bold: true }
            Item { Layout.fillWidth: true }
            // Live status dot — red + pulsing while recording.
            Rectangle {
                width: 12; height: 12; radius: 6
                color: root.recording ? root.cRed : "#33414d"
                SequentialAnimation on opacity {
                    running: root.recording; loops: Animation.Infinite
                    NumberAnimation { from: 1.0; to: 0.25; duration: 700 }
                    NumberAnimation { from: 0.25; to: 1.0; duration: 700 }
                }
            }
        }

        // ── Elapsed timer ──
        Text {
            Layout.alignment: Qt.AlignHCenter
            text: root.hhmmss(Recorder ? Recorder.elapsedMs : 0)
            color: root.recording ? root.cRed : root.cMuted
            font.pixelSize: 34; font.bold: true
            font.family: "Consolas, Menlo, monospace"
        }

        // ── REC / Stop ──
        Button {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            text: root.recording ? "■  Stop recording" : "●  Record"
            enabled: Recorder !== null
            onClicked: if (Recorder) Recorder.toggle()
            contentItem: Text {
                text: parent.text; color: "#04121a"
                font.pixelSize: 15; font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 8
                color: root.recording ? root.cRed : root.cAccent
                opacity: parent.down ? 0.8 : 1.0
            }
        }

        RowLayout {
            Layout.fillWidth: true
            CheckBox {
                id: snapChk
                text: "Capture pan/waterfall snapshots"
                checked: Recorder ? Recorder.snapshotsOn : true
                onToggled: if (Recorder) Recorder.snapshotsOn = checked
                contentItem: Text {
                    text: snapChk.text; color: root.cText; font.pixelSize: 12
                    leftPadding: snapChk.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Records RX audio to a timestamped session folder. "
                  + "Path, auto-split, storage cap and the sessions list are on "
                  + "Settings → Recording."
            color: root.cMuted; font.pixelSize: 11
        }

        Item { Layout.fillHeight: true }
    }
}

// Lyra — shared trashcan delete control (matches the Tuner panel #188).
//
// Click the can to ARM, then ✓ confirms / ✗ cancels — a two-step guard against
// an accidental delete.  Drawn mono line-art (not a colour emoji) to match the
// flat dark UI.  Used by the Voice Keyer + CW console clip/macro/token rows so
// every "delete" reads the same.  Usage:
//     TrashCan { onConfirmed: Model.remove(index) }

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: ctl
    property color iconColor:  "#ff8a80"   // the can (armed ✓ shares it)
    property color mutedColor: "#8a9aac"   // the ✗ cancel
    property string tip: qsTr("Delete")
    signal confirmed()

    property bool arming: false
    implicitWidth: arming ? 48 : 24
    implicitHeight: 24

    // Re-arm safety: if the row leaves edit mode etc., callers can reset via
    // visibility; onVisibleChanged clears a dangling armed state.
    onVisibleChanged: if (!visible) arming = false

    RowLayout {
        anchors.centerIn: parent
        spacing: 2

        ToolButton {
            visible: !ctl.arming; implicitWidth: 24; implicitHeight: 24
            background: Item {}
            contentItem: Canvas {
                anchors.centerIn: parent
                width: 18; height: 20
                readonly property color col: ctl.iconColor
                onColChanged: requestPaint()
                Component.onCompleted: requestPaint()
                onPaint: {
                    var c = getContext("2d"); c.reset();
                    c.strokeStyle = col; c.lineWidth = 1.6;
                    c.lineCap = "round"; c.lineJoin = "round";
                    var w = width, h = height;
                    c.beginPath();                        // lid
                    c.moveTo(1.5, 3.5); c.lineTo(w - 1.5, 3.5); c.stroke();
                    c.beginPath();                        // handle
                    c.moveTo(w * 0.34, 3.5); c.lineTo(w * 0.38, 1.4);
                    c.lineTo(w * 0.62, 1.4); c.lineTo(w * 0.66, 3.5); c.stroke();
                    c.beginPath();                        // body (tapered)
                    c.moveTo(2.6, 4.6); c.lineTo(w - 2.6, 4.6);
                    c.lineTo(w - 3.6, h - 1.2); c.lineTo(3.6, h - 1.2);
                    c.closePath(); c.stroke();
                    c.beginPath();                        // ribs
                    c.moveTo(w * 0.37, 6.2); c.lineTo(w * 0.37, h - 3.0);
                    c.moveTo(w * 0.50, 6.2); c.lineTo(w * 0.50, h - 3.0);
                    c.moveTo(w * 0.63, 6.2); c.lineTo(w * 0.63, h - 3.0);
                    c.stroke();
                }
            }
            onClicked: ctl.arming = true
            ToolTip.text: ctl.tip
            ToolTip.visible: hovered; ToolTip.delay: 500
        }
        ToolButton {
            visible: ctl.arming; implicitWidth: 20; implicitHeight: 22
            background: Item {}
            contentItem: Label { text: "✓"; anchors.centerIn: parent
                color: ctl.iconColor; font.pixelSize: 14; font.bold: true }
            onClicked: { ctl.arming = false; ctl.confirmed() }
            ToolTip.text: qsTr("Confirm delete"); ToolTip.visible: hovered; ToolTip.delay: 300
        }
        ToolButton {
            visible: ctl.arming; implicitWidth: 20; implicitHeight: 22
            background: Item {}
            contentItem: Label { text: "✗"; anchors.centerIn: parent
                color: ctl.mutedColor; font.pixelSize: 14; font.bold: true }
            onClicked: ctl.arming = false
            ToolTip.text: qsTr("Cancel"); ToolTip.visible: hovered; ToolTip.delay: 300
        }
    }
}

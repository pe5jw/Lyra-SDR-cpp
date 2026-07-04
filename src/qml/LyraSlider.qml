// Lyra — shared themed horizontal slider.
//
// A drop-in replacement for a stock Controls `Slider` that draws its OWN
// groove + handle at a FIXED size, so it renders identically regardless of
// the global Qt Quick Controls style (native / Basic / …) and can never
// balloon or clip a panel.  All the usual Slider properties (from / to /
// value / stepSize / snapMode / onMoved / hovered / WheelHandler / ToolTip)
// pass straight through — callers only change `Slider {` → `LyraSlider {`.
//
// Look: thin dark groove with a cyan-filled track up to a compact round
// handle (Lyra's cyan accent #50d0ff, dark surfaces).
import QtQuick
import QtQuick.Controls

Slider {
    id: control
    implicitHeight: 20

    // White track with a subtle light-grey fill up to the handle.
    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 4
        radius: 2
        color: control.enabled ? "#ffffff" : "#c8ccd0"
        border.color: "#8fa6b8"
        border.width: 1
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: parent.radius
            color: control.enabled ? "#b9c6d0" : "#c8ccd0"
        }
    }

    // Compact round handle — fixed 14 px so the style can't resize it.
    // A tad darker than the panel accent so it reads on the white track.
    handle: Rectangle {
        x: control.leftPadding
           + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        implicitWidth: 14
        implicitHeight: 14
        radius: 7
        color: !control.enabled ? "#5a6670"
             : control.pressed  ? "#1f88b0"
             : control.hovered  ? "#369fc6"
             :                     "#2f93bd"
        border.color: "#0a2530"
        border.width: 1
    }
}

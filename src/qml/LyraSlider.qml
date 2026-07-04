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

    // Filled track (left edge → handle) over a dark groove.
    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 4
        radius: 2
        color: "#101820"
        border.color: "#2a3a4a"
        border.width: 1
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: parent.radius
            color: control.enabled ? "#2a5a6a" : "#22303a"
        }
    }

    // Compact round handle — fixed 14 px so the style can't resize it.
    handle: Rectangle {
        x: control.leftPadding
           + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        implicitWidth: 14
        implicitHeight: 14
        radius: 7
        color: !control.enabled ? "#3a4650"
             : control.pressed  ? "#8fe0ff"
             : control.hovered  ? "#7ad8ff"
             :                     "#50d0ff"
        border.color: "#0a2530"
        border.width: 1
    }
}

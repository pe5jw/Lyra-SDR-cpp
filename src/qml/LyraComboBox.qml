// Lyra — shared themed drop-down picker.
//
// Drop-in replacement for a stock Controls `ComboBox` that draws its own
// field, chevron and popup at FIXED metrics, so it renders identically on
// any Qt Quick Controls style and never balloons/clips a panel.  All the
// usual ComboBox properties (model / currentIndex / onActivated / textRole /
// displayText) pass through — callers only change `ComboBox {` → `LyraComboBox {`.
//
// Look: compact dark field (#161e28), light text, cyan chevron + cyan focus
// border, dark popup list with a cyan highlight.
import QtQuick
import QtQuick.Controls

ComboBox {
    id: control
    implicitHeight: 24
    font.pixelSize: 12

    // Selected-value text.  Leaves room on the right for the chevron.
    contentItem: Text {
        leftPadding: 8
        rightPadding: 20
        text: control.displayText
        font: control.font
        color: control.enabled ? "#cdd9e5" : "#5a6670"
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    // Down chevron.
    indicator: Text {
        x: control.width - width - 6
        y: control.topPadding + (control.availableHeight - height) / 2
        text: "▾"
        font.pixelSize: 11
        color: control.enabled ? "#50d0ff" : "#5a6670"
    }

    background: Rectangle {
        implicitWidth: 64
        color: control.enabled ? "#161e28" : "#12171d"
        border.color: control.activeFocus ? "#50d0ff"
                    : control.hovered      ? "#3a5a6a"
                    :                        "#2a3a4a"
        border.width: 1
        radius: 3
    }

    // Dropdown rows.
    delegate: ItemDelegate {
        width: ListView.view.width
        height: 24
        contentItem: Text {
            text: control.textRole
                  ? (Array.isArray(control.model)
                        ? modelData[control.textRole]
                        : model[control.textRole])
                  : modelData
            font: control.font
            color: "#cdd9e5"
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        highlighted: control.highlightedIndex === index
        background: Rectangle {
            color: highlighted ? "#1f4655" : "transparent"
        }
    }

    popup: Popup {
        y: control.height
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 2, 260)
        padding: 1
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        background: Rectangle {
            color: "#12181f"
            border.color: "#2a3a4a"
            border.width: 1
            radius: 3
        }
    }
}

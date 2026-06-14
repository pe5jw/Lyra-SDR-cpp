// Lyra — front-facing TX/RX Profile quick-recall dock (#55).
//
// A thin convenience surface: a dropdown to recall a saved profile, a
// Save button (overwrite the active profile), and a "● modified" dot
// that lights when the live chain differs from the active profile.
// Create / rename / delete / set-default + the per-mode auto-recall
// table live in Settings → Profiles (#49) — this dock is recall + Save.
//
// Context property Profiles (ProfileManager) is provided by the host.
// Manual select only; the only automatic recall is the per-mode
// binding configured in Settings.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitHeight: 44
    implicitWidth: 360
    color: "#101820"
    border.color: "#2a4a5a"

    readonly property color cAccent: "#00e5ff"
    readonly property color cText:   "#cdd9e5"
    readonly property color cMuted:  "#8a9aac"
    readonly property color cOn:     "#ff9a3c"   // "modified" orange

    RowLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        Label { text: qsTr("Profile"); color: root.cAccent; font.bold: true }

        ComboBox {
            id: profCombo
            Layout.fillWidth: true
            implicitHeight: 26
            font.pixelSize: 12
            model: Profiles.names
            displayText: count > 0 ? currentText : qsTr("(no profiles)")
            onActivated: (i) => Profiles.load(textAt(i))
            function syncToActive() {
                var i = Profiles.names.indexOf(Profiles.activeName)
                currentIndex = i >= 0 ? i
                               : (Profiles.names.length > 0 ? 0 : -1)
            }
            Component.onCompleted: syncToActive()
            Connections {
                target: Profiles
                function onActiveChanged() { profCombo.syncToActive() }
                function onNamesChanged()  { profCombo.syncToActive() }
            }
            ToolTip.text: qsTr("Recall a saved TX/RX profile. Create / rename / "
                + "delete and set the default in Settings → Profiles.")
            ToolTip.visible: hovered; ToolTip.delay: 400
        }

        // ● modified — orange when the live chain differs from the active
        // profile (i.e. there's something to Save).
        Rectangle {
            width: 12; height: 12; radius: 6
            visible: Profiles.activeName.length > 0
            color: Profiles.modified ? root.cOn : "#26323c"
            border.width: 1
            border.color: Profiles.modified ? "#ffb060" : "#33424e"
            ToolTip.text: Profiles.modified
                ? qsTr("Modified — live settings differ from “%1”. Save to keep them.")
                    .arg(Profiles.activeName)
                : qsTr("Saved — live settings match the active profile.")
            ToolTip.visible: dotMa.containsMouse
            MouseArea { id: dotMa; anchors.fill: parent; hoverEnabled: true }
        }

        // Save — always available.  Pops a small "overwrite the active
        // profile, or save these settings as a NEW one" chooser, so the
        // operator can build a profile for a just-set mode right here
        // without opening Settings (the only place "Save As" used to
        // live).  Uses a top-level-window popup because a plain QML Popup
        // is clipped to this short dock (same idiom as AudioPanel).
        Button {
            id: saveBtn
            text: qsTr("Save")
            implicitHeight: 26; implicitWidth: 56
            font.pixelSize: 12
            onClicked: { newName.text = ""; savePop.open() }
            ToolTip.text: qsTr("Save the current live settings — overwrite the "
                + "active profile, or create a new one.")
            ToolTip.visible: hovered && !savePop.opened; ToolTip.delay: 400

            Popup {
                id: savePop
                popupType: Popup.Window
                y: -implicitHeight - 6           // pop ABOVE (dock sits low)
                x: saveBtn.width - width         // right-align to the button
                width: 260; padding: 12
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                onOpened: newName.forceActiveFocus()
                background: Rectangle { color: "#0c141c"; radius: 6
                                        border.color: root.cAccent }
                contentItem: ColumnLayout {
                    spacing: 8
                    Label { text: qsTr("Save profile"); color: root.cAccent
                            font.bold: true; font.pixelSize: 12 }

                    // Overwrite the active profile (only when one exists).
                    Button {
                        Layout.fillWidth: true
                        visible: Profiles.activeName.length > 0
                        text: qsTr("Overwrite: %1").arg(Profiles.activeName)
                        onClicked: { Profiles.saveActive(); savePop.close() }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1
                                color: "#2a3a4a"
                                visible: Profiles.activeName.length > 0 }

                    Label { text: qsTr("Save as a new profile:"); color: root.cText
                            font.pixelSize: 11 }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        TextField {
                            id: newName
                            Layout.fillWidth: true
                            placeholderText: qsTr("New profile name")
                            onAccepted: if (text.trim().length > 0) {
                                Profiles.saveAs(text.trim()); savePop.close() }
                        }
                        Button {
                            text: qsTr("Save")
                            enabled: newName.text.trim().length > 0
                            onClicked: { Profiles.saveAs(newName.text.trim())
                                         savePop.close() }
                        }
                    }
                    Button { Layout.fillWidth: true; text: qsTr("Cancel")
                             onClicked: savePop.close() }
                }
            }
        }
    }
}

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

        Button {
            text: qsTr("Save")
            implicitHeight: 26; implicitWidth: 56
            font.pixelSize: 12
            enabled: Profiles.activeName.length > 0
            opacity: enabled ? 1.0 : 0.45
            onClicked: Profiles.saveActive()
            ToolTip.text: qsTr("Overwrite the active profile with the current live "
                + "settings. “Save As…” (a new profile) is in Settings → Profiles.")
            ToolTip.visible: hovered; ToolTip.delay: 400
        }
    }
}

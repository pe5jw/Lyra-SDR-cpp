// Lyra — Tuner panel: native manual-ATU memory (the "Tuner Reminder" idea,
// built in).  Tracks the live dial against the active antenna's stored
// Input / Output / Inductor settings and shows the matching — or nearest —
// point, with live colour-coded SWR embedded.  Collapsed = basics only;
// expanded = the full editable table.
//
// Context properties: Tuner (TunerMemory), Stream (fwd/rev power for SWR),
// Prefs (tooltips toggle).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitWidth: 360
    implicitHeight: col.implicitHeight + 16
    color: "#101820"
    border.color: "#2a4a5a"

    readonly property color cAccent: "#00e5ff"
    readonly property color cText:   "#cdd9e5"
    readonly property color cMuted:  "#8a9aac"
    readonly property color cDim:    "#5f7186"
    readonly property color cGreen:  "#34c759"
    readonly property color cAmber:  "#ff9a3c"
    readonly property color cRed:    "#ff4136"

    // Live SWR from forward / reflected power (only meaningful while keyed).
    // Re-evaluates as Stream's power readings change.  −1 = no reading.
    readonly property real swr: {
        var pf = Stream.fwdPowerW, pr = Stream.revPowerW
        if (pf <= 0.05) return -1
        var r = Math.sqrt(Math.max(0, pr) / pf)
        return r >= 0.999 ? 99.9 : (1 + r) / (1 - r)
    }
    function swrColor(s) {
        if (s < 0) return cMuted
        if (s <= 1.5) return cGreen
        if (s <= 2.4) return cAmber
        return cRed
    }

    // Match-state colour: green when on a stored point, amber when showing a
    // nearest, dim when the antenna has nothing stored yet.
    readonly property color matchCol: !Tuner.matchValid ? cDim
                                      : (Tuner.matchExact ? cGreen : cAmber)

    // Store-a-new-point editor state.
    property bool editingNew: false
    // Edit-existing toggle (✎): rows become editable + deletable.
    property bool editRows: false

    function fmtDelta(hz) {
        var k = hz / 1000
        return (k >= 0 ? "+" : "") + k.toFixed(1) + " kHz"
    }

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // ── Header ──
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label { text: qsTr("TUNER"); color: root.cAccent; font.bold: true
                    font.letterSpacing: 1 }
            Label { text: qsTr("manual ATU memory"); color: root.cMuted
                    font.pixelSize: 11; Layout.fillWidth: true }
            // live SWR pill
            Rectangle {
                implicitHeight: 20; implicitWidth: swrLbl.implicitWidth + 16
                radius: 5; color: "#0d141b"
                border.color: root.swrColor(root.swr)
                Label {
                    id: swrLbl; anchors.centerIn: parent
                    text: root.swr < 0 ? qsTr("SWR —")
                          : qsTr("SWR ") + root.swr.toFixed(1) + ":1"
                    color: root.swrColor(root.swr); font.pixelSize: 11; font.bold: true
                }
            }
            // edit-existing toggle (hidden when collapsed)
            ToolButton {
                visible: !Tuner.collapsed
                checkable: true; checked: root.editRows
                onToggled: root.editRows = checked
                implicitWidth: 26; implicitHeight: 22
                contentItem: Label { text: "✎"; anchors.centerIn: parent
                    color: root.editRows ? root.cAccent : root.cMuted }
                ToolTip.text: qsTr("Edit / delete stored points")
                ToolTip.visible: hovered && Prefs.tooltipsEnabled; ToolTip.delay: 500
            }
            // collapse / expand
            ToolButton {
                onClicked: Tuner.collapsed = !Tuner.collapsed
                implicitWidth: 26; implicitHeight: 22
                contentItem: Label {
                    text: Tuner.collapsed ? "▸" : "▾"
                    anchors.centerIn: parent; color: root.cMuted }
                ToolTip.text: Tuner.collapsed ? qsTr("Expand") : qsTr("Collapse to basics")
                ToolTip.visible: hovered && Prefs.tooltipsEnabled; ToolTip.delay: 500
            }
        }

        // ── Antenna chips (expanded only) ──
        RowLayout {
            Layout.fillWidth: true
            visible: !Tuner.collapsed
            spacing: 6
            Label { text: qsTr("antenna"); color: root.cMuted; font.pixelSize: 11 }
            Repeater {
                model: Tuner.antennaNames
                delegate: Rectangle {
                    required property int index
                    required property string modelData
                    readonly property bool sel: Tuner.activeAntenna === index
                    radius: 7; implicitHeight: 24
                    implicitWidth: antLbl.implicitWidth + 22
                    color: sel ? "#10323a" : "#161e28"
                    border.color: sel ? root.cAccent : "#2a3a4a"
                    Label { id: antLbl; anchors.centerIn: parent; text: parent.modelData
                            color: parent.sel ? root.cAccent : root.cText; font.pixelSize: 13 }
                    MouseArea { anchors.fill: parent
                        onClicked: Tuner.activeAntenna = parent.index }
                }
            }
            Item { Layout.fillWidth: true }
        }

        // ── Context line: band + freq + match badge ──
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label { text: Tuner.currentBand !== "" ? Tuner.currentBand : qsTr("—")
                    color: "#efb340"; font.bold: true; font.pixelSize: 13 }
            Label { text: Tuner.currentFreqText; color: root.cText
                    font.family: "Consolas"; font.pixelSize: 18 }
            Item { Layout.fillWidth: true }
            RowLayout {
                spacing: 4
                visible: Tuner.matchValid
                Rectangle { width: 8; height: 8; radius: 4; color: root.matchCol }
                Label {
                    text: Tuner.matchExact ? qsTr("exact match")
                          : qsTr("nearest ") + root.fmtDelta(Tuner.matchDeltaHz)
                    color: root.matchCol; font.pixelSize: 11 }
            }
        }

        // ── Match tiles: Input / Output / Inductor ──
        GridLayout {
            Layout.fillWidth: true
            columns: 3; columnSpacing: 8; rowSpacing: 0
            Repeater {
                model: [
                    { k: qsTr("INPUT"),    v: Tuner.matchInput },
                    { k: qsTr("OUTPUT"),   v: Tuner.matchOutput },
                    { k: qsTr("INDUCTOR"), v: Tuner.matchInductor }
                ]
                delegate: Rectangle {
                    id: tile
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 52; radius: 9
                    color: "#0d141b"
                    border.color: Qt.rgba(root.matchCol.r, root.matchCol.g, root.matchCol.b, 0.55)
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 8; spacing: 2
                        Label { text: tile.modelData.k; color: root.cMuted
                                font.pixelSize: 10; font.letterSpacing: 1 }
                        Label {
                            text: (tile.modelData.v && tile.modelData.v.length > 0)
                                  ? tile.modelData.v : "—"
                            color: Tuner.matchValid ? root.cText : root.cDim
                            font.family: "Consolas"; font.pixelSize: 22 }
                    }
                }
            }
        }

        // ── Nearest / no-match banner (amber) ──
        Rectangle {
            Layout.fillWidth: true
            visible: !root.editingNew && (!Tuner.matchValid || !Tuner.matchExact)
            implicitHeight: nrCol.implicitHeight + 14
            radius: 9; color: "#241a0c"; border.color: "#6e4f1c"
            RowLayout {
                id: nrCol
                anchors.fill: parent; anchors.margins: 7; spacing: 8
                Label { text: "⚠"; color: root.cAmber; font.pixelSize: 16 }
                Label {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: 11
                    text: !Tuner.matchValid
                          ? qsTr("Nothing stored for this antenna yet — store this frequency.")
                          : qsTr("No exact match — showing nearest (") +
                            Tuner.fmtFreq(Tuner.matchFreqHz) + qsTr(", ") +
                            root.fmtDelta(Tuner.matchDeltaHz) + qsTr(")")
                    color: root.cText }
                Button {
                    text: qsTr("Store this freq")
                    implicitHeight: 24; font.pixelSize: 11
                    onClicked: {
                        inF.text = ""; outF.text = ""; indF.text = ""; noteF.text = ""
                        root.editingNew = true
                    }
                }
            }
        }

        // ── New-point editor (Store → fill → Save) ──
        Rectangle {
            Layout.fillWidth: true
            visible: root.editingNew
            implicitHeight: edCol.implicitHeight + 16
            radius: 9; color: "#0d141b"; border.color: root.cAccent
            ColumnLayout {
                id: edCol
                anchors.fill: parent; anchors.margins: 8; spacing: 6
                Label {
                    text: qsTr("New point · ") + Tuner.currentBand + " · " +
                          Tuner.currentFreqText
                    color: root.cAccent; font.pixelSize: 12 }
                GridLayout {
                    Layout.fillWidth: true; columns: 3; columnSpacing: 6; rowSpacing: 4
                    TextField { id: inF;  placeholderText: qsTr("Input");    Layout.fillWidth: true }
                    TextField { id: outF; placeholderText: qsTr("Output");   Layout.fillWidth: true }
                    TextField { id: indF; placeholderText: qsTr("Inductor"); Layout.fillWidth: true }
                }
                TextField { id: noteF; placeholderText: qsTr("Note (optional)"); Layout.fillWidth: true }
                RowLayout {
                    Layout.fillWidth: true; spacing: 6
                    Item { Layout.fillWidth: true }
                    Button { text: qsTr("Cancel"); implicitHeight: 24; font.pixelSize: 11
                             onClicked: root.editingNew = false }
                    Button {
                        text: qsTr("Save"); implicitHeight: 24; font.pixelSize: 11
                        highlighted: true
                        onClicked: {
                            Tuner.storePoint(Tuner.currentFreqHz, inF.text, outF.text,
                                             indF.text, noteF.text)
                            root.editingNew = false
                        }
                    }
                }
            }
        }

        // ── Stored-points table (expanded only) ──
        ColumnLayout {
            Layout.fillWidth: true
            visible: !Tuner.collapsed
            spacing: 0
            RowLayout {
                Layout.fillWidth: true; spacing: 0
                Label { text: qsTr("Band"); color: root.cMuted; font.pixelSize: 11; Layout.preferredWidth: 46 }
                Label { text: qsTr("Freq");  color: root.cMuted; font.pixelSize: 11; Layout.fillWidth: true }
                Label { text: qsTr("In");    color: root.cMuted; font.pixelSize: 11; Layout.preferredWidth: 48 }
                Label { text: qsTr("Out");   color: root.cMuted; font.pixelSize: 11; Layout.preferredWidth: 48 }
                Label { text: qsTr("Ind");   color: root.cMuted; font.pixelSize: 11; Layout.preferredWidth: 48 }
                Item { Layout.preferredWidth: root.editRows ? 22 : 0 }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: "#22323e" }

            Repeater {
                model: Tuner.points
                delegate: Rectangle {
                    id: row
                    required property int index
                    required property var modelData
                    readonly property bool here: Tuner.matchIndex === index
                    Layout.fillWidth: true
                    implicitHeight: 26
                    color: here ? "#10323a" : "transparent"
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 0; spacing: 0
                        Label { text: row.modelData.band; color: row.here ? root.cAccent : root.cMuted
                                font.pixelSize: 12; Layout.preferredWidth: 46 }
                        Label { text: row.modelData.freqText
                                color: row.here ? root.cAccent : root.cText
                                font.family: "Consolas"; font.pixelSize: 12; Layout.fillWidth: true }
                        // read-only labels OR edit fields
                        Label { visible: !root.editRows; text: row.modelData.input || "—"
                                color: root.cText; font.family: "Consolas"; font.pixelSize: 12; Layout.preferredWidth: 48 }
                        Label { visible: !root.editRows; text: row.modelData.output || "—"
                                color: root.cText; font.family: "Consolas"; font.pixelSize: 12; Layout.preferredWidth: 48 }
                        Label { visible: !root.editRows; text: row.modelData.inductor || "—"
                                color: root.cText; font.family: "Consolas"; font.pixelSize: 12; Layout.preferredWidth: 48 }

                        TextField { id: reIn;  visible: root.editRows; text: row.modelData.input
                                    Layout.preferredWidth: 48; font.pixelSize: 12
                                    onEditingFinished: Tuner.updatePoint(row.index, reIn.text, reOut.text, reInd.text, row.modelData.note) }
                        TextField { id: reOut; visible: root.editRows; text: row.modelData.output
                                    Layout.preferredWidth: 48; font.pixelSize: 12
                                    onEditingFinished: Tuner.updatePoint(row.index, reIn.text, reOut.text, reInd.text, row.modelData.note) }
                        TextField { id: reInd; visible: root.editRows; text: row.modelData.inductor
                                    Layout.preferredWidth: 48; font.pixelSize: 12
                                    onEditingFinished: Tuner.updatePoint(row.index, reIn.text, reOut.text, reInd.text, row.modelData.note) }
                        ToolButton { visible: root.editRows; implicitWidth: 22; implicitHeight: 22
                                     contentItem: Label { text: "−"; anchors.centerIn: parent; color: root.cRed }
                                     onClicked: Tuner.deletePoint(row.index) }
                    }
                }
            }

            // Add-current-freq affordance.
            RowLayout {
                Layout.fillWidth: true; Layout.topMargin: 8; spacing: 8
                Button {
                    text: qsTr("Add current freq"); implicitHeight: 24; font.pixelSize: 11
                    onClicked: {
                        inF.text = ""; outF.text = ""; indF.text = ""; noteF.text = ""
                        root.editingNew = true
                    }
                }
                Label {
                    visible: root.editRows; color: root.cDim; font.pixelSize: 10
                    text: qsTr("edit a cell then click away to save · − deletes")
                    Layout.fillWidth: true }
            }
        }
    }
}

// Lyra — CW decoder dock panel (#173 CW-5b).
//
// Separate from the CW console (keyer/macros) — run the decoder alone, the
// keyer alone, or both.  RX-only: decoded text + adaptive WPM + AFC-lock
// readout + detector knobs + display (font/colour) controls.  Decoding runs
// ONLY in CWU/CWL (the tap is CW-mode-gated in WdspEngine); the AFC centre is
// bound to the unified CW pitch (no separate "tone" knob).
//
// Styled to match CwConsolePanel — hand-drawn chips + section dividers, not
// native controls.  Bindings: WdspEngine cwDecodeEnabled + setCwDecode* +
// the cwDecoded* signals; Stream.cwKeyerSpeedWpm for the opt-in TX-speed
// coupling; Prefs.cwDecodeColor / cwDecodeFontSize for the display prefs.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitWidth: 470
    implicitHeight: collapsed ? 40 : (body.implicitHeight + 12)
    color: "#0d141b"
    border.color: "#2a4a5a"

    readonly property color cAccent: "#00e5ff"
    readonly property color cText:   "#cdd9e5"
    readonly property color cMuted:  "#8a9aac"

    property bool   collapsed: false
    property string decodedText: ""
    property int    rxWpm: 0
    property bool   afcLocked: false
    property real   afcHz: 0
    property bool   matchTxSpeed: false

    // Detector knob state mirrored here (the engine has no getters for these).
    // Persisted via Prefs so the operator's tuned values recall on launch;
    // first-run defaults are Squelch ×1.9 / Threshold 75% / NB+DSP+AFC on.
    property bool afcOn: Prefs.cwDecodeAfc
    property bool nbOn: Prefs.cwDecodeNb
    property bool dspOn: Prefs.cwDecodeDsp
    property int  afcRangeHz: Prefs.cwDecodeAfcRange

    // Decoded-text colour presets (SDRLogger+-style choice).
    readonly property var colorChoices:
        ["#39ff14", "#ffbe5a", "#00e5ff", "#e6edf3", "#ff9a3c", "#ff6b6b"]

    readonly property bool cwActive: {
        var m = WdspEngine.mode.toUpperCase()
        return m === "CWU" || m === "CWL"
    }

    Component.onCompleted: {
        WdspEngine.setCwDecodeSquelch(squelch.value)
        WdspEngine.setCwDecodeThreshold(threshold.value / 100)
        WdspEngine.setCwDecodeAfcEnabled(root.afcOn)
        WdspEngine.setCwDecodeAfcRange(root.afcRangeHz)
        WdspEngine.setCwDecodeNoiseBlanker(root.nbOn)
        WdspEngine.setCwDecodeDspFilter(root.dspOn)
        WdspEngine.setCwDecodeTxWpm(Math.round(Stream.cwKeyerSpeedWpm))
    }

    function applyWpmToKeyer(w) {
        var c = Math.max(5, Math.min(60, Math.round(w)))
        if (Math.abs(c - Stream.cwKeyerSpeedWpm) < 2) return
        Stream.cwKeyerSpeedWpm = c
    }

    Connections {
        target: WdspEngine
        function onCwDecodedChar(ch) {
            root.decodedText += ch
            if (root.decodedText.length > 4000)
                root.decodedText = root.decodedText.slice(-3000)
        }
        function onCwRxWpmChanged(w) {
            root.rxWpm = w
            if (root.matchTxSpeed) root.applyWpmToKeyer(w)
        }
        function onCwAfcLockChanged(locked, hz) {
            root.afcLocked = locked
            root.afcHz = hz
        }
    }

    // ── Reusable hand-styled chip (lit = active), matching the console ──
    component ChipButton: Rectangle {
        id: chip
        property string label: ""
        property bool   lit: false
        property bool   chipEnabled: true
        signal clicked()
        implicitHeight: 26
        implicitWidth: chipTxt.implicitWidth + 22
        radius: 4
        opacity: chipEnabled ? 1.0 : 0.4
        color: chip.lit ? "#2e7d9a" : "#1c252b"
        border.color: chip.lit ? root.cAccent : "#3a4750"
        Text {
            id: chipTxt
            anchors.centerIn: parent
            text: chip.label
            color: chip.lit ? "#ffffff" : root.cMuted
            font.pixelSize: 12; font.bold: chip.lit
        }
        MouseArea {
            anchors.fill: parent
            enabled: chip.chipEnabled
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.clicked()
        }
    }

    // ── Reusable labelled divider, matching the console "My macros" rule ──
    component Divider: RowLayout {
        id: div
        property string label: ""
        Layout.fillWidth: true
        spacing: 8
        Rectangle { Layout.fillWidth: true; height: 1; color: "#243845" }
        Label { text: div.label; color: root.cMuted; font.pixelSize: 11 }
        Rectangle { Layout.fillWidth: true; height: 1; color: "#243845" }
    }

    ColumnLayout {
        id: body
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // ── Header ──────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Label { text: qsTr("CW Decoder"); color: root.cAccent
                    font.bold: true; font.pixelSize: 14 }
            Label {
                visible: WdspEngine.cwDecodeEnabled && !root.cwActive
                text: qsTr("• switch to CW to decode")
                color: "#e0a030"; font.pixelSize: 11
            }
            Item { Layout.fillWidth: true }
            Label {
                text: (root.rxWpm > 0 ? root.rxWpm : "—") + qsTr(" wpm")
                color: root.cText; font.family: "Consolas"; font.bold: true
                font.pixelSize: 12
            }
            Rectangle {
                implicitHeight: 20; radius: 4
                implicitWidth: afcLbl.implicitWidth + 14
                color: root.afcLocked ? "#10303a" : "transparent"
                border.color: root.afcLocked ? root.cAccent : "#3a4750"
                Text {
                    id: afcLbl; anchors.centerIn: parent
                    text: root.afcLocked
                          ? ("AFC " + Math.round(root.afcHz)) : qsTr("AFC —")
                    color: root.afcLocked ? root.cAccent : root.cMuted
                    font.family: "Consolas"; font.pixelSize: 11
                }
            }
            ChipButton {
                label: root.collapsed ? "▲" : "▼"
                onClicked: root.collapsed = !root.collapsed
            }
        }

        // ── Decode on/off + Clear ───────────────────────────────────────
        RowLayout {
            visible: !root.collapsed
            Layout.fillWidth: true
            spacing: 8
            ChipButton {
                label: WdspEngine.cwDecodeEnabled ? qsTr("Decoding") : qsTr("Decode OFF")
                lit: WdspEngine.cwDecodeEnabled
                onClicked: WdspEngine.cwDecodeEnabled = !WdspEngine.cwDecodeEnabled
            }
            Item { Layout.fillWidth: true }
            ChipButton {
                label: qsTr("Clear")
                onClicked: root.decodedText = ""
            }
        }

        // ── Decoded-text pane (operator font + colour) ──────────────────
        Rectangle {
            visible: !root.collapsed
            Layout.fillWidth: true
            Layout.preferredHeight: 132
            radius: 6
            color: "#070d12"
            border.color: "#1c2a36"
            ScrollView {
                anchors.fill: parent
                anchors.margins: 8
                clip: true
                TextArea {
                    id: decodeOut
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextArea.WrapAnywhere
                    text: root.decodedText
                    color: Prefs.cwDecodeColor
                    font.family: "Consolas"
                    font.pixelSize: Prefs.cwDecodeFontSize
                    background: null
                    onTextChanged: cursorPosition = length

                    // Double-click a word → grab it straight to His Call.
                    // (Drag-select still works for multi-word picks; tap-to-
                    // position is unaffected — TapHandler only claims taps.)
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onDoubleTapped: (pt) => {
                            decodeOut.cursorPosition =
                                decodeOut.positionAt(pt.position.x, pt.position.y)
                            decodeOut.selectWord()
                            var w = decodeOut.selectedText.trim()
                            if (w.length > 0) CwMacros.hisCall = w.toUpperCase()
                        }
                    }
                    // Right-click a word → menu (His Call / Name).
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: (pt) => {
                            if (decodeOut.selectedText.trim().length === 0) {
                                decodeOut.cursorPosition =
                                    decodeOut.positionAt(pt.position.x, pt.position.y)
                                decodeOut.selectWord()
                            }
                            grabMenu.popup()
                        }
                    }
                }
            }
        }

        // ── Grab a decoded selection into the CW console contact row ─────
        // Select the call (or name) in the decoded text, then push it to the
        // console so the {CALL}/{NAME} macros expand to this station.
        RowLayout {
            visible: !root.collapsed
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: qsTr("Double-click a call → His Call · right-click for menu:")
                color: root.cMuted; font.pixelSize: 11
            }
            ChipButton {
                label: qsTr("→ His Call")
                chipEnabled: decodeOut.selectedText.length > 0
                onClicked: CwMacros.hisCall = decodeOut.selectedText.trim().toUpperCase()
            }
            ChipButton {
                label: qsTr("→ Name")
                chipEnabled: decodeOut.selectedText.length > 0
                onClicked: CwMacros.opName = decodeOut.selectedText.trim()
            }
            Item { Layout.fillWidth: true }
        }

        // ── Detector ────────────────────────────────────────────────────
        Divider { label: qsTr("Detector") }

        RowLayout {
            visible: !root.collapsed
            Layout.fillWidth: true
            spacing: 8
            opacity: root.cwActive ? 1.0 : 0.6
            ChipButton {
                label: qsTr("AFC")
                lit: root.afcOn
                onClicked: { root.afcOn = !root.afcOn; Prefs.cwDecodeAfc = root.afcOn
                             WdspEngine.setCwDecodeAfcEnabled(root.afcOn) }
            }
            Repeater {
                model: [50, 100, 150, 200]
                delegate: ChipButton {
                    label: "±" + modelData
                    lit: root.afcRangeHz === modelData
                    chipEnabled: root.afcOn
                    onClicked: { root.afcRangeHz = modelData; Prefs.cwDecodeAfcRange = modelData
                                 WdspEngine.setCwDecodeAfcRange(modelData) }
                }
            }
            Item { Layout.fillWidth: true }
            ChipButton {
                label: qsTr("NB")
                lit: root.nbOn
                onClicked: { root.nbOn = !root.nbOn; Prefs.cwDecodeNb = root.nbOn
                             WdspEngine.setCwDecodeNoiseBlanker(root.nbOn) }
            }
            ChipButton {
                label: qsTr("DSP filter")
                lit: root.dspOn
                onClicked: { root.dspOn = !root.dspOn; Prefs.cwDecodeDsp = root.dspOn
                             WdspEngine.setCwDecodeDspFilter(root.dspOn) }
            }
        }

        // Squelch + Threshold side-by-side (shorter sliders, compact panel),
        // each with a starting-point hint underneath.
        RowLayout {
            visible: !root.collapsed
            Layout.fillWidth: true
            spacing: 16
            opacity: root.cwActive ? 1.0 : 0.6

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Squelch"); color: root.cText; font.pixelSize: 12 }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: "×" + squelch.value.toFixed(1)
                        color: root.cText; font.family: "Consolas"; font.pixelSize: 12
                    }
                }
                Slider {
                    id: squelch
                    Layout.fillWidth: true
                    from: 1.0; to: 3.0; stepSize: 0.1; value: Prefs.cwDecodeSquelch
                    onMoved: { Prefs.cwDecodeSquelch = value
                               WdspEngine.setCwDecodeSquelch(value) }
                }
                Label { text: qsTr("start ≈ ×1.9"); color: "#e0a030"
                        font.pixelSize: 12; font.bold: true }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Threshold"); color: root.cText; font.pixelSize: 12 }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: Math.round(threshold.value) + "%"
                        color: root.cText; font.family: "Consolas"; font.pixelSize: 12
                    }
                }
                Slider {
                    id: threshold
                    Layout.fillWidth: true
                    from: 15; to: 90; stepSize: 1; value: Prefs.cwDecodeThreshold
                    onMoved: { Prefs.cwDecodeThreshold = Math.round(value)
                               WdspEngine.setCwDecodeThreshold(value / 100) }
                }
                Label { text: qsTr("start 70–85%"); color: "#e0a030"
                        font.pixelSize: 12; font.bold: true }
            }
        }

        // ── Display (font size + decoded-text colour) ───────────────────
        Divider { label: qsTr("Display") }

        RowLayout {
            visible: !root.collapsed
            Layout.fillWidth: true
            spacing: 10
            Label { text: qsTr("Font"); color: root.cText; font.pixelSize: 12 }
            ChipButton {
                label: "−"
                onClicked: Prefs.cwDecodeFontSize = Prefs.cwDecodeFontSize - 1
            }
            Label {
                text: Prefs.cwDecodeFontSize + " px"
                color: root.cText; font.family: "Consolas"; font.pixelSize: 12
                Layout.preferredWidth: 48; horizontalAlignment: Text.AlignHCenter
            }
            ChipButton {
                label: "+"
                onClicked: Prefs.cwDecodeFontSize = Prefs.cwDecodeFontSize + 1
            }
            Item { Layout.preferredWidth: 12 }
            Label { text: qsTr("Colour"); color: root.cText; font.pixelSize: 12 }
            Repeater {
                model: root.colorChoices
                delegate: Rectangle {
                    width: 22; height: 22; radius: 4
                    color: modelData
                    border.width: Prefs.cwDecodeColor === modelData ? 2 : 1
                    border.color: Prefs.cwDecodeColor === modelData
                                  ? root.cAccent : "#3a4750"
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Prefs.cwDecodeColor = modelData
                    }
                }
            }
            Item { Layout.fillWidth: true }
        }

        // ── Keyer coupling ──────────────────────────────────────────────
        Divider { label: qsTr("Keyer") }

        RowLayout {
            visible: !root.collapsed
            Layout.fillWidth: true
            spacing: 10
            ChipButton {
                label: qsTr("Match TX speed to RX WPM")
                lit: root.matchTxSpeed
                onClicked: {
                    root.matchTxSpeed = !root.matchTxSpeed
                    if (root.matchTxSpeed && root.rxWpm > 0)
                        root.applyWpmToKeyer(root.rxWpm)
                }
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: root.matchTxSpeed
                text: qsTr("keyer → ") + Math.round(Stream.cwKeyerSpeedWpm) + qsTr(" wpm")
                color: root.cMuted; font.family: "Consolas"; font.pixelSize: 11
            }
        }

        Label {
            visible: !root.collapsed
            Layout.fillWidth: true
            text: qsTr("Tune the signal to your CW pitch on the panadapter — the "
                       + "decoder follows the pitch and AFC-tracks drift.")
            color: root.cMuted; font.pixelSize: 10; wrapMode: Text.WordWrap
        }
    }

    // Right-click grab menu (the word under the cursor was selected by the
    // RightButton TapHandler before this popped).
    Menu {
        id: grabMenu
        MenuItem {
            text: qsTr("→ His Call:  ") + decodeOut.selectedText.trim().toUpperCase()
            enabled: decodeOut.selectedText.trim().length > 0
            onTriggered: CwMacros.hisCall = decodeOut.selectedText.trim().toUpperCase()
        }
        MenuItem {
            text: qsTr("→ Name:  ") + decodeOut.selectedText.trim()
            enabled: decodeOut.selectedText.trim().length > 0
            onTriggered: CwMacros.opName = decodeOut.selectedText.trim()
        }
    }
}

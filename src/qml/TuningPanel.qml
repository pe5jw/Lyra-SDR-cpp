// Lyra — Tuning dock panel (RX1 / DDC0), VFO-cluster layout.
//
// Operator-curated layout (2026-06-20): each VFO is a self-contained
// CLUSTER — a big LED-style readout (FreqDisplay) you tune by clicking a
// digit + wheeling, with its Step + Mode combos directly UNDERNEATH it,
// and the Lyra logo centred between VFO A and the reserved VFO-B slot.
// Mode moved here from the (now "Filters") dock; the Prefs.mode →
// WdspEngine.mode binding came with it.  A controls row under the VFOs
// holds CW Pitch now and reserves space for SUB / 1→2 / 2→1 / ⇄ / RIT /
// XIT / CTCSS / repeater-offset — each rendered functional WHEN its
// feature lands (SPLIT, RIT/XIT, repeaters), never as a dead widget.
//
// VFO indication model:
//   • amber RX/TX text tag, upper-left of the digits = ROLE (which VFO
//     listens, which transmits) — amber lives ONLY in the tag, never
//     blended into the border (an amber→red border blend read pinkish).
//   • border = LIVE state: green = receiving, red = transmitting now
//     (Stream.moxActive, the wire-MOX truth), neutral gray = an armed-
//     but-not-keyed TX VFO (only meaningful once SPLIT lands).
//   • simplex (one VFO): the tag flips RX→TX and the border green→red
//     together on key.
//
// CW carrier convention (standard HF SDR practice): the LED shows the
// signal CARRIER (VFO); the hardware DDS is offset by ±pitch so the
// carrier lands in the pitch-centred filter.  VFO = DDS +
// WdspEngine.markerOffsetHz, so we display centerHz+offset and write
// (vfo − offset) to the wire.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LyraUI

Rectangle {
    id: root
    implicitHeight: 200
    implicitWidth: 680
    color: "#101820"
    border.color: "#2a4a5a"

    // Local mirror of the wire (DDS) freq — signal-driven (a direct
    // binding to Stream.rx1FreqHz is unreliable in this QQuickWidget).
    property int centerHz: 0
    // VFO B (split TX VFO) mirror — vfoBHz IS the carrier (the TX-NCO
    // offset is applied at push time), so display it directly.
    property int vfoBHz: 0
    Component.onCompleted: { centerHz = Stream.rx1FreqHz; vfoBHz = Stream.vfoBHz }
    Connections {
        target: Stream
        function onRx1FreqChanged() { root.centerHz = Stream.rx1FreqHz }
        function onVfoBHzChanged()  { root.vfoBHz   = Stream.vfoBHz }
    }

    // Mode picker moved here from the Filters dock — keep the engine in
    // sync with the operator's mode choice (was in ModeFilterPanel).
    Binding { target: WdspEngine; property: "mode"; value: Prefs.mode }

    // Indication palette (matches the locked VFO indication model).
    readonly property color cRx:   "#34c759"   // receiving (green border)
    readonly property color cTx:   "#ff4136"   // transmitting now (red border)
    readonly property color cArm:  "#3a4250"   // armed TX, not keyed (gray)
    readonly property color cRole: "#efb340"   // amber RX/TX role tag

    readonly property var modeList: ["LSB", "USB", "CWL", "CWU",
                                     "DSB", "AM", "FM", "DIGU", "DIGL"]
    readonly property bool cwMode: Prefs.mode === "CWU" || Prefs.mode === "CWL"

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 6
        anchors.bottomMargin: 10
        spacing: 14

        // ── VFO row: [VFO-A cluster + its Step/Mode]  [big logo]  [VFO-B] ──
        // Per-VFO Step+Mode sit DIRECTLY under each VFO (old-Lyra idiom);
        // the logo is large and centred between A and B's reserved slot.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            // VFO-A cluster — one bordered box wrapping the freq AND its
            // Step+Mode (mockup/old-Lyra idiom).  Fixed snug width so the
            // border outlines the cluster, not a giant block.
            Rectangle {
                id: vfoA
                Layout.preferredWidth: 360
                Layout.maximumWidth: 360
                Layout.preferredHeight: 108
                Layout.alignment: Qt.AlignVCenter
                color: "#0a0e12"
                radius: 6
                border.width: 2
                // VFO A is the RX.  Red only when transmitting on A —
                // i.e. keyed AND not split (in SPLIT, A stays the receiver
                // and VFO B carries the red TX border).
                border.color: (Stream.moxActive && !Stream.splitEnabled)
                              ? root.cTx : root.cRx

                // Amber role tag, upper-left — flips RX→TX on key (simplex).
                Label {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.leftMargin: 8
                    anchors.topMargin: 4
                    text: (Stream.moxActive && !Stream.splitEnabled)
                          ? qsTr("TX") : qsTr("RX")
                    color: root.cRole
                    font.bold: true
                    font.pixelSize: 12
                    z: 2
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    anchors.topMargin: 4
                    anchors.bottomMargin: 6
                    spacing: 10

                    // LED VFO readout (carrier) + typed-entry overlay.
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredHeight: 58

                        FreqDisplay {
                            id: led
                            anchors.fill: parent
                            // VFO (carrier) = DDS + CW marker offset.
                            freqHz: root.centerHz + WdspEngine.markerOffsetHz
                            externalStepHz: stepCombo.stepVals[stepCombo.currentIndex]
                            // Tune: convert carrier back to the DDS wire freq.
                            onFreqEdited: (hz) =>
                                Stream.setRx1FreqHz(hz - WdspEngine.markerOffsetHz)
                            onEditRequested: {
                                editField.text = (led.freqHz / 1.0e6).toFixed(6)
                                editField.visible = true
                                editField.selectAll()
                                editField.forceActiveFocus()
                            }
                        }
                        TextField {
                            id: editField
                            anchors.fill: parent
                            visible: false
                            horizontalAlignment: TextInput.AlignHCenter
                            verticalAlignment: TextInput.AlignVCenter
                            font.family: "Consolas"
                            font.pixelSize: 22
                            font.bold: true
                            color: "#ffb000"
                            background: Rectangle {
                                color: "#000000"
                                border.color: "#00d8ff"
                                border.width: 2
                                radius: 3
                            }
                            function commit() {
                                if (!visible) return
                                var hz = led.parseFreqInput(text)   // entered carrier
                                visible = false
                                if (hz >= 0)
                                    Stream.setRx1FreqHz(hz - WdspEngine.markerOffsetHz)
                            }
                            onAccepted: commit()
                            onActiveFocusChanged: if (!activeFocus) commit()
                            Keys.onEscapePressed: visible = false
                        }
                    }

                    // Step + Mode — VFO A's per-VFO controls, INSIDE the box,
                    // centred (balanced spacers each side).
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Item { Layout.fillWidth: true }
                        Label { text: qsTr("Step"); color: "#cccccc"; font.bold: true }
                        ComboBox {
                            id: stepCombo
                            Layout.preferredWidth: 78
                            property var stepVals: [1, 10, 100, 1000, 5000, 10000]
                            model: ["1 Hz", "10 Hz", "100 Hz", "1 kHz", "5 kHz", "10 kHz"]
                            currentIndex: 3   // 1 kHz default
                        }
                        Label { text: qsTr("Mode"); color: "#cccccc"; font.bold: true }
                        ComboBox {
                            id: modeCombo
                            Layout.preferredWidth: 78
                            model: root.modeList
                            currentIndex: Math.max(0, root.modeList.indexOf(Prefs.mode))
                            onActivated: Prefs.mode = root.modeList[currentIndex]
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            Item { Layout.fillWidth: true }   // balance: centres the logo

            Image {
                source: "qrc:/qt/qml/Lyra/src/assets/logo/lyra-icon-256.png"
                Layout.fillHeight: true
                Layout.preferredHeight: 132
                Layout.preferredWidth: height   // square, as tall as the row
                Layout.maximumHeight: 140
                Layout.alignment: Qt.AlignVCenter
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
            }

            Item { Layout.fillWidth: true }   // balance: VFO-B's reserved gap

            // VFO-B slot — empty (reserved, no dead widget) in simplex;
            // SPLIT fills it with the TX VFO cluster (no relayout).
            Item {
                id: vfoBSlot
                Layout.preferredWidth: 360
                Layout.maximumWidth: 360
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignVCenter

                // VFO-B cluster — the split TX VFO.  Shares VFO A's mode
                // (option a): TX uses Prefs.mode, so B shows freq + Step
                // only.  Border = armed-gray → red on key; amber TX tag.
                Rectangle {
                    id: vfoB
                    visible: Stream.splitEnabled
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 108
                    color: "#0a0e12"
                    radius: 6
                    border.width: 2
                    // Armed (gray) until keyed, then red (B is the TX VFO).
                    border.color: Stream.moxActive ? root.cTx : root.cArm

                    Label {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.leftMargin: 8
                        anchors.topMargin: 4
                        text: qsTr("TX")
                        color: root.cRole
                        font.bold: true
                        font.pixelSize: 12
                        z: 2
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        anchors.topMargin: 4
                        anchors.bottomMargin: 6
                        spacing: 10

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredHeight: 58

                            FreqDisplay {
                                id: ledB
                                anchors.fill: parent
                                // vfoBHz IS the carrier (TX-NCO offset
                                // applied at push), so show/tune it directly.
                                freqHz: root.vfoBHz
                                externalStepHz: stepComboB.stepVals[stepComboB.currentIndex]
                                onFreqEdited: (hz) => Stream.setVfoBHz(hz)
                                onEditRequested: {
                                    editFieldB.text = (ledB.freqHz / 1.0e6).toFixed(6)
                                    editFieldB.visible = true
                                    editFieldB.selectAll()
                                    editFieldB.forceActiveFocus()
                                }
                            }
                            TextField {
                                id: editFieldB
                                anchors.fill: parent
                                visible: false
                                horizontalAlignment: TextInput.AlignHCenter
                                verticalAlignment: TextInput.AlignVCenter
                                font.family: "Consolas"
                                font.pixelSize: 22
                                font.bold: true
                                color: "#ffb000"
                                background: Rectangle {
                                    color: "#000000"
                                    border.color: "#00d8ff"
                                    border.width: 2
                                    radius: 3
                                }
                                function commit() {
                                    if (!visible) return
                                    var hz = ledB.parseFreqInput(text)
                                    visible = false
                                    if (hz >= 0) Stream.setVfoBHz(hz)
                                }
                                onAccepted: commit()
                                onActiveFocusChanged: if (!activeFocus) commit()
                                Keys.onEscapePressed: visible = false
                            }
                        }

                        // VFO B's Step (mode is shared with VFO A).
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Item { Layout.fillWidth: true }
                            Label { text: qsTr("Step"); color: "#cccccc"; font.bold: true }
                            ComboBox {
                                id: stepComboB
                                Layout.preferredWidth: 78
                                property var stepVals: [1, 10, 100, 1000, 5000, 10000]
                                model: ["1 Hz", "10 Hz", "100 Hz", "1 kHz", "5 kHz", "10 kHz"]
                                currentIndex: 3
                            }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }
            }
        }

        // ── centred action row (under the VFOs) ─────────────────────────
        // SPLIT + VFO copy/swap + CW Pitch, centred.  Reserved still for
        // RIT / XIT / CTCSS / offset — each added functional WITH its
        // feature (RIT, repeaters), never as a dead widget.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }   // centre the action controls

            // SPLIT — TX on VFO B, RX on VFO A.  Lit cyan when on.
            Button {
                id: splitBtn
                checkable: true
                implicitHeight: 26
                implicitWidth: 64
                checked: Stream.splitEnabled
                onToggled: Stream.setSplitEnabled(checked)
                text: qsTr("SPLIT")
                font.bold: true
                font.pixelSize: 12
                background: Rectangle {
                    radius: 4
                    color: splitBtn.checked ? "#10323a" : "#161e28"
                    border.width: 2
                    border.color: splitBtn.checked ? "#00e5ff" : "#2a3a4a"
                }
                contentItem: Text {
                    text: splitBtn.text
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: splitBtn.checked ? "#00e5ff" : "#cdd9e5"
                    font: splitBtn.font
                }
                ToolTip.text: qsTr("SPLIT — receive on VFO A, transmit on VFO B "
                    + "(same band).  Set VFO B to your TX freq; key and the red "
                    + "TX border + panadapter marker move to B.")
                ToolTip.visible: hovered; ToolTip.delay: 600
            }

            // VFO copy / swap.  1→2 = A→B, 2→1 = B→A, ⇄ = swap.
            Button {
                implicitHeight: 26; implicitWidth: 40; font.pixelSize: 12
                text: qsTr("1→2")
                onClicked: Stream.setVfoBHz(Stream.rx1FreqHz)
                ToolTip.text: qsTr("Copy VFO A → VFO B"); ToolTip.visible: hovered; ToolTip.delay: 600
            }
            Button {
                implicitHeight: 26; implicitWidth: 40; font.pixelSize: 12
                text: qsTr("2→1")
                onClicked: Stream.setRx1FreqHz(Stream.vfoBHz)
                ToolTip.text: qsTr("Copy VFO B → VFO A"); ToolTip.visible: hovered; ToolTip.delay: 600
            }
            Button {
                implicitHeight: 26; implicitWidth: 36; font.pixelSize: 14
                text: qsTr("⇄")
                onClicked: {
                    var a = Stream.rx1FreqHz
                    var b = Stream.vfoBHz
                    Stream.setVfoBHz(a)
                    Stream.setRx1FreqHz(b)
                }
                ToolTip.text: qsTr("Swap VFO A ↔ VFO B"); ToolTip.visible: hovered; ToolTip.delay: 600
            }

            Label {
                text: qsTr("CW Pitch")
                color: "#cccccc"; font.bold: true
                visible: root.cwMode
            }
            SpinBox {
                id: pitchSpin
                visible: root.cwMode
                Layout.preferredWidth: 110
                from: 200; to: 1500; stepSize: 10
                value: WdspEngine.cwPitchHz
                onValueModified: WdspEngine.setCwPitchHz(value)
            }

            Item { Layout.fillWidth: true }
        }
    }
}

// Lyra — TX dock panel (front-facing TX controls).
//
// Operator-facing surface for the TX controls that get touched during
// a session: TX Drive % (tuned between QSOs) and MOX (the keying action
// itself).  PA Enable + safety timeout stay in Settings → Hardware →
// Transmit — PA Enable is the deliberate-arm safety gate, and the
// timeout is a set-once config; neither belongs on the operating-time
// front panel.
//
// Visual hierarchy:
//   left                centre                              right
//   [TX label]  [Drive ──●── 25 %]                              [ MOX ]
//
// The MOX button is the largest, sits on the right (away from the LED
// on the Tuning dock if they're docked side-by-side), and is lit red
// whenever the wire-MOX bit is high (Stream.moxActive — the post-TR-
// delay truth, NOT the click state — so it stays honest through the
// ~65 ms keydown TR window and through the space-bar momentary).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitHeight: 64
    implicitWidth: 720
    color: "#101820"
    border.color: "#2a4a5a"

    readonly property color cAccent:   "#00e5ff"
    readonly property color cText:     "#cdd9e5"
    readonly property color cMuted:    "#8a9aac"
    readonly property color cOn:       "#ff9a3c"   // tune-armed orange
    readonly property color cMox:      "#d11515"   // wire-MOX red
    readonly property color cMoxEdge:  "#ff8080"

    // ── Press-intent indicator (task #18) ────────────────────────────
    // Operator-flagged UX gap: a short MOX press (space-bar tap shorter
    // than the ~65 ms TR delay) never lit the button red because the
    // wire-MOX FSM cancelled mid-mox_delay and moxActive never went
    // true — operator wondered "did it key?".  HL2Stream now fires
    // moxIntentPulse() on EVERY requestMox(true) call (click, TUN, or
    // space-bar press, regardless of FSM outcome).  We latch
    // pressIntent for ~220 ms so even a 5 ms tap shows an orange flash,
    // giving the operator instant feedback that their press registered.
    // Wire-MOX-red overrides pressIntent-orange the moment moxActive
    // goes true — the FSM outcome takes visual priority.
    property bool pressIntent: false

    // ── Wire-truth atomics from HL2Stream ────────────────────────────
    // txDriveLevel is raw 0..255; UI works in 0..100 %.  Math:
    //   pct = round(raw * 100 / 255)
    //   raw = round(pct * 255 / 100)
    // The setter clamps + persists + emits txDriveLevelChanged, so the
    // round-trip stays stable (a 25 % click → raw 64 → reads back 25 %).
    function rawToPct(raw) { return Math.round(raw * 100 / 255) }
    function pctToRaw(pct) { return Math.round(pct * 255 / 100) }

    // Press-intent decay timer (#18 — see pressIntent property above).
    Timer {
        id: pressIntentTimer
        interval: 220
        repeat: false
        onTriggered: root.pressIntent = false
    }
    Connections {
        target: Stream
        function onMoxIntentPulse() {
            // restart() (not start()) so back-to-back re-keys extend
            // the visible orange rather than truncating to whatever
            // remained from the previous pulse.
            root.pressIntent = true
            pressIntentTimer.restart()
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        spacing: 12

        Label {
            text: qsTr("TX")
            color: root.cAccent
            font.bold: true
            font.pixelSize: 14
        }

        // ── TX Drive % — operator tunes between QSOs ────────────────
        Label { text: qsTr("Drive"); color: root.cMuted }
        Slider {
            id: driveSlider
            Layout.preferredWidth: 200
            from: 0; to: 100; stepSize: 1; snapMode: Slider.SnapAlways
            value: root.rawToPct(Stream.txDriveLevel)
            onMoved: Stream.setTxDriveLevel(root.pctToRaw(value))
            WheelHandler {
                onWheel: (ev) => {
                    var step = (ev.modifiers & Qt.ShiftModifier) ? 5 : 1
                    var nv = driveSlider.value + (ev.angleDelta.y > 0 ? step : -step)
                    nv = Math.max(0, Math.min(100, nv))
                    Stream.setTxDriveLevel(root.pctToRaw(nv))
                }
            }
            ToolTip.text: qsTr("How hard the HL2 drives its PA.  0 % = no carrier "
                + "even with PA + MOX.  Start LOW (5–10 %) on a dummy load; raise "
                + "gradually while watching the watt-meter.  Wheel adjusts (Shift = 5 %).")
            // Long delay so the tip can't intercept an operator click/drag —
            // hover-with-intent (≥1.5 s) still gets it, but instant flicks +
            // wheel adjustments aren't fighting a popup that's already up.
            ToolTip.delay: 1500
            ToolTip.visible: hovered && !pressed
        }
        Label {
            // Live readback from the wire atomic — so an external
            // change (CAT, persistence reload) updates the readout.
            text: root.rawToPct(Stream.txDriveLevel) + qsTr(" %")
            color: cText
            font.family: "Consolas"
            font.bold: true
            Layout.preferredWidth: 44
        }

        Item { Layout.fillWidth: true }   // pushes TUN + MOX to the right

        // ── TUN — armed-tune button (carrier @ TX freq + 1 kHz) ─────
        // Clicking arms the host-streamed 1 kHz tone AND requests MOX
        // in a single gesture (the operator's "press to tune" pattern).
        // Clicking again releases MOX through the normal FSM keyup —
        // the moxActiveChanged(false) edge auto-disarms the tune (via
        // HL2Stream's self-wired safety) so the visual goes back
        // gray-orange→gray.  The button visual tracks Stream.tuneEnabled
        // (wire truth) so the auto-clear is reflected immediately.
        //
        // Useful only when PA is enabled (Settings → Hardware → Transmit)
        // AND TX Drive > 0 % — otherwise the carrier exists in the EP2
        // I/Q stream but the gateware DAC scales it to inaudible.
        Button {
            id: tunBtn
            checkable: true
            implicitWidth: 76
            implicitHeight: 44
            text: qsTr("TUN")
            font.bold: true
            font.pixelSize: 16
            checked: Stream.tuneEnabled
            onClicked: {
                if (checked) {
                    // Arming: set tone first so the very first EP2 frame
                    // after wire-MOX rises already carries the carrier.
                    Stream.setTuneEnabled(true)
                    Stream.requestMox(true)
                } else {
                    // Disarming: release MOX through the FSM; the
                    // moxActiveChanged(false) edge auto-clears tune.
                    Stream.requestMox(false)
                }
            }
            background: Rectangle {
                radius: 4
                color: Stream.tuneEnabled ? "#3a2a14" : "#1f2a35"
                border.color: Stream.tuneEnabled ? root.cOn : "#3a5060"
                border.width: 2
            }
            contentItem: Text {
                text: tunBtn.text
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: Stream.tuneEnabled ? root.cOn : root.cText
                font: tunBtn.font
            }
            ToolTip.text: qsTr("Tune carrier — emits a 1 kHz complex tone in the "
                + "TX I/Q stream and keys MOX.  Carrier power = TX Drive %.  "
                + "Auto-disarms when MOX clears for any reason (release, "
                + "safety timer, etc.).  Set Drive low for first session.")
            // Tooltip was intercepting clicks on the most-pressed button on
            // the panel — long delay + hide-on-press so a fast tune-arm
            // gesture isn't fighting a popup that's already up.
            ToolTip.delay: 1500
            ToolTip.visible: hovered && !pressed
        }

        // ── MOX — the keying button, big + lit red on wire truth ────
        // Funnels through Stream.requestMox (the FSM single funnel —
        // same path as the space-bar momentary in MainWindow).  The
        // lit state tracks Stream.moxActive (wire truth, post-TR-delay
        // settle), NOT the click checked-state, so the visual stays
        // honest through the ~65 ms keydown window.
        Button {
            id: moxBtn
            checkable: true
            implicitWidth: 96
            implicitHeight: 44
            text: qsTr("MOX")
            font.bold: true
            font.pixelSize: 18
            checked: Stream.moxActive
            onClicked: Stream.requestMox(checked)
            background: Rectangle {
                // Three-way state: moxActive (wire MOX live) → red;
                // pressIntent (operator pressed within last ~220 ms,
                // wire MOX not yet settled) → tune-armed-style orange-
                // tinted dark fill; otherwise gray.  Matches the TUN
                // armed visual idiom so operators read both buttons
                // the same way.
                color: Stream.moxActive ? root.cMox
                     : root.pressIntent ? "#3a2a14"
                     : "#1f2a35"
                border.color: Stream.moxActive ? root.cMoxEdge
                            : root.pressIntent ? root.cOn
                            : "#3a5060"
                border.width: 2
                radius: 5
            }
            contentItem: Text {
                text: moxBtn.text
                color: Stream.moxActive ? "#ffffff"
                     : root.pressIntent ? root.cOn
                     : root.cText
                font: moxBtn.font
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            ToolTip.text: qsTr("Key the radio (MOX).  Goes red the instant the "
                + "wire-MOX bit settles after the TR-delay.  Space-bar momentary "
                + "(when no text widget has focus) routes through the same FSM.")
            // Operator-reported 2026-05-29: a tooltip that pops the instant
            // the cursor lands on MOX intercepts the click — you end up
            // hunting for the bottom edge of the button to dodge it.  Long
            // delay + hide-on-press means hover-with-intent still gets the
            // hint, but key-the-radio gestures are instant.
            ToolTip.delay: 1500
            ToolTip.visible: hovered && !pressed
        }
    }
}

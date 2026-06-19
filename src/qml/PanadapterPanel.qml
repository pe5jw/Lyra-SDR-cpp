// Lyra — Panadapter dock panel (spectrum over waterfall).
//
// Hosted by a QQuickWidget inside a QDockWidget (see mainwindow.cpp).
// Root is a plain Item so the QQuickWidget can host it.  A vertical
// SplitView stacks the Vulkan scene-graph Panadapter (top) over the
// Waterfall (bottom) — both share the exact same frequency extent and
// dB window, so the spectrum trace and its rolling history read as one
// instrument.  The spectrum carries the dBm scale (both edges) + the
// 3-zone right-edge dB drag + the shared frequency scale (drawn at the
// boundary so it labels both); the waterfall is GPU-textured and
// palette-coloured.  Display options come from the shared Prefs object
// (Settings -> Visuals).

import QtQuick
import QtQuick.Controls
import LyraUI

Item {
    id: root
    implicitWidth: 600
    implicitHeight: 360

    // RX1 centre frequency.  The panadapter is always centred on the
    // tuned DDC freq, so this drives the frequency scale AND click-to-
    // tune.  Held in a LOCAL property updated via the rx1FreqChanged
    // SIGNAL — a direct binding to Stream.rx1FreqHz is unreliable in
    // this QQuickWidget setup (same lesson as the Band panel), which
    // would leave the freq labels stale after a tune.
    property int centerHz: 0
    Component.onCompleted: {
        centerHz = Stream.rx1FreqHz
        effMin = pan.effDbMin
        effMax = pan.effDbMax
    }
    Connections {
        target: Stream
        function onRx1FreqChanged() { root.centerHz = Stream.rx1FreqHz }
    }

    // Display panel "Clear" button → flush the peak-hold buffer (the
    // panadapter lives in this dock, the button in another).
    Connections {
        target: Prefs
        function onPeakClearRequested() { pan.clearPeaks() }
    }

    // Drive the engine's zoom from the shared Prefs (Display panel).  The
    // engine crops the centre 1/zoom of the full-resolution spectrum;
    // WdspEngine.spanHz reports inRate/zoom so the frequency scale,
    // drag-pan and cursor readout all track the zoom for free.
    Binding {
        target: WdspEngine
        property: "zoom"
        value: Prefs.zoom
    }

    // Effective dB range the panadapter is actually rendering with
    // (auto-fit when on, else the manual dbMin/dbMax).  Mirrored from
    // the panadapter's effRangeChanged SIGNAL into LOCAL properties so
    // the dB-scale labels track it reliably (same proven pattern as
    // centerHz — avoids the context-property binding flakiness).
    property real effMin: -130
    property real effMax: -20
    Connections {
        target: pan
        function onEffRangeChanged() {
            root.effMin = pan.effDbMin
            root.effMax = pan.effDbMax
        }
    }

    // dB labels at the 8 horizontal grid divisions: dbMax at the top
    // down to dbMin at the bottom.  Dim, outlined, tracks Prefs live.
    component DbLabels : Repeater {
        id: rep
        property bool rightSide: false
        property Item area      // the spectrum area these label
        property real dMax: 0   // effective ceiling (auto or manual)
        property real dMin: 0   // effective floor
        model: 9   // 0..8 -> top edge .. bottom edge
        delegate: Text {
            required property int index
            readonly property real frac: index / 8.0
            x: rep.rightSide ? (rep.area.width - width - 5) : 5
            y: Math.max(0, Math.min(rep.area.height - height,
                                    frac * rep.area.height - height / 2))
            text: Math.round(rep.dMax - frac * (rep.dMax - rep.dMin))
            color: "#8fa6ba"
            font.pixelSize: 13
            font.family: "Consolas"
            style: Text.Outline
            styleColor: "#80000000"
        }
    }

    SplitView {
        id: splitV
        anchors.fill: parent
        orientation: Qt.Vertical

        // The divider position is NOT covered by QMainWindow.saveState()
        // (it lives inside this dock's QML content), so persist it via
        // Prefs (→ QSettings) using the Qt-intended SplitView
        // saveState()/restoreState().  This ties into session restore
        // AND the View → Save/Restore/Reset layout actions (which write
        // Prefs.panadapterSplit from C++).  `_applying` guards the
        // save↔notify feedback loop.  Same approach old Lyra used for
        // its central spectrum/waterfall splitter.
        property bool _applying: false
        function applyFromPrefs() {
            _applying = true
            var st = Prefs.panadapterSplit
            if (st === undefined || st === null || st === "")
                wf.SplitView.preferredHeight = splitV.height * 0.4   // factory 60/40
            else
                splitV.restoreState(st)
            _applying = false
        }
        Component.onCompleted: applyFromPrefs()
        onResizingChanged: {
            // Persist only when the operator finishes dragging the handle.
            if (!resizing && !_applying) {
                _applying = true
                Prefs.panadapterSplit = splitV.saveState()
                _applying = false
            }
        }
        Connections {
            target: Prefs
            // External change (Restore/Reset from the View menu) re-applies.
            function onPanadapterSplitChanged() {
                if (!splitV._applying) splitV.applyFromPrefs()
            }
        }

        // ===== Spectrum area (panadapter + overlays) =====
        Item {
            id: spectrumArea
            SplitView.fillHeight: true
            SplitView.minimumHeight: 90

            Panadapter {
                id: pan
                anchors.fill: parent
                engine: WdspEngine
                gridLevel: Prefs.gridLevel
                glassSheen: Prefs.glassSheen
                targetFps: Prefs.targetFps
                // Auto-fit when enabled; dbMin/dbMax used otherwise.
                autoScale: Prefs.dbAuto
                dbMin: Prefs.dbMin
                dbMax: Prefs.dbMax
                traceMode: Prefs.traceMode
                traceColor: Prefs.traceColor
                palette: Prefs.palette
                strengthColor: Prefs.strengthColor
                fillEnabled: Prefs.fillEnabled
                fillColor: Prefs.fillColor
                smoothing: Prefs.smoothing
                // Peak-hold markers (Display panel + Settings → Visuals).
                peakEnabled: Prefs.peakEnabled
                peakHoldSecs: Prefs.peakHoldSecs
                peakDecayDbps: Prefs.peakDecayDbps
                peakStyle: Prefs.peakStyle
                peakColor: Prefs.peakColor
                peakShowDb: Prefs.peakShowDb

                // Peak glow: render to a layer texture + additive-bloom
                // ShaderEffect (off when glow == 0, so no GPU cost).
                layer.enabled: Prefs.glow > 0
                layer.effect: ShaderEffect {
                    fragmentShader: "qrc:/shaders/glow.frag.qsb"
                    property real glowStrength: Prefs.glow / 100.0 * 1.6
                    property real glowThreshold: 0.12
                    property real glowSpread: 2.0
                    property size texSize: Qt.size(width, height)
                }
            }

            // ---- Lyra constellation watermark (luminance-keyed) ----
            Image {
                id: wmSource
                source: "qrc:/qt/qml/Lyra/src/assets/watermarks/lyra-watermark.jpg"
                visible: false          // texture provider only
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
            }
            ShaderEffect {
                id: watermark
                visible: Prefs.watermark
                fragmentShader: "qrc:/shaders/watermark.frag.qsb"
                property variant source: wmSource
                anchors.centerIn: parent
                height: pan.height * 0.92
                width: height * 1.3
                opacity: 0.42
                z: 1
            }

            // ---- Vega pulse ----
            Rectangle {
                id: vegaGlow
                visible: Prefs.watermark
                width: 24; height: 24; radius: 12
                color: "#bfe6ff"
                opacity: vegaCore.opacity * 0.22
                x: pan.width * 0.5 - width / 2
                y: pan.height * 0.20 - height / 2
                z: 1
            }
            Rectangle {
                id: vegaCore
                visible: Prefs.watermark
                width: 6; height: 6; radius: 3
                color: "#e8f6ff"
                x: pan.width * 0.5 - width / 2
                y: pan.height * 0.20 - height / 2
                z: 2
                SequentialAnimation on opacity {
                    running: vegaCore.visible
                    loops: Animation.Infinite
                    NumberAnimation { from: 0.30; to: 0.95; duration: 1900
                                      easing.type: Easing.InOutSine }
                    NumberAnimation { from: 0.95; to: 0.30; duration: 1900
                                      easing.type: Easing.InOutSine }
                }
            }

            // ---- Meteor streaks (rare ambient shooting stars) ----
            Canvas {
                id: meteorCanvas
                anchors.fill: parent
                z: 3
                visible: Prefs.meteors
                renderStrategy: Canvas.Cooperative

                property var meteors: []
                property real nextSpawn: 0
                property bool schedInit: false

                readonly property real durMin: 0.50
                readonly property real durMax: 0.85
                readonly property real headR: 2.5
                readonly property int  trailN: 12
                readonly property real trailLenS: 0.18
                readonly property real fadeInS: 0.05
                readonly property real fadeOutS: 0.15
                readonly property real trailMaxA: 0.55
                readonly property real gravity: 600.0
                readonly property real lateral: 400.0

                function nowS() { return Date.now() / 1000.0; }
                function rnd(a, b) { return a + Math.random() * (b - a); }

                function scheduleNext(now) {
                    var avg = Prefs.meteorGap;
                    nextSpawn = now + rnd(avg * 0.5, avg * 1.6);
                }

                function spawn() {
                    var w = width, h = height;
                    var roll = Math.random();
                    var sx, sy;
                    if (roll < 0.6)      { sx = rnd(0.15 * w, 0.85 * w); sy = -10; }
                    else if (roll < 0.8) { sx = -10;      sy = rnd(0, 0.30 * h); }
                    else                 { sx = w + 10;   sy = rnd(0, 0.30 * h); }
                    var tx = (w - sx) + rnd(-0.15 * w, 0.15 * w);
                    var ty = h + 20;
                    var dur = rnd(durMin, durMax);
                    var g = gravity;
                    var vx = (tx - sx) / dur;
                    var vy = (ty - sy - 0.5 * g * dur * dur) / dur;
                    if (lateral > 0) vx += rnd(-lateral, lateral);
                    var gold = Math.random() < (Prefs.meteorGold / 100.0);
                    return { t0: nowS(), sx: sx, sy: sy, vx: vx, vy: vy, ay: g,
                             dur: dur, gold: gold };
                }

                Timer {
                    interval: 33; repeat: true; running: meteorCanvas.visible
                    onTriggered: meteorCanvas.tick()
                }

                function tick() {
                    var now = nowS();
                    if (!schedInit) { scheduleNext(now); schedInit = true; }
                    if (now >= nextSpawn && meteors.length < 1) {
                        meteors.push(spawn());
                        scheduleNext(now);
                    }
                    var alive = [];
                    for (var i = 0; i < meteors.length; i++)
                        if (now - meteors[i].t0 < meteors[i].dur) alive.push(meteors[i]);
                    var wasActive = meteors.length > 0;
                    meteors = alive;
                    if (meteors.length > 0 || wasActive) requestPaint();
                }

                onPaint: {
                    var ctx = getContext("2d");
                    ctx.clearRect(0, 0, width, height);
                    if (meteors.length === 0) return;
                    var now = nowS();
                    ctx.globalCompositeOperation = "lighter";
                    for (var i = 0; i < meteors.length; i++)
                        drawOne(ctx, meteors[i], now);
                    ctx.globalCompositeOperation = "source-over";
                }

                function drawOne(ctx, m, now) {
                    var age = now - m.t0;
                    if (age >= m.dur) return;
                    var lifeA;
                    if (age < fadeInS)            lifeA = age / fadeInS;
                    else if (age > m.dur - fadeOutS) lifeA = Math.max(0, (m.dur - age) / fadeOutS);
                    else                         lifeA = 1.0;
                    if (lifeA <= 0) return;
                    var rgb = m.gold ? "255,215,130" : "225,235,255";
                    for (var k = trailN; k > 0; k--) {
                        var tBack = (k / trailN) * trailLenS;
                        var pa = age - tBack;
                        if (pa < 0) continue;
                        var tx = m.sx + m.vx * pa;
                        var ty = m.sy + m.vy * pa + 0.5 * m.ay * pa * pa;
                        var frac = 1.0 - (k / trailN);
                        var a = lifeA * frac * trailMaxA;
                        if (a < 0.01) continue;
                        var rad = headR * (0.35 + 0.65 * frac);
                        ctx.fillStyle = "rgba(" + rgb + "," + a.toFixed(3) + ")";
                        ctx.beginPath(); ctx.arc(tx, ty, rad, 0, 2 * Math.PI); ctx.fill();
                    }
                    var hx = m.sx + m.vx * age;
                    var hy = m.sy + m.vy * age + 0.5 * m.ay * age * age;
                    ctx.fillStyle = "rgba(" + rgb + "," + lifeA.toFixed(3) + ")";
                    ctx.beginPath(); ctx.arc(hx, hy, headR, 0, 2 * Math.PI); ctx.fill();
                }
            }

            // ---- dBm vertical scale, BOTH edges ----
            DbLabels { rightSide: false; area: spectrumArea
                       dMax: root.effMax; dMin: root.effMin }
            DbLabels { rightSide: true;  area: spectrumArea
                       dMax: root.effMax; dMin: root.effMin }

            // ---- waterfall collapse toggle (old-Lyra triangle) ----
            // ▼ = waterfall shown (click to collapse it away); ▲ =
            // collapsed (click to bring it back).  Lives in the spectrum
            // area so it stays reachable even when the waterfall is gone.
            Rectangle {
                id: wfToggle
                z: 6
                width: 20; height: 18; radius: 3
                x: 4
                y: spectrumArea.height - height - 20
                color: wfToggleMA.containsMouse ? "#1f3a4a" : "#cc14202a"
                border.color: "#2a4a5a"
                Text {
                    anchors.centerIn: parent
                    text: Prefs.waterfallCollapsed ? "▲" : "▼"
                    color: "#8fd0ff"
                    font.pixelSize: 11
                }
                MouseArea {
                    id: wfToggleMA
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Prefs.waterfallCollapsed =
                                   !Prefs.waterfallCollapsed
                    ToolTip.visible: containsMouse
                    ToolTip.text: Prefs.waterfallCollapsed
                                  ? qsTr("Show waterfall")
                                  : qsTr("Collapse waterfall")
                }
            }

            // ---- frequency scale at the spectrum/waterfall boundary ----
            // MHz labels at the vertical grid divisions; shared X axis for
            // the spectrum above and the waterfall below.  Centre = RX1
            // DDC freq; full width spans WdspEngine.spanHz (the IQ rate).
            Repeater {
                model: 11   // 0..10 -> left edge .. right edge
                delegate: Text {
                    required property int index
                    readonly property real frac: index / 10.0
                    readonly property real fhz:
                        root.centerHz + (frac - 0.5) * WdspEngine.spanHz
                    text: (fhz / 1.0e6).toFixed(3)
                    color: "#8fa6ba"
                    font.pixelSize: 12
                    font.family: "Consolas"
                    style: Text.Outline
                    styleColor: "#80000000"
                    x: Math.max(0, Math.min(spectrumArea.width - width,
                                            frac * spectrumArea.width - width / 2))
                    y: spectrumArea.height - height - 2
                }
            }

            // ---- VFO marker — RX1 sits at the panadapter centre ----
            // The DDC is tuned to rx1FreqHz, so the spectrum is always
            // centred on the receive frequency.  Matches old Lyra: a 1px
            // DASHED orange (#ffaa50) line at centre, NO top tab (the top
            // strip is reserved for the band-plan / EiBi / spot / NCDXF
            // overlays that land later).
            //
            // Built from plain Rectangles, NOT a QtQuick.Shapes Shape:
            // a Shape here regressed the shared scene-graph render path
            // and turned the waterfall (lower pane) patchy.  Pure Items
            // keep the scene on the same renderer the waterfall needs.
            Repeater {
                model: Math.max(1, Math.ceil(spectrumArea.height / 8))
                delegate: Rectangle {
                    required property int index
                    z: 4
                    width: 1
                    height: 4               // 4px dash + 4px gap (8px pitch)
                    color: "#ffaa50"
                    opacity: 0.86
                    // Carrier marker: centre in non-CW; offset by the CW
                    // pitch (markerOffsetHz) so it sits on the carrier the
                    // operator hears (which is +pitch above the DDS centre).
                    x: Math.round(spectrumArea.width
                                  * (0.5 + WdspEngine.markerOffsetHz
                                           / Math.max(1, WdspEngine.spanHz)))
                    y: index * 8
                }
            }

            // ---- peak dB labels (strongest in-view peaks) ----
            // Numeric readout for the top peaks the panadapter detected
            // (peakLabels = [{frac, db}, …]); only populated when Peak
            // Hold + "Show dB" are on.  Positioned by fraction-of-width
            // and mapped to the live effective dB range.
            Repeater {
                model: pan.peakLabels
                delegate: Text {
                    required property var modelData
                    readonly property real db: modelData.db
                    readonly property real norm: Math.max(0, Math.min(1,
                        (db - root.effMin)
                        / Math.max(1, root.effMax - root.effMin)))
                    z: 5
                    text: (db >= 0 ? "+" : "") + Math.round(db)
                    color: Prefs.peakColor
                    font.pixelSize: 11
                    font.bold: true
                    font.family: "Consolas"
                    style: Text.Outline
                    styleColor: "#cc000000"
                    x: Math.max(0, Math.min(spectrumArea.width - width,
                                 modelData.frac * spectrumArea.width + 4))
                    y: Math.max(2, Math.min(spectrumArea.height - height - 2,
                                 (1 - norm) * spectrumArea.height - height - 2))
                }
            }

            // ---- noise-floor reference line + NF label ----
            // Dashed line at the rolling ~20th-percentile floor
            // (pan.noiseFloorDb), mapped to the live effective dB range,
            // with an "NF -NN dBFS" label.  Colour + on/off from Prefs.
            Item {
                id: nfLine
                anchors.fill: parent
                z: 4
                readonly property real nfNorm:
                    (pan.noiseFloorDb - root.effMin)
                    / Math.max(1, root.effMax - root.effMin)
                visible: Prefs.noiseFloorEnabled
                         && nfNorm >= 0 && nfNorm <= 1
                readonly property real nfY:
                    Math.max(0, Math.min(spectrumArea.height - 1,
                             (1 - nfNorm) * spectrumArea.height))
                Repeater {                 // 5 px dash + 5 px gap
                    model: nfLine.visible
                           ? Math.max(1, Math.ceil(spectrumArea.width / 10)) : 0
                    delegate: Rectangle {
                        required property int index
                        x: index * 10
                        y: nfLine.nfY
                        width: 5; height: 1
                        color: Prefs.noiseFloorColor
                        opacity: 0.7
                    }
                }
                Text {
                    visible: nfLine.visible
                    text: "NF " + (pan.noiseFloorDb >= 0 ? "+" : "")
                          + Math.round(pan.noiseFloorDb) + qsTr(" dBFS")
                    color: Prefs.noiseFloorColor
                    font.pixelSize: 11
                    font.family: "Consolas"
                    style: Text.Outline
                    styleColor: "#cc000000"
                    // Sit just left of the right-edge dB scale numbers.
                    x: Math.max(2, spectrumArea.width - width - 46)
                    y: Math.max(0, nfLine.nfY - height - 1)
                }
            }

            // ---- cursor frequency readout (floats near the pointer) ----
            // Operator-toggleable (Settings → Visuals → Cursor readout).
            // Follows the cursor instead of pinning to the top, so it
            // never collides with the reserved top overlay strip or the
            // bottom frequency scale.
            Text {
                z: 5
                visible: Prefs.cursorReadout && specMouse.containsMouse
                         && !specMouse.tuning && specMouse.dbMode === ""
                // Full-Hz, dot-grouped (14.234.723) so the readout shows the
                // exact frequency under the cursor, not a 100 Hz-rounded one.
                text: specMouse.fmtHzGrouped(specMouse.cursorHz)
                color: "#cdd9e5"
                font.pixelSize: 12
                font.family: "Consolas"
                style: Text.Outline
                styleColor: "#cc000000"
                x: Math.max(2, Math.min(spectrumArea.width - width - 2,
                                        specMouse.mouseX + 12))
                y: Math.max(2, Math.min(spectrumArea.height - height - 2,
                                        specMouse.mouseY - 18))
            }

            // ---- spectrum mouse: click / drag / wheel tune + dB-drag ----
            // Click the spectrum to tune RX1 there; drag to pan across
            // the band; wheel to step.  The right-edge strip (3 zones)
            // still adjusts the dB scale.  Reference: old Lyra
            // spectrum_gpu.py (freq_at_pixel, release-based click-tune,
            // drag-pan throttle, wheel-tune).
            MouseArea {
                id: specMouse
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                hoverEnabled: true
                readonly property int zonePx: 50
                readonly property int dragThreshPx: 6
                // Right-button = manual-notch placement on empty spectrum
                // (right-click ON a notch is handled by the notch's own
                // hit-area MouseArea in notchOverlay, which steals the press).
                property bool rightPress: false
                property bool rightDragged: false
                property real rightX: 0

                // dB-drag (right edge) state
                property string dbMode: ""
                property real startY: 0
                property real startMin: 0
                property real startMax: 0
                // tune state
                property bool tuning: false
                property bool dragged: false
                property real startX: 0
                property int  startCenter: 0
                property real lastEmitMs: 0
                // live cursor freq for the hover readout
                readonly property real cursorHz: freqAtX(mouseX)
                // Full-Hz, dot-grouped like the VFO readout: 14.234.723.
                function fmtHzGrouped(hz) {
                    var s = Math.round(hz).toString()
                    var out = ""
                    while (s.length > 3) { out = "." + s.slice(-3) + out
                                           s = s.slice(0, -3) }
                    return s + out
                }

                function dbModeAt(mx, my) {
                    if (mx < spectrumArea.width - zonePx) return ""
                    if (my < 24) return ""          // reserve top strip
                    var third = spectrumArea.height / 3
                    if (my < third) return "max"
                    if (my > 2 * third) return "min"
                    return "pan"
                }
                function freqAtX(mx) {
                    return root.centerHz
                           + (mx / Math.max(1, width) - 0.5) * WdspEngine.spanHz
                }
                // Tune to an operator-facing CARRIER freq, snapped to a grid:
                //   • "100 Hz" toggle → round to the 100 Hz grid (override).
                //   • "Exact"        → snap to the selected Panafall step
                //                      grid (step 1 Hz = truly exact; 10/50/
                //                      1k… land on that grid).
                // Then offset by the CW pitch so the DDS lands the signal in
                // the filter.
                function tuneCarrier(carrierHz) {
                    var c = carrierHz
                    if (Prefs.panRound100)
                        c = Math.round(c / 100) * 100
                    else if (Prefs.panScrollStepHz > 1)
                        c = Math.round(c / Prefs.panScrollStepHz)
                              * Prefs.panScrollStepHz
                    Stream.setRx1FreqHz(Math.round(c - WdspEngine.markerOffsetHz))
                }

                cursorShape: dbModeAt(mouseX, mouseY) !== "" ? Qt.SizeVerCursor
                             : (tuning && dragged ? Qt.ClosedHandCursor
                                                  : Qt.CrossCursor)

                onPressed: (mouse) => {
                    if (mouse.button === Qt.RightButton) {
                        rightPress = true
                        rightDragged = false
                        rightX = mouse.x
                        return
                    }
                    dbMode = dbModeAt(mouse.x, mouse.y)
                    if (dbMode !== "") {
                        // Dragging the dB scale takes MANUAL control: if
                        // auto is on, seed the manual range from the
                        // current auto fit and turn auto off, then drag.
                        if (Prefs.dbAuto) {
                            Prefs.dbMin = Math.round(pan.effDbMin)
                            Prefs.dbMax = Math.round(pan.effDbMax)
                            Prefs.dbAuto = false
                        }
                        startY = mouse.y
                        startMin = Prefs.dbMin
                        startMax = Prefs.dbMax
                    } else {
                        tuning = true
                        dragged = false
                        startX = mouse.x
                        startCenter = root.centerHz
                        lastEmitMs = 0
                    }
                }
                onReleased: (mouse) => {
                    if (mouse.button === Qt.RightButton) {
                        // Right-click on empty spectrum drops a 200 Hz notch
                        // (default 200 Hz width) at the clicked frequency.
                        if (!rightDragged)
                            WdspEngine.addNotch(
                                freqAtX(rightX) - root.centerHz, 200)
                        rightPress = false
                        rightDragged = false
                        return
                    }
                    if (dbMode === "" && tuning && !dragged) {
                        // plain click → tune the CARRIER to the clicked RF
                        // point (round-to-100 + CW pitch offset handled in
                        // tuneCarrier).
                        tuneCarrier(freqAtX(startX))
                    }
                    dbMode = ""
                    tuning = false
                    dragged = false
                }
                onPositionChanged: (mouse) => {
                    if (rightPress) {
                        if (Math.abs(mouse.x - rightX) > dragThreshPx)
                            rightDragged = true
                        return
                    }
                    if (dbMode !== "") {
                        var h = Math.max(1, spectrumArea.height)
                        var span = startMax - startMin
                        var dbDelta = -(mouse.y - startY) * (span / h)
                        var nMin = startMin
                        var nMax = startMax
                        if (dbMode === "max")      nMax = startMax + dbDelta
                        else if (dbMode === "min") nMin = startMin + dbDelta
                        else { nMin = startMin + dbDelta
                               nMax = startMax + dbDelta }
                        // Task #44 Phase 1 — widened ceiling clamp from
                        // 0 → +30 dBFS so the TX-state pair (which
                        // defaults to +20 dBFS ceiling) is draggable
                        // across its natural range while keyed.
                        // RX-state drag is unaffected in practice
                        // (RX traces never reach 0 dBFS so the wider
                        // cap is dormant on RX).
                        nMin = Math.max(-150, Math.min(-3, nMin))
                        nMax = Math.max(nMin + 3, Math.min(30, nMax))
                        Prefs.dbMin = nMin
                        Prefs.dbMax = nMax
                        return
                    }
                    if (!tuning) return
                    var dx = mouse.x - startX
                    if (!dragged) {
                        if (Math.abs(dx) < dragThreshPx) return
                        dragged = true
                    }
                    // cursor right → spectrum follows finger → centre down
                    var hzPerPx = WdspEngine.spanHz / Math.max(1, width)
                    var newCenter = startCenter - dx * hzPerPx
                    // throttle ~30 Hz so the render pipeline keeps up
                    var nowMs = Date.now()
                    if (nowMs - lastEmitMs < 33) return
                    lastEmitMs = nowMs
                    tuneCarrier(newCenter + WdspEngine.markerOffsetHz)
                }
                onWheel: (wheel) => {
                    if (wheel.modifiers & Qt.ControlModifier) {
                        // Ctrl + wheel = zoom (escape hatch, old-Lyra gesture)
                        WdspEngine.setZoom(WdspEngine.zoom
                            * (wheel.angleDelta.y > 0 ? 1.25 : 0.8))
                    } else {
                        // Wheel = tune by the Panafall step (Display panel).
                        var dir = wheel.angleDelta.y > 0 ? 1 : -1
                        var carrier = Stream.rx1FreqHz + WdspEngine.markerOffsetHz
                        tuneCarrier(carrier + dir * Prefs.panScrollStepHz)
                    }
                    wheel.accepted = true
                }
            }

            // ---- Manual-notch overlay (NF) ----
            // Translucent red bands over each notch.  Right-click empty
            // spectrum drops one (handled in specMouse); here each band
            // can be DRAGGED to move, WHEELED to widen/narrow, and
            // RIGHT-CLICKED to remove.  Notch depth itself is deep
            // (WDSP NBP0: rectangular window, 2048-tap FIR) — see engine.
            Item {
                id: notchOverlay
                anchors.fill: parent
                z: 5
                readonly property real spanHz: Math.max(1, WdspEngine.spanHz)
                function xOf(offHz) {
                    return spectrumArea.width * (0.5 + offHz / spanHz)
                }
                Repeater {
                    model: WdspEngine.notches
                    delegate: Item {
                        id: notchItem
                        required property int index
                        required property var modelData
                        readonly property real cx: notchOverlay.xOf(modelData.offsetHz)
                        readonly property real wPx: Math.max(3,
                            spectrumArea.width * modelData.widthHz
                            / notchOverlay.spanHz)
                        readonly property real hitW: Math.max(wPx, 14)
                        x: cx - hitW / 2
                        y: 0; width: hitW; height: spectrumArea.height
                        // Translucent fill band.  Kept low so it doesn't
                        // wash out the trace — the brightness comes from the
                        // crisp edges below, NOT the fill (a Rectangle's
                        // opacity dims its border too, which is what made the
                        // old single-rect notch read as a mushy brown smear
                        // against the glassy sheen).
                        Rectangle {
                            id: notchFill
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 0; width: notchItem.wPx; height: parent.height
                            color: notchItem.modelData.active
                                   ? "#ff4d4d" : "#ff7a7a"
                            opacity: notchItem.modelData.active ? 0.16 : 0.08
                        }
                        // Crisp full-opacity edges — these are what the eye
                        // catches.  Bright hot-coral so they pop over the
                        // panadapter glow; dimmer/thinner when the notch is
                        // bypassed (inactive).
                        Repeater {
                            model: [0, 1]
                            delegate: Rectangle {
                                required property int modelData
                                x: notchFill.x + (modelData === 0
                                       ? 0 : notchFill.width - width)
                                y: 0
                                width: notchItem.modelData.active ? 2 : 1
                                height: parent.height
                                color: notchItem.modelData.active
                                       ? "#ff5555" : "#b86a6a"
                                opacity: notchItem.modelData.active ? 0.95 : 0.6
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            property bool moved: false
                            property real downX: 0
                            property double lastMs: 0
                            onPressed: (m) => { moved = false; downX = m.x }
                            onPositionChanged: (m) => {
                                if (!(pressedButtons
                                      & (Qt.LeftButton | Qt.RightButton))) return
                                if (Math.abs(m.x - downX) > 4) moved = true
                                var nowMs = Date.now()
                                if (nowMs - lastMs < 33) return
                                lastMs = nowMs
                                // local x -> absolute spectrum x -> centre offset
                                var gx = notchItem.x + m.x
                                var offHz = (gx / Math.max(1, spectrumArea.width)
                                             - 0.5) * notchOverlay.spanHz
                                WdspEngine.moveNotch(notchItem.index, offHz)
                            }
                            onReleased: (m) => {
                                if (!moved && m.button === Qt.RightButton)
                                    WdspEngine.removeNotch(notchItem.index)
                            }
                            // Wheel: down = wider, up = narrower (old-Lyra gesture).
                            onWheel: (w) => {
                                var f = w.angleDelta.y > 0 ? 0.8 : 1.25
                                WdspEngine.setNotchWidth(
                                    notchItem.index, notchItem.modelData.widthHz * f)
                                w.accepted = true
                            }
                        }
                    }
                }
            }

            // ---- RX filter passband overlay (drag an edge to resize) ----
            // Translucent box over the tuned signal showing the current
            // RX filter; the edges are draggable to widen/narrow the
            // bandwidth, which updates Prefs.rxBandwidth (→ engine filter
            // + the Mode+Filter RX BW readout, persisted per mode).
            Item {
                id: passband
                anchors.fill: parent
                z: 4
                readonly property real spanHz: Math.max(1, WdspEngine.spanHz)
                function xOf(offHz) {
                    return spectrumArea.width * (0.5 + offHz / spanHz)
                }
                readonly property real loX: xOf(WdspEngine.passbandLowHz)
                readonly property real hiX: xOf(WdspEngine.passbandHighHz)

                Rectangle {            // translucent fill
                    x: Math.min(passband.loX, passband.hiX)
                    width: Math.abs(passband.hiX - passband.loX)
                    y: 0; height: spectrumArea.height
                    color: "#3aa0ff"
                    opacity: 0.13
                }
                Repeater {             // two draggable edges
                    model: 2
                    delegate: Item {
                        required property int index
                        readonly property bool isLo: index === 0
                        x: (isLo ? passband.loX : passband.hiX) - width / 2
                        y: 0; width: 9; height: spectrumArea.height
                        Rectangle {    // visible edge line
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 0; width: 2; height: parent.height
                            color: "#8fd0ff"; opacity: 0.75
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.SizeHorCursor
                            preventStealing: true
                            property double lastMs: 0
                            // Task #54 + #80 (sideband symmetry fix) —
                            // distinguish lo-edge vs hi-edge drag, then
                            // map to filterLow vs rxBandwidth by mode.
                            //
                            // The Repeater spawns two edges by ARRAY
                            // INDEX (isLo = index 0); the SEMANTIC
                            // mapping flips by sideband:
                            //   USB/DIGU: passbandLowHz=+flo near
                            //       carrier, passbandHighHz=+bw far
                            //       out.  Lo edge IS the inner /
                            //       filterLow knob; hi edge IS the
                            //       outer / rxBandwidth knob.
                            //   LSB/DIGL: passbandLowHz=-bw far out,
                            //       passbandHighHz=-flo near carrier.
                            //       MIRRORED — lo edge is now the
                            //       OUTER (rxBandwidth), hi edge is
                            //       the INNER (filterLow).  Pre-fix
                            //       routed by isLo alone → in LSB the
                            //       outer drag wrote filterLow which
                            //       clamped to 500 Hz, collapsing the
                            //       inner edge instead of extending
                            //       the outer (#80 operator-reported).
                            //
                            // For symmetric modes (AM/DSB/FM) and CW
                            // (pitch-centred), both edges write
                            // rxBandwidth — the asymmetric SSB/DIG
                            // case is the only one where filterLow
                            // is independently meaningful.
                            onPositionChanged: (m) => {
                                var nowMs = Date.now()
                                if (nowMs - lastMs < 33) return
                                lastMs = nowMs
                                var px = mapToItem(spectrumArea, m.x, m.y).x
                                var off = (px / Math.max(1, spectrumArea.width)
                                           - 0.5) * passband.spanHz
                                var mode = Prefs.mode
                                var isSsbOrDig = (mode === "USB" || mode === "LSB"
                                               || mode === "DIGU" || mode === "DIGL")
                                if (isSsbOrDig) {
                                    var isLsb = (mode === "LSB" || mode === "DIGL")
                                    // Lower-sideband mirrors the
                                    // inner/outer role of the two
                                    // edges (see comment above).
                                    var isInnerEdge = isLsb ? !isLo : isLo
                                    if (isInnerEdge) {
                                        // |off| at the inner edge IS
                                        // the low cutoff for SSB/DIG.
                                        // Clamp to Prefs's 0..500
                                        // range; drag-below-0 just
                                        // pins at 0.
                                        var flo = Math.abs(off)
                                        Prefs.filterLow =
                                            Math.max(0, Math.min(500, Math.round(flo)))
                                    } else {
                                        // Outer edge → bandwidth.
                                        Prefs.rxBandwidth =
                                            WdspEngine.bandwidthForEdge(off)
                                    }
                                } else {
                                    Prefs.rxBandwidth =
                                        WdspEngine.bandwidthForEdge(off)
                                }
                            }
                        }
                    }
                }
            }

            // ---- Band-plan overlay (top strip: segments / edges /
            // landmarks) ----  Advisory amateur band plan from BandPlan
            // (region + data) gated by the Prefs layer toggles.  Maps a
            // frequency to x the same way the freq scale + markers do.
            Item {
                id: bandPlan
                anchors.fill: parent
                z: 6
                readonly property real spanHz: Math.max(1, WdspEngine.spanHz)
                function xOf(hz) {
                    return spectrumArea.width
                           * (0.5 + (hz - root.centerHz) / bandPlan.spanHz)
                }
                // All four bindings reference reactive deps (BandPlan.region
                // has a NOTIFY; the Prefs toggles + centerHz + spanHz are
                // QML props) so the overlay re-queries C++ on any change.
                readonly property var segs:
                    (Prefs.bandPlanSegments && BandPlan.region !== "NONE")
                    ? BandPlan.segments(root.centerHz, bandPlan.spanHz) : []
                readonly property var edges:
                    (Prefs.bandPlanEdges && BandPlan.region !== "NONE")
                    ? BandPlan.edges(root.centerHz, bandPlan.spanHz) : []
                readonly property var marks:
                    (BandPlan.region !== "NONE"
                     && (Prefs.bandPlanLandmarks || Prefs.bandPlanBeacons))
                    ? BandPlan.landmarks(root.centerHz, bandPlan.spanHz,
                                         Prefs.bandPlanLandmarks,
                                         Prefs.bandPlanBeacons) : []
                // CB channel markers — only when the 11m/CB band is enabled.
                readonly property var cbChans:
                    Prefs.cbBandEnabled
                    ? BandPlan.cbChannels(root.centerHz, bandPlan.spanHz) : []

                // EiBi shortwave broadcasters active in this span.  Eibi
                // gates internally (returns [] when the overlay is off, no
                // DB is loaded, or we're inside a ham band).  eibiRev is
                // bumped on a download/settings change so the binding
                // re-queries C++ even though those aren't QML properties.
                property int eibiRev: 0
                readonly property var eibiList:
                    (bandPlan.eibiRev,
                     Eibi.entriesInSpan(root.centerHz, bandPlan.spanHz))
                Connections {
                    target: Eibi
                    function onChanged()         { bandPlan.eibiRev++ }
                    function onSettingsChanged()  { bandPlan.eibiRev++ }
                }

                // DX-cluster spots pushed over TCI (SDRLogger+ / cluster).
                property int spotRev: 0
                readonly property var spotsList:
                    (bandPlan.spotRev,
                     Spots.spotsInSpan(root.centerHz, bandPlan.spanHz))
                Connections {
                    target: Spots
                    function onChanged() { bandPlan.spotRev++ }
                }

                // Sub-band segment bars (top 10 px), coloured by mode.
                Repeater {
                    model: bandPlan.segs
                    delegate: Item {
                        id: segItem
                        required property var modelData
                        readonly property real x0: bandPlan.xOf(modelData.lo)
                        readonly property real x1: bandPlan.xOf(modelData.hi)
                        Rectangle {
                            x: Math.min(segItem.x0, segItem.x1)
                            y: 0
                            width: Math.max(0, Math.abs(segItem.x1 - segItem.x0))
                            height: 10
                            color: segItem.modelData.color
                            opacity: 0.82
                        }
                        Text {
                            visible: Math.abs(segItem.x1 - segItem.x0) >= 24
                            x: Math.min(segItem.x0, segItem.x1) + 3
                            y: 0
                            text: segItem.modelData.label
                            color: "#f0f0f0"
                            font.pixelSize: 9
                            font.bold: true
                            style: Text.Outline
                            styleColor: "#a0000000"
                        }
                    }
                }

                // Band-edge warning lines — full-height red dashed + label.
                Repeater {
                    model: bandPlan.edges
                    delegate: Item {
                        id: edgeItem
                        required property var modelData
                        readonly property real ex: bandPlan.xOf(modelData.freq)
                        Repeater {           // dashed: 5 px dash + 5 px gap
                            model: Math.max(1, Math.ceil(spectrumArea.height / 10))
                            delegate: Rectangle {
                                required property int index
                                x: edgeItem.ex - 1
                                y: index * 10
                                width: 2; height: 5
                                color: "#ff5050"
                                opacity: 0.85
                            }
                        }
                        Text {
                            x: Math.max(0, Math.min(spectrumArea.width - width,
                                                    edgeItem.ex + 2))
                            y: spectrumArea.height - height - 16
                            text: edgeItem.modelData.name + qsTr(" EDGE")
                            color: "#ff8080"
                            font.pixelSize: 9
                            font.bold: true
                            style: Text.Outline
                            styleColor: "#cc000000"
                        }
                    }
                }

                // Watering-hole landmarks — ▼ marker + label + click-to-tune.
                Repeater {
                    model: bandPlan.marks
                    delegate: Item {
                        id: markItem
                        required property var modelData
                        readonly property real mx: bandPlan.xOf(modelData.freq)
                        readonly property color col:
                            modelData.beacon ? "#78dcff" : "#ffd700"
                        Rectangle {          // short tick below the strip
                            x: markItem.mx - 0.5; y: 10; width: 1; height: 9
                            color: markItem.col; opacity: 0.5
                        }
                        Text {
                            x: markItem.mx - width / 2; y: 8
                            text: "▼"; color: markItem.col; font.pixelSize: 10
                        }
                        Text {
                            x: markItem.mx + 6; y: 10
                            text: markItem.modelData.label
                            color: markItem.col
                            font.pixelSize: 9; font.bold: true
                            style: Text.Outline; styleColor: "#cc000000"
                        }
                        MouseArea {
                            x: markItem.mx - 9; y: 6; width: 18; height: 18
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                Prefs.mode = markItem.modelData.mode
                                Stream.setRx1FreqHz(Math.round(markItem.modelData.freq))
                                Status.show(qsTr("Tuned to ")
                                    + (markItem.modelData.freq / 1.0e6).toFixed(3)
                                    + " MHz " + markItem.modelData.mode
                                    + " · " + markItem.modelData.label, 2500)
                            }
                            ToolTip.visible: containsMouse
                            // NCDXF beacons show the LIVE rotating station
                            // (computed on hover); others show label/mode.
                            ToolTip.text: {
                                var f = (markItem.modelData.freq / 1.0e6).toFixed(3)
                                if (markItem.modelData.beacon) {
                                    var who = BandPlan.ncdxfStation(markItem.modelData.freq)
                                    return qsTr("NCDXF ") + f + qsTr(" MHz")
                                           + (who !== "" ? qsTr("\nNow: ") + who : "")
                                           + qsTr("\nClick to QSY (CWU)")
                                }
                                return markItem.modelData.label + " · "
                                       + markItem.modelData.mode + "  " + f
                                       + qsTr(" MHz\nClick to tune")
                            }
                        }
                    }
                }

                // CB channel ticks + numbers (Ch1…Ch40) — click to QSY (AM).
                Repeater {
                    model: bandPlan.cbChans
                    delegate: Item {
                        id: cbItem
                        required property var modelData
                        readonly property real cx: bandPlan.xOf(modelData.freq)
                        Rectangle {
                            x: cbItem.cx - 0.5; y: 14; width: 1; height: 9
                            color: "#b0c4d4"; opacity: 0.6
                        }
                        Text {
                            x: cbItem.cx - width / 2; y: 9
                            text: cbItem.modelData.label
                            color: "#b0c4d4"
                            font.pixelSize: 11; font.bold: true
                            style: Text.Outline; styleColor: "#cc000000"
                        }
                        MouseArea {
                            x: cbItem.cx - 11; y: 8; width: 22; height: 20
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                Prefs.mode = "AM"
                                Stream.setRx1FreqHz(Math.round(cbItem.modelData.freq))
                                Status.show(qsTr("Tuned to CB ") + cbItem.modelData.label
                                    + " — " + (cbItem.modelData.freq / 1.0e6).toFixed(4)
                                    + " MHz AM", 2000)
                            }
                            ToolTip.visible: containsMouse
                            ToolTip.text: "CB " + cbItem.modelData.label + "  "
                                + (cbItem.modelData.freq / 1.0e6).toFixed(3)
                                + qsTr(" MHz AM")
                                + (cbItem.modelData.note !== ""
                                   ? "\n" + cbItem.modelData.note : "")
                                + qsTr("\nClick to tune")
                        }
                    }
                }

                // EiBi shortwave broadcasters — station tick + name, cyan
                // when on-air now / grey when scheduled-but-off; staggered
                // over 3 rows to reduce label overlap.  Click tunes in AM.
                Repeater {
                    model: bandPlan.eibiList
                    delegate: Item {
                        id: ebItem
                        required property var modelData
                        required property int index
                        readonly property real ex: bandPlan.xOf(modelData.freqHz)
                        readonly property color col:
                            modelData.onAir ? "#50c8dc" : "#7a8a94"
                        readonly property real labelY: 25 + (index % 3) * 13
                        Rectangle {
                            x: ebItem.ex - 0.5; y: 20
                            width: 1; height: ebItem.labelY - 20
                            color: ebItem.col; opacity: 0.55
                        }
                        Text {
                            x: ebItem.ex + 3; y: ebItem.labelY
                            text: ebItem.modelData.station.length > 22
                                  ? ebItem.modelData.station.substring(0, 22)
                                  : ebItem.modelData.station
                            color: ebItem.col
                            font.pixelSize: 10; font.bold: true
                            style: Text.Outline; styleColor: "#cc000000"
                        }
                        MouseArea {
                            x: ebItem.ex - 6; y: 20
                            width: 52; height: ebItem.labelY - 20 + 14
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                Prefs.mode = "AM"
                                Stream.setRx1FreqHz(Math.round(ebItem.modelData.freqHz))
                                Status.show(qsTr("Tuned to ")
                                    + (ebItem.modelData.freqHz / 1.0e6).toFixed(3)
                                    + " MHz AM · " + ebItem.modelData.station, 2500)
                            }
                            ToolTip.visible: containsMouse
                            ToolTip.text: ebItem.modelData.station + "  "
                                + (ebItem.modelData.freqHz / 1.0e6).toFixed(3)
                                + qsTr(" MHz")
                                + (ebItem.modelData.onAir ? qsTr("  (ON AIR)")
                                                          : qsTr("  (off air)"))
                                + (ebItem.modelData.language !== ""
                                   ? qsTr("\nLanguage: ") + ebItem.modelData.language : "")
                                + (ebItem.modelData.target !== ""
                                   ? qsTr("\nTarget: ") + ebItem.modelData.target : "")
                                + qsTr("\nClick to tune (AM)")
                        }
                    }
                }

                // DX-cluster spots — colored callsign markers (color set by
                // the spotting client); click to tune + echo spot_activated.
                Repeater {
                    model: bandPlan.spotsList
                    delegate: Item {
                        id: spItem
                        required property var modelData
                        required property int index
                        readonly property real sx: bandPlan.xOf(modelData.freqHz)
                        // 3-row stagger; the C++ list is freq-sorted, so
                        // callsigns near the same frequency land on
                        // different rows instead of stacking on top.
                        readonly property real labelY: 40 + (index % 3) * 13
                        Rectangle {
                            // #172 — a freshly-arrived spot flashes in the
                            // operator-set flash colour (own-call highlight
                            // still wins via `mine`).
                            x: spItem.sx - (spItem.modelData.mine ? 1 : 0.5); y: 38
                            width: spItem.modelData.mine ? 2 : 1; height: 41
                            color: spItem.modelData.flash
                                   ? spItem.modelData.flashColor
                                   : spItem.modelData.color
                            opacity: (spItem.modelData.mine
                                      || spItem.modelData.flash) ? 1.0 : 0.8
                        }
                        Text {
                            x: spItem.sx + 3; y: spItem.labelY
                            // "★" marks the operator's own spotted callsign.
                            text: (spItem.modelData.mine ? "★ " : "")
                                  + spItem.modelData.label
                            color: spItem.modelData.flash
                                   ? spItem.modelData.flashColor
                                   : spItem.modelData.color
                            font.pixelSize: spItem.modelData.mine ? 13 : 11
                            font.bold: true
                            style: Text.Outline; styleColor: "#cc000000"
                        }
                        MouseArea {
                            x: spItem.sx - 6; y: spItem.labelY
                            width: 72; height: 14
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                Spots.activate(spItem.modelData.call)
                                Status.show(qsTr("Spot: ") + spItem.modelData.call
                                    + " — " + (spItem.modelData.freqHz / 1.0e6).toFixed(3)
                                    + " MHz " + spItem.modelData.mode, 2500)
                            }
                            ToolTip.visible: containsMouse
                            ToolTip.text: spItem.modelData.call + "  "
                                + (spItem.modelData.freqHz / 1.0e6).toFixed(3)
                                + qsTr(" MHz ") + spItem.modelData.mode
                                + (spItem.modelData.text !== ""
                                   ? "\n" + spItem.modelData.text : "")
                                + qsTr("\nClick to tune (and log)")
                        }
                    }
                }
            }
        }

        // ===== Waterfall (rolling spectrogram) =====
        Waterfall {
            id: wf
            // Collapse toggle (old-Lyra triangle): when collapsed the
            // pane is hidden and the SplitView gives the panadapter the
            // full height.  Persisted via Prefs.
            visible: !Prefs.waterfallCollapsed
            // Initial/factory height is set imperatively by
            // applyFromPrefs() (a live `root.height * 0.4` binding would
            // fight restoreState on every window resize).
            SplitView.minimumHeight: 60
            engine: WdspEngine
            // Independent waterfall dB range (Settings → Visuals) — NOT
            // the panadapter's dbMin/dbMax, so contrast tunes separately.
            // When autoScale is on, dbMin/dbMax are ignored (the item
            // auto-fits internally).
            autoScale: Prefs.waterfallDbAuto
            dbMin: Prefs.waterfallDbMin
            dbMax: Prefs.waterfallDbMax
            targetFps: Prefs.targetFps
            // The waterfall has its OWN palette + custom colour + speed,
            // independent of the panadapter's by-strength trace palette.
            palette: Prefs.waterfallPalette
            strengthColor: Prefs.waterfallColor
            speed: Prefs.waterfallSpeed
        }
    }
}

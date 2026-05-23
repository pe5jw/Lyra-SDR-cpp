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
    Component.onCompleted: centerHz = Stream.rx1FreqHz
    Connections {
        target: Stream
        function onRx1FreqChanged() { root.centerHz = Stream.rx1FreqHz }
    }

    // dB labels at the 8 horizontal grid divisions: dbMax at the top
    // down to dbMin at the bottom.  Dim, outlined, tracks Prefs live.
    component DbLabels : Repeater {
        id: rep
        property bool rightSide: false
        property Item area      // the spectrum area these label
        model: 9   // 0..8 -> top edge .. bottom edge
        delegate: Text {
            required property int index
            readonly property real frac: index / 8.0
            x: rep.rightSide ? (rep.area.width - width - 5) : 5
            y: Math.max(0, Math.min(rep.area.height - height,
                                    frac * rep.area.height - height / 2))
            text: Math.round(Prefs.dbMax - frac * (Prefs.dbMax - Prefs.dbMin))
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
                dbMin: Prefs.dbMin
                dbMax: Prefs.dbMax
                traceMode: Prefs.traceMode
                traceColor: Prefs.traceColor
                palette: Prefs.palette
                strengthColor: Prefs.strengthColor
                fillEnabled: Prefs.fillEnabled
                fillColor: Prefs.fillColor
                smoothing: Prefs.smoothing

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
            DbLabels { rightSide: false; area: spectrumArea }
            DbLabels { rightSide: true;  area: spectrumArea }

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
                    x: Math.round(spectrumArea.width / 2)
                    y: index * 8
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
                text: (specMouse.cursorHz / 1.0e6).toFixed(4) + qsTr(" MHz")
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
                acceptedButtons: Qt.LeftButton
                hoverEnabled: true
                readonly property int zonePx: 50
                readonly property int dragThreshPx: 6
                readonly property int wheelStepHz: 1000   // per wheel notch

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

                cursorShape: dbModeAt(mouseX, mouseY) !== "" ? Qt.SizeVerCursor
                             : (tuning && dragged ? Qt.ClosedHandCursor
                                                  : Qt.CrossCursor)

                onPressed: (mouse) => {
                    dbMode = dbModeAt(mouse.x, mouse.y)
                    if (dbMode !== "") {
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
                    if (dbMode === "" && tuning && !dragged) {
                        // plain click → literal tune to the press freq
                        Stream.setRx1FreqHz(Math.round(freqAtX(startX)))
                    }
                    dbMode = ""
                    tuning = false
                    dragged = false
                }
                onPositionChanged: (mouse) => {
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
                        nMin = Math.max(-150, Math.min(-3, nMin))
                        nMax = Math.max(nMin + 3, Math.min(0, nMax))
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
                    Stream.setRx1FreqHz(Math.round(newCenter))
                }
                onWheel: (wheel) => {
                    var dir = wheel.angleDelta.y > 0 ? 1 : -1
                    Stream.setRx1FreqHz(Stream.rx1FreqHz + dir * wheelStepHz)
                    wheel.accepted = true
                }
            }
        }

        // ===== Waterfall (rolling spectrogram) =====
        Waterfall {
            id: wf
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

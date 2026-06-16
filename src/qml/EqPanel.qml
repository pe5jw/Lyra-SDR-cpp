// Lyra — TX parametric-EQ dock panel (#50 Stage 2a).
//
// The operator-approved EESDR3-style layout, made live against the
// native ParamEq engine (Eq context property = lyra::ui::EqModel):
//   • white glowing summed-response curve (Eq.magnitudeDb → what you hear)
//   • 8 numbered typed nodes on the curve; click selects (white ring),
//     drag = freq + gain, wheel = Q
//   • per-band tile row: #, Type (click to cycle), freq, gain/Q;
//     right-click a tile resets that band
//   • top bar: ON (bypass) · Makeup trim · collapse ▼/▲
//
// Stage 2b adds the analyzer behind the curve (live spectrum, Accumulate
// peak-hold, pink-noise reference, Before/After-Mod, optional waterfall).
// Wire-INERT until Stage 3 routes the TX mic rack through Eq.engine().
//
// QML can't bind to a band struct, so node/tile bindings read index-keyed
// Eq.bandX(i) invokables and re-evaluate off `rev` (bumped on bandsChanged).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitWidth: 860
    implicitHeight: collapsed ? 40 : 320
    color: "#0d141b"
    border.color: "#2a4a5a"

    // ── palette (matches the glassy panadapter language) ───────────────
    readonly property color cAccent: "#00e5ff"
    readonly property color cText:   "#cdd9e5"
    readonly property color cMuted:  "#8a9aac"
    readonly property color cGrid:   "#2a3e4d"
    readonly property color cGrid0:  "#36546a"   // the 0 dB line
    readonly property color cCurve:  "#ffffff"

    // Per-band node colours (10) — distinct rainbow spread, readable on
    // the dark graph.
    readonly property var bandColor: [
        "#ff4d4d", "#ff8c3c", "#ffd23c", "#b6ff4d", "#4dff7a",
        "#3cffd2", "#3cc8ff", "#6c8cff", "#a96cff", "#ff6cf0"
    ]

    // Collapse is local for 2a (persisted as Prefs.eqCollapsed in Stage 4).
    property bool collapsed: false

    // Revision tick: any engine change bumps it so the invokable-derived
    // node/tile/curve bindings re-evaluate (QML doesn't auto-track
    // Q_INVOKABLE calls).
    property int rev: 0
    onRevChanged: curveCanvas.requestPaint()
    Connections {
        target: Eq
        function onBandsChanged()       { root.rev++ }
        function onSelectedBandChanged() { root.rev++ }
        function onBypassChanged()      { root.rev++ }
        function onMakeupDbChanged()    { root.rev++ }
    }

    function fmtFreq(f) {
        if (f >= 1000) return (f / 1000).toFixed(f < 10000 ? 2 : 1) + "k"
        return Math.round(f).toString()
    }

    // Per-band filter-TYPE picker.  Opened from a tile's Type chip; lists
    // ALL seven RBJ types so Notch / Bandpass / LP / HP are visibly
    // selectable (not hidden behind a click-cycle).  band = the tile that
    // opened it; the checked item is that band's current type.
    Menu {
        id: typeMenu
        property int band: -1
        function pick(t) { if (band >= 0) Eq.setBandType(band, t) }
        function isCur(t) { root.rev; return band >= 0 && Eq.bandType(band) === t }
        MenuItem { text: qsTr("Peak");     checkable: true; checked: typeMenu.isCur(0); onTriggered: typeMenu.pick(0) }
        MenuItem { text: qsTr("Lo-Shelf"); checkable: true; checked: typeMenu.isCur(1); onTriggered: typeMenu.pick(1) }
        MenuItem { text: qsTr("Hi-Shelf"); checkable: true; checked: typeMenu.isCur(2); onTriggered: typeMenu.pick(2) }
        MenuItem { text: qsTr("Lo-Pass");  checkable: true; checked: typeMenu.isCur(3); onTriggered: typeMenu.pick(3) }
        MenuItem { text: qsTr("Hi-Pass");  checkable: true; checked: typeMenu.isCur(4); onTriggered: typeMenu.pick(4) }
        MenuItem { text: qsTr("Bandpass"); checkable: true; checked: typeMenu.isCur(5); onTriggered: typeMenu.pick(5) }
        MenuItem { text: qsTr("Notch");    checkable: true; checked: typeMenu.isCur(6); onTriggered: typeMenu.pick(6) }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 6

        // ── Top bar ────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                text: qsTr("TX EQ")
                color: root.cAccent
                font.bold: true
                font.pixelSize: 14
            }

            // ON = engine active (inverse of bypass).
            Button {
                id: onBtn
                text: qsTr("ON")
                checkable: true
                implicitWidth: 50
                implicitHeight: 24
                checked: { root.rev; return !Eq.bypass }
                onClicked: Eq.bypass = !checked
                background: Rectangle {
                    radius: 4
                    color: onBtn.checked ? "#0b3a44" : "#1f2a35"
                    border.color: onBtn.checked ? root.cAccent : "#3a5060"
                    border.width: 2
                }
                contentItem: Text {
                    text: onBtn.text
                    color: onBtn.checked ? root.cAccent : root.cMuted
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Item { width: 8 }

            // Makeup output trim.
            Label { text: qsTr("Makeup"); color: root.cMuted }
            RoundButton {
                text: "−"; implicitWidth: 24; implicitHeight: 24
                onClicked: Eq.makeupDb = Eq.makeupDb - 1
            }
            Label {
                text: { root.rev; return (Eq.makeupDb >= 0 ? "+" : "")
                        + Eq.makeupDb.toFixed(0) + qsTr(" dB") }
                color: root.cText
                font.family: "Consolas"
                font.bold: true
                Layout.preferredWidth: 52
                horizontalAlignment: Text.AlignHCenter
            }
            RoundButton {
                text: "+"; implicitWidth: 24; implicitHeight: 24
                onClicked: Eq.makeupDb = Eq.makeupDb + 1
            }

            Item { Layout.fillWidth: true }

            // Selected-band quick readout.
            Label {
                visible: !root.collapsed
                text: { root.rev
                        var i = Eq.selectedBand
                        if (i < 0) return ""
                        return qsTr("Band ") + (i + 1) + " · " + Eq.typeName(Eq.bandType(i)) }
                color: root.cMuted
                font.pixelSize: 12
            }

            // Collapse ▼/▲ (mirrors the waterfall idiom).
            Button {
                implicitWidth: 28; implicitHeight: 24
                text: root.collapsed ? "▲" : "▼"
                onClicked: root.collapsed = !root.collapsed
                ToolTip.text: root.collapsed ? qsTr("Show EQ graph")
                                             : qsTr("Collapse EQ graph")
                ToolTip.visible: hovered
                ToolTip.delay: 800
                background: Rectangle { radius: 4; color: "#1f2a35"; border.color: "#3a5060" }
                contentItem: Text {
                    text: parent.text; color: root.cAccent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        // ── Graph (curve + grid + draggable nodes) ─────────────────────
        Item {
            id: graph
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.collapsed
            opacity: Eq.bypass ? 0.45 : 1.0

            // Axis maps: log freq 30 Hz–12 kHz, linear ±15 dB.
            readonly property real fMin: 30
            readonly property real fMax: 12000
            readonly property real dbR: 15
            function xForFreq(f) { return width * Math.log(f / fMin) / Math.log(fMax / fMin) }
            function freqForX(x) {
                var t = Math.max(0, Math.min(1, x / width))
                return fMin * Math.pow(fMax / fMin, t)
            }
            function yForGain(g) { return height * (dbR - g) / (2 * dbR) }
            function gainForY(y) { return dbR - (y / height) * 2 * dbR }
            function yForCurveFreq(f) { return yForGain(Eq.magnitudeDb(f)) }
            // Hard floor/ceiling: a node can never be drawn (or dragged)
            // outside the graph box — so a deep cut stays grabbable at the
            // bottom edge instead of disappearing into the tile field below.
            function clampY(y) { return Math.max(11, Math.min(height - 11, y)) }

            Rectangle {
                anchors.fill: parent; color: "#0e1924"
                border.color: root.cGrid0; border.width: 1   // the bounded box
            }

            Canvas {
                id: curveCanvas
                anchors.fill: parent
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                Component.onCompleted: requestPaint()
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    var w = width, h = height
                    ctx.clearRect(0, 0, w, h)

                    // Vertical grid at decade-ish freqs.
                    ctx.lineWidth = 1
                    var fl = [50, 100, 200, 500, 1000, 2000, 5000, 10000]
                    ctx.strokeStyle = root.cGrid
                    for (var k = 0; k < fl.length; ++k) {
                        var gx = graph.xForFreq(fl[k])
                        ctx.beginPath(); ctx.moveTo(gx, 0); ctx.lineTo(gx, h); ctx.stroke()
                    }
                    // Horizontal dB grid every 3 dB across the ±15 axis.
                    // Major lines (every 6 dB) are brighter + labelled on the
                    // left so the operator reads gain vs flat; the 3 dB
                    // intermediates are dim, unlabelled tick lines.  0 dB =
                    // flat/centre, emphasised.
                    ctx.font = "9px 'Segoe UI', sans-serif"
                    ctx.textBaseline = "bottom"
                    for (var db = -12; db <= 12; db += 3) {
                        var gy = graph.yForGain(db)
                        var major = (db % 6) === 0
                        ctx.strokeStyle = (db === 0) ? root.cGrid0
                                        : (major ? root.cGrid : "#1a2935")
                        ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(w, gy); ctx.stroke()
                        if (db === 0 || major) {
                            ctx.fillStyle = (db === 0) ? root.cGrid0 : root.cMuted
                            ctx.fillText((db > 0 ? "+" : "") + db
                                         + (db === 0 ? " dB (flat)" : ""),
                                         5, gy - 2)
                        }
                    }

                    // Summed response curve — sample magnitudeDb per ~3 px.
                    function plot() {
                        ctx.beginPath()
                        var first = true
                        for (var x = 0; x <= w; x += 3) {
                            var f = graph.freqForX(x)
                            var y = graph.yForGain(Eq.magnitudeDb(f))
                            if (first) { ctx.moveTo(x, y); first = false }
                            else ctx.lineTo(x, y)
                        }
                    }
                    // Soft glow pass + crisp pass.
                    ctx.strokeStyle = "rgba(120,225,255,0.30)"; ctx.lineWidth = 6
                    plot(); ctx.stroke()
                    ctx.strokeStyle = root.cCurve; ctx.lineWidth = 2
                    plot(); ctx.stroke()
                }
            }

            // Visual band nodes (event handling is the MouseArea below).
            Repeater {
                model: Eq.numBands
                delegate: Item {
                    id: nodeWrap
                    required property int index
                    readonly property bool usesGain: { root.rev; return Eq.typeUsesGain(Eq.bandType(index)) }
                    readonly property real nx: { root.rev; return graph.xForFreq(Eq.bandFreq(index)) }
                    // Gain types (Peak/shelves) ride their gain handle; cut/
                    // pass types (LP/HP/BP/Notch) ride their response curve.
                    // clampY keeps the handle inside the graph box so even a
                    // deep notch's node stays grabbable at the bottom edge.
                    readonly property real ny: { root.rev
                        var raw = usesGain ? graph.yForGain(Eq.bandGain(index))
                                           : graph.yForCurveFreq(Eq.bandFreq(index))
                        return graph.clampY(raw) }
                    readonly property bool sel: { root.rev; return Eq.selectedBand === index }
                    readonly property bool en:  { root.rev; return Eq.bandEnabled(index) }
                    x: nx - 11; y: ny - 11; width: 22; height: 22

                    Rectangle {       // selection ring
                        anchors.centerIn: parent
                        width: 22; height: 22; radius: 11
                        color: "transparent"
                        visible: nodeWrap.sel
                        border.color: "#ffffff"; border.width: 2
                    }
                    Rectangle {       // the node
                        anchors.centerIn: parent
                        width: 15; height: 15; radius: 8
                        color: nodeWrap.en ? root.bandColor[index] : "#3a4350"
                        border.color: "#0d141b"; border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: nodeWrap.index + 1
                            color: "#0d141b"; font.bold: true; font.pixelSize: 9
                        }
                    }
                }
            }

            // Single interaction layer: press hit-tests the nearest node
            // (select + start drag), drag edits freq (+ gain for gain
            // types), double-click toggles enable, wheel = selected Q.
            MouseArea {
                id: graphMouse
                anchors.fill: parent
                property int dragBand: -1
                function nearest(mx, my) {
                    var best = -1, bd = 1e9
                    for (var i = 0; i < Eq.numBands; ++i) {
                        var nx = graph.xForFreq(Eq.bandFreq(i))
                        var ny = graph.clampY(Eq.typeUsesGain(Eq.bandType(i))
                                 ? graph.yForGain(Eq.bandGain(i))
                                 : graph.yForCurveFreq(Eq.bandFreq(i)))
                        var d = Math.hypot(mx - nx, my - ny)
                        if (d < bd) { bd = d; best = i }
                    }
                    return (best >= 0 && bd <= 22) ? best : -1
                }
                onPressed: (m) => {
                    var b = nearest(m.x, m.y)
                    dragBand = b
                    if (b >= 0) Eq.selectedBand = b
                }
                onPositionChanged: (m) => {
                    if (dragBand < 0) return
                    Eq.setBandFreq(dragBand, graph.freqForX(m.x))
                    if (Eq.typeUsesGain(Eq.bandType(dragBand)))
                        Eq.setBandGain(dragBand, graph.gainForY(m.y))
                }
                onReleased: dragBand = -1
                onDoubleClicked: (m) => {
                    var b = nearest(m.x, m.y)
                    if (b >= 0) Eq.setBandEnabled(b, !Eq.bandEnabled(b))
                }
                WheelHandler {
                    onWheel: (ev) => {
                        var i = Eq.selectedBand
                        if (i < 0) return
                        var q = Eq.bandQ(i) * (ev.angleDelta.y > 0 ? 1.15 : 0.87)
                        Eq.setBandQ(i, q)
                    }
                }
            }
        }

        // ── Tile row (the EESDR3 idiom) ────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            visible: !root.collapsed
            spacing: 4
            Repeater {
                model: Eq.numBands
                delegate: Rectangle {
                    id: tile
                    required property int index
                    Layout.fillWidth: true
                    implicitHeight: 58
                    radius: 4
                    readonly property bool sel: { root.rev; return Eq.selectedBand === index }
                    readonly property bool en:  { root.rev; return Eq.bandEnabled(index) }
                    readonly property int ty:   { root.rev; return Eq.bandType(index) }
                    color: sel ? "#13222c" : "#0b141b"
                    border.color: sel ? root.bandColor[index] : "#1c2a36"
                    border.width: sel ? 2 : 1
                    opacity: en ? 1.0 : 0.5

                    // Select / right-click-reset layer — declared FIRST so it
                    // sits BELOW the column; the Type label's own MouseArea
                    // (in the column above) wins clicks on the Type chip, and
                    // the mouse-transparent #/freq/gain labels fall through to
                    // here for select + reset.
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (m) => {
                            if (m.button === Qt.RightButton) Eq.resetBand(tile.index)
                            else Eq.selectedBand = tile.index
                        }
                    }
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 3
                        spacing: 1
                        Label {
                            text: "#" + (tile.index + 1)
                            color: root.bandColor[tile.index]
                            font.bold: true; font.pixelSize: 10
                            Layout.alignment: Qt.AlignHCenter
                        }
                        // Type chip — click opens the full 7-type picker
                        // (the ▾ signals it's a dropdown).  Sized to its text
                        // but the MouseArea is padded so it's an easy target.
                        Label {
                            id: typeLabel
                            text: { root.rev; return Eq.typeName(tile.ty) + " ▾" }
                            color: root.cAccent; font.pixelSize: 10
                            font.bold: true
                            Layout.alignment: Qt.AlignHCenter
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -4   // bigger hit target
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    Eq.selectedBand = tile.index
                                    typeMenu.band = tile.index
                                    typeMenu.popup()
                                }
                            }
                        }
                        Label {
                            text: { root.rev; return root.fmtFreq(Eq.bandFreq(tile.index)) }
                            color: root.cMuted; font.pixelSize: 10
                            font.family: "Consolas"
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Label {
                            text: { root.rev
                                if (Eq.typeUsesGain(tile.ty)) {
                                    var g = Eq.bandGain(tile.index)
                                    return (g >= 0 ? "+" : "") + g.toFixed(1)
                                }
                                return "Q " + Eq.bandQ(tile.index).toFixed(2) }
                            color: root.cText; font.pixelSize: 10
                            font.family: "Consolas"
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }
                }
            }
        }
    }
}

// Lyra — Vertical Ladder (Multi-source meter).
//
// Stacks N source rows top-to-bottom — label (left), filled level bar
// with palette-gradient keyed off THAT source's danger point (middle),
// value chip (right).  Audio-mixer "channel strip" / rack-mount vibe.
//
// TX populates 5+ rows (PWR/SWR/PA/VDD/T … ALC/MIC/COMP land when the
// TX audio chain does).  RX collapses to a useful 3-row stack
// (S-meter / noise floor / SNR).  Driven entirely by the
// `Meter.ladderRows` Q_PROPERTY — no per-row state lives in QML, so
// the model owns all peak-hold / smoothing decisions.

import QtQuick

Item {
    id: ladder
    anchors.fill: parent
    anchors.margins: 6

    // Shared palette helper (mirrors the per-row zone semantics the
    // Arc/Bar use — but each row carries its own `danger` threshold,
    // so a row goes red when ITS source is hot, not when the primary
    // is).  level + danger both 0..1.
    function zoneColor(level, danger) {
        if (level >= danger + 0.16) return "#ff4d4d"   // deep red
        if (level >= danger)        return "#ff9a3c"   // amber
        if (level >= danger * 0.55) return "#5dff9a"   // green
        return "#36d6ff"                                // cyan
    }

    // Row spacing dynamically scaled by row count so a TX 5-row stack
    // and an RX 3-row stack each fill the panel.
    readonly property int rowCount: Meter.ladderRows ? Meter.ladderRows.length : 0
    readonly property real rowH: rowCount > 0
        ? Math.max(20, (height - 8) / rowCount)
        : 24

    Column {
        anchors.fill: parent
        spacing: 2

        Repeater {
            model: Meter.ladderRows

            delegate: Item {
                width: ladder.width - 12
                height: Math.max(18, ladder.rowH - 4)

                // Glassy track — a recessed groove the level bar fills,
                // matching the Arc's "track in a glass channel" look.
                Rectangle {
                    id: track
                    anchors.left: label.right
                    anchors.right: chip.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    height: Math.max(10, parent.height * 0.55)
                    radius: height / 2
                    color: "#0c1218"
                    border.color: "#16242e"
                    border.width: 1
                }

                // Filled level — palette-gradient keyed off THIS row's
                // own danger threshold (so SWR red kicks in at 2.0:1
                // even if PWR is calm).
                Rectangle {
                    anchors.left: track.left
                    anchors.top: track.top
                    anchors.bottom: track.bottom
                    anchors.leftMargin: 2
                    width: Math.max(0,
                        (track.width - 4) * Math.max(0, Math.min(1, modelData.level)))
                    radius: track.radius - 1
                    color: ladder.zoneColor(modelData.level, modelData.danger)
                    // Soft glow via stacked translucent overlay (same
                    // "no shadowBlur" discipline the Arc uses — area
                    // blur is what stalled the panadapter at large sizes).
                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color: parent.color
                        opacity: 0.25
                        scale: 1.0
                    }
                }

                // Danger-zone tick — a thin pip on the track at the
                // operator's red-zone threshold for THIS source.  Lets
                // the operator see "how close am I to the edge" at a
                // glance without reading the value chip.
                Rectangle {
                    anchors.verticalCenter: track.verticalCenter
                    x: track.x + 2 +
                       (track.width - 4) * Math.max(0, Math.min(1, modelData.danger))
                    width: 2
                    height: track.height + 4
                    color: "#ff8a6a"
                    opacity: 0.6
                    visible: modelData.danger < 0.999
                }

                // Source label, left-justified.
                Text {
                    id: label
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 36
                    text: modelData.label
                    color: "#7790a0"
                    font.family: "Consolas"
                    font.bold: true
                    font.pixelSize: Math.max(10, parent.height * 0.42)
                    horizontalAlignment: Text.AlignLeft
                }

                // Value chip, right-justified — tinted by zone so a hot
                // source's number itself reads as red.
                Text {
                    id: chip
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 90
                    text: modelData.value
                    color: ladder.zoneColor(modelData.level, modelData.danger)
                    font.family: "Consolas"
                    font.pixelSize: Math.max(10, parent.height * 0.42)
                    horizontalAlignment: Text.AlignRight
                }
            }
        }
    }
}

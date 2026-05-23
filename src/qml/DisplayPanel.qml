// Lyra — Display dock panel (front-facing display controls).
//
// Mirrors old Lyra's DISPLAY (ViewPanel): the few spectrum/waterfall
// controls the operator reaches for mid-QSO live in this thin panel,
// one click away instead of buried in Settings.  Everything here ALSO
// lives in Settings → Visuals and binds the SAME Prefs object, so the
// two stay in sync (single source of truth) — exactly the old-Lyra
// arrangement.
//
// SCOPE (operator-locked 2026-05-22): only controls that were on old
// Lyra's FRONT Display panel AND exist in the C++ build today belong
// here.  That is currently Spec FPS + Waterfall rate.  Palette,
// smoothing, glow, gridline etc. stay in Settings → Visuals (that's
// where they were in old Lyra — don't promote them here).  As Zoom,
// Panafall Step, Exact/100 Hz, and Peak Hold land, they slot in here in
// old Lyra's layout.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    implicitHeight: 50
    implicitWidth: 460
    color: "#101820"
    border.color: "#2a4a5a"

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 8

        // ----- Spectrum frame rate -----
        Label { text: qsTr("Spec"); color: "#cccccc"; font.bold: true }
        Slider {
            id: fpsSlider
            Layout.preferredWidth: 130
            from: 5; to: 120; stepSize: 1
            value: Prefs.targetFps
            onMoved: Prefs.targetFps = Math.round(value)
            // Mouse-wheel nudges ±1 (sets the bound Prefs source so the
            // value binding stays intact — never assign slider.value).
            WheelHandler {
                onWheel: (ev) => {
                    Prefs.targetFps = Math.max(fpsSlider.from,
                        Math.min(fpsSlider.to,
                            Prefs.targetFps + (ev.angleDelta.y > 0 ? 1 : -1)))
                }
            }
        }
        Label {
            text: Prefs.targetFps + qsTr(" fps")
            color: "#cdd9e5"; font.family: "Consolas"; font.bold: true
            Layout.preferredWidth: 56
        }

        Rectangle {   // divider
            Layout.preferredWidth: 1
            Layout.topMargin: 10; Layout.bottomMargin: 10
            Layout.fillHeight: true
            color: "#2a4a5a"
        }

        // ----- Waterfall scroll rate -----
        Label { text: qsTr("WF"); color: "#cccccc"; font.bold: true }
        Slider {
            id: wfSlider
            Layout.preferredWidth: 130
            from: 1; to: 120; stepSize: 1
            value: Prefs.waterfallSpeed
            onMoved: Prefs.waterfallSpeed = Math.round(value)
            WheelHandler {
                onWheel: (ev) => {
                    Prefs.waterfallSpeed = Math.max(wfSlider.from,
                        Math.min(wfSlider.to,
                            Prefs.waterfallSpeed + (ev.angleDelta.y > 0 ? 1 : -1)))
                }
            }
        }
        Label {
            text: Prefs.waterfallSpeed + qsTr(" rows/s")
            color: "#cdd9e5"; font.family: "Consolas"; font.bold: true
            Layout.preferredWidth: 64
        }

        Item { Layout.fillWidth: true }
    }
}

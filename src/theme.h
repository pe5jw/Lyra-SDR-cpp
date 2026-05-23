// Lyra — application theme (QtWidgets stylesheet).
//
// Ported from the Python-Lyra theme (lyra/ui/theme.py): the cool-CRT
// dark scheme — deep blue-black surfaces, electric-cyan primary,
// neon-green secondary.  Applied app-wide via qApp->setStyleSheet so
// the QtWidgets shell (menu bar, dock title bars, Settings dialog,
// tabs, spin boxes, sliders, etc.) matches old Lyra instead of the
// default light Windows style.  The panadapter glass background is the
// one deliberate new element; everything else tracks old Lyra.
//
// Tokens (resolved to literals here):
//   BG_APP    rgb(10,13,18)    BG_PANEL  rgb(17,22,32)
//   BG_RECESS rgb(16,20,28)    BG_CTRL   rgb(22,28,40)
//   ACCENT    rgb(0,229,255)   ACCENT_DIM rgb(0,168,200)
//   ACCENT2   rgb(57,255,20)
//   TEXT_PRIMARY rgb(205,217,229)  TEXT_MUTED rgb(138,154,172)
//   TEXT_FAINT rgb(90,112,128)     BORDER rgb(30,42,58)

#pragma once

#include <QString>

namespace lyra::ui {

inline QString lyraStyleSheet() {
    return QStringLiteral(R"QSS(
QMainWindow, QWidget {
    background: rgb(10,13,18);
    color: rgb(205,217,229);
}
QLabel { color: rgb(138,154,172); }

QToolTip {
    background: rgb(17,22,32);
    color: rgb(205,217,229);
    border: 1px solid rgb(0,229,255);
    border-radius: 4px;
    padding: 8px 10px;
    font-size: 13pt;
}

QLineEdit, QComboBox {
    background: rgb(22,28,40);
    color: rgb(205,217,229);
    border: 1px solid rgb(30,42,58);
    border-radius: 3px;
    padding: 4px 6px;
    selection-background-color: rgba(0,229,255,80);
}
QLineEdit:focus, QComboBox:focus { border-color: rgb(0,229,255); }
QComboBox QAbstractItemView {
    background: rgb(17,22,32);
    color: rgb(205,217,229);
    border: 1px solid rgb(30,42,58);
    selection-background-color: rgba(0,229,255,80);
}

QPushButton {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 rgb(22,28,40), stop:1 rgb(16,20,28));
    color: rgb(205,217,229);
    border: 1px solid rgb(30,42,58);
    border-radius: 4px;
    padding: 5px 12px;
    font-weight: 600;
    letter-spacing: 0.5px;
}
QPushButton:hover { border-color: rgb(0,229,255); color: rgb(0,229,255); }
QPushButton:checked {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 rgb(0,168,200), stop:1 rgb(22,28,40));
    border-color: rgb(0,229,255);
    color: rgb(10,13,18);
}
QPushButton:pressed { background: rgb(16,20,28); }
QPushButton:disabled {
    background: rgb(16,20,28);
    color: rgb(90,112,128);
    border-color: rgb(30,42,58);
}

QSlider::groove:horizontal {
    height: 4px;
    background: rgb(16,20,28);
    border-radius: 2px;
    border: 1px solid rgb(30,42,58);
}
QSlider::handle:horizontal {
    background: rgb(0,229,255);
    width: 12px; margin: -6px 0;
    border-radius: 2px;
    border: 1px solid rgb(10,13,18);
}
QSlider::handle:horizontal:hover { background: rgb(57,255,20); }

QStatusBar {
    background: rgb(16,20,28);
    color: rgb(138,154,172);
    border-top: 1px solid rgb(30,42,58);
}

/* Dockable panels */
QDockWidget {
    color: rgb(205,217,229);
    font-weight: 600;
    border: 1px solid rgb(30,42,58);
}
QDockWidget::title {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 rgb(17,22,32), stop:0.5 rgb(22,28,40), stop:1 rgb(16,20,28));
    color: rgb(0,229,255);
    padding-left: 8px;
    padding-top: 6px;
    padding-bottom: 6px;
    border-bottom: 1px solid rgba(0,229,255,100);
}
QDockWidget::close-button, QDockWidget::float-button {
    subcontrol-position: right center;
    padding: 1px;
    margin-right: 2px;
    border: 1px solid transparent;
    border-radius: 3px;
    background: transparent;
}
QDockWidget::close-button:hover, QDockWidget::float-button:hover {
    background: rgba(0,229,255,60);
    border: 1px solid rgb(0,229,255);
}

QTabWidget::pane {
    background: rgb(17,22,32);
    border: 1px solid rgb(30,42,58);
    border-radius: 4px;
    top: -1px;
}
QTabBar::tab {
    background: rgb(16,20,28);
    color: rgb(138,154,172);
    padding: 6px 14px;
    margin-right: 2px;
    border: 1px solid rgb(30,42,58);
    border-bottom: none;
    border-top-left-radius: 4px;
    border-top-right-radius: 4px;
    min-width: 70px;
}
QTabBar::tab:hover {
    color: rgb(205,217,229);
    background: rgb(22,28,40);
    border-color: rgb(0,229,255);
}
QTabBar::tab:selected {
    background: rgb(17,22,32);
    color: rgb(0,229,255);
    border-color: rgb(0,229,255);
    border-bottom: 1px solid rgb(17,22,32);
    font-weight: 700;
}

QGroupBox {
    color: rgb(0,229,255);
    font-weight: 700;
    border: 1px solid rgb(30,42,58);
    border-radius: 4px;
    margin-top: 14px;
    padding: 8px 6px 6px 6px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 10px;
    padding: 0 6px;
    background: rgb(10,13,18);
    color: rgb(0,229,255);
}

QCheckBox { color: rgb(205,217,229); spacing: 7px; }
QCheckBox::indicator {
    width: 16px; height: 16px;
    background: rgb(16,20,28);
    border: 1px solid rgb(138,154,172);
    border-radius: 3px;
}
QCheckBox::indicator:hover { border-color: rgb(0,229,255); background: rgb(17,22,32); }
QCheckBox::indicator:checked { background: rgb(0,229,255); border-color: rgb(0,229,255); }

QRadioButton { color: rgb(205,217,229); spacing: 6px; padding: 3px 0; }
QRadioButton::indicator {
    width: 16px; height: 16px;
    background: rgb(16,20,28);
    border: 1px solid rgb(138,154,172);
    border-radius: 9px;
}
QRadioButton::indicator:hover { border-color: rgb(0,229,255); background: rgb(17,22,32); }
QRadioButton::indicator:checked {
    background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5,
        stop:0 rgb(0,229,255), stop:0.45 rgb(0,229,255),
        stop:0.55 rgb(16,20,28), stop:1 rgb(16,20,28));
    border-color: rgb(0,229,255);
}

QAbstractSpinBox {
    background: rgb(22,28,40);
    color: rgb(205,217,229);
    border: 1px solid rgb(30,42,58);
    border-radius: 3px;
    padding: 3px 20px 3px 4px;
}
QAbstractSpinBox:focus { border-color: rgb(0,229,255); }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {
    subcontrol-origin: border;
    width: 16px;
    background: rgb(22,28,40);
    border-left: 1px solid rgb(30,42,58);
}
QAbstractSpinBox::up-button { subcontrol-position: top right; border-bottom: 1px solid rgb(30,42,58); }
QAbstractSpinBox::down-button { subcontrol-position: bottom right; }
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { background: rgb(0,168,200); }
QAbstractSpinBox::up-arrow {
    width: 0; height: 0;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-bottom: 5px solid rgb(205,217,229);
}
QAbstractSpinBox::up-arrow:hover { border-bottom-color: rgb(0,229,255); }
QAbstractSpinBox::down-arrow {
    width: 0; height: 0;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid rgb(205,217,229);
}
QAbstractSpinBox::down-arrow:hover { border-top-color: rgb(0,229,255); }

QSplitter::handle { background: rgb(16,20,28); border: 1px solid rgb(30,42,58); }
QSplitter::handle:hover { background: rgba(0,229,255,120); border-color: rgb(0,229,255); }

QMenuBar { background: rgb(10,13,18); color: rgb(205,217,229); border-bottom: 1px solid rgb(30,42,58); }
QMenuBar::item { background: transparent; padding: 4px 10px; }
QMenuBar::item:selected { background: rgba(0,229,255,50); color: rgb(0,229,255); }
QMenu { background: rgb(17,22,32); color: rgb(205,217,229); border: 1px solid rgb(30,42,58); }
QMenu::item { padding: 5px 24px 5px 12px; }
QMenu::item:selected { background: rgba(0,229,255,50); color: rgb(0,229,255); }
QDialog { background: rgb(10,13,18); }
QToolBar { background: rgb(10,13,18); border: none; spacing: 4px; }
)QSS");
}

} // namespace lyra::ui

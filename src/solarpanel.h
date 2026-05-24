// Lyra — solar / HF-propagation panel (dockable "PROP" strip).
//
// A compact horizontal readout (ported from old Lyra's
// propagation_panel.py): SFI / A-index / K-index value boxes (colour-
// coded) plus a 10-band Good/Fair/Poor heat-map keyed to the operator's
// current day or night.  Driven by SolarService; pure QtWidgets.

#pragma once

#include <QHash>
#include <QWidget>

class QLabel;
class QToolButton;

namespace lyra::solar { class SolarService; }

namespace lyra::ui {

class NcdxfFollow;

class SolarPanel : public QWidget {
    Q_OBJECT
public:
    SolarPanel(lyra::solar::SolarService *svc, NcdxfFollow *follow,
               QWidget *parent = nullptr);

private:
    QWidget *makeValueBox(const QString &key, QLabel **valueOut);
    void buildFollowButton();
    void refreshFollowLabel();
    void refresh();

    lyra::solar::SolarService *svc_ = nullptr;
    NcdxfFollow               *follow_ = nullptr;
    QToolButton               *followBtn_ = nullptr;
    QLabel  *sfiVal_ = nullptr;
    QLabel  *aVal_   = nullptr;
    QLabel  *kVal_   = nullptr;
    QWidget *sfiBox_ = nullptr;            // tooltip carrier for extras
    QHash<QString, QLabel *> bandCells_;   // band → coloured cell
};

} // namespace lyra::ui

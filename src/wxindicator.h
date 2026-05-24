// Lyra — weather-alert header indicator (⚡ lightning / 💨 wind / ⚠ severe).
//
// Three colored badges that appear in the toolbar between the clocks and
// the right edge when an alert is active, color-tiered yellow→orange→red,
// and FLASH on the top (red) tier — Close lightning, Extreme wind, active
// Severe warning.  Driven by WxService::snapshotChanged.

#pragma once

#include <QWidget>

#include "wxservice.h"

class QLabel;
class QTimer;

namespace lyra::ui {

class WxIndicator : public QWidget {
    Q_OBJECT
public:
    explicit WxIndicator(lyra::wx::WxService *svc, QWidget *parent = nullptr);

private slots:
    void onSnapshot(const lyra::wx::WxSnapshot &snap);
    void onEnabled(bool on);
    void onFlash();

private:
    void restyle();

    lyra::wx::WxService *svc_ = nullptr;
    QLabel *lightning_ = nullptr;
    QLabel *wind_      = nullptr;
    QLabel *severe_    = nullptr;
    QTimer *flashTimer_ = nullptr;
    bool    flashOn_   = true;
    // Current tier colour per badge (empty = hidden) + whether it's the
    // flashing (red) tier.
    QString lightColor_, windColor_, severeColor_;
    bool    lightAlert_ = false, windAlert_ = false, severeAlert_ = false;
};

} // namespace lyra::ui

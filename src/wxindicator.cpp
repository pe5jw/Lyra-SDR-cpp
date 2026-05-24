// Lyra — weather-alert header indicator.  See wxindicator.h.

#include "wxindicator.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>

#include <cmath>

namespace lyra::ui {

using lyra::wx::Lightning;
using lyra::wx::Wind;
using lyra::wx::Severe;
using lyra::wx::WxSnapshot;

namespace {
constexpr const char *kYellow = "#ffd700";
constexpr const char *kOrange = "#ff8c00";
constexpr const char *kRed    = "#ff4444";
constexpr const char *kDim    = "#5a6573";   // off-phase (flash) colour

QString badgeStyle(const QString &color) {
    return QStringLiteral(
        "QLabel{color:%1;font-family:Consolas,monospace;font-weight:700;"
        "font-size:16px;padding:3px 12px;border:1px solid %1;"
        "border-radius:10px;background:rgba(255,255,255,0.05);}").arg(color);
}
QString compass8(double deg) {
    static const char *d[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return QString::fromLatin1(d[static_cast<int>((deg + 22.5) / 45.0) % 8]);
}
} // namespace

WxIndicator::WxIndicator(lyra::wx::WxService *svc, QWidget *parent)
    : QWidget(parent), svc_(svc) {
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(8, 2, 16, 2);
    h->setSpacing(10);

    auto mk = [this, h]() {
        auto *l = new QLabel(this);
        l->setAlignment(Qt::AlignVCenter | Qt::AlignCenter);
        l->setVisible(false);
        h->addWidget(l);
        return l;
    };
    lightning_ = mk();
    wind_      = mk();
    severe_    = mk();

    flashTimer_ = new QTimer(this);
    flashTimer_->setInterval(550);
    connect(flashTimer_, &QTimer::timeout, this, &WxIndicator::onFlash);

    if (svc_) {
        connect(svc_, &lyra::wx::WxService::snapshotChanged,
                this, &WxIndicator::onSnapshot);
        connect(svc_, &lyra::wx::WxService::enabledChanged,
                this, &WxIndicator::onEnabled);
        onSnapshot(svc_->lastSnapshot());
        onEnabled(svc_->enabled());
    }
}

void WxIndicator::onEnabled(bool on) {
    if (!on) {
        lightning_->setVisible(false);
        wind_->setVisible(false);
        severe_->setVisible(false);
        flashTimer_->stop();
    }
}

void WxIndicator::onSnapshot(const WxSnapshot &snap) {
    // ---- Lightning ----
    if (snap.lightning == Lightning::None) {
        lightColor_.clear();
        lightning_->setVisible(false);
    } else {
        lightColor_ = (snap.lightning == Lightning::Close) ? QString::fromLatin1(kRed)
                    : (snap.lightning == Lightning::Mid)   ? QString::fromLatin1(kOrange)
                                                           : QString::fromLatin1(kYellow);
        lightAlert_ = (snap.lightning == Lightning::Close);
        const QString dist = (svc_ && svc_->distanceUnit() == QStringLiteral("km"))
            ? tr("%1 km").arg(qRound(snap.closestKm))
            : tr("%1 mi").arg(qRound(snap.closestKm / 1.60934));
        lightning_->setText(QStringLiteral("⚡ %1 · %2").arg(dist, compass8(snap.bearingDeg)));
        lightning_->setToolTip(tr("Lightning — closest strike %1, bearing %2°")
                                   .arg(dist).arg(qRound(snap.bearingDeg)));
        lightning_->setVisible(true);
    }

    // ---- Wind ----
    if (snap.wind == Wind::None) {
        windColor_.clear();
        wind_->setVisible(false);
    } else {
        windColor_ = (snap.wind == Wind::Extreme)  ? QString::fromLatin1(kRed)
                   : (snap.wind == Wind::High)      ? QString::fromLatin1(kOrange)
                                                    : QString::fromLatin1(kYellow);
        windAlert_ = (snap.wind == Wind::Extreme);
        const bool gust = snap.windGustMph > 0;
        double v = gust ? snap.windGustMph : snap.windSustainedMph;
        QString u = QStringLiteral("mph");
        if (svc_ && svc_->windUnit() == QStringLiteral("kt"))   { v *= 0.868976; u = QStringLiteral("kt"); }
        else if (svc_ && svc_->windUnit() == QStringLiteral("km/h")) { v *= 1.60934; u = QStringLiteral("km/h"); }
        wind_->setText(QStringLiteral("💨 %1%2 %3")
                           .arg(gust ? QStringLiteral("G") : QString())
                           .arg(qRound(v)).arg(u));
        wind_->setToolTip(snap.windHeadline.isEmpty()
            ? tr("Wind — sustained %1 / gust %2 mph")
                  .arg(qRound(snap.windSustainedMph)).arg(qRound(snap.windGustMph))
            : snap.windHeadline);
        wind_->setVisible(true);
    }

    // ---- Severe ----
    if (snap.severe == Severe::Active) {
        severeColor_ = QString::fromLatin1(kRed);
        severeAlert_ = true;
        severe_->setText(QStringLiteral("⚠"));
        severe_->setToolTip(snap.severeHeadline.isEmpty()
                                ? tr("Severe weather warning active")
                                : snap.severeHeadline);
        severe_->setVisible(true);
    } else {
        severeColor_.clear();
        severe_->setVisible(false);
    }

    const bool anyAlert = (lightAlert_ && !lightColor_.isEmpty()) ||
                          (windAlert_  && !windColor_.isEmpty())  ||
                          (severeAlert_ && !severeColor_.isEmpty());
    if (anyAlert && !flashTimer_->isActive()) { flashOn_ = true; flashTimer_->start(); }
    else if (!anyAlert && flashTimer_->isActive()) { flashTimer_->stop(); flashOn_ = true; }
    restyle();
}

void WxIndicator::onFlash() {
    flashOn_ = !flashOn_;
    restyle();
}

void WxIndicator::restyle() {
    auto apply = [this](QLabel *l, const QString &color, bool alert) {
        if (color.isEmpty()) return;
        const QString c = (alert && !flashOn_) ? QString::fromLatin1(kDim) : color;
        l->setStyleSheet(badgeStyle(c));
    };
    apply(lightning_, lightColor_, lightAlert_);
    apply(wind_,      windColor_,  windAlert_);
    apply(severe_,    severeColor_, severeAlert_);
}

} // namespace lyra::ui

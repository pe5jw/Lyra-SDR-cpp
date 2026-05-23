// Lyra — QML→C++ help bridge.
//
// Exposed to every embedded QML panel as the `Help` context property
// (alongside Prefs).  Each panel's "?" badge calls one of these from a
// small Help / Settings menu, passing the panel's topic string.  The
// methods just emit signals; MainWindow connects them to the in-app
// User-Guide viewer and the Settings dialog so this object stays
// decoupled from the window (no include cycle, mirrors old Lyra's
// duck-typed show_help).

#pragma once

#include <QObject>
#include <QString>

namespace lyra::ui {

class Help : public QObject {
    Q_OBJECT
public:
    explicit Help(QObject *parent = nullptr) : QObject(parent) {}

    // Open the in-app User Guide at <topic>'s section.
    Q_INVOKABLE void openGuide(const QString &topic) {
        emit guideRequested(topic);
    }
    // Jump to the Settings tab/section matching <topic>.
    Q_INVOKABLE void openSettings(const QString &topic) {
        emit settingsRequested(topic);
    }

signals:
    void guideRequested(const QString &topic);
    void settingsRequested(const QString &topic);
};

} // namespace lyra::ui

// Lyra — lightweight status-message bus.
//
// A single QObject both C++ and QML can use to post a transient message
// to the main-window status bar (out-of-band advisories, "Tuned to …"
// after a band-plan marker click, etc.).  Exposed to QML as `Status`;
// MainWindow connects message() to statusBar()->showMessage().

#pragma once

#include <QObject>
#include <QString>

namespace lyra::ui {

class StatusBus : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    // Post a transient status message (ms = 0 means until replaced).
    Q_INVOKABLE void show(const QString &text, int ms = 3000) {
        emit message(text, ms);
    }

signals:
    void message(const QString &text, int ms);
};

} // namespace lyra::ui

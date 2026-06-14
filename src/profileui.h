// Lyra — ProfileUi: the QtWidgets-native bits of the profile UX that the
// front ProfilePanel (a QML QQuickWidget dock) can't do itself.
//
// A top-level QML popup hosted from a QQuickWidget doesn't receive
// keyboard focus, so a QML TextField for the profile name can't be typed
// into (same limitation AudioPanel works around with native dialogs).
// So the "Save profile" flow is a native QDialog here: overwrite the
// active profile OR name a new one, plus an optional "auto-recall for
// mode family" binding (defaulting to the current mode's family) — the
// same association the Settings → Profiles per-family table manages.
//
// Kept OUT of ProfileManager so that class stays Qt-Core-only (its unit
// test links Core, not Widgets).  Exposed to QML as the `ProfileUi`
// context property; the front panel's Save button calls openSaveDialog().

#pragma once

#include <QObject>

class QWidget;
namespace lyra::profile { class ProfileManager; }

namespace lyra::ui {

class Prefs;

class ProfileUi : public QObject {
    Q_OBJECT
public:
    ProfileUi(lyra::profile::ProfileManager *mgr, Prefs *prefs,
              QWidget *parent = nullptr);

    // Native "Save Profile" dialog (overwrite active / new name + family
    // auto-recall).  No-op if there's no ProfileManager.
    Q_INVOKABLE void openSaveDialog();

private:
    lyra::profile::ProfileManager *mgr_   = nullptr;
    Prefs                         *prefs_ = nullptr;
    QWidget                       *parent_ = nullptr;
};

}  // namespace lyra::ui

// Lyra — ProfileManager: orchestrates the named-profile store + the live
// state (via ProfileBindings) + dirty-tracking + per-mode auto-recall.
//
// Stage 0 (docs/architecture/PROFILE_MODEL_STAGE0_DESIGN.md): whole-set
// capture/apply; mid-TX guard on every apply (never switch source/BW
// while keyed — §15.25); per-mode binding recalls a profile on mode
// change.  Decoupled from Prefs/HL2Stream/WdspEngine via ProfileBindings
// so it unit-tests with injected fakes.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include "profile/Profile.h"
#include "profile/ProfileBindings.h"
#include "profile/ProfileStore.h"

namespace lyra::profile {

class ProfileManager : public QObject {
    Q_OBJECT
public:
    ProfileManager(ProfileBindings bindings, ProfileStore store,
                   QObject *parent = nullptr);

    QStringList names() const            { return store_.names(); }
    QString     activeName() const       { return store_.active(); }
    QString     defaultName() const      { return store_.defaultName(); }
    bool        isModified() const;      // live capture != active baseline

    // Save the CURRENT live state into the active profile (no-op if no
    // active profile — use saveAs first).
    Q_INVOKABLE void saveActive();
    // Capture live state into a (new or overwritten) named profile and
    // make it active.
    Q_INVOKABLE void saveAs(const QString &name);
    // Recall a stored profile (apply its values + make active).  GUARDED:
    // a no-op while TX is active.  Returns true if applied.
    Q_INVOKABLE bool load(const QString &name);
    Q_INVOKABLE void remove(const QString &name);
    Q_INVOKABLE void rename(const QString &oldName, const QString &newName);
    Q_INVOKABLE void setDefault(const QString &name);
    // Apply the default profile (if set) — call once at startup.
    Q_INVOKABLE void applyDefaultAtStartup();

    Q_INVOKABLE QString modeBinding(const QString &mode) const
        { return store_.modeBinding(mode); }
    Q_INVOKABLE void bindMode(const QString &mode, const QString &name);
    Q_INVOKABLE void unbindMode(const QString &mode);

public slots:
    // Connect to Prefs::modeChanged — auto-recalls the bound profile.
    void onModeChanged(const QString &mode);
    // Recompute the modified state + emit modifiedChanged on transition.
    // main.cpp connects the relevant Prefs/stream/engine change signals
    // here so the front-dock "● modified" indicator stays live.
    void refreshModified();

signals:
    void namesChanged();
    void activeChanged(const QString &name);
    void modifiedChanged(bool modified);

private:
    void setBaselineFromActive();

    ProfileBindings b_;
    ProfileStore    store_;
    Profile         baseline_;        // active profile as stored
    bool            hasBaseline_ = false;
    bool            lastModified_ = false;
    bool            applying_ = false; // suppress dirty churn during apply
};

}  // namespace lyra::profile

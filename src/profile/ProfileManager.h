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
    // Reactive surface for the front ProfilePanel QML dock (#55).
    // Getters + NOTIFY signals already exist; these just expose them
    // as bindable QML properties.
    Q_PROPERTY(QStringList names READ names NOTIFY namesChanged)
    Q_PROPERTY(QString activeName READ activeName NOTIFY activeChanged)
    Q_PROPERTY(bool modified READ isModified NOTIFY modifiedChanged)
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
    // Explicit OPERATOR selection (Settings "Load" + the front quick-recall
    // dock): applies the profile AND emits userLoaded() so a companion app can
    // launch.  Auto-recall (onModeChanged) + startup-default call load()
    // directly and so NEVER fire userLoaded — only a deliberate pick launches
    // the companion app (#193).
    Q_INVOKABLE bool loadByUser(const QString &name);
    Q_INVOKABLE void remove(const QString &name);
    Q_INVOKABLE void rename(const QString &oldName, const QString &newName);
    Q_INVOKABLE void setDefault(const QString &name);
    // Apply the default profile (if set) — call once at startup.
    Q_INVOKABLE void applyDefaultAtStartup();

    // Per-FAMILY auto-recall bindings.  <family> is one of modeFamilies()
    // (CW/SSB/Digital/AM/SAM/DSB/FM) — sidebands collapse, since the TX/RX
    // chain is identical regardless of USB-vs-LSB etc.
    Q_INVOKABLE QString modeBinding(const QString &family) const
        { return store_.modeBinding(family); }
    Q_INVOKABLE void bindMode(const QString &family, const QString &name);
    Q_INVOKABLE void unbindMode(const QString &family);

    // Map a specific demod mode to its binding family.  USB/LSB->SSB,
    // CWU/CWL->CW, DIGU/DIGL->Digital; AM/SAM/DSB/FM stand alone; any
    // other mode maps to itself.  onModeChanged() + the Settings bind
    // table both key on these names.
    static QString     modeFamily(const QString &mode);
    static QStringList modeFamilies();   // canonical families, display order

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
    // Emitted ONLY on an explicit operator pick (loadByUser) — drives the
    // companion-app launch (#193).  NOT emitted by auto-recall / startup.
    void userLoaded(const QString &name);

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

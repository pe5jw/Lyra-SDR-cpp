// Lyra — ProfileStore: QSettings-JSON persistence for profiles.
//
// Layout (operator decision 2026-06-14 — QSettings-JSON, no SQLite):
//   profiles/order            StringList  display/recall order
//   profiles/active           QString     last-loaded profile name
//   profiles/default          QString     applied at startup
//   profiles/item/<name>      QString     compact JSON of the Profile
//   profiles/modeBind/<MODE>  QString     per-mode auto-recall target
//
// QSettings* is injected so unit tests use a temp INI file.

#pragma once

#include <QString>
#include <QStringList>
#include "profile/Profile.h"

class QSettings;

namespace lyra::profile {

class ProfileStore {
public:
    explicit ProfileStore(QSettings *settings) : s_(settings) {}

    QStringList names() const;                  // profiles/order
    bool        contains(const QString &name) const;
    Profile     get(const QString &name) const; // {} if absent
    void        put(const Profile &p);          // writes item; appends to order if new
    void        remove(const QString &name);
    void        rename(const QString &oldName, const QString &newName);

    QString active() const;
    void    setActive(const QString &name);
    QString defaultName() const;
    void    setDefault(const QString &name);

    QString modeBinding(const QString &mode) const;        // "" if none
    void    setModeBinding(const QString &mode, const QString &name);
    void    clearModeBinding(const QString &mode);

private:
    QString itemKey(const QString &name) const;
    QSettings *s_;
};

}  // namespace lyra::profile

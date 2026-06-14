// Lyra — ProfileManager.  See ProfileManager.h.

#include "profile/ProfileManager.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcProfile, "lyra.profile")

namespace lyra::profile {

ProfileManager::ProfileManager(ProfileBindings bindings, ProfileStore store,
                               QObject *parent)
    : QObject(parent), b_(std::move(bindings)), store_(std::move(store)) {
    // Seed the dirty baseline from whatever profile is marked active.
    setBaselineFromActive();
}

void ProfileManager::setBaselineFromActive() {
    const QString a = store_.active();
    if (!a.isEmpty() && store_.contains(a)) {
        baseline_ = store_.get(a);
        hasBaseline_ = true;
    } else {
        hasBaseline_ = false;
    }
    lastModified_ = isModified();
}

bool ProfileManager::isModified() const {
    if (!hasBaseline_ || !b_.capture) return false;
    return !b_.capture().sameValues(baseline_);
}

void ProfileManager::saveActive() {
    const QString a = store_.active();
    if (a.isEmpty()) {
        qCWarning(lcProfile) << "saveActive with no active profile — use saveAs";
        return;
    }
    saveAs(a);
}

void ProfileManager::saveAs(const QString &name) {
    if (name.isEmpty() || !b_.capture) return;
    const bool isNew = !store_.contains(name);
    Profile p = b_.capture();
    p.name = name;
    store_.put(p);
    store_.setActive(name);
    baseline_ = p;
    hasBaseline_ = true;
    if (isNew) emit namesChanged();
    emit activeChanged(name);
    emit modifiedChanged(false);
    lastModified_ = false;
}

bool ProfileManager::load(const QString &name) {
    if (!store_.contains(name) || !b_.apply) return false;
    // Mid-TX guard: never switch source/BW while keyed (§15.25).
    if (b_.isTxActive && b_.isTxActive()) {
        qCWarning(lcProfile) << "load(" << name << ") refused — TX active";
        return false;
    }
    Profile p = store_.get(name);
    b_.apply(p);
    store_.setActive(name);
    baseline_ = p;
    hasBaseline_ = true;
    emit activeChanged(name);
    emit modifiedChanged(false);
    lastModified_ = false;
    return true;
}

void ProfileManager::remove(const QString &name) {
    if (!store_.contains(name)) return;
    const bool wasActive = (store_.active() == name);
    store_.remove(name);
    if (wasActive) setBaselineFromActive();   // active cleared by store_.remove
    emit namesChanged();
    if (wasActive) { emit activeChanged(store_.active()); emit modifiedChanged(isModified()); }
}

void ProfileManager::rename(const QString &oldName, const QString &newName) {
    if (oldName == newName || newName.isEmpty() || !store_.contains(oldName)
        || store_.contains(newName)) return;
    store_.rename(oldName, newName);
    if (hasBaseline_ && baseline_.name == oldName) baseline_.name = newName;
    emit namesChanged();
    if (store_.active() == newName) emit activeChanged(newName);
}

void ProfileManager::setDefault(const QString &name) {
    if (store_.contains(name)) store_.setDefault(name);
}

void ProfileManager::applyDefaultAtStartup() {
    const QString d = store_.defaultName();
    if (!d.isEmpty() && store_.contains(d)) load(d);
}

void ProfileManager::bindMode(const QString &mode, const QString &name) {
    if (mode.isEmpty() || name.isEmpty()) return;
    store_.setModeBinding(mode, name);
}

void ProfileManager::unbindMode(const QString &mode) {
    store_.clearModeBinding(mode);
}

void ProfileManager::onModeChanged(const QString &mode) {
    const QString bound = store_.modeBinding(mode);
    if (bound.isEmpty() || bound == store_.active() || !store_.contains(bound))
        return;
    // load() applies the mid-TX guard itself.
    load(bound);
}

void ProfileManager::refreshModified() {
    const bool m = isModified();
    if (m != lastModified_) {
        lastModified_ = m;
        emit modifiedChanged(m);
    }
}

}  // namespace lyra::profile
